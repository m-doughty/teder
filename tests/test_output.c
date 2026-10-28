#include "support/harness.h"
#include "support/engine.h"
#include "analysis_pool.h"
#include <check.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "source.h"
#include "support/track.h"
#include "support/mixed.h"
#define LONG_TRACK OUTPUT_DIR "/output-long.flac"
#define AUDIT_TRACK OUTPUT_DIR "/output-audit.flac"
enum { LONG_FRAMES = 32769 };
#define TRACK FIXTURE_DIR "/track-a.flac"

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert((call) == TEDER_OK)
/* Check logs successful assertions under a lazy winpthreads mutex. Keep worker
 * success paths free of that unrelated handle; failed checks terminate the child. */
#define WORKER_CHECK(expr) \
	do { \
		if (!(expr)) { \
			fprintf(stderr, "%s:%d: worker check failed: %s\n", __FILE__, __LINE__, #expr); \
			fflush(stderr); \
			_Exit(42); \
		} \
	} while (0)

#define RETRY(code, call) \
	do { \
		(code) = (call); \
		if ((code) == TEDER_BUSY) \
			th_yield(); \
	} while ((code) == TEDER_BUSY)

typedef struct {
	th_tracker tracker;
	th_gate gate, render_gate, owner_gate;
	th_gate prepare_gate, probe_gate;
	th_thread thread;
	ma_device *native;
	atomic_uint stop, hold, captured, step, stepped;
	bool manual;
	bool worker, fail_start, fail_stop;
	float capture[LONG_FRAMES * 2];
	atomic_uint point;
	atomic_uint park_owner;
	atomic_uint hold_prepare, hold_probe, worker_entries[5], worker_exits[5];
	atomic_uint source_opened, source_closed, probe_opened, probe_closed;
	unsigned fail_stage;
	unsigned opened, closed, enumerated;
	bool reject, enum_error, device_error;
	unsigned devices, releases, starts, stops, rate;
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
	WORKER_CHECK(GetCurrentThreadId() == f->owner && f->owner != f->caller);
#else
	WORKER_CHECK(pthread_equal(pthread_self(), f->owner) && !pthread_equal(f->owner, f->caller));
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
	WORKER_CHECK(config->playback.pDeviceID);
	ma_wchar_win32 id = config->playback.pDeviceID->wasapi[0];
	WORKER_CHECK(id == 'A' || id == 'B');
	playback->format = ma_format_f32;
	playback->channels = 2;
	playback->sampleRate = f->rate ? f->rate : id == 'A' ? 44100 : 22050;
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
		WORKER_CHECK(frames <= 257);
		pcm[2 * frames] = 12345;
		uint64_t before = feed->report.pcm_frames;
		f->native->onData(f->native, pcm, NULL, frames);
		unsigned count = (unsigned)(feed->report.pcm_frames - before);
		WORKER_CHECK(count <= frames && captured + count <= LONG_FRAMES &&
		             pcm[2 * frames] == 12345);
		memcpy(f->capture + captured * 2, pcm, count * 2 * sizeof(float));
		captured += count;
		for (unsigned i = count * 2; i < frames * 2; i++)
			WORKER_CHECK(pcm[i] == 0);
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
	f->stops++;
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
	WORKER_CHECK(count == 4 || count == 1);
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
	fixture *f = p;
	const unsigned starts[] = {TD_PREPARE_STARTED, TD_REFILL_STARTED, TD_PROBE_STARTED};
	const unsigned stops[] = {TD_PREPARE_STOPPED, TD_REFILL_STOPPED, TD_PROBE_STOPPED};
	for (unsigned i = 0; i < 3; i++) {
		if (point == starts[i])
			atomic_fetch_add(&f->worker_entries[i], 1);
		if (point == stops[i])
			atomic_fetch_add(&f->worker_exits[i], 1);
	}
	if (point == TD_ANALYSIS_STARTED)
		atomic_fetch_add(&f->worker_entries[4], 1);
	if (point == TD_ANALYSIS_STOPPED)
		atomic_fetch_add(&f->worker_exits[4], 1);
	if (point == TD_SOURCE_OPENED) {
		atomic_fetch_add(&f->source_opened, 1);
		if (atomic_exchange(&f->hold_prepare, 0))
			th_gate_hit(&f->prepare_gate);
	}
	if (point == TD_SOURCE_CLOSED)
		atomic_fetch_add(&f->source_closed, 1);
	if (point == TD_PROBE_OPENED)
		atomic_fetch_add(&f->probe_opened, 1);
	if (point == TD_PROBE_CLOSED)
		atomic_fetch_add(&f->probe_closed, 1);
	if (point == TD_PROBE_READ && atomic_exchange(&f->hold_probe, 0))
		th_gate_hit(&f->probe_gate);
	if (point == TD_DECODE_IDLE && atomic_exchange(&f->park_owner, 0))
		th_gate_hit(&f->owner_gate);
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
	if (point == TD_MEDIA_AFTER_COMMIT)
		hit(p, 18);
	if (point == TD_REFILL_PUMP)
		hit(p, 15);
	if (point == TD_NEXT_BEFORE_PUBLISH)
		hit(p, 16);
	if (point == TD_NEXT_AFTER_PUBLISH)
		hit(p, 17);
	if (point == TD_ANALYSIS_BEFORE_PUBLISH)
		hit(p, 19);
	if (point == TD_PREPARATION_WAIT)
		hit(p, 20);
}

static void control_checkpoint(void *p, uint32_t point)
{
	fixture *f = p;
	if (point == TD_STARTED)
		atomic_fetch_add(&f->worker_entries[3], 1);
	if (point == TD_STOPPED)
		atomic_fetch_add(&f->worker_exits[3], 1);
}

static bool permit_start(void *p, uint32_t stage)
{
	return ((fixture *)p)->fail_stage != stage;
}

static void init(fixture *f)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->point, 0);
	atomic_init(&f->park_owner, 0);
	atomic_init(&f->stop, 0);
	atomic_init(&f->hold, 0);
	atomic_init(&f->captured, 0);
	atomic_init(&f->step, 0);
	atomic_init(&f->stepped, 0);
	atomic_init(&f->hold_prepare, 0);
	atomic_init(&f->hold_probe, 0);
	atomic_init(&f->source_opened, 0);
	atomic_init(&f->source_closed, 0);
	atomic_init(&f->probe_opened, 0);
	atomic_init(&f->probe_closed, 0);
	for (unsigned i = 0; i < 5; i++) {
		atomic_init(&f->worker_entries[i], 0);
		atomic_init(&f->worker_exits[i], 0);
	}
	ck_assert(th_gate_init(&f->render_gate));
	ck_assert(th_gate_init(&f->owner_gate));
	ck_assert(th_gate_init(&f->prepare_gate) && th_gate_init(&f->probe_gate));
	ck_assert(th_tracker_init(&f->tracker) && th_gate_init(&f->gate));
#ifdef _WIN32
	f->caller = GetCurrentThreadId();
#else
	f->caller = pthread_self();
#endif
}

static void dispose(fixture *f)
{
	for (unsigned i = 0; i < 5; i++)
		ck_assert(atomic_load(&f->worker_entries[i]) == atomic_load(&f->worker_exits[i]));
	ck_assert(atomic_load(&f->source_opened) == atomic_load(&f->source_closed) &&
	          atomic_load(&f->probe_opened) == atomic_load(&f->probe_closed));
	ck_assert(f->opened == f->closed && f->devices == f->releases && !f->worker &&
	          !f->native_heap && th_tracker_clean(&f->tracker));
	th_gate_uninit(&f->gate);
	th_gate_uninit(&f->render_gate);
	th_gate_uninit(&f->owner_gate);
	th_gate_uninit(&f->prepare_gate);
	th_gate_uninit(&f->probe_gate);
	th_tracker_uninit(&f->tracker);
}

