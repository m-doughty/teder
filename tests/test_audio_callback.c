#include "audio_callback.h"
#include "support/harness.h"
#include <check.h>
#include <math.h>
#ifndef _WIN32
#include <sched.h>
#endif

#define OK(call) ck_assert((call) == TEDER_OK)
static _Thread_local bool in_callback;

static void th_yield(void)
{
#ifdef _WIN32
	SwitchToThread();
#else
	sched_yield();
#endif
}

typedef struct {
	th_tracker tracker;
	atomic_uint violations;
	td_arena *arena;
	td_pcm_pool pool;
} fixture;

static void *allocate(void *p, size_t bytes, const char *site)
{
	fixture *f = p;
	if (in_callback)
		atomic_fetch_add(&f->violations, 1);
	return th_alloc(&f->tracker, bytes, site);
}

static void release(void *p, void *data)
{
	fixture *f = p;
	if (in_callback)
		atomic_fetch_add(&f->violations, 1);
	th_free(&f->tracker, data);
}

static td_allocator allocator(fixture *f)
{
	return (td_allocator){f, allocate, release};
}

static void init(fixture *f)
{
	ck_assert(th_tracker_init(&f->tracker));
	atomic_init(&f->violations, 0);
	size_t bytes = 0;
	td_pcm_layout layout;
	ck_assert(td_pcm_plan(&bytes, 3072, &layout));
	f->arena = td_arena_create(allocator(f), bytes);
	ck_assert_ptr_nonnull(f->arena);
	void *base = td_arena_alloc(f->arena, bytes, 1, _Alignof(max_align_t));
	ck_assert_ptr_nonnull(base);
	td_pcm_init(&f->pool, base, &layout, 3072);
}

