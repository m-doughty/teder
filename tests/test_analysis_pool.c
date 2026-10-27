#include "analysis_pool.h"
#include "support/harness.h"
#include <check.h>
#include <math.h>
#include <stdio.h>

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert((call) == TEDER_OK)

static teder_config config(void)
{
	teder_config c = INIT(teder_config);
	c.snapshot_slots = 2;
	c.snapshot_loans = 3;
	c.max_bands = 2;
	c.max_spans = 3;
	return c;
}

static td_snapshot_pool *create(th_tracker *t)
{
	teder_config c = config();
	td_snapshot_pool *p;
	OK(td_snapshot_pool_create(&c, 65536, th_allocator(t), (td_runtime){0}, &p));
	return p;
}

static teder_snapshot_info info(uint64_t value)
{
	teder_snapshot_info i = INIT(teder_snapshot_info);
	i.sequence = 999; /* the pool supplies sequence/drop-publication counters */
	i.epoch = 17;
	i.output_start = value * 7;
	i.frames = 7;
	i.config_revision = 2;
	i.output_rate = 48000;
	i.channels = 2;
	i.bands = 2;
	i.span_count = 3;
	i.dropped_frames = 11;
	i.dropped_publications = 999;
	i.peak[0] = 0.75f;
	i.peak[1] = 0.25f;
	i.rms[0] = 0.5f;
	i.rms[1] = 0.125f;
	return i;
}

static void publish(td_snapshot_pool *p, uint64_t value)
{
	teder_snapshot_info i = info(value);
	float bands[] = {-80, -(float)value};
	teder_span spans[] = {
	    {value * 7, 3, 77, 42}, {value * 7 + 3, 2, 88, 99}, {value * 7 + 5, 2, 0, 0}};
	OK(td_snapshot_publish(p, &i, bands, spans));
	memset(bands, 0, sizeof(bands));
	memset(spans, 0, sizeof(spans)); /* publication must own copies */
}

static teder_snapshot *acquire(td_snapshot_pool *p)
{
	teder_snapshot *s;
	OK(td_snapshot_acquire(p, &s));
	return s;
}

static void verify(teder_snapshot *s, uint64_t sequence, uint64_t value, uint64_t drops)
{
	teder_snapshot_info got = INIT(teder_snapshot_info), expected = info(value);
	expected.sequence = sequence;
	expected.dropped_publications = drops;
	OK(teder_snapshot_get_info(s, &got));
	ck_assert(!memcmp(&got, &expected, sizeof(got)));
	const float *bands;
	const teder_span *spans;
	uint32_t count;
	OK(teder_snapshot_bands(s, &bands, &count));
	ck_assert(count == 2 && bands[0] == -80 && bands[1] == -(float)value);
	OK(teder_snapshot_spans(s, &spans, &count));
	const teder_span expected_spans[] = {
	    {value * 7, 3, 77, 42}, {value * 7 + 3, 2, 88, 99}, {value * 7 + 5, 2, 0, 0}};
	ck_assert(count == 3 && !memcmp(spans, expected_spans, sizeof(expected_spans)));
}

static bool reject_gate(void *context, uint32_t stage)
{
	ck_assert(stage == TD_INIT_SNAPSHOT_GATE);
	(*(unsigned *)context)++;
	return false;
}