static teder_engine *create(fixture *f, teder_config config, bool native)
{
	td_runtime runtime = {.context = f,
	                      .permit = permit_start,
	                      .checkpoint = control_checkpoint,
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

/* Selection and stop both reopen the retained source after advancing epoch. Later
 * controls stay queued until that reopen commits, so PLAY with the new target never
 * starts output without a source, and the new epoch's clock never runs backwards. */
START_TEST(reopen_queue)
{
	for (unsigned stop = 0; stop < 2; stop++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		teder_target before = status(e).current;
		unsigned starts = f.starts;

		atomic_store(&f.hold_prepare, 1);
		uint64_t id = stop ? transport(e, TEDER_STOP) : select_device(e, (teder_blob){0});
		th_gate_await(&f.prepare_gate); /* reopen held after the epoch advanced */
		teder_status s = status(e);
		ck_assert(s.current.epoch == before.epoch + 1 && s.current.source == before.source &&
		          s.media_request == id && s.output_state == TEDER_OUTPUT_READY &&
		          s.transport == (stop ? TEDER_READY : TEDER_PAUSED) && !s.output_frame &&
		          !s.underrun_frames);

		/* Park control where it waits: stop inside its preparation wait, selection at
		 * the idle point. Admit a valid PLAY for the new target, then let control make
		 * one full pass over the queue and park again. PLAY must still be queued. */
		unsigned park = stop ? 20 : 7;
		atomic_store(&f.point, park);
		td_signal(e);
		th_gate_await(&f.gate);
		uint64_t play = transport(e, TEDER_PLAY);
		atomic_store(&f.point, park);
		th_gate_release(&f.gate);
		th_gate_await(&f.gate);
		teder_request_info r = INIT(teder_request_info);
		teder_code code;
		do {
			code = teder_request_get(e, play, &r);
		} while (code == TEDER_AGAIN);
		ck_assert_msg(code == TEDER_OK && r.state == TEDER_QUEUED,
		              "%s: PLAY state %u outcome %d while the reopen was held",
		              stop ? "stop" : "selection",
		              r.state,
		              r.outcome);
		ck_assert(f.starts == starts && !f.worker && status(e).transport != TEDER_PLAYING);

		atomic_store(&f.point, 12);
		th_gate_release(&f.prepare_gate);
		th_gate_release(&f.gate);
		th_gate_await(&f.gate); /* PLAY reached its commit point */
		do {
			code = teder_request_get(e, id, &r);
		} while (code == TEDER_AGAIN);
		ck_assert(code == TEDER_OK && r.state == TEDER_TERMINAL && r.outcome == TEDER_OK &&
		          r.committed.epoch == before.epoch + 1 && r.committed.source == before.source);
		s = status(e);
		ck_assert(s.transport == (stop ? TEDER_READY : TEDER_PAUSED) && !s.media_request &&
		          !s.output_frame && !s.underrun_frames && f.starts == starts && !f.worker);
		th_gate_release(&f.gate);

		outcome(e, play, TEDER_OK);
		ack(e, id);
		s = status(e);
		/* The 16-frame fixture decodes to EOF during prefill, so PLAY may drain. */
		ck_assert_msg((s.transport == TEDER_PLAYING || s.transport == TEDER_DRAINING) &&
		                  s.current.epoch == before.epoch + 1 && f.starts == starts + 1,
		              "%s: transport %u epoch %" PRIu64 " (expected %" PRIu64 ") starts %u/%u",
		              stop ? "stop" : "selection",
		              s.transport,
		              s.current.epoch,
		              before.epoch + 1,
		              f.starts,
		              starts + 1);
		capture_track(&f, e, 0, 16); /* the reopened source plays from its saved frame */
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
		idle(&f, e); /* include deferred empty-marker publication before checking EOF */
		s = status(e);
		ck_assert(s.transport == TEDER_ENDED && s.source_frame == 16 &&
		          s.clock_flags == (TEDER_DURATION_KNOWN | TEDER_DECODE_EOF | TEDER_SUBMIT_EOF));
		ck_assert(!f.worker);
		th_gate_release(&f.gate);
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
			ck_assert(status(e).transport == (mode ? TEDER_FAILED : TEDER_PAUSED));
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

#define GAP_A OUTPUT_DIR "/gapless-a.flac"
#define GAP_B OUTPUT_DIR "/gapless-b.flac"

static void gap_load(teder_engine *e, const char *path)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_open(e, path, (uint32_t)strlen(path), 77, 0, &id));
	OK(code);
	outcome(e, id, TEDER_OK);
}

static void gap_prepare(teder_engine *e, const char *path)
{
	teder_status s = status(e);
	uint64_t id;
	teder_code code;
	RETRY(code,
	      teder_prepare_next(e, &s.current, s.next_source, path, (uint32_t)strlen(path), 77, &id));
	OK(code);
	outcome(e, id, TEDER_OK);
}

static void gap_event(teder_engine *e, uint64_t from, uint64_t to, uint64_t frame)
{
	teder_event event = INIT(teder_event);
	for (;;) {
		teder_code code = teder_event_poll(e, &event);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && event.kind == TEDER_EVENT_TRANSITION)
			break;
		th_yield();
	}
	ck_assert(event.epoch == 3 && event.from_source == from && event.to_source == to &&
	          event.from_track == 77 && event.to_track == 77 && event.output_frame == frame &&
	          event.code == TEDER_OK);
}

static void gap_samples(fixture *f, unsigned offset, unsigned frames)
{
	for (unsigned i = 0; i < frames; i++) {
		float expected = i == 0 || i + 1 == frames ? 0 : ((int32_t)(i % 2048) - 1024) / 32768.0f;
		ck_assert(f->capture[2 * (offset + i)] == expected &&
		          f->capture[2 * (offset + i) + 1] == -expected);
	}
}

START_TEST(gapless_pairs)
{
	const unsigned lengths[][2] = {{16, 16}, {1, 1}, {0, 1}, {1, 0}, {0, 0}, {1031, 513}};
	const unsigned chunks[] = {1, 7, 31, 257};
	for (unsigned pair = 0; pair < 6; pair++)
		for (unsigned chunk = 0; chunk < 4; chunk++) {
			unsigned a = lengths[pair][0], b = lengths[pair][1];
			th_make_track_samples(GAP_A, a, true);
			th_make_track_samples(GAP_B, b, true);
			fixture f;
			init(&f);
			f.manual = true;
			teder_config config = INIT(teder_config);
			config.pcm_frames = 4096;
			teder_engine *e = create(&f, config, false);
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
			gap_load(e, GAP_A);
			gap_prepare(e, GAP_B);
			ck_assert(status(e).next_state == TEDER_NEXT_READY);
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			ck_assert(status(e).next_state == TEDER_NEXT_COMMITTED);
			do {
				step(&f, chunks[chunk]);
			} while (atomic_load(&f.captured) < a + b);
			/* A trailing empty marker may follow an exactly full callback. */
			if (!b)
				step(&f, 1);
			teder_status s;
			do {
				s = status(e);
				th_yield();
			} while (s.transport != TEDER_ENDED);
			ck_assert(s.current.epoch == 3 && s.current.source == 2 && s.track == 77 &&
			          s.source_frame == b && s.duration_frames == b && s.output_frame == a + b &&
			          !s.underrun_frames && !s.next_source && s.next_state == TEDER_NEXT_NONE &&
			          (s.clock_flags & TEDER_SUBMIT_EOF) && s.last_error == TEDER_OK);
			ck_assert(atomic_load(&f.captured) == a + b && f.starts == 1 && !f.stops &&
			          !f.releases);
			gap_samples(&f, 0, a);
			gap_samples(&f, a, b);
			gap_event(e, 1, 2, a);
			step(&f, 31);
			ck_assert(status(e).output_frame == a + b && atomic_load(&f.captured) == a + b);
			close_engine(&e);
			dispose(&f);
		}
}
END_TEST

START_TEST(gapless_repeat)
{
	th_make_track_samples(GAP_A, 512, true);
	th_make_track_samples(GAP_B, 32, true);
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	config.pcm_frames = 4096;
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	gap_load(e, GAP_A);
	gap_prepare(e, GAP_B);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 257);
	step(&f, 256); /* consume one successor frame, leaving 31 */
	while (status(e).current.source != 2)
		th_yield();
	gap_prepare(e, GAP_B); /* repeated file/label, distinct source instance */
	while (status(e).next_state != TEDER_NEXT_COMMITTED)
		th_yield();
	step(&f, 100);
	while (status(e).transport != TEDER_ENDED)
		th_yield();
	teder_status s = status(e);
	ck_assert(s.current.source == 3 && s.current.epoch == 3 && s.source_frame == 32 &&
	          s.output_frame == 576 && !s.underrun_frames && f.starts == 1 && !f.stops &&
	          !f.releases);
	gap_samples(&f, 0, 512);
	gap_samples(&f, 512, 32);
	gap_samples(&f, 544, 32);
	gap_event(e, 1, 2, 512);
	gap_event(e, 2, 3, 544);
	close_engine(&e);
	dispose(&f);
}
END_TEST

