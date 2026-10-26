#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <stdio.h>
#include <math.h>
#include "source.h"
#include "support/track.h"
#define LONG_TRACK OUTPUT_DIR "/output-long.flac"
enum { LONG_FRAMES = 32769 };
#define TRACK FIXTURE_DIR "/track-a.flac"

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert((call) == TEDER_OK)
#define RETRY(code, call) \
	do { \
		(code) = (call); \
		if ((code) == TEDER_BUSY) \
			th_yield(); \
	} while ((code) == TEDER_BUSY)

typedef struct {
	th_tracker tracker;
	th_gate gate, render_gate;
	th_thread thread;
	ma_device *native;
	atomic_uint stop, hold, captured, step, stepped;
	bool manual;
	bool worker, fail_start, fail_stop;
	float capture[LONG_FRAMES * 2];
	atomic_uint point;
	unsigned opened, closed, enumerated;
	bool reject, enum_error, device_error;
	unsigned devices, releases, starts;
	void *native_heap;
#ifdef _WIN32
	DWORD caller, owner;
#else
	pthread_t caller, owner;
#endif
} fixture;

static void hit(fixture *f, unsigned point)
{
	unsigned expected = point;
	if (atomic_compare_exchange_strong(&f->point, &expected, 0))
		th_gate_hit(&f->gate);
}

static void owner(fixture *f)
{
#ifdef _WIN32
	ck_assert(GetCurrentThreadId() == f->owner && f->owner != f->caller);
#else
	ck_assert(pthread_equal(pthread_self(), f->owner) && !pthread_equal(f->owner, f->caller));
#endif
}

static ma_result
describe(ma_context *c, ma_device_type type, const ma_device_id *id, ma_device_info *info)
{
	(void)c;
	(void)type;
	memset(info, 0, sizeof(*info));
	info->id.wasapi[0] = id ? id->wasapi[0] : 'A';
	memcpy(info->name, "Selected", 9);
	return MA_SUCCESS;
}

static ma_result device_info(ma_device *d, ma_device_type type, ma_device_info *info)
{
	return describe(d->pContext, type, &d->playback.id, info);
}

static ma_result device_init(ma_device *d,
                             const ma_device_config *config,
                             ma_device_descriptor *playback,
                             ma_device_descriptor *capture)
{
	(void)capture;
	fixture *f = d->pContext->pUserData;
	owner(f);
	if (f->device_error)
		return MA_ERROR;
	ck_assert(config->playback.pDeviceID);
	ma_wchar_win32 id = config->playback.pDeviceID->wasapi[0];
	ck_assert(id == 'A' || id == 'B');
	playback->format = ma_format_f32;
	playback->channels = 2;
	playback->sampleRate = id == 'A' ? 44100 : 22050;
	playback->periodSizeInFrames = 8;
	f->native = d;
	f->devices++;
	return MA_SUCCESS;
}

static void join_output(fixture *f)
{
	atomic_store(&f->stop, 1);
	if (f->worker) {
		th_thread_join(&f->thread);
		f->worker = false;
	}
}

static ma_result device_uninit(ma_device *d)
{
	fixture *f = d->pContext->pUserData;
	owner(f);
	hit(f, 8);
	join_output(f);
	f->releases++;
	return MA_SUCCESS;
}

static void render_output(void *context)
{
	fixture *f = context;
	if (atomic_exchange(&f->hold, 0))
		th_gate_hit(&f->render_gate);
	td_device_feed *feed = f->native->pUserData;
	const unsigned sizes[] = {1, 31, 257, 7};
	unsigned call = 0, captured = 0;
	while (!atomic_load(&f->stop)) {
		float pcm[516];
		unsigned frames = f->manual ? atomic_exchange(&f->step, 0) : sizes[call++ % 4];
		if (!frames) {
			th_yield();
			continue;
		}
		ck_assert(frames <= 257);
		pcm[2 * frames] = 12345;
		uint64_t before = feed->report.pcm_frames;
		f->native->onData(f->native, pcm, NULL, frames);
		unsigned count = (unsigned)(feed->report.pcm_frames - before);
		ck_assert(count <= frames && captured + count <= LONG_FRAMES && pcm[2 * frames] == 12345);
		memcpy(f->capture + captured * 2, pcm, count * 2 * sizeof(float));
		captured += count;
		for (unsigned i = count * 2; i < frames * 2; i++)
			ck_assert(pcm[i] == 0);
		atomic_store(&f->captured, captured);
		atomic_fetch_add(&f->stepped, 1);
		th_yield(); /* no producer wake; this fake is deliberately faster than hardware */
	}
}

static ma_result device_start(ma_device *d)
{
	fixture *f = d->pContext->pUserData;
	owner(f);
	f->starts++;
	atomic_store(&f->stop, 0);
	atomic_store(&f->captured, 0);
	f->worker = th_thread_start(&f->thread, render_output, f);
	return !f->worker || f->fail_start ? MA_ERROR : MA_SUCCESS;
}

static ma_result device_stop(ma_device *d)
{
	fixture *f = d->pContext->pUserData;
	owner(f);
	hit(f, 11);
	if (f->fail_stop)
		return MA_ERROR;
	join_output(f);
	return MA_SUCCESS;
}

static ma_result context_init(const ma_backend *backends,
                              ma_uint32 count,
                              const ma_context_config *config,
                              ma_context *c)
{
	(void)backends;
	ck_assert(count == 4 || count == 1);
	fixture *f = config->pUserData;
#ifdef _WIN32
	f->owner = GetCurrentThreadId();
#else
	f->owner = pthread_self();
#endif
	owner(f);
	hit(f, 1);
	if (f->reject)
		return MA_NO_BACKEND;
	memset(c, 0, sizeof(*c));
	c->backend = ma_backend_wasapi;
	c->pUserData = f;
	c->allocationCallbacks = config->allocationCallbacks;
	f->native_heap = ma_malloc(64, &c->allocationCallbacks);
	if (!f->native_heap)
		return MA_OUT_OF_MEMORY;
	ma_result code = ma_mutex_init(&c->deviceInfoLock);
	if (code != MA_SUCCESS) {
		ma_free(f->native_heap, &c->allocationCallbacks);
		f->native_heap = NULL;
		return code;
	}
	c->callbacks.onContextGetDeviceInfo = describe;
	c->callbacks.onDeviceGetInfo = device_info;
	c->callbacks.onDeviceInit = device_init;
	c->callbacks.onDeviceUninit = device_uninit;
	c->callbacks.onDeviceStart = device_start;
	c->callbacks.onDeviceStop = device_stop;
	f->opened++;
	return MA_SUCCESS;
}

static ma_result context_uninit(ma_context *c)
{
	fixture *f = c->pUserData;
	owner(f);
	ma_free(f->native_heap, &c->allocationCallbacks);
	ma_mutex_uninit(&c->deviceInfoLock);
	f->native_heap = NULL;
	f->closed++;
	return MA_SUCCESS;
}

static ma_result enumerate(ma_context *c, ma_enum_devices_callback_proc callback, void *user)
{
	fixture *f = c->pUserData;
	owner(f);
	hit(f, 2);
	f->enumerated++;
	void *temporary = ma_malloc(32, &c->allocationCallbacks);
	if (!temporary)
		return MA_OUT_OF_MEMORY;
	ma_free(temporary, &c->allocationCallbacks);
	if (f->enum_error)
		return MA_ERROR;
	for (unsigned i = 0; i < 2; i++) {
		ma_device_info info = {0};
		info.id.wasapi[0] = (ma_wchar_win32)('A' + i);
		info.isDefault = i == 0;
		memcpy(info.name, i ? "Beta" : "Alpha", i ? 5 : 6);
		if (!callback(c, ma_device_type_playback, &info, user))
			break;
	}
	return MA_SUCCESS;
}

static void checkpoint(void *p, uint64_t request, unsigned point)
{
	(void)request;
	if (point == TD_OUTPUT_BEFORE_COMMIT)
		hit(p, 3);
	if (point == TD_OUTPUT_AFTER_COMMIT)
		hit(p, 4);
	if (point == TD_SELECT_BEFORE_COMMIT)
		hit(p, 5);
	if (point == TD_SELECT_AFTER_COMMIT)
		hit(p, 6);
	if (point == TD_DECODE_IDLE)
		hit(p, 7);
	if (point == TD_SOURCE_OPENED)
		hit(p, 9);
	if (point == TD_PREPARE_JOB)
		hit(p, 10);
	if (point == TD_TRANSPORT_BEFORE_COMMIT)
		hit(p, 12);
	if (point == TD_TRANSPORT_PUBLISHED)
		hit(p, 13);
	if (point == TD_MEDIA_BEFORE_COMMIT)
		hit(p, 14);
	if (point == TD_REFILL_PUMP)
		hit(p, 15);
}

