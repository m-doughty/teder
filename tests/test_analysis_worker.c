#include "analysis_pool.h"
#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <math.h>

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert_msg((call) == TEDER_OK, "%s failed", #call)
#define RETRY(code, call) \
	do { \
		(code) = (call); \
		if ((code) == TEDER_BUSY) \
			th_yield(); \
	} while ((code) == TEDER_BUSY)

typedef struct {
	th_tracker tracker;
	th_gate gate;
	atomic_uint point, starts, stops;
} fixture;

static void checkpoint(void *context, uint64_t request, unsigned point)
{
	(void)request;
	fixture *f = context;
	if (point == TD_ANALYSIS_STARTED)
		atomic_fetch_add(&f->starts, 1);
	if (point == TD_ANALYSIS_STOPPED)
		atomic_fetch_add(&f->stops, 1);
	unsigned expected = point;
	if (atomic_compare_exchange_strong(&f->point, &expected, 0))
		th_gate_hit(&f->gate);
}

static teder_engine *create(fixture *f)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->point, 0);
	atomic_init(&f->starts, 0);
	atomic_init(&f->stops, 0);
	ck_assert(th_tracker_init(&f->tracker) && th_gate_init(&f->gate));
	teder_config c = INIT(teder_config);
	c.snapshot_slots = 2;
	c.snapshot_loans = 3;
	c.max_spans = c.max_bands = 2;
	td_runtime r = {.context = f, .media_checkpoint = checkpoint};
	teder_engine *e;
	OK(td_engine_create_with(&c, th_allocator(&f->tracker), r, &e));
	return e;
}

static void dispose(fixture *f, teder_engine **e)
{
	if (*e) {
		OK(th_close(*e));
		OK(teder_destroy(e));
	}
	ck_assert(atomic_load(&f->starts) == 1 && atomic_load(&f->stops) == 1);
	ck_assert(th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
	th_gate_uninit(&f->gate);
}

static size_t owned(teder_engine *e)
{
	td_lock(e);
	size_t bytes = e->owned_bytes;
	td_unlock(e);
	return bytes;
}

static uint64_t configure(teder_engine *e, unsigned enabled, unsigned window)
{
	teder_analysis_config c = INIT(teder_analysis_config);
	c.enabled = enabled;
	c.window_frames = window;
	uint64_t id;
	teder_code code;
	RETRY(code, teder_configure_analysis(e, &c, &id));
	OK(code);
	memset(&c, 0xff, sizeof(c)); /* admission must own a copy */
	return id;
}

static void completed(teder_engine *e, uint64_t id, teder_code outcome)
{
	teder_request_info r = INIT(teder_request_info);
	for (;;) {
		teder_code code = teder_request_get(e, id, &r);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && r.state == TEDER_TERMINAL)
			break;
		th_yield();
	}
	ck_assert_msg(r.operation == TEDER_OP_ANALYSIS && r.outcome == outcome,
	              "request %u: outcome %u, expected %u",
	              (unsigned)id,
	              r.outcome,
	              outcome);
	teder_code code;
	RETRY(code, teder_request_ack(e, id));
	OK(code);
}

static void
feed(teder_engine *e, uint64_t epoch, uint64_t start, unsigned frames, float value, uint64_t source)
{
	float samples[512];
	ck_assert(frames <= 256);
	for (unsigned i = 0; i < frames; i++) {
		samples[2 * i] = value;
		samples[2 * i + 1] = -value;
	}
	td_analysis_write w = td_analysis_begin(&e->analysis, epoch, 48000, start, true);
	td_analysis_append(&w, samples, frames, source, source ? source + 10 : 0);
	td_analysis_finish(&w);
}

static void drain(teder_engine *e)
{
	while (atomic_load_explicit(&e->analysis.read, memory_order_acquire) !=
	       atomic_load_explicit(&e->analysis.write, memory_order_acquire))
		th_yield();
}