START_TEST(gapless_cutoff)
{
	for (unsigned after = 0; after < 2; after++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		config.pcm_frames = 4096;
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		atomic_store(&f.point, after ? 17 : 16);
		gap_prepare(e, TRACK);
		th_gate_await(&f.gate);
		step(&f, 17); /* native EOF wins before linking, or consumes the successor */
		ck_assert(atomic_load(&f.captured) == (after ? 17 : 16));
		if (after) {
			teder_status s = status(e);
			uint64_t rejected = 99;
			teder_code code;
			RETRY(code, teder_clear_next(e, &s.current, s.next_source, &rejected));
			ck_assert(code == TEDER_TOO_LATE && !rejected);
			OK(teder_request_close(e));
			ck_assert(teder_destroy(&e) == TEDER_BUSY);
		}
		th_gate_release(&f.gate);
		if (!after) {
			while (status(e).transport != TEDER_ENDED)
				th_yield();
			teder_status s = status(e);
			ck_assert(s.current.source == 1 && s.last_error == TEDER_TOO_LATE && !s.next_source &&
			          s.output_frame == 16 && !s.underrun_frames);
			teder_event event = INIT(teder_event);
			bool late = false;
			while (!late) {
				teder_code code = teder_event_poll(e, &event);
				ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
				if (code == TEDER_OK) {
					ck_assert(event.kind != TEDER_EVENT_TRANSITION);
					late = event.kind == TEDER_EVENT_ERROR && event.code == TEDER_TOO_LATE &&
					       event.to_source == 2;
				}
			}
		}
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(gapless_failures)
{
	for (unsigned mixed = 0; mixed < 2; mixed++)
		for (unsigned adopted = 0; adopted < 2; adopted++) {
			if (mixed)
				th_make_mixed(GAP_B, (th_format){48000, 8, 24, adopted ? 2048 : 512});
			else
				th_make_track(GAP_B, adopted ? 2048 : 512);
			FILE *file = fopen(GAP_B, "r+b");
			ck_assert(file && !fseek(file, -1, SEEK_END));
			int byte = fgetc(file);
			ck_assert(byte != EOF && !fseek(file, -1, SEEK_END));
			ck_assert(fputc(byte ^ 1, file) != EOF && !fclose(file));
			fixture f;
			init(&f);
			f.manual = true;
			teder_config config = INIT(teder_config);
			config.pcm_frames = 4096;
			teder_engine *e = create(&f, config, false);
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
			load_track(e);
			gap_prepare(e, GAP_B); /* first FLAC frame is valid; later CRC fails */
			outcome(e, transport(e, TEDER_PLAY), adopted ? TEDER_CORRUPT : TEDER_OK);
			if (!adopted)
				capture_track(&f, e, 0, 16);
			teder_status s = status(e);
			ck_assert(s.current.source == 1 && s.last_error == TEDER_CORRUPT &&
			          s.transport == (adopted ? TEDER_FAILED : TEDER_ENDED) &&
			          s.next_state == (adopted ? TEDER_NEXT_COMMITTED : TEDER_NEXT_NONE) &&
			          f.starts == (adopted ? 0 : 1));
			close_engine(&e);
			dispose(&f);
		}
}
END_TEST

static double mixed_pair(th_format a, th_format b, unsigned rate, unsigned chunk)
{
	th_make_mixed(GAP_A, a);
	th_make_mixed(GAP_B, b);
	unsigned first = th_mixed_frames(a, rate), second = th_mixed_frames(b, rate);
	fixture f;
	init(&f);
	f.manual = true;
	f.rate = rate;
	teder_config config = INIT(teder_config);
	config.pcm_frames = first + second + 2048;
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	gap_load(e, GAP_A);
	gap_prepare(e, GAP_B);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	ck_assert(status(e).next_state == TEDER_NEXT_COMMITTED);
	unsigned submitted = first && second && chunk > 1 ? first - 1 : first;
	for (unsigned remaining = submitted; remaining;) {
		unsigned count = remaining < chunk ? remaining : chunk;
		step(&f, count);
		remaining -= count;
	}
	if (submitted) {
		teder_status s = clock_at(e, submitted);
		ck_assert(s.current.source == 1 && s.source_rate == a.rate &&
		          s.source_channels == a.channels && s.duration_frames == a.frames &&
		          s.output_rate == rate);
		if (submitted == first)
			ck_assert(s.source_frame == a.frames);
	}
	/* Consume through the boundary in one callback, including empty markers. */
	do {
		step(&f, chunk);
	} while (atomic_load(&f.captured) < first + second);
	teder_status s;
	do {
		s = status(e);
		th_yield();
	} while (s.transport != TEDER_ENDED);
	ck_assert(s.current.epoch == 3 && s.current.source == 2 && s.track == 77 &&
	          s.source_rate == b.rate && s.source_channels == b.channels &&
	          s.source_frame == b.frames && s.duration_frames == b.frames &&
	          s.output_frame == first + second && s.output_rate == rate && s.output_channels == 2 &&
	          !s.underrun_frames && !s.next_source && s.next_state == TEDER_NEXT_NONE &&
	          (s.clock_flags & TEDER_SUBMIT_EOF) && s.last_error == TEDER_OK);
	ck_assert(s.position_quality ==
	          (b.rate == rate ? TEDER_QUALITY_MEASURED : TEDER_QUALITY_ESTIMATED));
	ck_assert(atomic_load(&f.captured) == first + second && f.starts == 1 && !f.stops &&
	          f.devices == 1 && !f.releases);
	double left = th_check_mixed(f.capture, a, rate);
	double right = th_check_mixed(f.capture + first * 2, b, rate);
	gap_event(e, 1, 2, first);
	close_engine(&e);
	dispose(&f);
	return left > right ? left : right;
}

START_TEST(gapless_rates)
{
	const unsigned rates[] = {8000, 22050, 44100, 48000, 88200, 96000, 192000};
	double worst = 0;
	for (unsigned output = 44100; output <= 48000; output += 3900)
		for (unsigned a = 0; a < 7; a++)
			for (unsigned b = 0; b < 7; b++) {
				double error = mixed_pair(
				    (th_format){rates[a], 2, 16, 37}, (th_format){rates[b], 2, 24, 29}, output, 7);
				if (error > worst)
					worst = error;
			}
	printf("98 rate pairs; worst independent-reference sample error %.9g\n", worst);
}
END_TEST

START_TEST(gapless_formats)
{
	double worst = 0;
	for (unsigned a = 1; a <= 8; a++)
		for (unsigned b = 1; b <= 8; b++) {
			double error = mixed_pair(
			    (th_format){22050, a, 4 * a, 41}, (th_format){96000, b, 4 * b, 23}, 48000, 31);
			if (error > worst)
				worst = error;
		}
	for (unsigned bits = 4; bits <= 32; bits++) {
		double error = mixed_pair(
		    (th_format){44100, 2, bits, 31}, (th_format){44100, 2, 36 - bits, 19}, 44100, 257);
		if (error > worst)
			worst = error;
	}
	printf("64 layout pairs and 29 depth pairs; worst sample error %.9g\n", worst);
}
END_TEST

START_TEST(gapless_edges)
{
	const unsigned lengths[][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {1031, 513}};
	const unsigned chunks[] = {1, 7, 257};
	for (unsigned pair = 0; pair < 5; pair++)
		for (unsigned chunk = 0; chunk < 3; chunk++)
			for (unsigned reverse = 0; reverse < 2; reverse++)
				mixed_pair((th_format){reverse ? 96000 : 22050, 8, 32, lengths[pair][0]},
				           (th_format){reverse ? 22050 : 96000, 1, 4, lengths[pair][1]},
				           48000,
				           chunks[chunk]);
	/* Both inclusive conversion-ratio limits, and the largest FLAC source rate. */
	mixed_pair((th_format){125, 1, 4, 17}, (th_format){512000, 8, 32, 1031}, 8000, 257);
	mixed_pair((th_format){512000, 8, 32, 1031}, (th_format){125, 1, 4, 17}, 8000, 257);
	mixed_pair((th_format){1048575, 8, 32, 1031}, (th_format){44100, 1, 4, 17}, 44100, 7);
}
END_TEST

START_TEST(gapless_limits)
{
	th_make_mixed(GAP_A, (th_format){8000, 2, 16, 8193});
	th_make_mixed(GAP_B, (th_format){16000, 8, 24, 31});
	fixture f;
	init(&f);
	f.manual = true;
	f.rate = 8000;
	teder_config config = INIT(teder_config);
	config.pcm_frames = 4096;
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	gap_load(e, GAP_A);
	gap_prepare(e, GAP_B);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 7);
	teder_status before = clock_at(e, 7);
	ck_assert(before.next_state == TEDER_NEXT_READY);
	size_t bytes = owned(e);
	const unsigned rejected[] = {1, 124, 512001, 1048575};
	for (unsigned i = 0; i < 4; i++) {
		const char *path = OUTPUT_DIR "/gapless-rejected.flac";
		th_make_mixed(path, (th_format){rejected[i], 8, 32, 17});
		uint64_t id;
		teder_code code;
		RETRY(code,
		      teder_prepare_next(
		          e, &before.current, before.next_source, path, (uint32_t)strlen(path), 99, &id));
		OK(code);
		outcome(e, id, TEDER_UNSUPPORTED);
		teder_status s = status(e);
		ck_assert(s.current.epoch == before.current.epoch && s.current.source == 1 &&
		          s.next_source == 2 && s.next_track == 77 && s.next_state == TEDER_NEXT_READY &&
		          s.transport == TEDER_PLAYING && s.source_frame == 7 && s.output_frame == 7 &&
		          owned(e) == bytes && f.starts == 1 && !f.stops && !f.releases);
	}
	step(&f, 7);
	ck_assert(clock_at(e, 14).source_frame == 14);
	th_check_mixed(f.capture, (th_format){8000, 2, 16, 14}, 8000);
	close_engine(&e);
	dispose(&f);
}
END_TEST

/* Park control with refill acknowledged, independently of a held preparation. */
static void park_owner(fixture *f, teder_engine *e)
{
	atomic_store(&f->park_owner, 1);
	td_signal(e);
	th_gate_await(&f->owner_gate);
}

/* Caller holds control, so the notification's native/userdata borrow is stable. */
static void lose(fixture *f)
{
	ma_device_notification n = {.pDevice = f->native, .type = ma_device_notification_type_rerouted};
	f->native->onNotification(&n);
	f->native->onNotification(&n);
	n.type = ma_device_notification_type_started;
	f->native->onNotification(&n); /* resumption cannot undo the invalidation */
}

static teder_status lost_status(teder_engine *e, teder_target before, uint64_t frame)
{
	teder_status s;
	do {
		s = status(e);
		th_yield();
	} while (s.output_state != TEDER_OUTPUT_LOST);
	ck_assert(s.current.epoch == before.epoch + 1 && s.current.source == before.source &&
	          s.source_frame == frame && !s.output_frame && !s.underrun_frames && !s.output_rate &&
	          !s.output_channels && !s.output_period_frames && !s.backend && !s.latency_frames &&
	          s.latency_quality == TEDER_QUALITY_UNKNOWN &&
	          s.position_quality == TEDER_QUALITY_UNKNOWN && !s.next_source && !s.next_track &&
	          s.next_state == TEDER_NEXT_NONE && !(s.clock_flags & ~TEDER_DURATION_KNOWN) &&
	          s.transport == (before.source ? TEDER_PAUSED : TEDER_EMPTY) &&
	          s.last_error == TEDER_DEVICE_LOST);
	return s;
}

static void loss_event(teder_engine *e, teder_status s)
{
	teder_event event = INIT(teder_event);
	for (;;) {
		teder_code code = teder_event_poll(e, &event);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && event.kind == TEDER_EVENT_DEVICE &&
		    event.code == TEDER_DEVICE_LOST && event.epoch == s.current.epoch)
			break;
		th_yield();
	}
	ck_assert(event.epoch == s.current.epoch && event.from_source == s.current.source &&
	          event.from_track == s.track && !event.to_source && !event.to_track &&
	          !event.output_frame);
	while (teder_event_poll(e, &event) == TEDER_OK)
		ck_assert(event.kind != TEDER_EVENT_DEVICE || event.code != TEDER_DEVICE_LOST);
}