static void uninit(fixture *f)
{
	td_pcm_clear(&f->pool);
	td_arena_destroy(f->arena);
	ck_assert(atomic_load(&f->violations) == 0 && th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
}

static td_pcm_generation *generation(fixture *f, uint64_t epoch)
{
	td_arena *arena = td_arena_create(allocator(f), 256);
	ck_assert_ptr_nonnull(arena);
	td_pcm_generation *g = td_arena_alloc(arena, 1, sizeof(*g), _Alignof(td_pcm_generation));
	ck_assert_ptr_nonnull(g);
	OK(td_pcm_generation_init(g, arena, epoch, epoch, 0));
	return g;
}

static void fill(fixture *f, uint32_t slot, uint32_t frames, uint64_t start, uint32_t flags)
{
	ck_assert(frames <= f->pool.blocks[slot].capacity);
	for (uint32_t i = 0; i < frames; i++) {
		f->pool.blocks[slot].samples[i * 2] = (float)(start + i + 1);
		f->pool.blocks[slot].samples[i * 2 + 1] = -(float)(start + i + 1);
	}
	td_pcm_meta meta = {start, frames, start, frames, flags};
	OK(td_pcm_publish(&f->pool, slot, &meta));
}

static void
publish(fixture *f, td_pcm_generation *g, uint32_t frames, uint64_t start, uint32_t flags)
{
	uint32_t slot;
	OK(td_pcm_acquire(&f->pool, g, &slot));
	fill(f, slot, frames, start, flags);
}

static teder_code render(td_pcm_pool *pool,
                         td_audio_config config,
                         float *out,
                         size_t capacity,
                         uint32_t frames,
                         td_audio_result *result)
{
	in_callback = true;
	teder_code code = td_audio_render(pool, config, out, capacity, frames, result);
	in_callback = false;
	return code;
}

/* Sentinel beyond the requested span must survive, even with excess capacity. */
static void output(
    fixture *f, td_audio_config config, uint32_t frames, uint32_t pcm, uint64_t start, float gain)
{
	float out[66];
	for (size_t i = 0; i < 66; i++)
		out[i] = 12345;
	td_audio_result result = {.pcm_frames = 99, .missing_frames = 99};
	OK(render(&f->pool, config, out, 66, frames, &result));
	ck_assert(result.pcm_frames == pcm);
	ck_assert(result.missing_frames == (config.enabled ? frames - pcm : 0));
	for (uint32_t i = 0; i < frames; i++) {
		float expected = i < pcm ? (float)(start + i + 1) * gain : 0;
		ck_assert(out[i * 2] == expected && out[i * 2 + 1] == -expected);
	}
	for (size_t i = (size_t)frames * 2; i < 66; i++)
		ck_assert(out[i] == 12345);
}

START_TEST(samples)
{
	fixture f = {0};
	init(&f);
	td_pcm_generation *g = generation(&f, 1);
	td_audio_config config = {1, 1, 1, 0};
	output(&f, config, 5, 0, 0, 1);
	publish(&f, g, 7, 0, TD_PCM_DISCONTINUITY);
	publish(&f, g, 3, 7, TD_PCM_FINAL);
	publish(&f, g, 5, 10, TD_PCM_BOUNDARY | TD_PCM_FINAL);
	ck_assert(td_pcm_retire(&g) == TEDER_BUSY && g->references == 3);

	output(&f, config, 2, 2, 0, 1);
	ck_assert(td_pcm_reap(&f.pool) == 0 && g->references == 3);
	output(&f, config, 10, 10, 2, 1);
	ck_assert(td_pcm_reap(&f.pool) == 2 && g->references == 1);
	output(&f, config, 7, 3, 12, 1);
	ck_assert(g->references == 1 && f.tracker.live == 2);
	ck_assert(td_pcm_reap(&f.pool) == 1 && g->references == 0);
	OK(td_pcm_retire(&g));
	ck_assert(!g && f.tracker.live == 1 && f.tracker.attempts == 2);
	output(&f, config, 1, 0, 0, 1);
	uninit(&f);
}
END_TEST

START_TEST(controls)
{
	fixture f = {0};
	init(&f);
	td_pcm_generation *g = generation(&f, 1);
	publish(&f, g, 10, 0, 0);
	td_audio_config config = {1, 0.5f, 1, 0};
	output(&f, config, 2, 2, 0, 0.5f);
	config.enabled = 0;
	output(&f, config, 4, 0, 0, 0);
	ck_assert(f.pool.offset == 2);
	config.enabled = 1;
	output(&f, config, 1, 1, 2, 0.5f);

	config.mute = 1;
	output(&f, config, 3, 3, 3, 0);
	ck_assert(f.pool.offset == 6);
	config.mute = 0;
	config.gain = 0;
	output(&f, config, 2, 2, 6, 0);
	config.gain = 1;
	output(&f, config, 3, 2, 8, 1);
	ck_assert(td_pcm_reap(&f.pool) == 1);
	OK(td_pcm_retire(&g));
	uninit(&f);
}
END_TEST

START_TEST(generations)
{
	fixture f = {0};
	init(&f);
	td_pcm_generation *old = generation(&f, 1);
	td_pcm_generation *next = generation(&f, 2);
	td_pcm_generation *future = generation(&f, 3);
	publish(&f, old, 8, 0, 0);
	publish(&f, next, 4, 100, 0);
	td_audio_config config = {1, 1, 1, 0};
	output(&f, config, 2, 2, 0, 1);
	ck_assert(td_pcm_retire(&old) == TEDER_BUSY);
	config.generation = 2;
	config.enabled = 0;
	output(&f, config, 3, 0, 0, 0);
	ck_assert(old->references == 1 && next->references == 1);
	ck_assert(td_pcm_reap(&f.pool) == 1 && old->references == 0);
	OK(td_pcm_retire(&old));
	ck_assert(f.pool.offset == 0);

	float buffer[4] = {99, 99, 99, 99};
	td_audio_result result;
	ck_assert(render(&f.pool, (td_audio_config){1, 1, 1, 0}, buffer, 4, 2, &result) == TEDER_STALE);
	ck_assert(result.pcm_frames == 0 && result.missing_frames == 0 && f.pool.selected == 2);
	for (unsigned i = 0; i < 4; i++)
		ck_assert(buffer[i] == 0);
	publish(&f, future, 2, 200, TD_PCM_FINAL);
	config.enabled = 1;
	output(&f, config, 6, 4, 100, 1);
	output(&f, config, 2, 0, 0, 1);
	ck_assert(td_pcm_reap(&f.pool) == 1);
	OK(td_pcm_retire(&next));
	config.generation = 3;
	output(&f, config, 2, 2, 200, 1);
	ck_assert(td_pcm_reap(&f.pool) == 1);
	OK(td_pcm_retire(&future));
	uninit(&f);
}
END_TEST

START_TEST(positions)
{
	fixture f = {0};
	init(&f);
	td_pcm_generation *g = generation(&f, 1);
	const uint64_t spans[] = {5, 0, UINT64_MAX - 10};
	const uint64_t expected[][3] = {
	    {11, 13, 15},
	    {10, 10, 10},
	    {UINT64_C(4611686018427387911), UINT64_C(13835058055282163713), UINT64_MAX}};
	for (unsigned n = 0; n < 3; n++) {
		uint32_t slot;
		OK(td_pcm_acquire(&f.pool, g, &slot));
		memset(f.pool.blocks[slot].samples, 0, 8 * sizeof(float));
		td_pcm_meta m = {10, spans[n], n * 4, 4, TD_PCM_FINAL};
		OK(td_pcm_publish(&f.pool, slot, &m));
		const uint32_t chunks[] = {1, 2, 1};
		for (unsigned i = 0; i < 3; i++) {
			float out[4];
			td_audio_result r;
			OK(render(&f.pool, (td_audio_config){1, 1, 1, 1}, out, 4, chunks[i], &r));
			ck_assert(r.source == 1 && r.track == 0 && r.source_frame == expected[n][i] &&
			          r.pcm_frames == chunks[i] && r.final == (i == 2));
		}
		ck_assert(td_pcm_reap(&f.pool) == 1);
	}
	OK(td_pcm_retire(&g));
	uninit(&f);
}
END_TEST

START_TEST(bounds)
{
	fixture f = {0};
	init(&f);
	td_pcm_generation *g = generation(&f, 1);
	publish(&f, g, 2, 0, 0);
	td_audio_config config = {1, 1, 1, 0};
	float out[5] = {99, 99, 99, 99, 99};
	td_audio_result result = {.pcm_frames = 99, .missing_frames = 99};
	ck_assert(render(&f.pool, config, out, 3, 2, &result) == TEDER_INVALID);
	ck_assert(result.pcm_frames == 0 && result.missing_frames == 0);
	ck_assert(render(&f.pool, config, out, 5, UINT32_MAX, &result) == TEDER_INVALID);
	ck_assert(render(&f.pool, config, NULL, 4, 2, &result) == TEDER_INVALID);
	ck_assert(render(&f.pool, config, out, 5, 2, NULL) == TEDER_INVALID);
	for (unsigned i = 0; i < 5; i++)
		ck_assert(out[i] == 99);
	OK(render(&f.pool, config, NULL, 0, 0, &result));
	ck_assert(f.pool.reading == TD_PCM_NONE && result.pcm_frames == 0);

	const td_audio_config invalid[] = {{0, 1, 1, 0},
	                                   {1, -0.1f, 1, 0},
	                                   {1, 1.1f, 1, 0},
	                                   {1, NAN, 1, 0},
	                                   {1, INFINITY, 1, 0},
	                                   {1, -INFINITY, 1, 0},
	                                   {1, 1, 2, 0},
	                                   {1, 1, 1, 2}};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
		for (unsigned j = 0; j < 4; j++)
			out[j] = 99;
		ck_assert(render(&f.pool, invalid[i], out, 5, 2, &result) == TEDER_INVALID);
		ck_assert(result.pcm_frames == 0 && result.missing_frames == 0);
		for (unsigned j = 0; j < 4; j++)
			ck_assert(out[j] == 0);
		ck_assert(out[4] == 99 && f.pool.reading == TD_PCM_NONE);
	}
	ck_assert(render(NULL, config, out, 5, 2, &result) == TEDER_INVALID);
	output(&f, (td_audio_config){0, 1, 0, 0}, 2, 0, 0, 0);
	ck_assert(f.pool.reading == TD_PCM_NONE);
	output(&f, config, 2, 2, 0, 1);
	ck_assert(td_pcm_reap(&f.pool) == 1);
	OK(td_pcm_retire(&g));
	ck_assert(td_pcm_stop(&f.pool) == TEDER_CLOSED);
	ck_assert(render(&f.pool, config, out, 5, 2, &result) == TEDER_CLOSED);
	for (unsigned i = 0; i < 4; i++)
		ck_assert(out[i] == 0);
	uninit(&f);
}
END_TEST