static void init(fixture *f)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->point, 0);
	atomic_init(&f->stop, 0);
	atomic_init(&f->hold, 0);
	atomic_init(&f->captured, 0);
	atomic_init(&f->step, 0);
	atomic_init(&f->stepped, 0);
	ck_assert(th_gate_init(&f->render_gate));
	ck_assert(th_tracker_init(&f->tracker) && th_gate_init(&f->gate));
#ifdef _WIN32
	f->caller = GetCurrentThreadId();
#else
	f->caller = pthread_self();
#endif
}

static void dispose(fixture *f)
{
	ck_assert(f->opened == f->closed && f->devices == f->releases && !f->worker &&
	          !f->native_heap && th_tracker_clean(&f->tracker));
	th_gate_uninit(&f->gate);
	th_gate_uninit(&f->render_gate);
	th_tracker_uninit(&f->tracker);
}

static teder_engine *create(fixture *f, teder_config config, bool native)
{
	td_runtime runtime = {.context = f,
	                      .media_checkpoint = checkpoint,
	                      .backend = {context_init, context_uninit, enumerate, f}};
	teder_engine *e;
	OK(td_engine_create_with(
	    &config, th_allocator(&f->tracker), native ? (td_runtime){0} : runtime, &e));
	return e;
}

static teder_request_info terminal(teder_engine *e, uint64_t id)
{
	teder_request_info r = INIT(teder_request_info);
	for (;;) {
		teder_code code = teder_request_get(e, id, &r);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && r.state == TEDER_TERMINAL)
			return r;
		th_yield();
	}
}

static uint64_t submit(teder_engine *e)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_enumerate_devices(e, &id));
	OK(code);
	ck_assert(id != 0);
	return id;
}

static void ack(teder_engine *e, uint64_t id)
{
	teder_code code;
	RETRY(code, teder_request_ack(e, id));
	OK(code);
}

static teder_result *take(teder_engine *e, uint64_t id)
{
	teder_result *r;
	teder_code code;
	RETRY(code, teder_request_take_result(e, id, &r));
	OK(code);
	ack(e, id);
	return r;
}

static teder_device_info entry(teder_result *r, unsigned index)
{
	teder_device_info d = INIT(teder_device_info);
	OK(teder_result_device(r, index, &d));
	return d;
}

static size_t owned(teder_engine *e)
{
	td_lock(e);
	size_t bytes = e->owned_bytes;
	td_unlock(e);
	return bytes;
}

static void close_engine(teder_engine **e)
{
	OK(th_close(*e));
	OK(teder_destroy(e));
}

START_TEST(snapshot)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	size_t base = owned(e);
	uint64_t id = submit(e);
	teder_request_info done = terminal(e, id);
	ck_assert(done.outcome == TEDER_OK && done.operation == TEDER_OP_DEVICES &&
	          done.result_kind == TEDER_RESULT_DEVICES && done.committed.epoch == 0);
	teder_result *r = take(e, id);
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	ck_assert(count == 2);
	teder_device_info a = entry(r, 0), b = entry(r, 1);
	ck_assert(a.backend == TEDER_BACKEND_WASAPI && a.is_default == 1 && b.is_default == 0);
	ck_assert(a.name.bytes == 5 && !memcmp(a.name.data, "Alpha", 5));
	ck_assert(b.name.bytes == 4 && !memcmp(b.name.data, "Beta", 4));
	ck_assert(a.key.bytes == 20 && b.key.bytes == 20 && !memcmp(a.key.data, "TDK1", 4));
	ck_assert(((const unsigned char *)a.key.data)[16] == 'A' &&
	          ((const unsigned char *)b.key.data)[16] == 'B');
	size_t context_bytes = owned(e);
	ck_assert(context_bytes == base + e->backend_reservation && f.opened == 1);

	id = submit(e);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ck_assert(owned(e) > context_bytes);
	ack(e, id); /* untaken result dies with acknowledgement */
	ck_assert(owned(e) == context_bytes && f.opened == 1 && f.enumerated == 2);
	teder_status status = INIT(teder_status);
	OK(th_read_status(e, &status));
	ck_assert(status.output_state == TEDER_OUTPUT_NONE && status.backend == 0 &&
	          status.current.epoch == 1 && status.current.source == 0 && status.media_request == 0);

	close_engine(&e);
	ck_assert(f.closed == 1 && f.tracker.live == 1);
	teder_device_info retained = entry(r, 0);
	ck_assert(retained.name.bytes == 5 && !memcmp(retained.name.data, "Alpha", 5));
	ck_assert(retained.key.bytes == a.key.bytes &&
	          !memcmp(retained.key.data, a.key.data, a.key.bytes));
	teder_result_release(r);
	dispose(&f);
}
END_TEST

START_TEST(cancellation)
{
	for (unsigned mode = 0; mode < 4; mode++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		config.request_capacity = 2;
		teder_engine *e = create(&f, config, false);
		if (mode == 3) {
			uint64_t warm = submit(e);
			ck_assert(terminal(e, warm).outcome == TEDER_OK);
			ack(e, warm);
		}
		atomic_store(&f.point, mode == 0 ? 1 : mode == 2 ? 4 : 3);
		uint64_t id = submit(e);
		th_gate_await(&f.gate);
		teder_result *r = NULL;
		ck_assert(teder_request_take_result(e, id, &r) == TEDER_BUSY && !r);
		OK(teder_request_cancel(e, id));
		uint64_t next = submit(e), rejected = 99;
		ck_assert(teder_enumerate_devices(e, &rejected) == TEDER_LIMIT && rejected == 0);

		th_gate_release(&f.gate);
		teder_request_info done = terminal(e, id);
		ck_assert(done.outcome == (mode == 2 ? TEDER_OK : TEDER_CANCELLED));
		ck_assert(done.result_kind == (mode == 2 ? TEDER_RESULT_DEVICES : TEDER_RESULT_NONE));
		ck_assert(terminal(e, next).outcome == TEDER_OK);
		ack(e, id);
		ack(e, next);
		ck_assert(f.opened == (mode < 2 ? 2 : 1));
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(failures)
{
	size_t base = 0, reservation;
	OK(td_backend_plan(8 * 1024 * 1024, &reservation));
	for (unsigned mode = 0; mode < 10; mode++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		if (mode == 4 || mode == 5)
			config.engine_bytes_limit = base + reservation - (mode == 4);
		if (mode == 6)
			config.result_bytes_limit = 1;
		if (mode == 7)
			config.max_input_bytes = 19;
		f.reject = mode == 8;
		f.enum_error = mode == 9;
		teder_engine *e = create(&f, config, false);
		size_t original = owned(e);
		if (mode == 0)
			base = original;
		if (mode < 4)
			f.tracker.fail_at = f.tracker.attempts + mode + 1;
		uint64_t id = submit(e);
		teder_code expected = mode < 4    ? TEDER_NO_MEMORY
		                      : mode < 8  ? TEDER_LIMIT
		                      : mode == 8 ? TEDER_UNSUPPORTED
		                                  : TEDER_DEVICE_LOST;
		ck_assert(terminal(e, id).outcome == expected);
		ck_assert(!e->backend && owned(e) == original && f.tracker.bytes == original);
		ack(e, id);
		if (mode < 4) {
			f.tracker.fail_at = 0;
			id = submit(e);
			ck_assert(terminal(e, id).outcome == TEDER_OK);
			ack(e, id);
		}
		close_engine(&e);
		dispose(&f);
	}

	/* Existing healthy contexts survive result allocation failure. A failed
	 * dependency allocation poisons the context, which must be rebuilt. */
	for (unsigned fail = 1; fail <= 2; fail++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		uint64_t id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_OK);
		ack(e, id);
		uint64_t token = e->backend_token;
		f.tracker.fail_at = f.tracker.attempts + fail;
		id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_NO_MEMORY);
		ck_assert((e->backend != NULL) == (fail == 2));
		ack(e, id);
		f.tracker.fail_at = 0;
		id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_OK && e->backend_token == token);
		close_engine(&e); /* also frees the final untaken result */
		dispose(&f);
	}
}
END_TEST

START_TEST(identity)
{
	atomic_uint_fast64_t counter;
	atomic_init(&counter, UINT64_MAX - 1);
	uint64_t value = 0;
	OK(td_token_next(&counter, &value));
	ck_assert(value == UINT64_MAX);
	ck_assert(td_token_next(&counter, &value) == TEDER_LIMIT && value == UINT64_MAX);
	ck_assert(atomic_load(&counter) == UINT64_MAX);

	fixture a, b;
	init(&a);
	init(&b);
	teder_config config = INIT(teder_config);
	teder_engine *first = create(&a, config, false), *second = create(&b, config, false);
	uint64_t one = submit(first), two = submit(second);
	ck_assert(terminal(first, one).outcome == TEDER_OK &&
	          terminal(second, two).outcome == TEDER_OK);
	teder_result *r = take(first, one), *s = take(second, two);
	teder_device_info x = entry(r, 0), y = entry(s, 0);
	ck_assert(first->backend_token && second->backend_token &&
	          first->backend_token != second->backend_token);
	ck_assert(
	    memcmp((const unsigned char *)x.key.data + 4, (const unsigned char *)y.key.data + 4, 8));
	close_engine(&first);
	close_engine(&second);

	first = create(&a, config, false);
	one = submit(first);
	ck_assert(terminal(first, one).outcome == TEDER_OK);
	teder_result *later = take(first, one);
	y = entry(later, 0);
	ck_assert(
	    memcmp((const unsigned char *)x.key.data + 4, (const unsigned char *)y.key.data + 4, 8));
	close_engine(&first);
	teder_result_release(r);
	teder_result_release(s);
	teder_result_release(later);
	dispose(&a);
	dispose(&b);
}
END_TEST