START_TEST(loss_states)
{
	for (unsigned mode = 0; mode < 5; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		if (mode)
			load_track(e);
		if (mode >= 2) {
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			step(&f, mode == 4 ? 16 : 4);
			clock_at(e, mode == 4 ? 16 : 4);
		}
		if (mode == 3) {
			atomic_store(&f.point, 13);
			boundary(&f, e, transport(e, TEDER_PAUSE));
		}
		park_owner(&f, e);
		teder_status before = status(e);
		ck_assert(before.transport == (mode == 0   ? TEDER_EMPTY
		                               : mode == 1 ? TEDER_READY
		                               : mode == 2 ? TEDER_DRAINING
		                               : mode == 3 ? TEDER_PAUSED
		                                           : TEDER_ENDED));
		size_t budget = owned(e) - e->device_reservation;
		size_t attempts = f.tracker.attempts;
		unsigned starts = f.starts;
		lose(&f);
		th_gate_release(&f.owner_gate); /* no request or callback wake: monitor must notice */
		teder_status after = lost_status(e, before.current, before.source_frame);
		loss_event(e, after);
		ck_assert(owned(e) == budget && f.releases == 1 && !f.worker && !f.stops &&
		          f.starts == starts && f.tracker.attempts == attempts);
		uint64_t id = 99;
		teder_code code;
		RETRY(code, teder_transport(e, &before.current, TEDER_PLAY, &id));
		ck_assert(code == TEDER_STALE && !id);
		outcome(e, transport(e, TEDER_PLAY), TEDER_BAD_STATE);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		after = status(e);
		ck_assert(after.output_state == TEDER_OUTPUT_READY &&
		          after.transport == (mode ? TEDER_PAUSED : TEDER_EMPTY) &&
		          after.current.source == before.current.source &&
		          after.source_frame == before.source_frame && f.starts == starts);
		if (mode && mode != 4) {
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			step(&f, 1);
			ck_assert(clock_at(e, 1).source_frame == before.source_frame + 1);
			ck_assert(f.capture[0] == ((int)before.source_frame - 8) * 1000 / 32768.0f &&
			          f.capture[1] == -f.capture[0]);
		}
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(loss_controls)
{
	for (unsigned mode = 0; mode < 4; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		step(&f, 3);
		teder_status before = clock_at(e, 3);
		atomic_store(&f.point, mode == 2 ? 12 : 13);
		uint64_t id = mode == 1 || mode == 2 ? transport(e, mode == 1 ? TEDER_PAUSE : TEDER_STOP)
		                                     : gain_to(e, 0.25f, 0);
		th_gate_await(&f.gate);
		lose(&f);
		if (mode == 3)
			OK(teder_request_close(e));
		th_gate_release(&f.gate);
		outcome(e, id, TEDER_DEVICE_LOST); /* no additional callback can acknowledge it */
		teder_status after = lost_status(e, before.current, 3);
		ck_assert(after.gain == 1 && !after.mute && !f.stops && f.releases == 1);
		loss_event(e, after);
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(loss_preparing)
{
	for (unsigned mode = 0; mode < 4; mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		step(&f, 3);
		clock_at(e, 3);
		atomic_store(&f.point, 9); /* preparation owns native file and request input */
		uint64_t id;
		teder_code code;
		teder_status before = status(e);
		if (mode == 0)
			id = seek_frame(e, 9);
		else if (mode == 1) {
			RETRY(code,
			      teder_prepare_next(e, &before.current, 0, TRACK, sizeof(TRACK) - 1, 88, &id));
			OK(code);
		} else if (mode == 2)
			id = select_device(e, (teder_blob){0});
		else
			id = autoplay(e, TRACK);
		th_gate_await(&f.gate);
		park_owner(&f, e);
		before = status(e);
		lose(&f);
		th_gate_release(&f.owner_gate);
		teder_status after = lost_status(e, before.current, 3);
		loss_event(e, after);
		ck_assert(after.media_request == id);
		teder_request_info r = INIT(teder_request_info);
		td_lock(e);
		code = teder_request_get(e, id, &r);
		td_unlock(e);
		ck_assert(code == TEDER_AGAIN); /* bounded reads report gate contention */
		while ((code = teder_request_get(e, id, &r)) == TEDER_AGAIN)
			th_yield();
		OK(code);
		ck_assert(r.state == TEDER_RUNNING);
		ck_assert(teder_request_ack(e, id) == TEDER_BUSY);
		th_gate_release(&f.gate);
		outcome(e, id, TEDER_STALE);
		ck_assert(!status(e).media_request && status(e).current.source == before.current.source);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		ck_assert(status(e).source_frame == 3 && status(e).transport == TEDER_PAUSED);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		step(&f, 1);
		ck_assert(clock_at(e, 1).source_frame == 4);
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(loss_closing)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
	atomic_store(&f.hold, 1);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	th_gate_await(&f.render_gate); /* native dispatch retains device/userdata */
	td_analysis_feed *analysis = ((td_device_feed *)f.native->pUserData)->analysis;
	ck_assert(analysis == &e->analysis && analysis->capacity == 64);
	park_owner(&f, e);
	teder_status before = status(e);
	size_t budget = owned(e);
	lose(&f);
	atomic_store(&f.point, 8);
	th_gate_release(&f.owner_gate);
	th_gate_await(&f.gate);
	ck_assert(status(e).current.epoch == before.current.epoch && owned(e) == budget &&
	          !f.releases && !f.stops);
	OK(teder_request_close(e));
	th_gate_release(&f.gate);
	while (!atomic_load(&f.stop))
		th_yield(); /* uninit is now joining the held native user */
	ck_assert(teder_destroy(&e) == TEDER_BUSY && owned(e) == budget && !f.releases);
	ck_assert(&e->analysis == analysis && !atomic_load(&analysis->revision));
	th_gate_release(&f.render_gate);
	close_engine(&e);
	ck_assert(f.releases == 1 && !f.stops);
	dispose(&f);
}
END_TEST

START_TEST(loss_commit)
{
	for (unsigned committed = 0; committed < 2; committed++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		step(&f, 3);
		clock_at(e, 3);
		atomic_store(&f.point, committed ? 18 : 14);
		uint64_t id = seek_frame(e, 9);
		th_gate_await(&f.gate);
		teder_status before = status(e);
		lose(&f);
		th_gate_release(&f.gate);
		outcome(e, id, committed ? TEDER_DEVICE_LOST : TEDER_STALE);
		loss_event(e, lost_status(e, before.current, committed ? 9 : 3));
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(loss_transition)
{
	for (unsigned crossed = 0; crossed < 2; crossed++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		gap_prepare(e, TRACK);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		ck_assert(status(e).next_state == TEDER_NEXT_COMMITTED);
		park_owner(&f, e);
		teder_status before = status(e);
		step(&f, crossed ? 17 : 4); /* final report has not reached control */
		lose(&f);
		th_gate_release(&f.owner_gate);
		if (crossed)
			before.current.source = 2;
		teder_status after = lost_status(e, before.current, crossed ? 1 : 4);
		if (crossed)
			gap_event(e, 1, 2, 16); /* emitted before the new-epoch loss event */
		loss_event(e, after);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		ck_assert(status(e).current.source == before.current.source);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		step(&f, 1);
		ck_assert(clock_at(e, 1).source_frame == (crossed ? 2 : 5));
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(loss_repeated)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
#ifdef _WIN32
	DWORD handles, after;
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &handles));
#endif
	for (unsigned i = 0; i < 8; i++) {
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		step(&f, 1);
		clock_at(e, 1);
		park_owner(&f, e);
		teder_target before = status(e).current;
		lose(&f);
		f.device_error = true;
		th_gate_release(&f.owner_gate);
		loss_event(e, lost_status(e, before, i + 1));
		outcome(e, select_device(e, (teder_blob){0}), TEDER_DEVICE_LOST);
		park_owner(&f, e);
		ck_assert(!e->device && !e->device_reservation && f.devices == f.releases);
		f.device_error = false;
		th_gate_release(&f.owner_gate);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		ck_assert(status(e).source_frame == i + 1 && status(e).transport == TEDER_PAUSED);
	}
	park_owner(&f, e);
#ifdef _WIN32
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &after));
	ck_assert_msg(handles == after,
	              "native handles: before=%lu after=%lu",
	              (unsigned long)handles,
	              (unsigned long)after);
#endif
	e->status.current.epoch = UINT64_MAX;
	lose(&f);
	th_gate_release(&f.owner_gate);
	while (status(e).lifecycle == TEDER_LIVE)
		th_yield();
	OK(th_close(e));
	ck_assert(status(e).current.epoch == UINT64_MAX && status(e).last_error == TEDER_LIMIT);
	OK(teder_destroy(&e));
	dispose(&f);
}
END_TEST

static uint64_t probe_track(teder_engine *e)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_probe(e, TRACK, sizeof(TRACK) - 1, 0, &id));
	OK(code);
	return id;
}

static void verify_retained(teder_result **results)
{
	uint32_t count;
	OK(teder_result_device_count(results[0], &count));
	ck_assert(count == 2);
	for (unsigned i = 0; i < 2; i++) {
		teder_device_info d = entry(results[0], i);
		ck_assert(d.backend == TEDER_BACKEND_WASAPI && d.is_default == !i &&
		          d.name.bytes == (i ? 4 : 5) &&
		          !memcmp(d.name.data, i ? "Beta" : "Alpha", d.name.bytes) && d.key.bytes == 20 &&
		          d.key.data[16] == 'A' + i);
	}
	teder_probe_info info = INIT(teder_probe_info);
	OK(teder_result_probe(results[1], &info));
	ck_assert(info.codec == TEDER_CODEC_FLAC && info.sample_rate == 44100 && info.channels == 2 &&
	          info.bits_per_sample == 16 && info.duration_frames == 16 && info.duration_known &&
	          info.seekable && info.tag_count == 1 && !info.artwork_count);
	teder_text key, value;
	OK(teder_result_tag(results[1], 0, &key, &value));
	ck_assert(key.bytes == 5 && !memcmp(key.data, "TITLE", 5) && value.bytes == 7 &&
	          !memcmp(value.data, "track-a", 7));
}

static void retain_pair(teder_engine *e, teder_result **results)
{
	uint64_t id = submit(e);
	OK(terminal(e, id).outcome);
	results[0] = take(e, id);
	id = probe_track(e);
	OK(terminal(e, id).outcome);
	results[1] = take(e, id);
	verify_retained(results);
}

static void destroy_with_results(fixture *f, teder_engine **e, teder_result **results)
{
	for (unsigned i = 0; i < 5; i++)
		ck_assert(atomic_load(&f->worker_entries[i]) == 1 && atomic_load(&f->worker_exits[i]) == 1);
	ck_assert(!f->worker && f->devices == f->releases && f->opened == f->closed);
	OK(teder_destroy(e));
	ck_assert(!*e && f->tracker.live == 2 &&
	          f->tracker.bytes == results[0]->bytes + results[1]->bytes);
	verify_retained(results);
	for (unsigned i = 0; i < 2; i++)
		teder_result_release(results[i]);
	dispose(f);
}

START_TEST(shutdown_states)
{
	const unsigned states[] = {TEDER_EMPTY,
	                           TEDER_EMPTY,
	                           TEDER_READY,
	                           TEDER_PLAYING,
	                           TEDER_PAUSED,
	                           TEDER_DRAINING,
	                           TEDER_ENDED,
	                           TEDER_FAILED,
	                           TEDER_PAUSED};
	for (unsigned mode = 0; mode < sizeof(states) / sizeof(*states); mode++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		config.request_capacity = 3;
		config.event_capacity = 1;
		config.pcm_frames = mode == 3 ? 4 : 4096;
		teder_engine *e = create(&f, config, false);
		size_t base = owned(e);
		teder_result *results[2];
		retain_pair(e, results);
		if (mode)
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		if (mode >= 2)
			load_track(e);
		if (mode >= 3 && mode != 7) {
			outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
			step(&f, mode == 6 ? 16 : 1);
			clock_at(e, mode == 6 ? 16 : 1);
		}
		if (mode == 4) {
			atomic_store(&f.point, 13);
			boundary(&f, e, transport(e, TEDER_PAUSE));
		}
		if (mode == 7) {
			park_owner(&f, e);
			f.tracker.fail_at = f.tracker.attempts + 1;
			uint64_t id = transport(e, TEDER_STOP);
			th_gate_release(&f.owner_gate);
			outcome(e, id, TEDER_NO_MEMORY);
			ma_mutex_lock(&f.tracker.lock);
			f.tracker.fail_at = 0;
			ma_mutex_unlock(&f.tracker.lock);
		}
		if (mode == 8) {
			park_owner(&f, e);
			teder_target before = status(e).current;
			lose(&f);
			th_gate_release(&f.owner_gate);
			lost_status(e, before, 1);
		}
		ck_assert(status(e).transport == states[mode]);
		uint64_t ids[] = {probe_track(e), submit(e), gain_to(e, 1, 0)};
		for (unsigned i = 0; i < 3; i++)
			OK(terminal(e, ids[i]).outcome); /* terminal results deliberately remain untaken */
		uint64_t rejected = 99;
		teder_code code;
		RETRY(code, teder_probe(e, TRACK, sizeof(TRACK) - 1, 0, &rejected));
		ck_assert(code == TEDER_LIMIT && !rejected);
		OK(teder_request_close(e));
		OK(teder_request_close(e));
		OK(th_close(e));
		ck_assert(status(e).lifecycle == TEDER_CLOSED_STATE &&
		          owned(e) == base + results[0]->bytes + results[1]->bytes);
		for (unsigned i = 0; i < 3; i++) {
			teder_request_info r = terminal(e, ids[i]);
			ck_assert(r.request == ids[i] && r.outcome == TEDER_OK &&
			          r.result_kind == (i == 0   ? TEDER_RESULT_PROBE
			                            : i == 1 ? TEDER_RESULT_DEVICES
			                                     : TEDER_RESULT_NONE));
		}
		ck_assert(atomic_load(&f.source_opened) == (mode >= 2 ? 1 : 0) &&
		          atomic_load(&f.source_closed) == (mode >= 2 ? 1 : 0) &&
		          atomic_load(&f.probe_opened) == 2 && atomic_load(&f.probe_closed) == 2);
		destroy_with_results(&f, &e, results);
	}
}
END_TEST

START_TEST(shutdown_borrowers)
{
	for (unsigned probe_first = 0; probe_first < 2; probe_first++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		config.request_capacity = 5;
		config.pcm_frames = 4096;
		teder_engine *e = create(&f, config, false);
		teder_result *results[2];
		retain_pair(e, results);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		load_track(e);
		gap_prepare(e, TRACK);
		atomic_store(&f.hold, 1);
		outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
		th_gate_await(&f.render_gate);
		ck_assert(status(e).next_state == TEDER_NEXT_COMMITTED);
		uint64_t ids[5] = {probe_track(e), submit(e), 0, 0, 0};
		OK(terminal(e, ids[0]).outcome);
		OK(terminal(e, ids[1]).outcome);
		atomic_store(&f.hold_prepare, 1);
		ids[2] = seek_frame(e, 7);
		th_gate_await(&f.prepare_gate);
		atomic_store(&f.hold_probe, 1);
		ids[3] = probe_track(e);
		th_gate_await(&f.probe_gate);
		park_owner(&f, e);
		ids[4] = gain_to(e, 0.25f, 1);
		size_t remaining = owned(e) - e->backend_reservation - e->device_reservation;
		for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
			remaining -= e->sources[i].reservation;
		uint64_t rejected = 99;
		ck_assert(teder_probe(e, TRACK, sizeof(TRACK) - 1, 0, &rejected) == TEDER_LIMIT &&
		          !rejected);
		OK(teder_request_close(e));
		th_gate_release(&f.owner_gate);
		for (unsigned i = 0; i < 64; i++) {
			ck_assert(status(e).lifecycle == TEDER_CLOSING && teder_destroy(&e) == TEDER_BUSY);
			verify_retained(results);
		}
		ck_assert(!atomic_load(&f.worker_exits[0]) && !atomic_load(&f.worker_exits[2]));
		if (probe_first) {
			th_gate_release(&f.probe_gate);
			while (!atomic_load(&f.worker_exits[2]))
				th_yield();
			ck_assert(terminal(e, ids[3]).outcome == TEDER_CANCELLED && !f.releases);
		}
		th_gate_release(&f.prepare_gate);
		while (!atomic_load(&f.stop))
			th_yield(); /* native stop now joins the still-held callback dispatcher */
		ck_assert(atomic_load(&f.worker_exits[0]) == 1 && atomic_load(&f.worker_exits[1]) == 1 &&
		          !f.releases && teder_destroy(&e) == TEDER_BUSY);
		th_gate_release(&f.render_gate);
		if (!probe_first) {
			while (owned(e) != remaining)
				th_yield(); /* source retirement releases its reservation after actual frees */
			ck_assert(f.releases == 1 && f.closed == 1 && !f.worker &&
			          atomic_load(&f.source_closed) == 3 && !atomic_load(&f.worker_exits[2]) &&
			          teder_destroy(&e) == TEDER_BUSY);
			th_gate_release(&f.probe_gate);
		}
		OK(th_close(e));
		for (unsigned i = 0; i < 5; i++) {
			teder_request_info r = terminal(e, ids[i]);
			ck_assert(r.request == ids[i] && r.outcome == (i < 2 ? TEDER_OK : TEDER_CANCELLED));
		}
		ck_assert(atomic_load(&f.source_opened) == 3 && atomic_load(&f.source_closed) == 3 &&
		          atomic_load(&f.probe_opened) == 3 && atomic_load(&f.probe_closed) == 3);
		destroy_with_results(&f, &e, results);
	}
}
END_TEST

static teder_snapshot *analysis_current(teder_engine *e)
{
	while (atomic_load_explicit(&e->analysis.read, memory_order_acquire) !=
	       atomic_load_explicit(&e->analysis.write, memory_order_acquire))
		th_yield();
	teder_snapshot *s;
	teder_code code;
	do {
		code = teder_snapshot_acquire(e, &s);
	} while (code == TEDER_AGAIN);
	OK(code);
	return s;
}

typedef struct {
	teder_snapshot *loan;
	teder_snapshot_info info;
	float bands[2];
	teder_span span;
} held_spectrum;

static uint64_t configure_spectrum(teder_engine *e, unsigned window)
{
	teder_analysis_config c = INIT(teder_analysis_config);
	c.enabled = 1;
	c.window_frames = window;
	c.hop_frames = 16;
	c.bands = 2;
	c.low_hz = 750;
	c.high_hz = 12000;
	uint64_t id;
	teder_code code;
	RETRY(code, teder_configure_analysis(e, &c, &id));
	OK(code);
	return id;
}

static held_spectrum retain_spectrum(teder_engine *e)
{
	held_spectrum s = {.loan = analysis_current(e), .info = INIT(teder_snapshot_info)};
	OK(teder_snapshot_get_info(s.loan, &s.info));
	const float *bands;
	const teder_span *spans;
	uint32_t count;
	OK(teder_snapshot_bands(s.loan, &bands, &count));
	ck_assert(count == 2 && s.info.bands == 2 && s.info.frames == 32 &&
	          s.info.config_revision == 1 && s.info.output_start == 0);
	memcpy(s.bands, bands, sizeof(s.bands));
	OK(teder_snapshot_spans(s.loan, &spans, &count));
	ck_assert(count == 1 && s.info.span_count == 1 && spans->output_start == 0 &&
	          spans->frames == 32 && spans->source == 1 && spans->track == 99);
	s.span = *spans;
	return s;
}

static void verify_spectrum(const held_spectrum *s)
{
	teder_snapshot_info info = INIT(teder_snapshot_info);
	const float *bands;
	const teder_span *spans;
	uint32_t count;
	OK(teder_snapshot_get_info(s->loan, &info));
	ck_assert(!memcmp(&info, &s->info, sizeof(info)));
	OK(teder_snapshot_bands(s->loan, &bands, &count));
	ck_assert(count == 2 && !memcmp(bands, s->bands, sizeof(s->bands)));
	OK(teder_snapshot_spans(s->loan, &spans, &count));
	ck_assert(count == 1 && !memcmp(spans, &s->span, sizeof(s->span)));
}

static void
destroy_with_spectrum(fixture *f, teder_engine **e, teder_result **results, const held_spectrum *s)
{
	size_t pool_bytes = (*e)->snapshots->bytes;
	for (unsigned i = 0; i < 5; i++)
		ck_assert(atomic_load(&f->worker_entries[i]) == 1 && atomic_load(&f->worker_exits[i]) == 1);
	OK(teder_destroy(e));
	ck_assert(!*e && f->tracker.live == 3 &&
	          f->tracker.bytes == pool_bytes + results[0]->bytes + results[1]->bytes);
	verify_retained(results);
	verify_spectrum(s);
	for (unsigned i = 0; i < 2; i++)
		teder_result_release(results[i]);
	ck_assert(f->tracker.live == 1 && f->tracker.bytes == pool_bytes);
	teder_snapshot_release(s->loan);
	dispose(f);
}

START_TEST(shutdown_analysis)
{
	th_make_track(AUDIT_TRACK, 128);
	for (unsigned analysis_first = 0; analysis_first < 2; analysis_first++) {
		fixture f;
		init(&f);
		f.manual = true;
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		teder_result *results[2];
		retain_pair(e, results);
		outcome(e, configure_spectrum(e, 32), TEDER_OK);
		outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
		outcome(e, autoplay(e, AUDIT_TRACK), TEDER_OK);
		step(&f, 32);
		held_spectrum s = retain_spectrum(e);
		atomic_store(&f.point, 19);
		step(&f, 16);
		th_gate_await(&f.gate); /* analysis still borrows a copied feed record */
		atomic_store(&f.hold_probe, 1);
		uint64_t probe = probe_track(e);
		th_gate_await(&f.probe_gate);
		OK(teder_request_close(e));
		while (!atomic_load(&f.source_closed))
			th_yield(); /* native output is quiescent before source retirement */
		ck_assert(f.devices == 1 && f.releases == 1 && f.closed == 1 && !f.worker &&
		          !atomic_load(&f.worker_exits[2]) && !atomic_load(&f.worker_exits[4]));
		ck_assert(status(e).lifecycle == TEDER_CLOSING && teder_destroy(&e) == TEDER_BUSY);
		verify_retained(results);
		verify_spectrum(&s);

		th_gate_release(analysis_first ? &f.gate : &f.probe_gate);
		unsigned first = analysis_first ? 4 : 2, last = analysis_first ? 2 : 4;
		while (!atomic_load(&f.worker_exits[first]))
			th_yield();
		ck_assert(!atomic_load(&f.worker_exits[last]) && status(e).lifecycle == TEDER_CLOSING &&
		          teder_destroy(&e) == TEDER_BUSY);
		verify_spectrum(&s);
		th_gate_release(analysis_first ? &f.probe_gate : &f.gate);
		OK(th_close(e));
		outcome(e, probe, TEDER_CANCELLED);
		ck_assert(status(e).analysis_sequence == s.info.sequence &&
		          atomic_load(&f.source_closed) == 1 && atomic_load(&f.probe_closed) == 2);
		destroy_with_spectrum(&f, &e, results, &s);
	}
}
END_TEST

START_TEST(analysis_playback)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
	outcome(e, gain_to(e, .5f, 0), TEDER_OK);
	teder_analysis_config c = INIT(teder_analysis_config);
	c.enabled = c.window_frames = 1;
	uint64_t id;
	teder_code code;
	RETRY(code, teder_configure_analysis(e, &c, &id));
	OK(code);
	outcome(e, id, TEDER_OK);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 1);
	teder_snapshot *a = analysis_current(e);
	teder_snapshot_info saved = INIT(teder_snapshot_info), got = INIT(teder_snapshot_info);
	OK(teder_snapshot_get_info(a, &saved));
	ck_assert(saved.epoch == 3 && saved.output_start == 0 && saved.frames == 1 &&
	          saved.peak[0] == 4000 / 32768.0f && saved.rms[1] == saved.peak[0]);
	atomic_store(&f.point, 13);
	boundary(&f, e, transport(e, TEDER_PAUSE));
	step(&f, 5);
	teder_snapshot *b = analysis_current(e);
	OK(teder_snapshot_get_info(b, &got));
	ck_assert(got.sequence == saved.sequence); /* pause acknowledgement consumes no PCM */
	teder_snapshot_release(b);
	outcome(e, seek_frame(e, 7), TEDER_OK);
	b = NULL;
	ck_assert(teder_snapshot_acquire(e, &b) == TEDER_AGAIN && !b);
	outcome(e, transport(e, TEDER_STOP), TEDER_OK);
	ck_assert(teder_snapshot_acquire(e, &b) == TEDER_AGAIN && !b);
	close_engine(&e);
	OK(teder_snapshot_get_info(a, &got));
	ck_assert(!memcmp(&saved, &got, sizeof(got)));
	teder_snapshot_release(a);
	dispose(&f);
}
END_TEST

