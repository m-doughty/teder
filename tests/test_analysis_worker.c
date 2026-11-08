#include "analysis_pool.h"
#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <float.h>
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

static uint64_t configure_with(teder_engine *e, teder_analysis_config c)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_configure_analysis(e, &c, &id));
	OK(code);
	memset(&c, 0xff, sizeof(c)); /* admission must own a copy */
	return id;
}

static uint64_t configure(teder_engine *e, unsigned enabled, unsigned window)
{
	teder_analysis_config c = INIT(teder_analysis_config);
	c.enabled = enabled;
	c.window_frames = window;
	return configure_with(e, c);
}

static teder_analysis_config spectral(unsigned window, unsigned hop)
{
	teder_analysis_config c = INIT(teder_analysis_config);
	c.enabled = 1;
	c.bands = 2;
	c.window_frames = window;
	c.hop_frames = hop;
	c.low_hz = 750;
	c.high_hz = 12000;
	return c;
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
	for (unsigned mode = 0; mode < 19; mode++) {
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
			c.window_frames = 33;
			break;
		case 10:
			c.hop_frames = 2049;
			break;
		case 11:
			c.floor_db = -80;
			expected = TEDER_UNSUPPORTED;
			break;
		case 13:
			c.smoothing_ms = -1;
			break;
		case 14:
			c.peak_decay_db_per_s = -1;
			break;
		case 15:
			c.bands = 1;
			c.window_frames = 16;
			break;
		case 16:
			c.bands = 1;
			c.low_hz = -1;
			break;
		case 17:
			c.bands = 1;
			c.high_hz = 20;
			break;
		case 18:
			c.bands = 1;
			c.floor_db = 1;
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
	completed(e, configure_with(e, spectral(32, 16)), TEDER_NO_MEMORY);
	f.tracker.fail_at = 0;
	ck_assert(owned(e) == bytes && f.tracker.live == 3);
	uint64_t limit = e->config.engine_bytes_limit;
	td_lock(e);
	e->config.engine_bytes_limit = bytes;
	td_unlock(e);
	completed(e, configure_with(e, spectral(32, 16)), TEDER_LIMIT);
	td_lock(e);
	e->config.engine_bytes_limit = limit;
	e->analysis_version = UINT64_MAX;
	td_unlock(e);
	completed(e, configure_with(e, spectral(32, 16)), TEDER_LIMIT);
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
		uint64_t id = configure_with(e, spectral(32, 16)), other = 99;
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
			feed(e, 1, 0, 32, 1, 1);
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

/* Model one producer callback at a known output clock, without wall-clock sleeps. */
static void signal(teder_engine *e,
                   uint64_t start,
                   unsigned frames,
                   unsigned rate,
                   float amplitude,
                   bool tone,
                   uint64_t source)
{
	float pcm[512];
	ck_assert(frames <= 256);
	for (unsigned n = 0; n < frames; n++) {
		float x = tone ? (float)sin(2 * 3.141592653589793 * (double)((start + n) % 8) / 8) : 1;
		pcm[2 * n] = amplitude * x;
		pcm[2 * n + 1] = -pcm[2 * n];
	}
	td_analysis_write w = td_analysis_begin(&e->analysis, 1, rate, start, true);
	td_analysis_append(&w, pcm, frames, source, source + 10);
	td_analysis_finish(&w);
	drain(e);
}

static float band(teder_snapshot *s, unsigned index)
{
	const float *values;
	uint32_t count;
	OK(teder_snapshot_bands(s, &values, &count));
	ck_assert(count == 2 && index < count);
	return values[index];
}

START_TEST(overlap)
{
	fixture f;
	teder_engine *e = create(&f);
	completed(e, configure_with(e, spectral(32, 16)), TEDER_OK);
	signal(e, 0, 16, 48000, 1, true, 11);
	empty(e);
	signal(e, 16, 16, 48000, 1, true, 22);
	teder_snapshot *a = acquire(e);
	teder_snapshot_info first = info(a);
	ck_assert(first.sequence == 1 && first.frames == 32 && first.output_start == 0 &&
	          first.config_revision == 1 && first.peak[0] == 1 && first.peak[1] == 1);
	ck_assert_float_eq_tol(first.rms[0], sqrt(.5), 1e-6);
	ck_assert_float_eq_tol(band(a, 1), 0, 1e-5);
	ck_assert(band(a, 0) == -90);
	const teder_span *spans;
	uint32_t count;
	OK(teder_snapshot_spans(a, &spans, &count));
	const teder_span expected[] = {{0, 16, 11, 21}, {16, 16, 22, 32}};
	ck_assert(count == 2 && !memcmp(spans, expected, sizeof(expected)));
	for (unsigned n = 0; n < 2; n++) {
		signal(e, 32 + 16 * n, 16, 48000, 1, true, n ? 33 : 22);
		teder_snapshot *b = acquire(e);
		teder_snapshot_info i = info(b);
		ck_assert(i.output_start == 16 + 16 * n && i.frames == 32 && i.sequence == 2 + n);
		const teder_span *actual;
		OK(teder_snapshot_spans(b, &actual, &count));
		ck_assert(count == n + 1 && actual[0].source == 22 && actual[0].track == 32 &&
		          actual[0].output_start == i.output_start && actual[0].frames == (n ? 16 : 32));
		if (n)
			ck_assert(actual[1].source == 33 && actual[1].track == 43 &&
			          actual[1].output_start == 48 && actual[1].frames == 16);
		teder_snapshot_release(b);
	}
	teder_analysis_config c = spectral(0, 0);
	c.low_hz = c.high_hz = 0; /* public defaults: 2048/512, 20 Hz/Nyquist, -90 dB */
	completed(e, configure_with(e, c), TEDER_OK);
	for (unsigned n = 0; n < 8; n++)
		signal(e, 64 + 256 * n, 256, 48000, 0, false, 44);
	teder_snapshot *b = acquire(e);
	ck_assert(info(b).frames == 2048 && info(b).output_start == 64 && band(b, 1) == -90);
	teder_snapshot_release(b);
	for (unsigned n = 0; n < 2; n++)
		signal(e, 2112 + 256 * n, 256, 48000, 0, false, 44);
	b = acquire(e);
	ck_assert(info(b).output_start == 576 && info(b).frames == 2048);
	teder_snapshot_release(b);
	completed(e, configure(e, 0, 0), TEDER_OK);
	OK(th_close(e));
	OK(teder_destroy(&e));
	teder_snapshot_info retained = info(a);
	ck_assert(!memcmp(&first, &retained, sizeof(first)) &&
	          !memcmp(spans, expected, sizeof(expected)) && band(a, 0) == -90);
	ck_assert_float_eq_tol(band(a, 1), 0, 1e-5);
	teder_snapshot_release(a);
	dispose(&f, &e);
}
END_TEST

START_TEST(filters)
{
	teder_snapshot_info results[2];
	for (unsigned polling = 0; polling < 2; polling++) {
		fixture f;
		teder_engine *e = create(&f);
		teder_analysis_config c = INIT(teder_analysis_config);
		c.enabled = 1;
		c.window_frames = 4;
		c.hop_frames = 2;
		c.smoothing_ms = (float)(1000 / log(2)); /* one-second hops give alpha=1/2 */
		c.peak_decay_db_per_s = (float)(20 * log10(2));
		completed(e, configure_with(e, c), TEDER_OK);
		const float levels[] = {1, 0, 0};
		for (unsigned n = 0; n < 3; n++) {
			signal(e, n ? 2 + 2 * n : 0, n ? 2 : 4, 2, levels[n], false, 1);
			if (polling) {
				teder_snapshot *s = acquire(e);
				teder_snapshot_info i = info(s);
				ck_assert_float_eq_tol(i.peak[0], n == 2 ? .5 : 1, 1e-6);
				ck_assert_float_eq_tol(i.rms[0], n ? sqrt(n == 1 ? .75 : .375) : 1, 1e-6);
				teder_snapshot_release(s);
			}
		}
		teder_snapshot *s = acquire(e);
		results[polling] = info(s);
		teder_snapshot_release(s);
		/* Pause, clock gaps and rate changes prime a new raw window. */
		for (unsigned n = 0; n < 3; n++) {
			uint64_t start = n == 0 ? 8 : 20 + 4 * n;
			if (!n)
				(void)td_analysis_begin(&e->analysis, 1, 2, start, false);
			signal(e, start, 4, n == 2 ? 4 : 2, .125f, false, 1);
			s = acquire(e);
			ck_assert(info(s).rms[0] == .125f && info(s).peak[0] == .125f &&
			          !info(s).dropped_frames);
			teder_snapshot_release(s);
		}
		td_lock(e);
		e->status.current.epoch = 2;
		td_analysis_invalidate(e);
		td_unlock(e);
		for (unsigned n = 0; n < 2; n++) {
			if (n)
				completed(e, configure_with(e, c), TEDER_OK);
			feed(e, 2, 4 * n, 4, n ? .03125f : .0625f, 2);
			drain(e);
			s = acquire(e);
			ck_assert(info(s).epoch == 2 && info(s).config_revision == 1 + n &&
			          info(s).rms[0] == (n ? .03125f : .0625f) &&
			          info(s).peak[0] == info(s).rms[0]);
			teder_snapshot_release(s);
		}
		dispose(&f, &e);
	}
	ck_assert(!memcmp(results, results + 1, sizeof(results[0])));
}
END_TEST

START_TEST(spectrum_filters)
{
	float result[2];
	for (unsigned pinned = 0; pinned < 2; pinned++) {
		fixture f;
		teder_engine *e = create(&f);
		teder_analysis_config c = spectral(32, 32);
		c.smoothing_ms = (float)((32.0 / 48) / log(2));
		completed(e, configure_with(e, c), TEDER_OK);
		teder_snapshot *held[2] = {NULL, NULL};
		const float levels[] = {1, .5f, 0, 0};
		for (unsigned n = 0; n < 4; n++) {
			if (n == 3)
				teder_snapshot_release(held[1]);
			signal(e, 32 * n, 32, 48000, levels[n], true, 1);
			if (pinned && n < 2)
				held[n] = acquire(e);
		}
		teder_snapshot *s = acquire(e);
		result[pinned] = band(s, 1);
		ck_assert_float_eq_tol(result[pinned], (-3.0102999566 - 90 - 180) / 4, 1e-4);
		ck_assert(info(s).output_start == 96 && info(s).sequence == 4 - pinned &&
		          info(s).dropped_publications == pinned && !info(s).dropped_frames);
		teder_snapshot_release(s);
		teder_snapshot_release(held[0]);
		dispose(&f, &e);
	}
	ck_assert(result[0] == result[1]); /* dropped publications still advance audio-time filters */
}
END_TEST

typedef struct {
	double rms[2], peak[2], band;
} filtered;

/* Publish one window per level (the first window is four frames, then one hop of
 * two) and read each back. Meters use a 4-frame window and a 2-frame hop at 2 Hz,
 * so every hop is one second; spectra use disjoint 32-frame tone windows at 48 kHz. */
static void run_filters(
    bool banded, const float *levels, unsigned count, float smoothing, float decay, filtered *out)
{
	fixture f;
	teder_engine *e = create(&f);
	teder_analysis_config c = INIT(teder_analysis_config);
	if (banded) {
		c = spectral(32, 32);
	} else {
		c.enabled = 1;
		c.window_frames = 4;
		c.hop_frames = 2;
	}
	c.smoothing_ms = smoothing;
	c.peak_decay_db_per_s = decay;
	completed(e, configure_with(e, c), TEDER_OK);
	for (unsigned k = 0; k < count; k++) {
		if (banded)
			signal(e, 32 * k, 32, 48000, levels[k], true, 1);
		else
			signal(e, k ? 2 + 2 * k : 0, k ? 2 : 4, 2, levels[k], false, 1);
		teder_snapshot *s = acquire(e);
		teder_snapshot_info i = info(s);
		ck_assert(i.sequence == k + 1 && !i.dropped_publications);
		for (unsigned ch = 0; ch < 2; ch++) {
			out[k].rms[ch] = i.rms[ch];
			out[k].peak[ch] = i.peak[ch];
		}
		out[k].band = banded ? band(s, 1) : 0;
		teder_snapshot_release(s);
	}
	dispose(&f, &e);
}

static void expect_near(double got, double expected, const char *what, unsigned k)
{
	bool within = expected == 0 ? got == 0 : fabs(got - expected) <= 1e-6 * fabs(expected);
	ck_assert_msg(
	    isfinite(got) && within, "%s of window %u: got %.9g expected %.9g", what, k, got, expected);
}

START_TEST(filter_extremes)
{
	/* Zero, the smallest subnormal, the smallest normal and the largest finite value
	 * for each control, alone and in the four mixed combinations. */
	const float controls[][2] = {{0, 0},
	                             {FLT_TRUE_MIN, FLT_TRUE_MIN},
	                             {FLT_MIN, FLT_MIN},
	                             {FLT_MAX, FLT_MAX},
	                             {0, FLT_MAX},
	                             {FLT_MAX, 0},
	                             {FLT_TRUE_MIN, FLT_MAX},
	                             {FLT_MAX, FLT_TRUE_MIN},
	                             {1e30f, 1e30f}};
	/* One sequence starts loud and one starts silent, so the extreme smoothing
	 * constants are applied to a nonzero and to a zero previous state. */
	const float sequences[][6] = {{1, .5f, 0, .25f, 0, 2}, {0, 1, .5f, 0, .25f, 0}};
	for (unsigned banded = 0; banded < 2; banded++)
		for (unsigned q = 0; q < 2; q++) {
			const float *levels = sequences[q];
			filtered raw[6];
			run_filters(banded, levels, 6, 0, 0, raw);
			double raw_rms[6], raw_peak[6];
			for (unsigned k = 0; k < 6; k++) {
				double previous = k ? levels[k - 1] : levels[k];
				raw_peak[k] = fmax(fabs(previous), fabs(levels[k]));
				raw_rms[k] = banded ? levels[k] / sqrt(2.0)
				                    : sqrt((previous * previous + levels[k] * levels[k]) / 2);
				/* With both controls at zero the meters are exactly the raw windows. */
				expect_near(raw[k].rms[0], raw_rms[k], "raw rms", k);
				expect_near(raw[k].rms[1], raw_rms[k], "raw rms", k);
				expect_near(raw[k].peak[0], banded ? levels[k] : raw_peak[k], "raw peak", k);
				/* A unit tone in both channels reads 0 dB; silence reads the -90 dB floor. */
				double raw_db = levels[k] ? fmax(-90, 20 * log10(levels[k])) : -90;
				if (banded)
					ck_assert_msg(fabs(raw[k].band - raw_db) < 1e-3,
					              "raw band of window %u: %g",
					              k,
					              raw[k].band);
			}
			const double seconds = banded ? 32.0 / 48000 : 1;
			for (unsigned n = 0; n < sizeof(controls) / sizeof(controls[0]); n++) {
				filtered got[6];
				run_filters(banded, levels, 6, controls[n][0], controls[n][1], got);
				double power = 0, peak = 0, band = 0;
				for (unsigned k = 0; k < 6; k++) {
					/* The first window primes the filters; later ones follow the documented
					 * recurrences, with alpha from expm1 so a vanishing rate stays exact. */
					double smoothing = controls[n][0], dB = controls[n][1];
					double alpha = !k || !smoothing ? 1 : -expm1(-seconds * 1000 / smoothing);
					double decay = !k || !dB ? 0 : pow(10, -seconds * dB / 20);
					power = (1 - alpha) * (k ? power : 0) + alpha * raw_rms[k] * raw_rms[k];
					peak = fmax(banded ? levels[k] : raw_peak[k], peak * decay);
					band = (1 - alpha) * (k ? band : 0) + alpha * raw[k].band;
					for (unsigned ch = 0; ch < 2; ch++) {
						expect_near(got[k].rms[ch], sqrt(power), "rms", k);
						expect_near(got[k].peak[ch], peak, "peak", k);
					}
					if (banded)
						expect_near(got[k].band, band, "band", k);
				}
			}
		}
}
END_TEST

START_TEST(overlap_drops)
{
	fixture f;
	teder_engine *e = create(&f);
	completed(e, configure_with(e, spectral(32, 16)), TEDER_OK);
	feed(e, 1, 0, 8, 1, 1);
	feed(e, 1, 8, 8, 1, 2);
	feed(e, 1, 16, 16, 1, 3);
	drain(e);
	empty(e); /* three identities exceed the configured span capacity */
	feed(e, 1, 32, 16, .5f, 3);
	drain(e);
	teder_snapshot *s = acquire(e);
	ck_assert(info(s).output_start == 16 && info(s).span_count == 1 &&
	          info(s).dropped_frames == 32 && info(s).dropped_publications == 1);
	teder_snapshot_release(s);
	feed(e, 1, 48, 16, NAN, 3);
	feed(e, 1, 64, 16, .25f, 3);
	feed(e, 1, 80, 16, .25f, 3);
	feed(e, 1, 96, 8, .25f, 3);
	drain(e);
	feed(e, 1, 112, 32, .125f, 3); /* only eight fresh partial frames are discarded */
	drain(e);
	s = acquire(e);
	ck_assert(info(s).output_start == 112 && info(s).dropped_frames == 72 &&
	          info(s).dropped_publications == 3 && info(s).rms[0] == .125f);
	teder_snapshot_release(s);
	teder_analysis_config c = spectral(32, 16);
	c.high_hz = 0;
	completed(e, configure_with(e, c), TEDER_OK);
	signal(e, 0, 32, 1000, .5f, false, 1); /* auto Nyquist below low edge */
	empty(e);
	signal(e, 32, 32, 48000, .25f, false, 1);
	s = acquire(e);
	ck_assert(info(s).output_start == 32 && info(s).dropped_frames == 104 &&
	          info(s).dropped_publications == 4 && info(s).rms[0] == .25f);
	teder_snapshot_release(s);
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
		completed(e, configure_with(e, spectral(32, 16)), TEDER_OK);
		feed(e, 1, 0, 32, .5f, 1);
		drain(e);
		teder_snapshot *s = acquire(e), *empty_snapshot = NULL;
		atomic_store(&f.point, points[n]);
		uint64_t id = 0;
		if (n == 2)
			id = configure_with(e, spectral(64, 16));
		else
			feed(e, 1, 32, 16, 1, 1);
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
	             {"closing", closing},
	             {"overlap", overlap},
	             {"filters", filters},
	             {"spectrum-filters", spectrum_filters},
	             {"filter-extremes", filter_extremes},
	             {"overlap-drops", overlap_drops}};
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