START_TEST(closing)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	size_t base = owned(e);
	atomic_store(&f.point, 2);
	uint64_t id = submit(e);
	th_gate_await(&f.gate);
	uint64_t queued = submit(e);
	OK(teder_request_close(e));
	teder_status status = INIT(teder_status);
	OK(th_read_status(e, &status));
	ck_assert(status.lifecycle == TEDER_CLOSING && f.closed == 0);
	ck_assert(teder_destroy(&e) == TEDER_BUSY && e);

	th_gate_release(&f.gate);
	OK(th_close(e));
	ck_assert(terminal(e, id).outcome == TEDER_CANCELLED &&
	          terminal(e, queued).outcome == TEDER_CANCELLED);
	ck_assert(owned(e) == base && f.opened == 1 && f.closed == 1 && !e->backend);
	uint64_t rejected = 99;
	ck_assert(teder_enumerate_devices(e, &rejected) == TEDER_CLOSED && rejected == 0);
	OK(teder_destroy(&e));
	dispose(&f);
}
END_TEST

static uint64_t select_device(teder_engine *e, teder_blob key)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_select_device(e, key.data, key.bytes, &id));
	OK(code);
	return id;
}

static void load_track(teder_engine *e)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 77, 0, &id));
	OK(code);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
}

static teder_status status(teder_engine *e)
{
	teder_status s = INIT(teder_status);
	OK(th_read_status(e, &s));
	return s;
}

static void idle(fixture *f, teder_engine *e)
{
	while (!(status(e).clock_flags & TEDER_DECODE_EOF))
		th_yield();
	atomic_store(&f->point, 7);
	td_signal(e);
	th_gate_await(&f->gate); /* sole producer/idle consumer is parked; device is stopped */
}

START_TEST(selection)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	unsigned char unissued[20] = {'T', 'D', 'K', '1'};
	unissued[12] = 1;
	unissued[13] = 4;
	unissued[16] = 'A';
	uint64_t id = select_device(e, (teder_blob){unissued, sizeof(unissued)});
	ck_assert(terminal(e, id).outcome == TEDER_STALE && f.opened == 0);
	ack(e, id);
	id = submit(e);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	teder_result *r = take(e, id);
	teder_device_info b = entry(r, 1);
	unsigned char key[20];
	memcpy(key, b.key.data, sizeof(key));
	teder_result_release(r);
	atomic_store(&f.point, 5);
	id = select_device(e, (teder_blob){key, sizeof(key)});
	th_gate_await(&f.gate);
	memset(key, 0, sizeof(key));
	uint64_t other;
	ck_assert(teder_select_device(e, NULL, 0, &other) == TEDER_BUSY && other == 0);
	ck_assert(teder_open(e, TRACK, sizeof(TRACK) - 1, 9, 0, &other) == TEDER_BUSY);
	th_gate_release(&f.gate);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	teder_status s = status(e);
	ck_assert(s.current.epoch == 2 && s.current.source == 0 && s.transport == TEDER_EMPTY &&
	          s.output_state == TEDER_OUTPUT_READY && s.output_rate == 22050 &&
	          s.output_channels == 2 && s.output_period_frames == 8 && s.backend == 1 &&
	          s.latency_quality == TEDER_QUALITY_UNKNOWN && s.latency_frames == 0);
	teder_event event = INIT(teder_event);
	OK(teder_event_poll(e, &event));
	ck_assert(event.kind == TEDER_EVENT_DEVICE && event.epoch == 2 && event.code == TEDER_OK);

	for (unsigned mode = 0; mode < 2; mode++) {
		memcpy(key, "TDK1", 4);
		if (mode) { /* structurally valid, different engine */
			key[4] = 255;
			key[12] = 1;
			key[13] = 4;
			key[16] = 'A';
		}
		id = select_device(e, (teder_blob){key, sizeof(key)});
		ck_assert(terminal(e, id).outcome == (mode ? TEDER_STALE : TEDER_INVALID));
		ack(e, id);
		ck_assert(status(e).current.epoch == 2 && f.devices == 1 && f.releases == 0);
	}
	f.device_error = true;
	id = select_device(e, (teder_blob){0});
	ck_assert(terminal(e, id).outcome == TEDER_DEVICE_LOST);
	ack(e, id);
	s = status(e);
	ck_assert(s.current.epoch == 3 && s.output_state == TEDER_OUTPUT_LOST && s.output_rate == 0 &&
	          !e->device && f.releases == 1);
	f.device_error = false;
	id = select_device(e, (teder_blob){0});
	ck_assert(terminal(e, id).outcome == TEDER_OK && status(e).current.epoch == 4);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(rates)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	config.pcm_frames = 64;
	teder_engine *e = create(&f, config, false);
	load_track(e);
	idle(&f, e);
	ck_assert(e->pcm.free_count == 0); /* old published PCM must be returned on selection */
	th_gate_release(&f.gate);
	uint64_t id = submit(e);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	teder_result *r = take(e, id);
	teder_device_info b = entry(r, 1);
	id = select_device(e, b.key);
	teder_result_release(r);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	idle(&f, e);
	teder_status s = status(e);
	ck_assert(s.current.epoch == 3 && s.current.source == 1 && s.track == 77 &&
	          s.transport == TEDER_PAUSED && s.source_frame == 0 && s.output_rate == 22050);
	/* Independent binary64 reference from fixtures/conversion_reference.py. */
	static const float down[] = {-0.07722943774f,
	                             -0.0684246029f,
	                             0.02713067635f,
	                             0.06513578667f,
	                             0.1416162903f,
	                             0.1716223316f,
	                             0.05848782661f,
	                             -0.04445810882f};
	td_pcm_view v;
	OK(td_pcm_peek(&e->pcm, &v));
	ck_assert(v.frames == 8 && v.generation->generation == 3 && v.generation->source == 1);
	for (unsigned i = 0; i < 8; i++)
		ck_assert(fabsf(v.samples[2 * i] - down[i]) < 1e-6f &&
		          fabsf(v.samples[2 * i + 1] + down[i]) < 1e-6f);
	th_gate_release(&f.gate);

	teder_code code;
	RETRY(code, teder_seek(e, &s.current, 7, &id));
	OK(code);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	id = select_device(e, (teder_blob){0});
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	idle(&f, e);
	s = status(e);
	ck_assert(s.current.epoch == 5 && s.current.source == 1 && s.source_frame == 7 &&
	          s.transport == TEDER_PAUSED && s.output_rate == 44100);
	OK(td_pcm_peek(&e->pcm, &v));
	ck_assert(v.frames == 9 && v.generation->generation == 5);
	for (unsigned i = 0; i < 9; i++) {
		float expected = ((int)i * 1000 - 1000) / 32768.0f;
		ck_assert(v.samples[2 * i] == expected && v.samples[2 * i + 1] == -expected);
	}
	th_gate_release(&f.gate);
	RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 88, TEDER_OPEN_AUTOPLAY, &id));
	OK(code);
	ck_assert(terminal(e, id).outcome == TEDER_OK && status(e).track == 88 && f.starts == 1);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(select_failures)
{
	for (unsigned fail = 1; fail <= 3; fail++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		load_track(e);
		uint64_t id = select_device(e, (teder_blob){0});
		ck_assert(terminal(e, id).outcome == TEDER_OK);
		ack(e, id);
		idle(&f, e);
		f.tracker.fail_at =
		    f.tracker.attempts + fail; /* device arena, converter heap, source arena */
		id = select_device(e, (teder_blob){0});
		th_gate_release(&f.gate);
		ck_assert(terminal(e, id).outcome == TEDER_NO_MEMORY);
		ack(e, id);
		teder_status s = status(e);
		ck_assert_msg(s.current.epoch == 4 && s.current.source == 1 && s.track == 77 &&
		                  s.transport == (fail < 3 ? TEDER_PAUSED : TEDER_FAILED) &&
		                  s.output_state == (fail < 3 ? TEDER_OUTPUT_LOST : TEDER_OUTPUT_READY),
		              "fail=%u epoch=%llu source=%llu transport=%u output=%u site=%s",
		              fail,
		              (unsigned long long)s.current.epoch,
		              (unsigned long long)s.current.source,
		              s.transport,
		              s.output_state,
		              f.tracker.failed_site);
		f.tracker.fail_at = 0;
		id = select_device(e, (teder_blob){0});
		ck_assert(terminal(e, id).outcome == TEDER_OK);
		ack(e, id);
		idle(&f, e);
		f.tracker.fail_at = f.tracker.attempts + 1; /* native enumeration with a selected device */
		id = submit(e);
		th_gate_release(&f.gate);
		ck_assert(terminal(e, id).outcome == TEDER_NO_MEMORY);
		ck_assert(!e->device && !e->backend && status(e).current.epoch == 6 &&
		          status(e).output_state == TEDER_OUTPUT_LOST);
		close_engine(&e);
		dispose(&f);
	}
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	size_t base = owned(e), context_bytes, device_bytes;
	OK(td_backend_plan(8 * 1024 * 1024, &context_bytes));
	OK(td_device_plan(&device_bytes));
	close_engine(&e);
	config.engine_bytes_limit = base + context_bytes + device_bytes - 1;
	e = create(&f, config, false);
	uint64_t id = submit(e);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	size_t allocations = f.tracker.attempts;
	id = select_device(e, (teder_blob){0});
	ck_assert(terminal(e, id).outcome == TEDER_LIMIT && status(e).current.epoch == 1 &&
	          f.devices == 0 && f.tracker.attempts == allocations);
	close_engine(&e);
	dispose(&f);

	init(&f);
	config.engine_bytes_limit = 0;
	e = create(&f, config, false);
	td_lock(e);
	e->status.current.epoch = UINT64_MAX;
	td_unlock(e);
	id = select_device(e, (teder_blob){0});
	ck_assert(terminal(e, id).outcome == TEDER_LIMIT);
	OK(th_close(e));
	ck_assert(status(e).current.epoch == UINT64_MAX && f.devices == 0);
	close_engine(&e);
	dispose(&f);
	/* Device loss between dispatch and preparation must invalidate that job,
	 * including when the loss consumes the last representable epoch. */
	init(&f);
	e = create(&f, config, false);
	load_track(e);
	id = select_device(e, (teder_blob){0});
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	idle(&f, e);
	td_lock(e);
	e->status.current.epoch = UINT64_MAX - 1;
	td_unlock(e);
	atomic_store(&f.point, 10);
	th_gate_release(&f.gate);
	teder_code code;
	RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 99, 0, &id));
	OK(code);
	th_gate_await(&f.gate);
	f.tracker.fail_at = f.tracker.attempts + 1;
	uint64_t enumeration = submit(e);
	ck_assert(terminal(e, enumeration).outcome == TEDER_NO_MEMORY);
	f.tracker.fail_at = 0;
	th_gate_release(&f.gate);
	ck_assert_msg(terminal(e, id).outcome == TEDER_STALE,
	              "Dispatched generation changed after device loss");
	ck_assert(status(e).current.epoch == UINT64_MAX && status(e).current.source == 1);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(select_cancel)
{
	for (unsigned point = 5; point <= 9; point++) {
		if (point == 7)
			continue;
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		load_track(e);
		uint64_t id = select_device(e, (teder_blob){0});
		ck_assert(terminal(e, id).outcome == TEDER_OK);
		ack(e, id);
		atomic_store(&f.point, point);
		id = select_device(e, (teder_blob){0});
		th_gate_await(&f.gate);
		if (point == 8 || point == 9) {
			OK(teder_request_close(e));
			ck_assert(teder_destroy(&e) == TEDER_BUSY && e);
		} else
			OK(teder_request_cancel(e, id));
		th_gate_release(&f.gate);
		if (point == 8 || point == 9)
			OK(th_close(e));
		teder_request_info done = terminal(e, id);
		ck_assert(done.outcome == (point == 6 ? TEDER_OK : TEDER_CANCELLED));
		ck_assert(status(e).current.epoch == (point == 5 ? 3 : 4));
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(native)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, true);
	size_t before = f.tracker.attempts;
	uint64_t id = submit(e);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	size_t attempts = f.tracker.attempts - before;
	teder_result *r = take(e, id);
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	printf("Public native enumeration: devices=%u allocations=%zu\n", count, attempts);
	close_engine(&e);
	for (uint32_t i = 0; i < count; i++) {
		teder_device_info d = entry(r, i);
		ck_assert(d.backend >= TEDER_BACKEND_WASAPI && d.backend <= TEDER_BACKEND_PULSE &&
		          d.key.bytes >= 17 && d.key.bytes <= 272 && d.is_default <= 1);
		printf("device=%u backend=%u default=%u name=%.*s\n",
		       i,
		       d.backend,
		       d.is_default,
		       (int)d.name.bytes,
		       d.name.data);
	}
	teder_result_release(r);
	dispose(&f);

	for (size_t fail = 1; fail <= attempts; fail++) {
		init(&f);
		e = create(&f, config, true);
		size_t base = owned(e);
		f.tracker.fail_at = f.tracker.attempts + fail;
		id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_NO_MEMORY);
		ck_assert(owned(e) == base && !e->backend);
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

static uint64_t autoplay(teder_engine *e, const char *path)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_open(e, path, (uint32_t)strlen(path), 99, TEDER_OPEN_AUTOPLAY, &id));
	OK(code);
	return id;
}