static teder_snapshot *acquire(teder_engine *e)
{
	teder_snapshot *s;
	teder_code code;
	do {
		code = teder_snapshot_acquire(e, &s);
		if (code == TEDER_AGAIN)
			th_yield();
	} while (code == TEDER_AGAIN);
	OK(code);
	return s;
}

static teder_snapshot_info info(teder_snapshot *s)
{
	teder_snapshot_info i = INIT(teder_snapshot_info);
	OK(teder_snapshot_get_info(s, &i));
	return i;
}

static void empty(teder_engine *e)
{
	teder_snapshot *s = NULL;
	ck_assert(teder_snapshot_acquire(e, &s) == TEDER_AGAIN && !s);
}

START_TEST(meters)
{
	fixture f;
	teder_engine *e = create(&f);
	size_t base = owned(e);
	empty(e);
	completed(e, configure(e, 1, 4), TEDER_OK);
	ck_assert(owned(e) > base && f.tracker.live == 3);
	float samples[] = {1, -.5f, 0, 0};
	td_analysis_write w = td_analysis_begin(&e->analysis, 1, 48000, 100, true);
	td_analysis_append(&w, samples, 2, 11, 22);
	float silence[4] = {0};
	td_analysis_append(&w, silence, 2, 0, 0);
	td_analysis_finish(&w);
	drain(e);
	teder_snapshot *a = acquire(e), *duplicate = acquire(e);
	ck_assert(a == duplicate);
	teder_snapshot_info i = info(a);
	ck_assert(i.sequence == 1 && i.epoch == 1 && i.config_revision == 1 && i.output_start == 100 &&
	          i.frames == 4 && i.output_rate == 48000 && i.channels == 2 && i.bands == 0 &&
	          i.span_count == 2 && i.peak[0] == 1 && i.peak[1] == .5f && i.rms[0] == .5f &&
	          i.rms[1] == .25f && !i.dropped_frames && !i.dropped_publications);
	const teder_span *spans;
	const float *bands;
	uint32_t count;
	OK(teder_snapshot_spans(a, &spans, &count));
	const teder_span expected[] = {{100, 2, 11, 22}, {102, 2, 0, 0}};
	ck_assert(count == 2 && !memcmp(spans, expected, sizeof(expected)));
	OK(teder_snapshot_bands(a, &bands, &count));
	ck_assert(!bands && !count);
	completed(e, configure(e, 1, 2), TEDER_OK);
	empty(e);
	feed(e, 1, 104, 2, .25f, 11);
	drain(e);
	teder_snapshot *b = acquire(e);
	teder_snapshot_info j = info(b);
	ck_assert(j.sequence == 2 && j.config_revision == 2 && j.frames == 2 && j.peak[0] == .25f &&
	          j.rms[1] == .25f);
	completed(e, configure(e, 0, 0), TEDER_OK);
	ck_assert(owned(e) == base && f.tracker.live == 2);
	empty(e);
	OK(th_close(e));
	OK(teder_destroy(&e));
	ck_assert(f.tracker.live == 1);
	j = info(a);
	ck_assert(!memcmp(&i, &j, sizeof(i)) && !memcmp(spans, expected, sizeof(expected)));
	teder_snapshot_release(a);
	teder_snapshot_release(duplicate);
	j = info(b);
	ck_assert(j.sequence == 2 && j.rms[0] == .25f);
	teder_snapshot_release(b);
	dispose(&f, &e);
}
END_TEST