START_TEST(bounds)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	teder_config c = config();
	size_t bytes;
	OK(td_snapshot_pool_plan(&c, &bytes));
	td_snapshot_pool *p = NULL;
	ck_assert(td_snapshot_pool_create(&c, bytes - 1, th_allocator(&t), (td_runtime){0}, &p) ==
	              TEDER_LIMIT &&
	          !p && !t.attempts);
	t.fail_at = 1;
	ck_assert(td_snapshot_pool_create(&c, bytes, th_allocator(&t), (td_runtime){0}, &p) ==
	              TEDER_NO_MEMORY &&
	          !p && th_tracker_clean(&t));
	t.fail_at = 0;
	unsigned calls = 0;
	td_runtime r = {.context = &calls, .permit = reject_gate};
	ck_assert(td_snapshot_pool_create(&c, bytes, th_allocator(&t), r, &p) == TEDER_INTERNAL &&
	          calls == 1 && !p && th_tracker_clean(&t));
	OK(td_snapshot_pool_create(&c, bytes, th_allocator(&t), (td_runtime){0}, &p));
	ck_assert(t.attempts == 3 && t.live == 1 && t.bytes == bytes && p->bytes == bytes);
	td_snapshot_pool_release(&p);
	ck_assert(!p && th_tracker_clean(&t));
	td_snapshot_pool_release(&p);
	td_snapshot_pool_release(NULL);
	for (unsigned field = 0; field < 4; field++) {
		c = config();
		if (field == 0)
			c.snapshot_slots = 0;
		else if (field == 1)
			c.snapshot_loans = 0;
		else if (field == 2)
			c.max_bands = 0;
		else
			c.max_spans = 0;
		ck_assert(td_snapshot_pool_plan(&c, &bytes) == TEDER_INVALID && !bytes);
	}
	c = config();
	c.snapshot_slots = c.max_bands = c.max_spans = UINT32_MAX;
	ck_assert(td_snapshot_pool_plan(&c, &bytes) == TEDER_LIMIT && !bytes);
	c = config();
	ck_assert(td_snapshot_pool_plan(NULL, &bytes) == TEDER_INVALID);
	ck_assert(td_snapshot_pool_plan(&c, NULL) == TEDER_INVALID);
	ck_assert(td_snapshot_pool_create(&c, 65536, (td_allocator){0}, r, &p) == TEDER_INVALID);
	c.abi_version++;
	ck_assert(td_snapshot_pool_plan(&c, &bytes) == TEDER_ABI_MISMATCH);
	ck_assert(t.attempts == 3);
	th_tracker_uninit(&t);
}
END_TEST

START_TEST(retention)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	td_snapshot_pool *p = create(&t);
	teder_snapshot *empty = NULL;
	ck_assert(td_snapshot_acquire(p, &empty) == TEDER_AGAIN && !empty);
	publish(p, 10);
	teder_snapshot *a = acquire(p), *duplicate = acquire(p);
	ck_assert(a == duplicate); /* two independent release obligations */
	publish(p, 20);
	teder_snapshot *b = acquire(p);
	ck_assert(a != b && td_snapshot_acquire(p, &empty) == TEDER_LIMIT && !empty);
	teder_snapshot_info i = info(30);
	float bands[] = {-80, -30};
	teder_span spans[] = {{210, 3, 77, 42}, {213, 2, 88, 99}, {215, 2, 0, 0}};
	ck_assert(td_snapshot_publish(p, &i, bands, spans) == TEDER_AGAIN && p->dropped == 1);
	verify(a, 1, 10, 0);
	verify(b, 2, 20, 0);
	teder_snapshot_release(duplicate);
	teder_snapshot_release(a);
	publish(p, 30);
	a = acquire(p);
	verify(a, 3, 30, 1);
	verify(b, 2, 20, 0);
	td_snapshot_clear(p);
	ck_assert(td_snapshot_acquire(p, &empty) == TEDER_AGAIN && !empty);
	verify(a, 3, 30, 1);
	verify(b, 2, 20, 0);
	td_snapshot_pool_release(&p);
	ck_assert(!p && t.live == 1 && t.attempts == 1);
	verify(a, 3, 30, 1);
	teder_snapshot_release(a);
	verify(b, 2, 20, 0);
	teder_snapshot_release(b);
	teder_snapshot_release(NULL);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