typedef struct {
	fixture *f;
	uint64_t frames;
	bool valid;
} stream;

static void read_stream(void *p)
{
	stream *s = p;
	uint64_t read = 0;
	uint32_t turn = 0;
	s->valid = true;
	while (read < s->frames) {
		float out[66];
		for (unsigned i = 0; i < 66; i++)
			out[i] = 12345;
		uint32_t frames = turn++ % 31 + 1;
		td_audio_result result;
		if (render(&s->f->pool, (td_audio_config){1, 0.5f, 1, 0}, out, 66, frames, &result) !=
		        TEDER_OK ||
		    result.pcm_frames > frames || result.missing_frames != frames - result.pcm_frames) {
			s->valid = false;
			return;
		}
		for (uint32_t i = 0; i < frames; i++) {
			float expected = i < result.pcm_frames ? (float)(read + i + 1) * 0.5f : 0;
			if (out[i * 2] != expected || out[i * 2 + 1] != -expected)
				s->valid = false;
		}
		for (size_t i = (size_t)frames * 2; i < 66; i++)
			if (out[i] != 12345)
				s->valid = false;
		read += result.pcm_frames;
		th_yield(); /* scheduling belongs to the test driver, outside the callback */
	}
	if (read != s->frames)
		s->valid = false;
}