static void ended(fixture *f, teder_engine *e, unsigned frames)
{
	while (status(e).transport != TEDER_ENDED || atomic_load(&f->captured) != frames)
		th_yield();
	ck_assert((status(e).clock_flags & (TEDER_DECODE_EOF | TEDER_SUBMIT_EOF)) ==
	          (TEDER_DECODE_EOF | TEDER_SUBMIT_EOF));
}

START_TEST(playback)
{
	th_make_track(LONG_TRACK, LONG_FRAMES);
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	config.pcm_frames = 2048;
	teder_engine *e = create(&f, config, false);
	OK(terminal(e, select_device(e, (teder_blob){0})).outcome);
	atomic_store(&f.hold, 1);
	uint64_t id = autoplay(e, LONG_TRACK);
	th_gate_await(&f.render_gate);
	OK(terminal(e, id).outcome);
	ck_assert(status(e).transport == TEDER_PLAYING && atomic_load(&f.captured) == 0);
	atomic_store(&f.point, 2);
	uint64_t enumeration = submit(e);
	th_gate_await(&f.gate);
	th_gate_release(&f.render_gate);
	ended(&f, e, LONG_FRAMES); /* callback/refill finish while native control remains blocked */
	for (unsigned i = 0; i < LONG_FRAMES; i++) {
		float expected = ((int32_t)(i % 2048) - 1024) / 32768.0f;
		ck_assert(f.capture[i * 2] == expected && f.capture[i * 2 + 1] == -expected);
	}
	td_device_report report;
	teder_code code;
	do {
		code = td_device_get_report(e->device, &report); /* control/config are held above */
	} while (code == TEDER_AGAIN);
	OK(code);
	ck_assert(report.generation == status(e).current.epoch && report.final == 1 &&
	          report.pcm_frames == LONG_FRAMES && report.error == TEDER_OK);
	ck_assert(status(e).position_quality == TEDER_QUALITY_MEASURED &&
	          status(e).source_frame == LONG_FRAMES &&
	          status(e).output_frame == LONG_FRAMES + status(e).underrun_frames &&
	          status(e).underrun_frames == report.underrun_frames);
	th_gate_release(&f.gate);
	OK(terminal(e, enumeration).outcome);
	close_engine(&e);
	ck_assert(f.starts == 1);
	dispose(&f);
}
END_TEST