START_TEST(configuration)
{
	fixture f;
	teder_engine *e = create(&f);
	size_t base = owned(e);
	for (unsigned mode = 0; mode < 13; mode++) {
		teder_analysis_config c = INIT(teder_analysis_config);
		c.enabled = 1;
		teder_code expected = TEDER_INVALID;
		switch (mode) {
		case 0:
			c.enabled = 2;
			break;
		case 1:
			c.reserved = 1;
			break;
		case 2:
			c.low_hz = NAN;
			break;
		case 3:
			c.high_hz = INFINITY;
			break;
		case 4:
			c.floor_db = NAN;
			break;
		case 5:
			c.smoothing_ms = NAN;
			break;
		case 6:
			c.peak_decay_db_per_s = INFINITY;
			break;
		case 7:
			c.window_frames = 65537;
			expected = TEDER_LIMIT;
			break;
		case 8:
			c.bands = 3;
			expected = TEDER_LIMIT;
			break;
		case 9:
			c.bands = 1;
			expected = TEDER_UNSUPPORTED;
			break;
		case 10:
			c.hop_frames = 1;
			expected = TEDER_UNSUPPORTED;
			break;
		case 11:
			c.floor_db = -80;
			expected = TEDER_UNSUPPORTED;
			break;
		case 12:
			c.abi_version++;
			expected = TEDER_ABI_MISMATCH;
			break;
		}
		uint64_t id = 99;
		ck_assert(teder_configure_analysis(e, &c, &id) == expected && !id);
	}
	completed(e, configure(e, 1, 4), TEDER_OK);
	feed(e, 1, 0, 2, .5f, 1);
	drain(e);
	size_t bytes = owned(e);
	f.tracker.fail_at = f.tracker.attempts + 1;
	completed(e, configure(e, 1, 2), TEDER_NO_MEMORY);
	f.tracker.fail_at = 0;
	ck_assert(owned(e) == bytes && f.tracker.live == 3);
	uint64_t limit = e->config.engine_bytes_limit;
	td_lock(e);
	e->config.engine_bytes_limit = bytes;
	td_unlock(e);
	completed(e, configure(e, 1, 2), TEDER_LIMIT);
	td_lock(e);
	e->config.engine_bytes_limit = limit;
	e->analysis_version = UINT64_MAX;
	td_unlock(e);
	completed(e, configure(e, 1, 2), TEDER_LIMIT);
	td_lock(e);
	e->analysis_version = 1;
	td_unlock(e);
	feed(e, 1, 2, 2, .5f, 1);
	drain(e);
	teder_snapshot *s = acquire(e);
	teder_snapshot_info i = info(s);
	ck_assert(i.frames == 4 && i.config_revision == 1 && i.rms[0] == .5f && i.output_start == 0);
	teder_snapshot_release(s);
	completed(e, configure(e, 1, 0), TEDER_OK);
	for (unsigned n = 0; n < 8; n++) {
		feed(e, 1, 4 + 256 * n, 256, .125f, 1);
		drain(e);
	}
	s = acquire(e);
	i = info(s);
	ck_assert(i.frames == 2048 && i.rms[0] == .125f && i.span_count == 1);
	teder_snapshot_release(s);
	completed(e, configure(e, 0, 0), TEDER_OK);
	ck_assert(owned(e) == base);
	for (unsigned n = 0; n < 4; n++) {
		completed(e, configure(e, 1, 1), TEDER_OK);
		feed(e, 1, 3000 + n, 1, .25f, 1);
		drain(e);
		s = acquire(e);
		i = info(s);
		ck_assert(i.config_revision == 4 + 2 * n && i.frames == 1 && i.rms[0] == .25f);
		completed(e, configure(e, 0, 0), TEDER_OK);
		empty(e);
		ck_assert(owned(e) == base && f.tracker.live == 2 && info(s).sequence == i.sequence);
		teder_snapshot_release(s);
	}
	dispose(&f, &e);
}
END_TEST