START_TEST(concurrent)
{
	fixture f = {0};
	init(&f);
	td_pcm_generation *g = generation(&f, 1);
	stream s = {.f = &f};
	for (uint32_t i = 0; i < 4000; i++)
		s.frames += i % 23 + 1;
	th_thread reader;
	ck_assert(th_thread_start(&reader, read_stream, &s));
	uint64_t written = 0;
	for (uint32_t i = 0; i < 4000; i++) {
		uint32_t slot;
		teder_code code;
		do {
			td_pcm_reap(&f.pool);
			code = td_pcm_acquire(&f.pool, g, &slot);
			if (code == TEDER_AGAIN)
				th_yield();
		} while (code == TEDER_AGAIN);
		OK(code);
		uint32_t frames = i % 23 + 1;
		fill(&f, slot, frames, written, i == 3999 ? TD_PCM_FINAL : 0);
		written += frames;
	}
	ck_assert(td_pcm_retire(&g) == TEDER_BUSY);
	th_thread_join(&reader);
	ck_assert(s.valid && written == s.frames);
	uint32_t refs = g->references;
	ck_assert(td_pcm_reap(&f.pool) == refs && g->references == 0);
	ck_assert(f.tracker.attempts == 2 && f.tracker.live == 2);
	OK(td_pcm_retire(&g));
	uninit(&f);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "samples"))
		test = samples;
	else if (!strcmp(argv[1], "controls"))
		test = controls;
	else if (!strcmp(argv[1], "generations"))
		test = generations;
	else if (!strcmp(argv[1], "positions"))
		test = positions;
	else if (!strcmp(argv[1], "bounds"))
		test = bounds;
	else if (!strcmp(argv[1], "concurrent"))
		test = concurrent;
	else
		return 2;
	Suite *suite = suite_create("audio");
	TCase *tc = tcase_create(argv[1]);
	tcase_add_test(tc, test);
	suite_add_tcase(suite, tc);
	SRunner *runner = srunner_create(suite);
	srunner_set_fork_status(runner, CK_NOFORK);
	srunner_run_all(runner, CK_NORMAL);
	int failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? 1 : 0;
}