START_TEST(analysis_owners)
{
	fixture f;
	init(&f);
	f.manual = true;
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	size_t base = owned(e);
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	load_track(e);
	td_analysis_feed *feed = &e->analysis;
	ck_assert(((td_device_feed *)f.native->pUserData)->analysis == feed);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 1);
	const td_analysis_block *first = NULL;
	ck_assert(td_analysis_peek(feed, &first) == TEDER_AGAIN && !first);

	/* Private activation only: host is the sole consumer until worker integration. */
	td_analysis_revision(feed, 7);
	step(&f, 3);
	OK(td_analysis_peek(feed, &first));
	ck_assert(first->epoch == 3 && first->revision == 7 && first->output_start == 1 &&
	          first->source == 1 && first->track == 77 && first->rate == 44100 &&
	          first->frames == 3 && first->flags == TD_ANALYSIS_DISCONTINUITY);
	td_analysis_block saved = *first;

	/* Replacement joins the old native writer without resetting queued data. */
	outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
	ck_assert(f.devices == 2 && f.releases == 1 &&
	          ((td_device_feed *)f.native->pUserData)->analysis == feed);
	outcome(e, transport(e, TEDER_PLAY), TEDER_OK);
	step(&f, 2);
	OK(th_close(e));
	ck_assert(f.releases == 2 && !f.worker && f.closed == 1 && owned(e) == base);
	ck_assert(f.tracker.live == 2 && f.tracker.bytes == base);
	ck_assert(!memcmp(first, &saved, sizeof(saved))); /* borrow survives native retirement */
	for (unsigned i = 0; i < 3; i++) {
		float expected = ((int)i - 7) * 1000 / 32768.0f;
		ck_assert(first->samples[2 * i] == expected && first->samples[2 * i + 1] == -expected);
	}
	OK(td_analysis_consume(feed));
	const td_analysis_block *second;
	OK(td_analysis_peek(feed, &second));
	ck_assert(second->epoch == 4 && second->revision == 7 && second->output_start == 0 &&
	          second->source == 1 && second->track == 77 && second->rate == 44100 &&
	          second->frames == 2 && second->flags == TD_ANALYSIS_DISCONTINUITY);
	for (unsigned i = 0; i < 2; i++) {
		float expected = ((int)i - 4) * 1000 / 32768.0f;
		ck_assert(second->samples[2 * i] == expected && second->samples[2 * i + 1] == -expected);
	}
	OK(td_analysis_consume(feed));
	ck_assert(td_analysis_peek(feed, &second) == TEDER_AGAIN && !second);
	ck_assert(!atomic_load(&feed->dropped_frames));
	/* Every consumer view has ended before destroying the containing arena. */
	OK(teder_destroy(&e));
	dispose(&f);
}
END_TEST