START_TEST(play_changes)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	config.pcm_frames = 4;
	teder_engine *e = create(&f, config, false);
	OK(terminal(e, select_device(e, (teder_blob){0})).outcome);
	atomic_store(&f.hold, 1);
	OK(terminal(e, autoplay(e, TRACK)).outcome);
	th_gate_await(&f.render_gate);
	teder_target target = status(e).current;
	atomic_store(&f.point, 11);
	uint64_t id;
	teder_code code;
	RETRY(code, teder_seek(e, &target, 7, &id));
	OK(code);
	th_gate_await(&f.gate); /* stop has been requested; the old native borrower is held */
	ck_assert(status(e).current.epoch == target.epoch);
	th_gate_release(&f.render_gate);
	th_gate_release(&f.gate);
	OK(terminal(e, id).outcome);
	ended(&f, e, 9);
	ck_assert(status(e).current.source == target.source && status(e).source_frame == 16);
	for (unsigned i = 0; i < 9; i++) {
		float expected = ((int)i - 1) * 1000 / 32768.0f;
		ck_assert(f.capture[i * 2] == expected && f.capture[i * 2 + 1] == -expected);
	}
	load_track(e); /* ordinary open stops the old native consumer and stays READY */
	ck_assert(status(e).transport == TEDER_READY && !f.worker && f.starts == 2);
	atomic_store(&f.hold, 1);
	OK(terminal(e, autoplay(e, TRACK)).outcome);
	th_gate_await(&f.render_gate);
	OK(teder_request_close(e));
	ck_assert(status(e).lifecycle == TEDER_CLOSING && teder_destroy(&e) == TEDER_BUSY);
	th_gate_release(&f.render_gate);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(play_failures)
{
	for (unsigned stopping = 0; stopping < 2; stopping++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		OK(terminal(e, select_device(e, (teder_blob){0})).outcome);
		f.fail_start = !stopping;
		uint64_t id = autoplay(e, TRACK);
		if (stopping) {
			OK(terminal(e, id).outcome);
			ended(&f, e, 16);
			f.fail_stop = true;
			id = autoplay(e, TRACK);
		}
		ck_assert(terminal(e, id).outcome == TEDER_DEVICE_LOST);
		ck_assert(status(e).output_state == TEDER_OUTPUT_LOST &&
		          status(e).transport == TEDER_PAUSED && !e->device && !f.worker);
		close_engine(&e);
		dispose(&f);
	}
	size_t count = 0;
	for (size_t n = 0; n <= count; n++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		OK(terminal(e, select_device(e, (teder_blob){0})).outcome);
		ma_mutex_lock(&f.tracker.lock);
		size_t before = f.tracker.attempts;
		f.tracker.fail_at = n ? before + n : 0;
		ma_mutex_unlock(&f.tracker.lock);
		uint64_t id = autoplay(e, TRACK);
		teder_request_info r = terminal(e, id);
		ck_assert(r.outcome == (n ? TEDER_NO_MEMORY : TEDER_OK));
		if (!n) {
			ended(&f, e, 16);
			ma_mutex_lock(&f.tracker.lock);
			count = f.tracker.attempts - before;
			ma_mutex_unlock(&f.tracker.lock);
			ck_assert(count > 5);
		}
		close_engine(&e);
		dispose(&f);
	}
	printf("autoplay allocation failures 1..%zu unwind\n", count);
}
END_TEST

static uint64_t transport(teder_engine *e, uint32_t action)
{
	teder_target target = status(e).current;
	uint64_t id;
	teder_code code;
	RETRY(code, teder_transport(e, &target, action, &id));
	OK(code);
	return id;
}

static uint64_t gain_to(teder_engine *e, float gain, uint32_t mute)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_set_gain(e, gain, mute, &id));
	OK(code);
	return id;
}

static void outcome(teder_engine *e, uint64_t id, teder_code expected)
{
	teder_request_info r = terminal(e, id);
	ck_assert_msg(r.outcome == expected,
	              "request %u operation %u: outcome %d, expected %d",
	              (unsigned)id,
	              r.operation,
	              r.outcome,
	              expected);
	ack(e, id);
}

static void step(fixture *f, unsigned frames)
{
	unsigned before = atomic_load(&f->stepped);
	atomic_store(&f->step, frames);
	while (atomic_load(&f->stepped) == before)
		th_yield();
}

static void boundary(fixture *f, teder_engine *e, uint64_t id)
{
	th_gate_await(&f->gate);
	teder_request_info r = INIT(teder_request_info);
	OK(teder_request_get(e, id, &r));
	ck_assert(r.state == TEDER_RUNNING);
	step(f, 1);
	th_gate_release(&f->gate);
	outcome(e, id, TEDER_OK);
}

START_TEST(transport_pcm)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	config.pcm_frames = 4;
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
	teder_target target = status(e).current;
	outcome(e, gain_to(e, 0.5f, 0), TEDER_OK);
	ma_mutex_lock(&f.tracker.lock);
	size_t allocations = f.tracker.attempts;
	f.tracker.fail_at = allocations + 1; /* transport must introduce no heap site */
	ma_mutex_unlock(&f.tracker.lock);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	ck_assert(status(e).transport == TEDER_PLAYING);
	step(&f, 2);
	for (unsigned i = 0; i < 2; i++)
		ck_assert(f.capture[2 * i] == ((int)i - 8) * 500 / 32768.0f);

	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PAUSE));
	ck_assert(status(e).transport == TEDER_PAUSED && atomic_load(&f.captured) == 2);
	outcome(e, transport(e, TEDER_PAUSE), TEDER_OK);
	step(&f, 31);
	ck_assert(atomic_load(&f.captured) == 2); /* pause preserves the partial block */
	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PLAY));
	ck_assert(f.capture[4] == -6000 * 0.5f / 32768.0f);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);

	atomic_store(&f.point, 13);
	boundary(&f, e, gain_to(e, 0.25f, 1));
	ck_assert(f.capture[6] == 0 && f.capture[7] == 0 && status(e).mute == 1);
	atomic_store(&f.point, 13);
	boundary(&f, e, gain_to(e, 0.25f, 0));
	/* Acknowledgement may carry silence while the next block is still filling.
	 * Observe the next captured frame, not an assumed producer scheduling turn. */
	while (atomic_load(&f.captured) < 5)
		step(&f, 1);
	ck_assert(atomic_load(&f.captured) == 5);
	ck_assert_msg(f.capture[8] == -4000 * 0.25f / 32768.0f && f.capture[9] == -f.capture[8],
	              "unmute captured=%u L=%g R=%g state=%u",
	              atomic_load(&f.captured),
	              (double)f.capture[8],
	              (double)f.capture[9],
	              status(e).transport);
	ck_assert(status(e).current.epoch == target.epoch && status(e).current.source == target.source);
	ck_assert(f.starts == 1); /* pause and gain never restart the native stream */
	ma_mutex_lock(&f.tracker.lock);
	ck_assert(f.tracker.attempts == allocations);
	f.tracker.fail_at = 0;
	ma_mutex_unlock(&f.tracker.lock);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(transport_states)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, transport(e, TEDER_PLAY), TEDER_BAD_STATE);
	outcome(e, transport(e, TEDER_PAUSE), TEDER_BAD_STATE);
	uint64_t id = 99;
	ck_assert(teder_set_gain(e, NAN, 0, &id) == TEDER_INVALID && !id);
	ck_assert(teder_set_gain(e, INFINITY, 0, &id) == TEDER_INVALID);
	ck_assert(teder_set_gain(e, -1, 0, &id) == TEDER_INVALID);
	ck_assert(teder_set_gain(e, 0.5f, 2, &id) == TEDER_INVALID);
	outcome(e, gain_to(e, 0, 1), TEDER_OK);
	ck_assert(status(e).gain == 0 && status(e).mute == 1);
	load_track(e);
	outcome(e, transport(e, TEDER_PLAY), TEDER_BAD_STATE);
	outcome(e, transport(e, TEDER_PAUSE), TEDER_OK);
	ck_assert(status(e).transport == TEDER_READY);
	/* Controls remain admissible while a separate media preparation owns its slot. */
	atomic_store(&f.point, 10);
	uint64_t opening;
	teder_code code;
	RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 77, 0, &opening));
	OK(code);
	th_gate_await(&f.gate);
	outcome(e, gain_to(e, 1, 0), TEDER_OK);
	outcome(e, transport(e, TEDER_PAUSE), TEDER_OK);
	ck_assert(status(e).media_request == opening);
	teder_target busy_target = status(e).current;
	busy_target.epoch++; /* an existing media mutation keeps its BUSY precedence */
	for (unsigned i = 0; i < 16; i++)
		ck_assert(teder_seek(e, &busy_target, 0, &id) == TEDER_BUSY && !id);
	th_gate_release(&f.gate);
	outcome(e, opening, TEDER_OK);
	teder_target stale = status(e).current;
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	ck_assert(teder_transport(e, &stale, TEDER_PLAY, &id) == TEDER_STALE && !id);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	ck_assert(status(e).transport == TEDER_DRAINING);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PAUSE));
	ck_assert(status(e).transport == TEDER_PAUSED);
	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PLAY));
	step(&f, 31);
	ended(&f, e, 16);
	outcome(e, transport(e, TEDER_PLAY), TEDER_BAD_STATE);
	outcome(e, transport(e, TEDER_PAUSE), TEDER_BAD_STATE);
	td_lock(e);
	e->status.transport = TEDER_FAILED; /* isolate rejection of this terminal state */
	td_unlock(e);
	outcome(e, transport(e, TEDER_PLAY), TEDER_BAD_STATE);
	outcome(e, transport(e, TEDER_PAUSE), TEDER_BAD_STATE);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(transport_boundaries)
{
	for (unsigned mode = 0; mode < 3; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		atomic_store(&f.point, mode == 0 ? 12 : 13);
		uint64_t id = gain_to(e, 0.125f, 1);
		uint64_t queued = 0;
		th_gate_await(&f.gate);
		ck_assert(status(e).gain == 1 && !status(e).mute);
		if (mode == 2) {
			OK(teder_request_close(e));
			ck_assert(teder_destroy(&e) == TEDER_BUSY && e);
		} else {
			OK(teder_request_cancel(e, id));
			if (mode == 1) {
				queued = gain_to(e, 0.5f, 0);
				atomic_store(&f.point, 13);
				step(&f, 1); /* publication committed: actual application wins */
			}
		}
		th_gate_release(&f.gate);
		outcome(e, id, mode == 1 ? TEDER_OK : TEDER_CANCELLED);
		ck_assert(status(e).gain == (mode == 1 ? 0.125f : 1));
		if (queued) {
			boundary(&f, e, queued);
			ck_assert(status(e).gain == 0.5f && !status(e).mute);
			ck_assert(f.capture[0] == 0 && f.capture[2] == -7000 * 0.5f / 32768.0f);
		}
		close_engine(&e);
		dispose(&f);
	}
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
	f.fail_start = true;
	outcome(e, transport(e, TEDER_PLAY), TEDER_DEVICE_LOST);
	ck_assert(status(e).transport == TEDER_PAUSED && status(e).output_state == TEDER_OUTPUT_LOST);
	close_engine(&e);
	dispose(&f);
}
END_TEST