START_TEST(validation)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	td_snapshot_pool *p = create(&t);
	publish(p, 1);
	for (unsigned mode = 0; mode < 19; mode++) {
		teder_snapshot_info i = info(2);
		float bands[] = {-80, -2};
		teder_span spans[] = {{14, 3, 77, 42}, {17, 2, 88, 99}, {19, 2, 0, 0}};
		teder_code expected = TEDER_INVALID;
		switch (mode) {
		case 0:
			i.abi_version++;
			expected = TEDER_ABI_MISMATCH;
			break;
		case 1:
			i.bands = 3;
			expected = TEDER_LIMIT;
			break;
		case 2:
			i.span_count = 4;
			expected = TEDER_LIMIT;
			break;
		case 3:
			i.epoch = 0;
			break;
		case 4:
			i.config_revision = 0;
			break;
		case 5:
			i.output_rate = 0;
			break;
		case 6:
			i.channels = 1;
			break;
		case 7:
			i.frames = 0;
			break;
		case 8:
			i.output_start = UINT64_MAX;
			break;
		case 9:
			i.span_count = 0;
			break;
		case 10:
			i.peak[0] = NAN;
			break;
		case 11:
			i.rms[1] = -1;
			break;
		case 12:
			bands[0] = INFINITY;
			break;
		case 13:
			spans[1].output_start++;
			break;
		case 14:
			spans[0].frames = 0;
			break;
		case 15:
			spans[1].frames = UINT64_MAX;
			break;
		case 16:
			spans[2].track = 1;
			break;
		case 17:
			spans[2].frames = 1;
			break;
		case 18:
			spans[1].output_start--;
			break;
		}
		ck_assert(td_snapshot_publish(p, &i, bands, spans) == expected);
		ck_assert(p->sequence == 1 && !p->dropped);
	}
	teder_snapshot *s = acquire(p);
	verify(s, 1, 1, 0);
	teder_snapshot_info output = INIT(teder_snapshot_info), saved = output;
	output.struct_size = saved.struct_size = 1;
	ck_assert(teder_snapshot_get_info(s, &output) == TEDER_ABI_MISMATCH &&
	          !memcmp(&output, &saved, sizeof(output)));
	td_handle wrong = {TD_RESULT, TEDER_ABI_VERSION};
	const float *bands = NULL;
	const teder_span *spans = NULL;
	uint32_t count = 99;
	ck_assert(teder_snapshot_bands((teder_snapshot *)&wrong, &bands, &count) == TEDER_BAD_STATE &&
	          !bands && count == 99);
	ck_assert(teder_snapshot_spans(NULL, &spans, &count) == TEDER_INVALID && !spans && count == 99);
	ck_assert(teder_snapshot_bands(s, NULL, &count) == TEDER_INVALID);
	ck_assert(teder_snapshot_spans(s, &spans, NULL) == TEDER_INVALID);
	teder_snapshot_release(s);
	teder_snapshot_info i = info(2);
	i.bands = 0;
	i.span_count = 1;
	teder_span span = {14, 7, 0, 0};
	OK(td_snapshot_publish(p, &i, NULL, &span));
	s = acquire(p);
	OK(teder_snapshot_bands(s, &bands, &count));
	ck_assert(!bands && !count);
	teder_snapshot_release(s);
	p->sequence = UINT64_MAX;
	ck_assert(td_snapshot_publish(p, &i, NULL, &span) == TEDER_LIMIT);
	p->sequence = 2;
	p->dropped = UINT64_MAX;
	s = acquire(p);
	publish(p, 3);
	teder_snapshot *other = acquire(p);
	ck_assert(td_snapshot_publish(p, &i, NULL, &span) == TEDER_AGAIN && p->dropped == UINT64_MAX);
	teder_snapshot_release(s);
	teder_snapshot_release(other);
	td_snapshot_pool_release(&p);
	ck_assert(t.attempts == 1 && th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

typedef struct {
	td_snapshot_pool *pool;
	teder_snapshot *snapshot;
	th_gate gate;
} reader;

static void release_thread(void *context)
{
	reader *r = context;
	th_gate_hit(&r->gate);
	if (r->snapshot) {
		verify(r->snapshot, 1, 1, 0);
		teder_snapshot_release(r->snapshot);
	} else
		td_snapshot_pool_release(&r->pool);
}

START_TEST(final_release)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	for (unsigned round = 0; round < 32; round++) {
		td_snapshot_pool *p = create(&t);
		publish(p, 1);
		reader readers[4] = {{0}};
		th_thread threads[4];
		for (unsigned j = 0; j < 4; j++) {
			if (j < 3)
				readers[j].snapshot = acquire(p);
			else
				readers[j].pool = p; /* transfer the one owner pin to this thread */
			ck_assert(th_gate_init(&readers[j].gate));
			ck_assert(th_thread_start(&threads[j], release_thread, &readers[j]));
			th_gate_await(&readers[j].gate);
		}
		p = NULL;
		for (unsigned j = 0; j < 4; j++)
			th_gate_release(&readers[(j + round) % 4].gate);
		for (unsigned j = 0; j < 4; j++) {
			th_thread_join(&threads[j]);
			th_gate_uninit(&readers[j].gate);
		}
		ck_assert(!readers[3].pool && t.attempts == round + 1 && th_tracker_clean(&t));
	}
	th_tracker_uninit(&t);
}
END_TEST