START_TEST(cancellation)
{
	for (unsigned after = 0; after < 2; after++) {
		fixture f;
		teder_engine *e = create(&f);
		size_t base = owned(e);
		atomic_store(&f.point, after ? TD_ANALYSIS_AFTER_COMMIT : TD_ANALYSIS_BEFORE_COMMIT);
		uint64_t id = configure(e, 1, 4), other = 99;
		th_gate_await(&f.gate);
		teder_analysis_config c = INIT(teder_analysis_config);
		ck_assert(teder_configure_analysis(e, &c, &other) == TEDER_BUSY && !other);
		teder_code code;
		RETRY(code, teder_request_cancel(e, id));
		OK(code);
		th_gate_release(&f.gate);
		completed(e, id, after ? TEDER_OK : TEDER_CANCELLED);
		ck_assert((owned(e) > base) == (after != 0));
		if (after) {
			feed(e, 1, 0, 4, 1, 1);
			drain(e);
			teder_snapshot *s = acquire(e);
			ck_assert(info(s).config_revision == 1);
			teder_snapshot_release(s);
		} else
			empty(e);
		dispose(&f, &e);
	}
}
END_TEST

START_TEST(drops)
{
	fixture f;
	teder_engine *e = create(&f);
	completed(e, configure(e, 1, 4), TEDER_OK);
	feed(e, 1, 0, 4, .25f, 1);
	drain(e);
	teder_snapshot *a = acquire(e);
	feed(e, 1, 4, 4, .5f, 1);
	drain(e);
	teder_snapshot *b = acquire(e);
	feed(e, 1, 8, 4, 1, 1);
	drain(e);
	teder_status status = INIT(teder_status);
	OK(th_read_status(e, &status));
	ck_assert(status.analysis_sequence == 2 && status.analysis_dropped_publications == 1);
	teder_snapshot_release(b);
	feed(e, 1, 12, 1, 1, 1);
	feed(e, 1, 13, 1, 1, 2);
	feed(e, 1, 14, 2, 1, 3); /* three spans cannot fit; never truncate attribution */
	drain(e);
	feed(e, 1, 16, 4, NAN, 1);
	drain(e);
	feed(e, 1, 20, 2, 1, 1);
	drain(e);
	feed(e, 1, 24, 4, .5f, 1); /* discard the incomplete pre-gap window */
	drain(e);
	b = acquire(e);
	teder_snapshot_info i = info(b);
	ck_assert(i.sequence == 3 && i.dropped_frames == 10 && i.dropped_publications == 3);
	atomic_store(&f.point, TD_ANALYSIS_BLOCK);
	feed(e, 1, 28, 4, 1, 1);
	th_gate_await(&f.gate);
	for (unsigned n = 1; n <= 64; n++)
		feed(e, 1, 28 + n * 4, 4, 1, 1);
	OK(th_read_status(e, &status));
	ck_assert(status.analysis_dropped_frames == 14); /* visible while consumer is held */
	th_gate_release(&f.gate);
	drain(e);
	OK(th_read_status(e, &status));
	ck_assert(status.analysis_sequence == 3 && status.analysis_dropped_publications == 67);
	ck_assert(info(a).rms[0] == .25f && info(b).dropped_frames == 10);
	teder_snapshot_release(a);
	teder_snapshot_release(b);
	dispose(&f, &e);
}
END_TEST