static uint64_t seek_frame(teder_engine *e, uint64_t frame)
{
	teder_target target = status(e).current;
	uint64_t id;
	teder_code code;
	RETRY(code, teder_seek(e, &target, frame, &id));
	OK(code);
	return id;
}

static void capture_track(fixture *f, teder_engine *e, unsigned first, unsigned count)
{
	while (atomic_load(&f->captured) < count)
		step(f, 7);
	ended(f, e, count);
	for (unsigned i = 0; i < count; i++) {
		ck_assert(f->capture[2 * i] == ((int)(first + i) - 8) * 1000 / 32768.0f);
		ck_assert(f->capture[2 * i + 1] == -f->capture[2 * i]);
	}
}

START_TEST(stop_rewind)
{
	for (unsigned mode = 0; mode < 5; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		config.pcm_frames = mode == 3 ? 32 : 4;
		teder_engine *e = create(&f, config, false);
		teder_target empty = status(e).current;
		outcome(e, transport(e, TEDER_STOP), TEDER_OK);
		ck_assert(status(e).transport == TEDER_EMPTY && status(e).current.epoch == empty.epoch);
		load_track(e);
		if (mode) {
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			step(&f, 2);
		}
		if (mode == 2) {
			atomic_store(&f.point, 13);
			boundary(&f, e, transport(e, TEDER_PAUSE));
		}
		if (mode == 4)
			capture_track(&f, e, 0, 16);
		const uint32_t states[] = {
		    TEDER_READY, TEDER_PLAYING, TEDER_PAUSED, TEDER_DRAINING, TEDER_ENDED};
		ck_assert(status(e).transport == states[mode]);
		teder_target before = status(e).current;
		uint64_t id = transport(e, TEDER_STOP);
		teder_request_info r = terminal(e, id);
		ck_assert(r.outcome == TEDER_OK && r.operation == TEDER_OP_TRANSPORT && r.track == 77 &&
		          r.committed.epoch == before.epoch + 1 && r.committed.source == before.source);
		ack(e, id);
		teder_status s = status(e);
		ck_assert(s.transport == TEDER_READY && s.source_frame == 0 && s.output_frame == 0 &&
		          !s.media_request && !s.next_source && s.next_state == TEDER_NEXT_NONE);
		ck_assert(!f.worker && s.gain == 1 && !s.mute);
		teder_code code;
		RETRY(code, teder_transport(e, &before, TEDER_PLAY, &id));
		ck_assert(code == TEDER_STALE && !id);
		outcome(e, transport(e, TEDER_STOP), TEDER_OK);
		ck_assert(status(e).current.epoch == before.epoch + 2);
		if (!mode)
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		capture_track(&f, e, 0, 16); /* no old partial block survives the rewind */
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(seek_states)
{
	for (unsigned mode = 0; mode < 5; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		config.pcm_frames = mode == 3 ? 32 : 4;
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		if (mode) {
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			step(&f, 2);
		}
		if (mode == 2) {
			atomic_store(&f.point, 13);
			boundary(&f, e, transport(e, TEDER_PAUSE));
		}
		if (mode == 4)
			capture_track(&f, e, 0, 16);
		teder_status before = status(e);
		outcome(e, seek_frame(e, 17), TEDER_INVALID);
		ck_assert(status(e).current.epoch == before.current.epoch &&
		          status(e).transport == before.transport);
		outcome(e, seek_frame(e, 7), TEDER_OK);
		teder_status s = status(e);
		ck_assert(s.current.epoch == before.current.epoch + 1 &&
		          s.current.source == before.current.source && s.track == 77 &&
		          s.source_frame == 7);
		if (mode == 1 || mode == 3)
			ck_assert(s.transport == TEDER_PLAYING || s.transport == TEDER_DRAINING);
		else {
			ck_assert(s.transport == (mode == 2 ? TEDER_PAUSED : TEDER_READY));
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		}
		capture_track(&f, e, 7, 9);
		outcome(e, seek_frame(e, 0), TEDER_OK);
		ck_assert(status(e).transport == TEDER_READY);
		if (mode) {
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			if (mode == 2) {
				atomic_store(&f.point, 13);
				boundary(&f, e, transport(e, TEDER_PAUSE));
			}
		}
		outcome(e, seek_frame(e, 16), TEDER_OK);
		s = status(e);
		ck_assert(s.transport == TEDER_ENDED && s.source_frame == 16 &&
		          s.clock_flags == (TEDER_DURATION_KNOWN | TEDER_DECODE_EOF | TEDER_SUBMIT_EOF));
		ck_assert(!f.worker);
		close_engine(&e);
		dispose(&f);
	}
	const char *empty_path = OUTPUT_DIR "/output-empty.flac";
	th_make_track(empty_path, 0);
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	outcome(e, autoplay(e, empty_path), TEDER_OK);
	teder_status s = status(e);
	ck_assert(s.transport == TEDER_ENDED && s.duration_frames == 0 &&
	          s.clock_flags == (TEDER_DURATION_KNOWN | TEDER_DECODE_EOF | TEDER_SUBMIT_EOF));
	outcome(e, transport(e, TEDER_STOP), TEDER_OK);
	ck_assert(status(e).transport == TEDER_READY);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	ck_assert(status(e).transport == TEDER_ENDED && f.starts == 0);
	outcome(e, seek_frame(e, 0), TEDER_OK);
	ck_assert(status(e).transport == TEDER_ENDED);
	outcome(e, seek_frame(e, 1), TEDER_INVALID);
	close_engine(&e);
	dispose(&f);
	ck_assert(remove(empty_path) == 0);
}
END_TEST

START_TEST(stop_supersession)
{
	for (unsigned mode = 0; mode < 4; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		config.pcm_frames = 4;
		teder_engine *e = create(&f, config, false);
		if (mode) {
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
			load_track(e);
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			step(&f, 2);
		}
		teder_target before = status(e).current;
		atomic_store(&f.point, 10);
		uint64_t pending;
		teder_code code;
		if (mode == 2)
			pending = seek_frame(e, 7);
		else {
			RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 99, 0, &pending));
			OK(code);
		}
		th_gate_await(&f.gate);
		uint64_t stop = transport(e, TEDER_STOP);
		while (status(e).media_request != stop || (mode && status(e).current.epoch == before.epoch))
			th_yield();
		teder_request_info r = INIT(teder_request_info);
		do {
			code = teder_request_get(e, pending, &r);
		} while (code == TEDER_AGAIN);
		ck_assert(code == TEDER_OK && r.state == TEDER_RUNNING);
		uint64_t rejected = 99;
		ck_assert(teder_open(e, TRACK, sizeof(TRACK) - 1, 0, 0, &rejected) == TEDER_BUSY &&
		          !rejected);
		RETRY(code, teder_request_cancel(e, stop));
		OK(code); /* already committed: cancellation cannot undo the rewind */
		if (mode == 3) {
			OK(teder_request_close(e));
			ck_assert(teder_destroy(&e) == TEDER_BUSY && e);
		}
		th_gate_release(&f.gate);
		outcome(e, pending, TEDER_CANCELLED);
		r = terminal(e, stop);
		ck_assert(r.outcome == (mode == 3 ? TEDER_CANCELLED : TEDER_OK) &&
		          r.committed.source == before.source &&
		          r.committed.epoch == before.epoch + (mode != 0));
		ack(e, stop);
		if (mode && mode != 3) {
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			capture_track(&f, e, 0, 16);
		}
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(stop_boundaries)
{
	for (unsigned mode = 0; mode < 5; mode++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		load_track(e);
		teder_target before = status(e).current;
		atomic_store(&f.point, mode == 2 ? 14 : mode == 3 ? 10 : mode == 4 ? 9 : 12);
		uint64_t stop, pending = 0;
		teder_code code;
		if (mode == 2) {
			RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 99, 0, &pending));
			OK(code);
			th_gate_await(&f.gate);
			stop = transport(e, TEDER_STOP);
		} else {
			stop = transport(e, TEDER_STOP);
			th_gate_await(&f.gate);
			if (mode == 1) {
				RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 99, 0, &pending));
				OK(code); /* queued after stop, but outstanding when stop commits */
			} else if (mode == 4) {
				OK(teder_request_close(e));
				ck_assert(teder_destroy(&e) == TEDER_BUSY && e);
			} else {
				RETRY(code, teder_request_cancel(e, stop));
				OK(code);
			}
		}
		th_gate_release(&f.gate);
		outcome(e,
		        stop,
		        (mode == 0 || mode == 4) ? TEDER_CANCELLED
		        : mode == 2              ? TEDER_STALE
		                                 : TEDER_OK);
		if (pending)
			outcome(e, pending, mode == 2 ? TEDER_OK : TEDER_CANCELLED);
		teder_status s = status(e);
		ck_assert(s.current.epoch == before.epoch + (mode != 0) &&
		          s.current.source == before.source + (mode == 2) &&
		          s.track == (mode == 2 ? 99 : 77) && s.transport == TEDER_READY);
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(stop_failures)
{
	size_t count = 0;
	for (size_t n = 0; n <= count; n++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		load_track(e);
		idle(&f, e);
		size_t before = f.tracker.attempts;
		f.tracker.fail_at = n ? before + n : 0;
		teder_target target = status(e).current;
		uint64_t id = transport(e, TEDER_STOP);
		th_gate_release(&f.gate);
		outcome(e, id, n ? TEDER_NO_MEMORY : TEDER_OK);
		teder_status s = status(e);
		ck_assert(s.current.epoch == target.epoch + 1 && s.current.source == target.source &&
		          s.transport == (n ? TEDER_FAILED : TEDER_READY) &&
		          s.last_error == (n ? TEDER_NO_MEMORY : TEDER_OK));
		if (!n) {
			idle(&f, e);
			count = f.tracker.attempts - before;
			ck_assert(count > 5);
			th_gate_release(&f.gate);
		}
		ma_mutex_lock(&f.tracker.lock);
		f.tracker.fail_at = 0;
		ma_mutex_unlock(&f.tracker.lock);
		outcome(e, seek_frame(e, 7), TEDER_OK); /* recover from actual rewind failure */
		ck_assert(status(e).transport == TEDER_READY && status(e).source_frame == 7);
		close_engine(&e);
		dispose(&f);
	}
	printf("stop rewind allocation failures 1..%zu unwind\n", count);
	for (unsigned mode = 0; mode < 3; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		if (!mode) {
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			f.fail_stop = true;
		} else {
			idle(&f, e);
			if (mode == 1)
				e->config.engine_bytes_limit = e->owned_bytes;
			else
				e->status.current.epoch = UINT64_MAX;
		}
		teder_target before = status(e).current;
		uint64_t id = transport(e, TEDER_STOP);
		if (mode)
			th_gate_release(&f.gate);
		outcome(e, id, mode ? TEDER_LIMIT : TEDER_DEVICE_LOST);
		ck_assert(status(e).current.epoch == (mode == 2 ? UINT64_MAX : before.epoch + 1));
		if (mode != 2)
			ck_assert(status(e).transport == TEDER_FAILED);
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(seek_failure)
{
	unsigned char bytes[4096];
	FILE *file = fopen(TRACK, "rb");
	ck_assert(file);
	size_t size = fread(bytes, 1, sizeof(bytes), file);
	ck_assert(size > 4 && size < sizeof(bytes) && fclose(file) == 0);
	const char *path = OUTPUT_DIR "/output-seek-failure.flac";
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	for (unsigned pass = 0; pass < 3; pass++) {
		if (pass)
			bytes[size - 4] ^= 1; /* corrupt, then restore the frame CRC */
		file = fopen(path, "wb");
		ck_assert(file && fwrite(bytes, 1, size, file) == size && fclose(file) == 0);
		if (!pass) {
			uint64_t id;
			teder_code code;
			RETRY(code, teder_open(e, path, (uint32_t)strlen(path), 77, 0, &id));
			OK(code);
			outcome(e, id, TEDER_OK);
			idle(&f, e); /* active PCM is decoded before the file changes */
		} else if (pass == 1) {
			teder_target before = status(e).current;
			th_gate_release(&f.gate);
			outcome(e, seek_frame(e, 7), TEDER_CORRUPT);
			ck_assert(status(e).current.epoch == before.epoch &&
			          status(e).transport == TEDER_READY);
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			capture_track(&f, e, 0, 16); /* failed seek leaves original PCM intact */
			outcome(e, transport(e, TEDER_STOP), TEDER_CORRUPT);
			ck_assert(status(e).current.epoch == before.epoch + 1 &&
			          status(e).current.source == before.source &&
			          status(e).transport == TEDER_FAILED && status(e).last_error == TEDER_CORRUPT);
		} else {
			outcome(e, seek_frame(e, 7), TEDER_OK);
			ck_assert(status(e).transport == TEDER_READY);
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			capture_track(&f, e, 7, 9);
		}
	}
	close_engine(&e);
	dispose(&f);
	ck_assert(remove(path) == 0);
}
END_TEST

static teder_status clock_at(teder_engine *e, uint64_t output)
{
	teder_status s;
	do {
		s = status(e);
		ck_assert(s.output_frame <= output);
		th_yield();
	} while (s.output_frame != output);
	return s;
}

START_TEST(clock_progress)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	config.pcm_frames = 4;
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	atomic_store(&f.point, 15);
	th_gate_await(&f.gate); /* no producer can refill until released */
	step(&f, 7);
	step(&f, 5);
	ck_assert(atomic_load(&f.captured) == 4);
	th_gate_release(&f.gate);
	teder_status s = clock_at(e, 12);
	ck_assert(s.source_frame == 4 && s.underrun_frames == 8 && s.track == 77 &&
	          s.position_quality == TEDER_QUALITY_MEASURED);
	uint64_t missing = 0;
	teder_event event = INIT(teder_event);
	while (missing < 8) {
		teder_code code = teder_event_poll(e, &event);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && event.kind == TEDER_EVENT_UNDERRUN) {
			ck_assert(event.epoch == s.current.epoch && event.from_source == s.current.source &&
			          event.from_track == 77 && event.output_frame == 12);
			missing += event.missing_frames;
		}
	}
	ck_assert(missing == 8);
	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PAUSE));
	step(&f, 31);
	for (unsigned i = 0; i < 100; i++) {
		s = status(e);
		ck_assert(s.transport == TEDER_PAUSED && s.source_frame == 4 && s.output_frame == 12 &&
		          s.underrun_frames == 8 && s.latency_quality == TEDER_QUALITY_UNKNOWN &&
		          s.latency_frames == 0 && !(s.clock_flags & TEDER_AUDIBLE_END_ESTIMATED));
	}
	atomic_store(&f.point, 7);
	td_signal(e);
	th_gate_await(&f.gate); /* paused producer has finished filling the next bounded block */
	ck_assert(e->pcm.free_count == 0);
	th_gate_release(&f.gate);
	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PLAY));
	s = clock_at(e, 13);
	ck_assert(s.source_frame == 5 && s.underrun_frames == 8);
	atomic_store(&f.point, 13);
	boundary(&f, e, gain_to(e, 0, 1));
	s = clock_at(e, 14);
	ck_assert(s.source_frame == 6 && s.underrun_frames == 8); /* mute still consumes media */
	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PAUSE));
	outcome(e, seek_frame(e, 7), TEDER_OK);
	s = status(e);
	ck_assert(s.transport == TEDER_PAUSED && s.source_frame == 7 && !s.output_frame &&
	          !s.underrun_frames && s.position_quality == TEDER_QUALITY_MEASURED);
	outcome(e, transport(e, TEDER_STOP), TEDER_OK);
	ck_assert(status(e).source_frame == 0 && status(e).output_frame == 0);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(clock_changes)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	config.pcm_frames = 64;
	teder_engine *e = create(&f, config, false);
	uint64_t id = submit(e);
	OK(terminal(e, id).outcome);
	teder_result *list = take(e, id);
	teder_device_info b = entry(list, 1);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 2);
	ck_assert(clock_at(e, 2).source_frame == 2);
	teder_target before = status(e).current;
	atomic_store(&f.point, 11);
	id = select_device(e, b.key);
	teder_result_release(list);
	th_gate_await(&f.gate); /* refill is paused, native stop has not joined yet */
	step(&f, 3);
	ck_assert(status(e).source_frame == 2);
	th_gate_release(&f.gate);
	outcome(e, id, TEDER_OK);
	teder_status s = status(e);
	ck_assert(s.current.epoch == before.epoch + 1 && s.current.source == before.source &&
	          s.source_frame == 5 && !s.output_frame && s.transport == TEDER_PAUSED &&
	          s.output_rate == 22050 && s.position_quality == TEDER_QUALITY_MEASURED);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 1);
	s = clock_at(e, 1);
	ck_assert(s.source_frame == 6 && s.position_quality == TEDER_QUALITY_ESTIMATED);
	step(&f, 2);
	ck_assert(clock_at(e, 3).source_frame == 10);
	step(&f, 7);
	ended(&f, e, 6);
	s = status(e);
	ck_assert(s.source_frame == 16 && s.output_frame == 6 && !s.underrun_frames);
	step(&f, 31);
	ck_assert(status(e).output_frame == 6 && status(e).source_frame == 16);
	outcome(e, seek_frame(e, 7), TEDER_OK);
	ck_assert(status(e).source_frame == 7 && status(e).output_frame == 0);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 1);
	s = clock_at(e, 1);
	ck_assert(s.source_frame == 8);
	f.device_error = true;
	outcome(e, select_device(e, (teder_blob){0}), TEDER_DEVICE_LOST);
	s = status(e);
	ck_assert(s.source_frame == 8 && !s.output_frame && s.output_state == TEDER_OUTPUT_LOST &&
	          s.position_quality == TEDER_QUALITY_UNKNOWN &&
	          s.latency_quality == TEDER_QUALITY_UNKNOWN);
	close_engine(&e);
	dispose(&f);
}
END_TEST