START_TEST(shutdown_startup)
{
	fixture warm;
	init(&warm);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&warm, config, false);
	close_engine(&e);
	dispose(&warm);
#ifdef _WIN32
	DWORD before, after;
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &before));
#endif
	/* Bits name preparation/refill/probe/control/analysis worker entries. */
	const unsigned expected[] = {0, 0, 0, 23, 31, 0, 0, 0, 0, 1, 0, 3, 0, 0, 7};
	for (unsigned stage = 0; stage <= TD_INIT_ANALYSIS_THREAD; stage++) {
		fixture f;
		init(&f);
		f.fail_stage = stage;
		f.tracker.fail_at = stage ? 0 : 1;
		td_runtime runtime = {.context = &f,
		                      .permit = permit_start,
		                      .checkpoint = control_checkpoint,
		                      .media_checkpoint = checkpoint};
		ck_assert(td_engine_create_with(&config, th_allocator(&f.tracker), runtime, &e) ==
		          (stage ? TEDER_INTERNAL : TEDER_NO_MEMORY));
		ck_assert(!e && f.tracker.attempts == (stage ? 2 : 1));
		for (unsigned i = 0; i < 5; i++)
			ck_assert(atomic_load(&f.worker_entries[i]) == ((expected[stage] >> i) & 1) &&
			          atomic_load(&f.worker_exits[i]) == ((expected[stage] >> i) & 1));
		dispose(&f);
	}