START_TEST(stale)
{
	fixture f;
	teder_engine *e = create(&f);
	completed(e, configure(e, 1, 4), TEDER_OK);
	atomic_store(&f.point, TD_ANALYSIS_BEFORE_PUBLISH);
	feed(e, 1, 0, 4, 1, 1);
	th_gate_await(&f.gate);
	td_lock(e);
	e->status.current.epoch = 2; /* same gate/invalidator used by seek, stop and loss */
	td_analysis_invalidate(e);
	td_unlock(e);
	th_gate_release(&f.gate);
	drain(e);
	empty(e);
	feed(e, 1, 4, 4, 1, 1);
	feed(e, 2, 0, 4, .5f, 2);
	drain(e);
	teder_snapshot *a = acquire(e);
	ck_assert(info(a).epoch == 2 && info(a).sequence == 1);
	td_analysis_write old = td_analysis_begin(&e->analysis, 2, 48000, 4, true);
	float samples[8] = {0};
	td_analysis_append(&old, samples, 4, 2, 12);
	completed(e, configure(e, 1, 4), TEDER_OK);
	td_analysis_finish(&old); /* callback captured the old revision before commit */
	drain(e);
	empty(e);
	feed(e, 2, 8, 4, .25f, 2);
	drain(e);
	teder_snapshot *b = acquire(e);
	ck_assert(info(b).config_revision == 2 && info(b).output_start == 8);
	ck_assert(info(a).config_revision == 1 && info(a).rms[0] == .5f);
	teder_snapshot_release(b);
	for (unsigned paused = 0; paused < 2; paused++) {
		uint64_t start = 12 + 6 * paused;
		td_analysis_write partial = td_analysis_begin(&e->analysis, 2, 48000, start, true);
		td_analysis_append(&partial, samples, 2, 2, 12);
		td_analysis_finish(&partial);
		drain(e);
		if (paused)
			(void)td_analysis_begin(&e->analysis, 2, 48000, start + 2, false);
		td_analysis_write resumed =
		    td_analysis_begin(&e->analysis, 2, paused ? 48000 : 44100, start + 2, true);
		td_analysis_append(&resumed, samples, 4, 2, 12);
		td_analysis_finish(&resumed);
		drain(e);
		b = acquire(e);
		teder_snapshot_info i = info(b);
		ck_assert(i.output_start == start + 2 && i.frames == 4 && i.rms[0] == 0 &&
		          i.output_rate == (paused ? 48000u : 44100u) &&
		          i.dropped_frames == 2 + 2 * paused);
		teder_snapshot_release(b);
	}
	teder_snapshot_release(a);
	dispose(&f, &e);
}
END_TEST

START_TEST(closing)
{
	const unsigned points[] = {
	    TD_ANALYSIS_BLOCK, TD_ANALYSIS_BEFORE_PUBLISH, TD_ANALYSIS_BEFORE_COMMIT};
	for (unsigned n = 0; n < 3; n++) {
		fixture f;
		teder_engine *e = create(&f);
		completed(e, configure(e, 1, 4), TEDER_OK);
		feed(e, 1, 0, 4, .5f, 1);
		drain(e);
		teder_snapshot *s = acquire(e), *empty_snapshot = NULL;
		atomic_store(&f.point, points[n]);
		uint64_t id = 0;
		if (n == 2)
			id = configure(e, 1, 2);
		else
			feed(e, 1, 4, 4, 1, 1);
		th_gate_await(&f.gate);
		OK(teder_request_close(e));
		ck_assert(teder_destroy(&e) == TEDER_BUSY && e);
		ck_assert(teder_snapshot_acquire(e, &empty_snapshot) == TEDER_CLOSED && !empty_snapshot);
		ck_assert(info(s).rms[0] == .5f);
		th_gate_release(&f.gate);
		OK(th_close(e));
		if (id)
			completed(e, id, TEDER_CANCELLED);
		ck_assert(f.tracker.live == 2);
		OK(teder_destroy(&e));
		ck_assert(f.tracker.live == 1 && info(s).sequence == 1);
		teder_snapshot_release(s);
		dispose(&f, &e);
	}
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const struct {
		const char *name;
		const TTest *test;
	} cases[] = {{"meters", meters},
	             {"configuration", configuration},
	             {"cancellation", cancellation},
	             {"drops", drops},
	             {"stale", stale},
	             {"closing", closing}};
	for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
		if (strcmp(argv[1], cases[n].name))
			continue;
		Suite *s = suite_create("analysis");
		TCase *tc = tcase_create(cases[n].name);
		tcase_add_test(tc, cases[n].test);
		suite_add_tcase(s, tc);
		SRunner *r = srunner_create(s);
		srunner_set_fork_status(r, CK_NOFORK);
		srunner_run_all(r, CK_NORMAL);
		int failed = srunner_ntests_failed(r);
		srunner_free(r);
		return failed ? 1 : 0;
	}
	return 2;
}
