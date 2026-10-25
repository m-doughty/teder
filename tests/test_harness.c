#include "support/harness.h"
#include "fixtures/reference.h"
#include <check.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { th_tracker *tracker; void *external; int *order; int id, cookie; } resource;
static void destroy_resource(void *p)
{
	resource *r = p;
	if (r->cookie != 1234) abort();
	*r->order = *r->order * 10 + r->id;
	th_free(r->tracker, r->external);
}
START_TEST(memory)
{
	th_tracker t;
	size_t value = 77;
	ck_assert(th_tracker_init(&t));
	ck_assert(!td_size_add(SIZE_MAX, 1, &value)); ck_assert((value) == (77));
	ck_assert(!td_size_mul(SIZE_MAX, 2, &value)); ck_assert((value) == (77));
	ck_assert(td_size_mul(0, SIZE_MAX, &value)); ck_assert((value) == (0));
	ck_assert_ptr_null(td_arena_create(th_allocator(&t), SIZE_MAX));
	ck_assert((t.attempts) == (0));
	for (size_t n = 1; n <= 3; n++) {
		int order = 0;
		t.attempts = 0; t.fail_at = n; t.failed_site = NULL;
		td_arena *arena = td_arena_create(th_allocator(&t), 256);
		if (arena) {
			resource *r = td_arena_alloc(arena, 1, sizeof(*r), _Alignof(resource));
			ck_assert_ptr_nonnull(r);
			*r = (resource){&t, th_alloc(&t, 8, "resource.open"), &order, 1, 1234};
			if (r->external) ck_assert(td_arena_defer(arena, destroy_resource, r));
			td_arena_destroy(arena);
		}
		ck_assert((order) == (n == 3 ? 1 : 0));
		if (n < 3) ck_assert_str_eq(t.failed_site, n == 1 ? "arena.chunk" : "resource.open");
		else ck_assert_ptr_null(t.failed_site);
		ck_assert((t.live) == (0)); ck_assert((t.bytes) == (0));
		printf("fail-at=%zu site=%s expected=%s live=%zu bytes=%zu\n", n,
			t.failed_site ? t.failed_site : "none", n < 3 ? "failure" : "success", t.live, t.bytes);
	}
	t.fail_at = 0;
	td_arena *a = td_arena_create(th_allocator(&t), 512);
	ck_assert_ptr_nonnull(a);
	size_t baseline = t.bytes;
	int order = 0;
	for (int i = 1; i <= 2; i++) {
		resource *r = td_arena_alloc(a, 1, sizeof(*r), _Alignof(resource));
		ck_assert_ptr_nonnull(r);
		*r = (resource){&t, th_alloc(&t, 8, "resource.open"), &order, i, 1234};
		ck_assert_ptr_nonnull(r->external); ck_assert(td_arena_defer(a, destroy_resource, r));
	}
	ck_assert((t.peak) == (baseline + 16));
	td_arena_reset(a); ck_assert((order) == (21)); ck_assert((t.bytes) == (baseline));
	for (size_t align = 1; align <= _Alignof(max_align_t); align *= 2) {
		void *p = td_arena_alloc(a, 1, 1, align);
		ck_assert_ptr_nonnull(p); ck_assert(((uintptr_t)p % align) == (0));
	}
	ck_assert_ptr_null(td_arena_alloc(a, SIZE_MAX, 2, 1));
	ck_assert_ptr_null(td_arena_alloc(a, 1, 1, 3));
	ck_assert_ptr_null(td_arena_alloc(a, 1, 1, 0));
	ck_assert_ptr_null(td_arena_alloc(a, 1, 1, 2 * _Alignof(max_align_t)));
	td_arena_destroy(a); ck_assert((order) == (21));
	a = td_arena_create(th_allocator(&t), 1);
	void *first = td_arena_alloc(a, 1, 1, 1);
	ck_assert_ptr_nonnull(first); ck_assert(!td_arena_defer(a, destroy_resource, NULL));
	ck_assert_ptr_null(td_arena_alloc(a, 1, 1, 1));
	td_arena_reset(a); ck_assert_ptr_eq(td_arena_alloc(a, 1, 1, 1), first);
	td_arena_destroy(a); ck_assert(th_tracker_clean(&t)); th_tracker_uninit(&t);
}
END_TEST
START_TEST(hooks)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	ma_allocation_callbacks cb = th_ma_allocator(&t);
	unsigned char *p = ma_malloc(8, &cb);
	ck_assert_ptr_nonnull(p); memset(p, 0x5a, 8);
	t.fail_at = 2;
	ck_assert_ptr_null(ma_realloc(p, 16, &cb));
	ck_assert_str_eq(t.failed_site, "miniaudio.realloc"); ck_assert((t.bytes) == (8));
	for (size_t i = 0; i < 8; i++) ck_assert((p[i]) == (0x5a));
	t.fail_at = 0;
	p = ma_realloc(p, 16, &cb); ck_assert_ptr_nonnull(p);
	for (size_t i = 0; i < 8; i++) ck_assert((p[i]) == (0x5a));
	ma_free(p, &cb); ck_assert(th_tracker_clean(&t));
	ma_data_converter converter;
	ma_data_converter_config config = ma_data_converter_config_init(ma_format_f32, ma_format_f32, 2, 2, 48000, 44100);
	t.attempts = 0;
	ck_assert((ma_data_converter_init(&config, &cb, &converter)) == (MA_SUCCESS));
	size_t count = t.attempts;
	ck_assert(count == 1);
	ma_data_converter_uninit(&converter, &cb); ck_assert(th_tracker_clean(&t));
	for (size_t n = 1; n <= count; n++) {
		t.attempts = 0; t.fail_at = n; t.failed_site = NULL;
		ck_assert((ma_data_converter_init(&config, &cb, &converter)) == (MA_OUT_OF_MEMORY));
		ck_assert_str_eq(t.failed_site, "miniaudio.malloc"); ck_assert(th_tracker_clean(&t));
		printf("converter fail-at=%zu site=%s expected=MA_OUT_OF_MEMORY live=%zu bytes=%zu\n", n, t.failed_site, t.live, t.bytes);
	}
	th_tracker_uninit(&t);
}
END_TEST
typedef struct { size_t cursor; th_gate *gate; } source;
static size_t render(void *p, float *out, size_t frames)
{
	source *s = p;
	if (s->gate) th_gate_hit(s->gate);
	size_t count = frames < 32 - s->cursor ? frames : 32 - s->cursor;
	for (size_t i = 0; i < count; i++, s->cursor++) for (size_t c = 0; c < 2; c++)
		out[i * 2 + c] = (float)(s->cursor < 16 ? track_a[s->cursor * 2 + c] : track_b[(s->cursor - 16) * 2 + c]) / 32768.0f;
	return count;
}
START_TEST(sink)
{
	float pcm[80], expected[80] = {0};
	source src = {0}; th_sink s; uint64_t now = 0;
	ck_assert(!th_sink_init(&s, pcm, SIZE_MAX, 2, render, &src));
	ck_assert(!th_sink_init(&s, pcm, 40, 0, render, &src));
	ck_assert(th_sink_init(&s, pcm, 40, 2, render, &src));
	const size_t sizes[] = {1, 7, 13, 11};
	for (size_t i = 0; i < 4; i++) ck_assert((th_sink_step(&s, sizes[i], false)) == (TH_OK));
	for (size_t i = 0; i < 32; i++) { expected[i] = (float)track_a[i] / 32768.0f; expected[32+i] = (float)track_b[i] / 32768.0f; }
	ck_assert((s.frames) == (32)); ck_assert((s.underruns) == (0));
	ck_assert(th_pcm_equal(pcm, expected, 32, 2));
	ck_assert((th_sink_step(&s, 2, true)) == (TH_OK));
	ck_assert((th_sink_step(&s, 3, false)) == (TH_OK));
	ck_assert((s.frames) == (37)); ck_assert((s.underruns) == (5));
	ck_assert(th_pcm_equal(pcm, expected, 37, 2)); ck_assert((src.cursor) == (32));
	ck_assert((th_sink_step(&s, 4, false)) == (TH_CAPACITY));
	ck_assert((th_sink_step(&s, 0, false)) == (TH_INVALID));
	ck_assert(th_clock_advance(&now, 100)); ck_assert((now) == (100));
	ck_assert(!th_clock_advance(&now, UINT64_MAX)); ck_assert((now) == (100));
	th_sink_device_error(&s); ck_assert((th_sink_step(&s, 1, false)) == (TH_DEVICE));
	ck_assert((s.frames) == (37)); ck_assert(th_sink_uninit(&s));
}
END_TEST
typedef struct { th_sink *sink; th_status result; } step_job;
static void step(void *p) { step_job *job = p; job->result = th_sink_step(job->sink, 1, false); }
static void hit(void *p) { th_gate_hit(p); }
START_TEST(gates)
{
	th_gate g; th_thread thread; th_sink s; float pcm[2]; source src = {0, &g};
	ck_assert(th_gate_init(&g)); ck_assert(th_sink_init(&s, pcm, 1, 2, render, &src));
	step_job job = {&s, TH_INVALID};
	if (!th_thread_start(&thread, step, &job)) { th_sink_uninit(&s); th_gate_uninit(&g); ck_abort_msg("thread start failed"); }
	th_gate_await(&g);
	th_status busy = th_sink_step(&s, 1, false);
	bool premature_stop = th_sink_stop(&s), premature_free = th_sink_uninit(&s);
	th_gate_release(&g); th_thread_join(&thread);
	ck_assert(!premature_stop); ck_assert(!premature_free); ck_assert((busy) == (TH_BUSY));
	ck_assert((job.result) == (TH_OK)); ck_assert((th_sink_step(&s, 1, false)) == (TH_STOPPED));
	ck_assert((s.frames) == (1)); ck_assert(th_sink_uninit(&s)); th_gate_uninit(&g);
}
END_TEST
static int control(const char *name)
{
	if (!strcmp(name, "missing-worker")) {
		th_gate gate; th_thread thread;
		if (!th_gate_init(&gate)) return 2;
		if (!th_thread_start(&thread, hit, &gate)) { th_gate_uninit(&gate); return 2; }
		th_gate_await(&gate); puts("worker pending at checkpoint"); fflush(stdout);
		th_thread_join(&thread); /* Deliberately never released. Supervisor kills process. */
		return 0;
	}
	if (!strcmp(name, "pcm")) {
		float actual[] = {0, 0, 0, 1}, expected[] = {0, 0, 0, 0};
		return th_pcm_equal(actual, expected, 2, 2) ? 0 : 42;
	}
	th_tracker t;
	if (!th_tracker_init(&t)) return 2;
	void *p = th_alloc(&t, 8, "control.leak");
	if (!p) { th_tracker_uninit(&t); return 2; }
	if (!strcmp(name, "wrong-free")) { int foreign = 0; th_free(&t, &foreign); th_free(&t, p); p = NULL; }
	bool detected = !th_tracker_clean(&t);
	th_free(&t, p); th_tracker_uninit(&t);
	return detected ? 42 : 0;
}
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	if (!strncmp(argv[1], "control-", 8)) return control(argv[1] + 8);
	if (strcmp(argv[1], "memory") && strcmp(argv[1], "hooks") && strcmp(argv[1], "sink") && strcmp(argv[1], "gates")) return 2;
	Suite *suite = suite_create("harness"); TCase *test = tcase_create(argv[1]);
	if (!strcmp(argv[1], "memory")) tcase_add_test(test, memory);
	else if (!strcmp(argv[1], "hooks")) tcase_add_test(test, hooks);
	else if (!strcmp(argv[1], "sink")) tcase_add_test(test, sink);
	else if (!strcmp(argv[1], "gates")) tcase_add_test(test, gates);
	else return 2;
	suite_add_tcase(suite, test);
	SRunner *runner = srunner_create(suite); srunner_set_fork_status(runner, CK_NOFORK);
	srunner_run_all(runner, CK_NORMAL); int failed = srunner_ntests_failed(runner);
	srunner_free(runner); return failed ? 1 : 0;
}