#ifdef _WIN32
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after);
#endif
}
END_TEST

START_TEST(rollback_composed)
{
	th_make_track(AUDIT_TRACK, 128);
	for (unsigned operation = 0; operation < 6; operation++) {
		size_t count = 0;
		for (size_t fail = 0; fail <= count; fail++) {
			fixture f;
			init(&f);
			f.manual = true;
			teder_config config = INIT(teder_config);
			config.pcm_frames = 4096;
			teder_engine *e = create(&f, config, false);
			size_t base = owned(e);
			teder_result *results[2];
			retain_pair(e, results);
			outcome(e, select_device(e, (teder_blob){0}), TEDER_OK);
			outcome(e, configure_spectrum(e, 32), TEDER_OK);
			outcome(e, autoplay(e, AUDIT_TRACK), TEDER_OK);
			step(&f, 32);
			clock_at(e, 32);
			held_spectrum spectrum = retain_spectrum(e);
			atomic_store(&f.point, 13);
			boundary(&f, e, transport(e, TEDER_PAUSE));
			gap_prepare(e, TRACK);
			uint64_t retained = probe_track(e);
			OK(terminal(e, retained).outcome); /* an untaken result also spans rollback */
			park_owner(&f, e);
			size_t baseline = owned(e), attempts = f.tracker.attempts;
			f.tracker.fail_at = fail ? attempts + fail : 0;
			uint64_t id;
			if (operation == 0)
				id = probe_track(e);
			else if (operation == 1) {
				teder_status s = status(e);
				teder_code code;
				RETRY(code,
				      teder_prepare_next(
				          e, &s.current, s.next_source, TRACK, sizeof(TRACK) - 1, 88, &id));
				OK(code);
			} else if (operation == 2)
				id = select_device(e, entry(results[0], 1).key);
			else if (operation == 3)
				id = seek_frame(e, 9);
			else if (operation == 4)
				id = transport(e, TEDER_STOP);
			else
				id = configure_spectrum(e, 64);
			th_gate_release(&f.owner_gate);
			outcome(e, id, fail ? TEDER_NO_MEMORY : TEDER_OK);
			ma_mutex_lock(&f.tracker.lock);
			if (!fail)
				count = f.tracker.attempts - attempts;
			else
				ck_assert(f.tracker.failed_site != NULL);
			f.tracker.fail_at = 0;
			ma_mutex_unlock(&f.tracker.lock);
			if (fail && (operation == 0 || operation == 1 || operation == 3 || operation == 5)) {
				teder_status s = status(e);
				ck_assert(owned(e) == baseline && s.transport == TEDER_PAUSED &&
				          s.source_frame == 32 && s.next_state == TEDER_NEXT_READY &&
				          s.next_source == 2);
			}
			if (operation == 5)
				ck_assert(e->analysis_version == (fail ? 1 : 2));
			verify_spectrum(&spectrum);
			OK(th_close(e));
			ck_assert(owned(e) == base + results[1]->bytes &&
			          terminal(e, retained).outcome == TEDER_OK);
			destroy_with_spectrum(&f, &e, results, &spectrum);
		}
		ck_assert(count > 0);
		printf(
		    "composed operation=%u allocation failures 1..%u unwind\n", operation, (unsigned)count);
	}
}
END_TEST