typedef struct {
	teder_engine *engine;
	atomic_uint stop;
	unsigned reads;
} clock_reader;

static void read_clock(void *p)
{
	clock_reader *reader = p;
	teder_status before = {0};
	while (!atomic_load(&reader->stop)) {
		teder_status s = status(reader->engine);
		uint64_t source = s.current.source;
		if (source) {
			ck_assert(s.track == 1000 + source && s.current.epoch >= 3 * source &&
			          s.current.epoch <= 3 * source + 2 && s.duration_frames == 16 &&
			          s.source_rate == 44100 && s.source_frame <= 16);
			if (s.current.epoch == 3 * source + 1)
				ck_assert(s.source_frame >= 7);
			if (s.current.epoch == 3 * source + 2)
				ck_assert(!s.source_frame && !s.output_frame && !s.underrun_frames);
		}
		if (before.current.epoch == s.current.epoch)
			ck_assert(s.source_frame >= before.source_frame &&
			          s.output_frame >= before.output_frame);
		before = s;
		reader->reads++;
	}
}

START_TEST(clock_readers)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	clock_reader readers[2] = {{.engine = e}, {.engine = e}};
	th_thread threads[2];
	for (unsigned i = 0; i < 2; i++) {
		atomic_init(&readers[i].stop, 0);
		ck_assert(th_thread_start(&threads[i], read_clock, &readers[i]));
	}
	for (unsigned i = 1; i <= 32; i++) {
		uint64_t id;
		teder_code code;
		RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 1000 + i, TEDER_OPEN_AUTOPLAY, &id));
		OK(code);
		outcome(e, id, TEDER_OK);
		ended(&f, e, 16);
		outcome(e, seek_frame(e, 7), TEDER_OK);
		outcome(e, transport(e, TEDER_STOP), TEDER_OK);
	}
	OK(th_close(e));
	for (unsigned i = 0; i < 2; i++) {
		atomic_store(&readers[i].stop, 1);
		th_thread_join(&threads[i]);
		ck_assert(readers[i].reads > 0);
	}
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(native_select)
{
	size_t attempts = 0;
	for (size_t fail = 0; fail <= attempts; fail++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, true);
		uint64_t id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_OK);
		teder_result *r = take(e, id);
		teder_blob key = {0};