static void hold_gate(void *context)
{
	reader *r = context;
#ifdef _WIN32
	AcquireSRWLockExclusive(&r->pool->gate);
#else
	ck_assert(!pthread_mutex_lock(&r->pool->gate));
#endif
	th_gate_hit(&r->gate);
#ifdef _WIN32
	ReleaseSRWLockExclusive(&r->pool->gate);
#else
	ck_assert(!pthread_mutex_unlock(&r->pool->gate));
#endif
}

static void replace(void *context)
{
	reader *r = context;
	th_gate_hit(&r->gate);
	for (unsigned i = 2; i <= 4096; i++)
		publish(r->pool, i);
}

START_TEST(concurrent)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	teder_config c = config();
	c.snapshot_slots = 3; /* held loan, concurrent reader, and one writable slot */
	td_snapshot_pool *p;
	OK(td_snapshot_pool_create(&c, 65536, th_allocator(&t), (td_runtime){0}, &p));
	publish(p, 1);
	teder_snapshot *held = acquire(p);
	reader r = {.pool = p};
	th_thread thread;
	ck_assert(th_gate_init(&r.gate) && th_thread_start(&thread, hold_gate, &r));
	th_gate_await(&r.gate);
	teder_snapshot *out = held;
	ck_assert(td_snapshot_acquire(p, &out) == TEDER_AGAIN && !out);
	th_gate_release(&r.gate);
	th_thread_join(&thread);
	th_gate_uninit(&r.gate);
	ck_assert(th_gate_init(&r.gate) && th_thread_start(&thread, replace, &r));
	th_gate_await(&r.gate);
	th_gate_release(&r.gate);
	for (unsigned i = 0; i < 4096; i++) {
		verify(held, 1, 1, 0);
		teder_code code = td_snapshot_acquire(p, &out);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK) {
			teder_snapshot_info current = INIT(teder_snapshot_info);
			OK(teder_snapshot_get_info(out, &current));
			verify(out, current.sequence, current.sequence, 0);
			teder_snapshot_release(out);
		}
	}
	th_thread_join(&thread);
	th_gate_uninit(&r.gate);
	out = acquire(p);
	verify(out, 4096, 4096, 0);
	td_snapshot_pool_release(&p);
	verify(held, 1, 1, 0);
	teder_snapshot_release(out);
	teder_snapshot_release(held);
	ck_assert(t.attempts == 1 && th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "bounds"))
		test = bounds;
	else if (!strcmp(argv[1], "retention"))
		test = retention;
	else if (!strcmp(argv[1], "validation"))
		test = validation;
	else if (!strcmp(argv[1], "final-release"))
		test = final_release;
	else if (!strcmp(argv[1], "concurrent"))
		test = concurrent;
	else
		return 2;
	Suite *suite = suite_create("snapshot");
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