static void worker_check_failure(void *unused)
{
	(void)unused;
	WORKER_CHECK(false);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "control-worker")) {
		th_thread thread;
		if (!th_thread_start(&thread, worker_check_failure, NULL))
			return 2;
		th_thread_join(&thread);
		return 0;
	}
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
	else if (!strcmp(argv[1], "reopen-queue"))
		test = reopen_queue;
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
	else if (!strcmp(argv[1], "gapless-pairs"))
		test = gapless_pairs;
	else if (!strcmp(argv[1], "gapless-repeat"))
		test = gapless_repeat;
	else if (!strcmp(argv[1], "gapless-cutoff"))
		test = gapless_cutoff;
	else if (!strcmp(argv[1], "gapless-failures"))
		test = gapless_failures;
	else if (!strcmp(argv[1], "gapless-rates"))
		test = gapless_rates;
	else if (!strcmp(argv[1], "gapless-formats"))
		test = gapless_formats;
	else if (!strcmp(argv[1], "gapless-edges"))
		test = gapless_edges;
	else if (!strcmp(argv[1], "gapless-limits"))
		test = gapless_limits;
	else if (!strcmp(argv[1], "loss-states"))
		test = loss_states;
	else if (!strcmp(argv[1], "loss-controls"))
		test = loss_controls;
	else if (!strcmp(argv[1], "loss-preparing"))
		test = loss_preparing;
	else if (!strcmp(argv[1], "loss-closing"))
		test = loss_closing;
	else if (!strcmp(argv[1], "loss-commit"))
		test = loss_commit;
	else if (!strcmp(argv[1], "loss-transition"))
		test = loss_transition;
	else if (!strcmp(argv[1], "loss-repeated"))
		test = loss_repeated;
	else if (!strcmp(argv[1], "shutdown-states"))
		test = shutdown_states;
	else if (!strcmp(argv[1], "shutdown-borrowers"))
		test = shutdown_borrowers;
	else if (!strcmp(argv[1], "shutdown-analysis"))
		test = shutdown_analysis;
	else if (!strcmp(argv[1], "analysis-playback"))
		test = analysis_playback;
	else if (!strcmp(argv[1], "analysis-owners"))
		test = analysis_owners;
	else if (!strcmp(argv[1], "shutdown-startup"))
		test = shutdown_startup;
	else if (!strcmp(argv[1], "rollback-composed"))
		test = rollback_composed;
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