#ifndef _WIN32
		teder_device_info d = entry(r, 0);
		key = d.key;
#endif
		size_t before = f.tracker.attempts;
		if (fail)
			f.tracker.fail_at = before + fail;
		id = select_device(e, key);
		ck_assert(terminal(e, id).outcome == (fail ? TEDER_NO_MEMORY : TEDER_OK));
		teder_status s = status(e);
		ck_assert(s.current.epoch == 2 && s.current.source == 0 && s.transport == TEDER_EMPTY);
		if (!fail) {
			attempts = f.tracker.attempts - before;
			ck_assert(s.output_state == TEDER_OUTPUT_READY && s.output_channels == 2 &&
			          s.output_rate);
			printf("Selected native backend=%u rate=%u channels=%u period=%u allocations=%zu\n",
			       s.backend,
			       s.output_rate,
			       s.output_channels,
			       s.output_period_frames,
			       attempts);
		} else
			ck_assert(!e->device && s.output_state == TEDER_OUTPUT_LOST && s.output_rate == 0);
		teder_result_release(r);
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(next_states)
{
	for (unsigned mode = 0; mode < 5; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		config.pcm_frames = 32;
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		teder_status s = status(e);
		uint64_t id;
		teder_code code;
		RETRY(code, teder_prepare_next(e, &s.current, 0, TRACK, sizeof(TRACK) - 1, 88, &id));
		OK(code);
		outcome(e, id, TEDER_OK);
		s = status(e);
		ck_assert(s.next_source == 2 && s.next_track == 88 && s.next_state == TEDER_NEXT_READY);
		if (mode < 2) {
			atomic_store(&f.point, 10);
			RETRY(code, teder_prepare_next(e, &s.current, 2, TRACK, sizeof(TRACK) - 1, 99, &id));
			OK(code);
			th_gate_await(&f.gate);
			if (mode == 0) {
				step(&f, 16);
				while (status(e).transport != TEDER_ENDED)
					th_yield();
				th_gate_release(&f.gate);
				outcome(e, id, TEDER_TOO_LATE);
			} else {
				uint64_t stop = transport(e, TEDER_STOP);
				while (status(e).current.epoch == s.current.epoch)
					th_yield();
				th_gate_release(&f.gate);
				outcome(e, id, TEDER_CANCELLED);
				outcome(e, stop, TEDER_OK);
			}
		} else if (mode == 2) {
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
			ck_assert(status(e).current.epoch == s.current.epoch + 1 &&
			          status(e).transport == TEDER_PAUSED);
		} else if (mode == 3) {
			td_device_feed *feed = f.native->pUserData;
			/* Manual output is idle: inject a stable callback error report. */
			atomic_store(&feed->error, TEDER_IO);
			td_signal(e);
			while (status(e).transport != TEDER_FAILED || status(e).next_source)
				th_yield();
		} else {
			step(&f, 16);
			while (status(e).transport != TEDER_ENDED || status(e).next_source)
				th_yield();
		}
		s = status(e);
		ck_assert(s.next_state == TEDER_NEXT_NONE && !s.next_source && !s.next_track);
		/* No new submission/wake may be needed to release discarded sources. */
		for (;;) {
			td_lock(e);
			unsigned free = 0;
			for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
				free += e->sources[i].state == TD_JOB_FREE;
			td_unlock(e);
			if (free == TD_SOURCE_JOBS - 1)
				break;
			th_yield();
		}
		if (mode == 0 || mode >= 3) {
			RETRY(code, teder_prepare_next(e, &s.current, 0, TRACK, sizeof(TRACK) - 1, 88, &id));
			ck_assert(code == TEDER_BAD_STATE && !id);
		}
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "snapshot"))
		test = snapshot;
	else if (!strcmp(argv[1], "cancellation"))
		test = cancellation;
	else if (!strcmp(argv[1], "failures"))
		test = failures;
	else if (!strcmp(argv[1], "identity"))
		test = identity;
	else if (!strcmp(argv[1], "closing"))
		test = closing;
	else if (!strcmp(argv[1], "selection"))
		test = selection;
	else if (!strcmp(argv[1], "rates"))
		test = rates;
	else if (!strcmp(argv[1], "select-failures"))
		test = select_failures;
	else if (!strcmp(argv[1], "select-cancel"))
		test = select_cancel;
	else if (!strcmp(argv[1], "playback"))
		test = playback;
	else if (!strcmp(argv[1], "play-changes"))
		test = play_changes;
	else if (!strcmp(argv[1], "play-failures"))
		test = play_failures;
	else if (!strcmp(argv[1], "transport-pcm"))
		test = transport_pcm;
	else if (!strcmp(argv[1], "transport-states"))
		test = transport_states;
	else if (!strcmp(argv[1], "transport-boundaries"))
		test = transport_boundaries;
	else if (!strcmp(argv[1], "stop-rewind"))
		test = stop_rewind;
	else if (!strcmp(argv[1], "seek-states"))
		test = seek_states;
	else if (!strcmp(argv[1], "stop-supersession"))
		test = stop_supersession;
	else if (!strcmp(argv[1], "stop-boundaries"))
		test = stop_boundaries;
	else if (!strcmp(argv[1], "stop-failures"))
		test = stop_failures;
	else if (!strcmp(argv[1], "seek-failure"))
		test = seek_failure;
	else if (!strcmp(argv[1], "clock-progress"))
		test = clock_progress;
	else if (!strcmp(argv[1], "clock-changes"))
		test = clock_changes;
	else if (!strcmp(argv[1], "clock-readers"))
		test = clock_readers;
	else if (!strcmp(argv[1], "next-states"))
		test = next_states;
	else if (!strcmp(argv[1], "native-select"))
		test = native_select;
	else if (!strcmp(argv[1], "native"))
		test = native;
	else
		return 2;
	Suite *suite = suite_create("output");
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
