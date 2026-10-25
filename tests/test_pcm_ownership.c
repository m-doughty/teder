#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <stdio.h>

#define OK(call) ck_assert((call) == TEDER_OK)
static _Thread_local bool callback;
typedef struct {
	th_tracker tracker;
	atomic_uint violations;
	td_arena *arena;
	td_pcm_pool pool;
} fixture;
typedef struct { unsigned calls; bool on_callback; uint64_t source; td_pcm_generation *generation; } cleanup;
static void *allocate(void *p, size_t bytes, const char *site)
{
	fixture *f = p; if (callback) atomic_fetch_add(&f->violations, 1);
	return th_alloc(&f->tracker, bytes, site);
}
static void release(void *p, void *data)
{
	fixture *f = p; if (callback) atomic_fetch_add(&f->violations, 1);
	th_free(&f->tracker, data);
}
static td_allocator allocator(fixture *f) { return (td_allocator){f, allocate, release}; }
static void init(fixture *f, uint32_t frames)
{
	ck_assert(th_tracker_init(&f->tracker)); atomic_init(&f->violations, 0);
	size_t bytes = 0; td_pcm_layout layout;
	ck_assert(td_pcm_plan(&bytes, frames, &layout));
	f->arena = td_arena_create(allocator(f), bytes); ck_assert_ptr_nonnull(f->arena);
	void *base = td_arena_alloc(f->arena, bytes, 1, _Alignof(max_align_t)); ck_assert_ptr_nonnull(base);
	td_pcm_init(&f->pool, base, &layout, frames);
}
static void uninit(fixture *f)
{
	td_pcm_clear(&f->pool); td_arena_destroy(f->arena);
	ck_assert(atomic_load(&f->violations) == 0 && th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
}
static void destroyed(void *p)
{
	cleanup *c = p; c->calls++; c->on_callback = callback; c->source = c->generation->source;
}
static td_pcm_generation *generation(fixture *f, uint64_t epoch, uint64_t source, uint64_t track, cleanup *c)
{
	td_arena *arena = td_arena_create(allocator(f), 256); ck_assert_ptr_nonnull(arena);
	td_pcm_generation *g = td_arena_alloc(arena, 1, sizeof(*g), _Alignof(td_pcm_generation)); ck_assert_ptr_nonnull(g);
	OK(td_pcm_generation_init(g, arena, epoch, source, track)); c->generation = g;
	ck_assert(td_arena_defer(arena, destroyed, c)); return g;
}
static uint32_t publish(fixture *f, td_pcm_generation *g, uint32_t frames, uint64_t source, uint64_t output, unsigned flags)
{
	uint32_t slot; OK(td_pcm_acquire(&f->pool, g, &slot));
	for (uint32_t i = 0; i < frames; i++) {
		f->pool.blocks[slot].samples[i*2] = (float)(output + i + 1);
		f->pool.blocks[slot].samples[i*2+1] = -(float)(output + i + 1);
	}
	td_pcm_meta meta = {source, frames, output, frames, flags};
	OK(td_pcm_publish(&f->pool, slot, &meta)); return slot;
}
static teder_code peek(td_pcm_pool *p, td_pcm_view *v)
{
	callback = true; teder_code code = td_pcm_peek(p, v); callback = false; return code;
}
static teder_code consume(td_pcm_pool *p, uint32_t frames)
{
	callback = true; teder_code code = td_pcm_consume(p, frames); callback = false; return code;
}
START_TEST(bounds)
{
	size_t bytes = SIZE_MAX - 1, before = bytes; td_pcm_layout layout = {0}, saved = layout;
	ck_assert(!td_pcm_plan(&bytes, 1, &layout) && bytes == before && !memcmp(&layout, &saved, sizeof(layout)));
	bytes = 0; ck_assert(!td_pcm_plan(&bytes, 0, &layout) && !bytes);
	fixture f = {0}; init(&f, 2051); cleanup c = {0}; td_pcm_generation *g = generation(&f, 1, 1, 0, &c);
	ck_assert(f.pool.count == 3 && f.pool.blocks[0].capacity == 1024 && f.pool.blocks[2].capacity == 3);
	ck_assert(f.pool.blocks[1].samples == f.pool.blocks[0].samples + 2048);
	uint32_t slots[3], rejected;
	for (unsigned i = 0; i < 3; i++) { OK(td_pcm_acquire(&f.pool, g, &slots[i])); ck_assert(slots[i] == i); }
	ck_assert(td_pcm_acquire(&f.pool, g, &rejected) == TEDER_AGAIN && rejected == TD_PCM_NONE);
	td_pcm_meta meta = {0,4,0,4,0}; ck_assert(td_pcm_publish(&f.pool, 2, &meta) == TEDER_INVALID);
	meta.frames = 1; meta.output_start = UINT64_MAX; ck_assert(td_pcm_publish(&f.pool, 2, &meta) == TEDER_LIMIT);
	meta.output_start = 0; meta.source_start = UINT64_MAX; ck_assert(td_pcm_publish(&f.pool, 2, &meta) == TEDER_LIMIT);
	meta.source_start = 0; meta.flags = 8; ck_assert(td_pcm_publish(&f.pool, 2, &meta) == TEDER_INVALID);
	for (unsigned i = 0; i < 3; i++) OK(td_pcm_abort(&f.pool, slots[i]));
	ck_assert(td_pcm_abort(&f.pool, slots[0]) == TEDER_BAD_STATE && g->references == 0);
	OK(td_pcm_retire(&g)); ck_assert(c.calls == 1 && !c.on_callback); uninit(&f);
	uint32_t cells[4], item = 99; td_pcm_queue q = {.cells = cells, .capacity = 3};
	atomic_init(&q.read, 0); atomic_init(&q.write, 0);
	for (unsigned cycle = 0; cycle < 30; cycle++) {
		ck_assert(!td_pcm_pop(&q, &item));
		for (unsigned i = 0; i < 3; i++) ck_assert(td_pcm_push(&q, cycle*3+i));
		ck_assert(!td_pcm_push(&q, 999));
		for (unsigned i = 0; i < 3; i++) ck_assert(td_pcm_pop(&q, &item) && item == cycle*3+i);
	}
	teder_config config = {.struct_size = sizeof(config), .abi_version = TEDER_ABI_VERSION, .pcm_frames = UINT32_MAX};
	th_tracker t; ck_assert(th_tracker_init(&t)); teder_engine *e = NULL;
	ck_assert(td_engine_create(&config, th_allocator(&t), &e) == TEDER_LIMIT && !e && !t.attempts);
	config.pcm_frames = 2051; OK(td_engine_create(&config, th_allocator(&t), &e));
	ck_assert(t.attempts == 1 && e->pcm.count == 3 && e->pcm.blocks[2].capacity == 3);
	OK(th_close(e)); ck_assert(e->pcm.closed && e->pcm.free_count == 3); OK(teder_destroy(&e));
	ck_assert(th_tracker_clean(&t)); th_tracker_uninit(&t);
}
END_TEST
typedef struct { td_pcm_pool *pool; uint64_t source[32], track[32], position[32], output[32]; uint32_t flags[32]; size_t count; bool valid; } capture;
static size_t render(void *p, float *out, size_t frames)
{
	capture *c = p; size_t got = 0;
	while (got < frames) {
		td_pcm_view v; teder_code code = peek(c->pool, &v);
		if (code == TEDER_AGAIN) break;
		if (code != TEDER_OK) { c->valid = false; break; }
		uint32_t n = v.frames < frames-got ? v.frames : (uint32_t)(frames-got);
		memcpy(out + got*2, v.samples, (size_t)n*2*sizeof(float));
		for (uint32_t i = 0; i < n; i++) {
			size_t index = c->count++;
			if (index >= 32) { c->valid = false; return got; }
			c->source[index] = v.generation->source; c->track[index] = v.generation->track;
			c->position[index] = v.meta->source_start + v.offset + i;
			c->output[index] = v.meta->output_start + v.offset + i;
			c->flags[index] = v.offset + i == 0 ? v.meta->flags : 0;
		}
		if (consume(c->pool, n) != TEDER_OK) c->valid = false;
		got += n;
	}
	return got;
}
START_TEST(stream)
{
	fixture f = {0}; init(&f, 3072); cleanup a = {0}, b = {0};
	td_pcm_generation *first = generation(&f, 1, 11, 99, &a), *next = generation(&f, 1, 22, 99, &b);
	OK(td_pcm_select(&f.pool, 1)); size_t attempts = f.tracker.attempts, bytes = f.tracker.bytes;
	for (unsigned cycle = 0; cycle < 64; cycle++) {
		uint64_t output = cycle*15;
		publish(&f, first, 5, 100, output, TD_PCM_DISCONTINUITY);
		publish(&f, next, 3, 200, output+5, TD_PCM_BOUNDARY);
		publish(&f, next, 7, 203, output+8, TD_PCM_FINAL);
		float samples[36]; capture c = {.pool = &f.pool, .valid = true}; th_sink sink;
		ck_assert(th_sink_init(&sink, samples, 18, 2, render, &c));
		const size_t sizes[] = {2,1,5,10};
		for (unsigned i = 0; i < 4; i++) ck_assert(th_sink_step(&sink, sizes[i], false) == TH_OK);
		ck_assert(c.valid && c.count == 15 && sink.frames == 18 && sink.underruns == 3);
		for (unsigned i = 0; i < 18; i++) {
			float value = i < 15 ? (float)(output+i+1) : 0;
			ck_assert(samples[i*2] == value && samples[i*2+1] == -value);
			if (i < 15) ck_assert(c.source[i] == (i < 5 ? 11 : 22) && c.track[i] == 99 &&
				c.output[i] == output+i && c.position[i] == (i < 5 ? 100+i : 200+i-5));
		}
		ck_assert(c.flags[0] == TD_PCM_DISCONTINUITY && c.flags[5] == TD_PCM_BOUNDARY && c.flags[8] == TD_PCM_FINAL);
		ck_assert(th_sink_uninit(&sink));
		ck_assert(first->references == 1 && next->references == 2 && !a.calls && !b.calls);
		uint32_t unavailable; ck_assert(td_pcm_acquire(&f.pool, first, &unavailable) == TEDER_AGAIN && unavailable == TD_PCM_NONE);
		ck_assert(td_pcm_reap(&f.pool) == 3 && f.pool.free_count == 3);
		ck_assert(f.tracker.attempts == attempts && f.tracker.bytes == bytes);
	}
	OK(td_pcm_retire(&first)); OK(td_pcm_retire(&next)); uninit(&f);
	printf("64 ring cycles; exact stereo samples/identities; callback heap calls=0\n");
}
END_TEST
START_TEST(switching)
{
	fixture f = {0}; init(&f, 3072); cleanup a = {0}, b = {0}, c = {0};
	td_pcm_generation *old = generation(&f, 1, 7, 10, &a), *next = generation(&f, 2, 7, 10, &b);
	publish(&f, old, 6, 0, 0, 0); publish(&f, old, 4, 6, 6, 0); publish(&f, next, 3, 100, 100, TD_PCM_DISCONTINUITY);
	OK(td_pcm_select(&f.pool, 1)); td_pcm_view view; OK(peek(&f.pool, &view)); OK(consume(&f.pool, 2));
	ck_assert(td_pcm_retire(&old) == TEDER_BUSY && old && !a.calls);
	OK(td_pcm_select(&f.pool, 2)); OK(peek(&f.pool, &view));
	ck_assert(view.generation == next && view.offset == 0 && view.samples[0] == 101 && view.frames == 3);
	ck_assert(view.meta->source_start == 100 && view.meta->output_start == 100 && view.meta->flags == TD_PCM_DISCONTINUITY);
	for (unsigned i = 0; i < 3; i++) ck_assert(view.samples[i*2] == (float)(101+i) && view.samples[i*2+1] == -(float)(101+i));
	ck_assert(td_pcm_select(&f.pool, 1) == TEDER_STALE && td_pcm_select(&f.pool, 0) == TEDER_INVALID);
	ck_assert(consume(&f.pool, 4) == TEDER_INVALID && f.pool.offset == 0);
	ck_assert(td_pcm_reap(&f.pool) == 2 && old->references == 0); OK(td_pcm_retire(&old));
	ck_assert(a.calls == 1 && a.source == 7 && !a.on_callback);
	OK(consume(&f.pool, 3)); ck_assert(td_pcm_reap(&f.pool) == 1);
	td_pcm_generation *future = generation(&f, 3, 8, 20, &c); publish(&f, future, 2, 0, 0, TD_PCM_BOUNDARY);
	ck_assert(peek(&f.pool, &view) == TEDER_AGAIN && future->references == 1);
	uint32_t stale; OK(td_pcm_acquire(&f.pool, next, &stale)); td_pcm_meta meta = {0,1,0,1,0};
	ck_assert(td_pcm_publish(&f.pool, stale, &meta) == TEDER_STALE); OK(td_pcm_abort(&f.pool, stale));
	OK(td_pcm_select(&f.pool, 3)); OK(peek(&f.pool, &view)); ck_assert(view.generation == future && view.samples[0] == 1);
	OK(consume(&f.pool, 1)); callback = true; teder_code code = td_pcm_stop(&f.pool); callback = false;
	ck_assert(code == TEDER_CLOSED && future->references == 1 && !c.calls);
	ck_assert(td_pcm_reap(&f.pool) == 1); OK(td_pcm_retire(&future)); OK(td_pcm_retire(&next)); uninit(&f);
}
END_TEST
typedef struct { td_pcm_pool *pool; th_gate gate; bool valid; } held;
static void hold_view(void *p)
{
	held *h = p; td_pcm_view view;
	h->valid = td_pcm_select(h->pool, 1) == TEDER_OK && peek(h->pool, &view) == TEDER_OK;
	th_gate_hit(&h->gate); /* test rendezvous outside the audited operations */
	if (h->valid) h->valid = view.samples[0] == 1 && view.generation->source == 7 && consume(h->pool, view.frames) == TEDER_OK;
}
START_TEST(retirement)
{
	for (unsigned state = TD_PCM_FILLING; state <= TD_PCM_RETURNED; state++) {
		fixture f = {0}; init(&f, 8); cleanup c = {0}; td_pcm_generation *g = generation(&f, 1, 7, 0, &c);
		uint32_t slot; OK(td_pcm_acquire(&f.pool, g, &slot));
		if (state >= TD_PCM_PUBLISHED) { td_pcm_meta meta = {0,8,0,8,0}; OK(td_pcm_publish(&f.pool, slot, &meta)); }
		if (state >= TD_PCM_READING) { td_pcm_view v; OK(td_pcm_select(&f.pool, 1)); OK(peek(&f.pool, &v)); OK(consume(&f.pool, state == TD_PCM_READING ? 1 : 8)); }
		ck_assert(td_pcm_retire(&g) == TEDER_BUSY && g->references == 1 && !c.calls);
		td_pcm_clear(&f.pool); ck_assert(g->references == 0 && f.pool.free_count == 1);
		ck_assert(td_pcm_acquire(&f.pool, g, &slot) == TEDER_CLOSED && slot == TD_PCM_NONE);
		OK(td_pcm_retire(&g)); ck_assert(c.calls == 1 && !c.on_callback); uninit(&f);
	}
	fixture f = {0}; init(&f, 8); cleanup c = {0}; td_pcm_generation *g = generation(&f, 1, 7, 0, &c);
	publish(&f, g, 8, 0, 0, 0); held h = {.pool = &f.pool}; ck_assert(th_gate_init(&h.gate)); th_thread thread;
	ck_assert(th_thread_start(&thread, hold_view, &h)); th_gate_await(&h.gate);
	ck_assert(td_pcm_retire(&g) == TEDER_BUSY && td_pcm_reap(&f.pool) == 0 && !c.calls);
	uint32_t slot; ck_assert(td_pcm_acquire(&f.pool, g, &slot) == TEDER_STALE && slot == TD_PCM_NONE);
	th_gate_release(&h.gate); th_thread_join(&thread); th_gate_uninit(&h.gate);
	ck_assert(h.valid && g->references == 1 && !c.calls);
	ck_assert(td_pcm_reap(&f.pool) == 1); OK(td_pcm_retire(&g)); ck_assert(c.calls == 1 && !c.on_callback); uninit(&f);
}
END_TEST
typedef struct { td_pcm_pool *pool; uint64_t frames; bool valid; } race;
static void read_stream(void *p)
{
	race *r = p; uint64_t read = 0; unsigned turn = 0; r->valid = td_pcm_select(r->pool, 1) == TEDER_OK;
	while (read < r->frames) {
		td_pcm_view v; teder_code code = peek(r->pool, &v);
		if (code == TEDER_AGAIN) { th_yield(); continue; }
		if (code != TEDER_OK) { r->valid = false; return; }
		uint32_t n = turn++ % 13 + 1; if (n > v.frames) n = v.frames;
		if (v.meta->output_start + v.offset != read || v.meta->source_start + v.offset != read || v.generation->source != 7) r->valid = false;
		for (uint32_t i = 0; i < n; i++) if (v.samples[i*2] != (float)(read+i+1) || v.samples[i*2+1] != -(float)(read+i+1)) r->valid = false;
		if (consume(r->pool, n) != TEDER_OK) { r->valid = false; return; }
		read += n;
	}
}
START_TEST(concurrent)
{
	fixture f = {0}; init(&f, 3072); cleanup c = {0}; td_pcm_generation *g = generation(&f, 1, 7, 0, &c);
	race r = {.pool = &f.pool};
	for (unsigned i = 0; i < 10000; i++) r.frames += i % 17 + 1;
	th_thread reader; ck_assert(th_thread_start(&reader, read_stream, &r));
	uint64_t output = 0;
	for (unsigned i = 0; i < 10000; i++) {
		uint32_t slot; teder_code code;
		while ((code = td_pcm_acquire(&f.pool, g, &slot)) == TEDER_AGAIN) { td_pcm_reap(&f.pool); th_yield(); }
		OK(code); uint32_t frames = i % 17 + 1;
		for (uint32_t j = 0; j < frames; j++) { f.pool.blocks[slot].samples[j*2] = (float)(output+j+1); f.pool.blocks[slot].samples[j*2+1] = -(float)(output+j+1); }
		td_pcm_meta meta = {output, frames, output, frames, 0}; OK(td_pcm_publish(&f.pool, slot, &meta)); output += frames;
	}
	ck_assert(td_pcm_retire(&g) == TEDER_BUSY); th_thread_join(&reader); ck_assert(r.valid && output == r.frames);
	uint32_t remaining = g->references; ck_assert(td_pcm_reap(&f.pool) == remaining && g->references == 0);
	ck_assert(f.tracker.attempts == 2 && !c.calls && f.pool.free_count == 3);
	OK(td_pcm_retire(&g)); uninit(&f);
	printf("10000 blocks, exact samples and frame ranges; callback heap calls=0\n");
}
END_TEST
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "bounds")) test = bounds;
	else if (!strcmp(argv[1], "stream")) test = stream;
	else if (!strcmp(argv[1], "switching")) test = switching;
	else if (!strcmp(argv[1], "retirement")) test = retirement;
	else if (!strcmp(argv[1], "concurrent")) test = concurrent;
	else return 2;
	Suite *suite = suite_create("pcm"); TCase *tc = tcase_create(argv[1]); tcase_add_test(tc, test); suite_add_tcase(suite, tc);
	SRunner *runner = srunner_create(suite); srunner_set_fork_status(runner, CK_NOFORK); srunner_run_all(runner, CK_NORMAL);
	int failed = srunner_ntests_failed(runner); srunner_free(runner); return failed ? 1 : 0;
}
