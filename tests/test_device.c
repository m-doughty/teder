#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "device.h"
#include "support/harness.h"
#include <check.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define OK(call) ck_assert((call) == TEDER_OK)

static void pause_for_device(void)
{
#ifdef _WIN32
	Sleep(1);
#else
	struct timespec pause = {0, 1000000};
	nanosleep(&pause, NULL);
#endif
}

typedef struct {
	th_tracker tracker;
	td_backend *backend;
	td_device *device;
	td_arena *arena;
	td_pcm_pool pool;
	td_pcm_generation *generation;
	th_thread thread;
	th_thread notifications;
	th_gate entered, ending;
	th_gate notifying;
	bool worker, gated, valid, reject, bad_format, fail_start, fail_stop;
	bool notification_worker, expect_silence, loss_on_init, loss_on_start;
	atomic_uint waiting_notifications;
	unsigned initialized, released;
	ma_device *native;
} fixture;

static void notify(ma_device *d, ma_device_notification_type type)
{
	ck_assert(d->onNotification != NULL);
	ma_device_notification event = {.pDevice = d, .type = type};
	d->onNotification(&event);
}

static ma_result
describe(ma_context *c, ma_device_type type, const ma_device_id *id, ma_device_info *info)
{
	(void)c;
	(void)type;
	(void)id;
	memset(info, 0, sizeof(*info));
	info->id.wasapi[0] = 'A';
	memcpy(info->name, "Test output", 12);
	return MA_SUCCESS;
}

static ma_result device_info(ma_device *d, ma_device_type type, ma_device_info *info)
{
	return describe(d->pContext, type, NULL, info);
}

static ma_result device_init(ma_device *d,
                             const ma_device_config *cfg,
                             ma_device_descriptor *playback,
                             ma_device_descriptor *capture)
{
	(void)capture;
	fixture *f = d->pContext->pUserData;
	if (f->reject)
		return MA_ERROR;
	ck_assert(cfg->playback.pDeviceID && cfg->playback.pDeviceID->wasapi[0] == 'A');
	ck_assert(cfg->playback.format == ma_format_f32 && cfg->playback.channels == 2);
	ck_assert(cfg->notificationCallback == td_device_notification &&
	          cfg->wasapi.noAutoStreamRouting);
	f->initialized++;
	f->native = d;
	playback->format = ma_format_f32;
	playback->channels = f->bad_format ? 0 : 2;
	playback->sampleRate = 48000;
	playback->periodSizeInFrames = 8;
	if (f->loss_on_init)
		notify(d, ma_device_notification_type_rerouted);
	return MA_SUCCESS;
}

static void render(void *context)
{
	fixture *f = context;
	ma_device *d = f->native;
	if (f->gated)
		th_gate_hit(&f->entered); /* native dispatch holds d/userdata across this gate */
	float out[18];
	for (unsigned i = 0; i < 18; i++)
		out[i] = 12345;
	d->onData(d, out, NULL, 8);
	f->valid = out[16] == 12345 && out[17] == 12345;
	for (unsigned i = 0; i < 8; i++) {
		float expected = i < 3 && !f->expect_silence ? (float)(i + 1) * 0.125f : 0;
		if (out[i * 2] != expected || out[i * 2 + 1] != -expected)
			f->valid = false;
	}
}

static ma_result device_start(ma_device *d)
{
	fixture *f = d->pContext->pUserData;
	if (!th_thread_start(&f->thread, render, f))
		return MA_ERROR;
	f->worker = true;
	if (f->loss_on_start)
		notify(d, ma_device_notification_type_rerouted);
	return f->fail_start ? MA_ERROR : MA_SUCCESS;
}

static void join(fixture *f)
{
	if (f->worker) {
		th_thread_join(&f->thread);
		f->worker = false;
	}
}

static ma_result device_stop(ma_device *d)
{
	fixture *f = d->pContext->pUserData;
	if (f->fail_stop)
		return MA_ERROR; /* uninit must still join the borrower */
	join(f);
	return MA_SUCCESS;
}

static ma_result device_uninit(ma_device *d)
{
	fixture *f = d->pContext->pUserData;
	if (f->gated)
		th_gate_hit(&f->ending);
	join(f);
	if (f->notification_worker) {
		atomic_store(&f->waiting_notifications, 1);
		th_thread_join(&f->notifications);
		f->notification_worker = false;
	}
	f->released++;
	return MA_SUCCESS;
}

static ma_result context_init(const ma_backend *backends,
                              ma_uint32 count,
                              const ma_context_config *config,
                              ma_context *c)
{
	(void)backends;
	(void)count;
	memset(c, 0, sizeof(*c));
	c->backend = ma_backend_wasapi;
	c->allocationCallbacks = config->allocationCallbacks;
	c->pUserData = config->pUserData;
	c->callbacks.onDeviceInit = device_init;
	c->callbacks.onDeviceStart = device_start;
	c->callbacks.onDeviceStop = device_stop;
	c->callbacks.onDeviceUninit = device_uninit;
	c->callbacks.onDeviceGetInfo = device_info;
	c->callbacks.onContextGetDeviceInfo = describe;
	return ma_mutex_init(&c->deviceInfoLock);
}

static ma_result context_uninit(ma_context *c)
{
	ma_mutex_uninit(&c->deviceInfoLock);
	return MA_SUCCESS;
}

static ma_result enumerate(ma_context *c, ma_enum_devices_callback_proc callback, void *user)
{
	ma_device_info info;
	describe(c, ma_device_type_playback, NULL, &info);
	info.isDefault = MA_TRUE;
	callback(c, ma_device_type_playback, &info, user);
	return MA_SUCCESS;
}

static void init(fixture *f, bool native)
{
	atomic_init(&f->waiting_notifications, 0);
	ck_assert(th_tracker_init(&f->tracker));
	td_allocator a = th_allocator(&f->tracker);
	td_backend_api api = {context_init, context_uninit, enumerate, f};
	OK(td_backend_open(1, 0, 8 * 1024 * 1024, a, native ? NULL : &api, &f->backend));
	size_t bytes = 0;
	td_pcm_layout layout;
	ck_assert(td_pcm_plan(&bytes, 128, &layout));
	f->arena = td_arena_create(a, bytes);
	ck_assert_ptr_nonnull(f->arena);
	void *base = td_arena_alloc(f->arena, bytes, 1, _Alignof(max_align_t));
	td_pcm_init(&f->pool, base, &layout, 128);
	td_arena *source = td_arena_create(a, sizeof(td_pcm_generation));
	ck_assert_ptr_nonnull(source);
	f->generation = td_arena_alloc(source, 1, sizeof(*f->generation), _Alignof(td_pcm_generation));
	OK(td_pcm_generation_init(f->generation, source, 1, 1, 0));
	uint32_t slot;
	OK(td_pcm_acquire(&f->pool, f->generation, &slot));
	for (unsigned i = 0; i < 3; i++) {
		f->pool.blocks[slot].samples[i * 2] = native ? 0 : (float)(i + 1) * 0.25f;
		f->pool.blocks[slot].samples[i * 2 + 1] = -f->pool.blocks[slot].samples[i * 2];
	}
	td_pcm_meta meta = {0, 3, 0, 3, TD_PCM_FINAL};
	OK(td_pcm_publish(&f->pool, slot, &meta));
}

static teder_code open_device(fixture *f, teder_blob key, uint32_t rate)
{
	return td_device_open(f->backend, &f->pool, key, rate, &f->device);
}

static void uninit(fixture *f)
{
	if (f->device)
		OK(td_device_close(&f->device));
	OK(td_backend_close(&f->backend));
	td_pcm_clear(&f->pool);
	OK(td_pcm_retire(&f->generation));
	td_arena_destroy(f->arena);
	ck_assert(th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
}

START_TEST(lifecycle)
{
	fixture f = {0};
	init(&f, false);
	OK(open_device(&f, (teder_blob){0}, 0));
	ck_assert(td_backend_close(&f.backend) == TEDER_BUSY && f.backend);
	td_device_info info;
	OK(td_device_get_info(f.device, &info));
	ck_assert(info.backend == 1 && info.rate == 48000 && info.channels == 2 &&
	          info.period_frames == 8);
	ck_assert(info.native_rate == 48000 && info.native_channels == 2 &&
	          info.native_format == ma_format_f32);

	ck_assert(td_device_configure(f.device, (td_audio_config){1, NAN, 1, 0}) == TEDER_INVALID);
	OK(td_device_configure(f.device, (td_audio_config){1, 0.5f, 1, 0}));
	OK(td_device_start(f.device));
	OK(td_device_start(f.device));
	ck_assert(td_device_configure(f.device, (td_audio_config){0, 1, 0, 0}) == TEDER_BUSY);
	td_device_report report;
	while (td_device_callbacks(f.device) != 1)
		pause_for_device();
	OK(td_device_get_report(f.device, &report));
	ck_assert(report.generation == 1 && report.final == 1 && report.pcm_frames == 3 &&
	          report.missing_frames == 5 && report.error == TEDER_OK);

	OK(td_device_stop(f.device));
	OK(td_device_stop(f.device));
	OK(td_device_get_report(f.device, &report));
	ck_assert(f.valid && report.pcm_frames == 3 && report.missing_frames == 5 &&
	          report.error == TEDER_OK);
	ck_assert(td_device_callbacks(f.device) == 1 && f.released == 0);
	ck_assert(td_pcm_reap(&f.pool) == 1);

	OK(td_device_configure(f.device, (td_audio_config){0, 1, 0, 0}));
	OK(td_device_start(f.device));
	OK(td_device_stop(f.device));
	OK(td_device_get_report(f.device, &report));
	ck_assert(report.generation == 0 && report.pcm_frames == 0 && report.missing_frames == 0 &&
	          report.final == 0 && td_device_callbacks(f.device) == 2);

	/* Counter exhaustion cannot wrap the snapshot version and admit ABA. */
	td_device_feed *feed = f.native->pUserData;
	atomic_store(&feed->sequence, UINT64_MAX - 1);
	float silence[2] = {1, 1};
	f.native->onData(f.native, silence, NULL, 1);
	ck_assert(silence[0] == 0 && silence[1] == 0);
	ck_assert(td_device_get_report(f.device, &report) == TEDER_LIMIT);

	uninit(&f);
	ck_assert(f.initialized == 1 && f.released == 1);
}
END_TEST

START_TEST(controls)
{
	fixture f = {0};
	init(&f, false);
	OK(open_device(&f, (teder_blob){0}, 0));
	OK(td_device_configure(f.device, (td_audio_config){1, 1, 0, 0}));
	OK(td_device_start(f.device));
	join(&f); /* the synthetic native endpoint is now driven serially below */
	uint64_t revision = 99;
	ck_assert(td_device_control(f.device, (td_audio_config){2, 1, 1, 0}, &revision) == TEDER_STALE);
	ck_assert(!revision);
	ck_assert(td_device_control(f.device, (td_audio_config){1, NAN, 1, 0}, &revision) ==
	          TEDER_INVALID);
	float pcm[2];
	OK(td_device_control(f.device, (td_audio_config){1, 0.25f, 1, 0}, &revision));
	ck_assert(revision == 2);
	f.native->onData(f.native, pcm, NULL, 1);
	ck_assert(pcm[0] == 0.0625f && pcm[1] == -0.0625f);
	td_device_report report;
	OK(td_device_get_report(f.device, &report));
	ck_assert(report.control == 2 && report.pcm_frames == 1 && !report.final);

	OK(td_device_control(f.device, (td_audio_config){1, 0.5f, 0, 0}, &revision));
	f.native->onData(f.native, pcm, NULL, 1);
	OK(td_device_get_report(f.device, &report));
	ck_assert(pcm[0] == 0 && pcm[1] == 0 && report.control == 4 && report.pcm_frames == 1 &&
	          !report.missing_frames);
	OK(td_device_control(f.device, (td_audio_config){1, 0.5f, 1, 1}, &revision));
	f.native->onData(f.native, pcm, NULL, 1);
	OK(td_device_get_report(f.device, &report));
	ck_assert(pcm[0] == 0 && pcm[1] == 0 && report.control == 6 && report.pcm_frames == 2);

	td_device_feed *feed = f.native->pUserData;
	atomic_store(&feed->wanted, 7); /* interrupted publication retains the complete old pair */
	atomic_store(&feed->settings, td_device_pack((td_audio_config){1, 1, 1, 0}));
	f.native->onData(f.native, pcm, NULL, 1);
	OK(td_device_get_report(f.device, &report));
	ck_assert(pcm[0] == 0 && pcm[1] == 0 && report.control == 6 && report.final == 1);
	atomic_store(&feed->wanted, UINT64_MAX - 1);
	ck_assert(td_device_control(f.device, (td_audio_config){1, 1, 1, 0}, &revision) == TEDER_LIMIT);
	ck_assert(!revision && atomic_load(&feed->wanted) == UINT64_MAX - 1);
	uninit(&f);
}
END_TEST

START_TEST(clock_counts)
{
	fixture f = {0};
	init(&f, false);
	OK(open_device(&f, (teder_blob){0}, 0));
	OK(td_device_configure(f.device, (td_audio_config){1, 1, 0, 0}));
	OK(td_device_start(f.device));
	join(&f);
	td_device_feed *feed = f.native->pUserData;
	float pcm[22];
	uint64_t revision;
	td_device_report r;
	OK(td_device_get_report(f.device, &r));
	ck_assert(!r.output_frames && !r.underrun_frames && !r.source);
	OK(td_device_control(f.device, (td_audio_config){1, 1, 1, 0}, &revision));
	f.native->onData(f.native, pcm, NULL, 1);
	OK(td_device_get_report(f.device, &r));
	ck_assert(r.source == 1 && r.source_frame == 1 && r.output_frames == 1 && !r.underrun_frames);
	f.native->onData(f.native, pcm, NULL, 11);
	OK(td_device_get_report(f.device, &r));
	ck_assert(r.source_frame == 3 && r.output_frames == 3 && !r.underrun_frames && r.final);
	f.native->onData(f.native, pcm, NULL, 7);
	OK(td_device_get_report(f.device, &r));
	ck_assert(r.source_frame == 3 && r.output_frames == 3 && !r.underrun_frames);

	OK(td_device_stop(f.device));
	OK(td_device_configure(f.device, (td_audio_config){2, 1, 1, 0}));
	f.native->onData(f.native, pcm, NULL, 7); /* empty generation: true starvation */
	OK(td_device_get_report(f.device, &r));
	ck_assert(r.generation == 2 && r.output_frames == 7 && r.underrun_frames == 7 && !r.source);
	atomic_store(&feed->sequence, 3); /* failed coherent read must not modify caller storage */
	td_device_report saved = r;
	ck_assert(td_device_get_report(f.device, &r) == TEDER_AGAIN && !memcmp(&r, &saved, sizeof(r)));
	atomic_store(&feed->sequence, 4);
	feed->report.output_frames = UINT64_MAX - 2;
	feed->report.underrun_frames = UINT64_MAX - 2;
	f.native->onData(f.native, pcm, NULL, 7);
	OK(td_device_get_report(f.device, &r));
	ck_assert(r.output_frames == UINT64_MAX && r.underrun_frames == UINT64_MAX &&
	          r.error == TEDER_LIMIT);
	uninit(&f);
}
END_TEST

START_TEST(failures)
{
	for (unsigned mode = 0; mode < 6; mode++) {
		fixture f = {0};
		init(&f, false);
		f.reject = mode == 0;
		f.bad_format = mode == 1;
		if (mode == 2 || mode == 3)
			f.tracker.fail_at = f.tracker.attempts + (mode == 2 ? 1 : 2);
		teder_code code = open_device(&f, (teder_blob){0}, mode == 3 ? 44100 : 0);
		if (mode < 4) {
			ck_assert(code == (mode < 2 ? TEDER_DEVICE_LOST : TEDER_NO_MEMORY) && !f.device);
			ck_assert(f.initialized == f.released);
		} else {
			OK(code);
			f.fail_start = mode == 4;
			f.fail_stop = mode == 5;
			OK(td_device_configure(f.device, (td_audio_config){1, 0.5f, 1, 0}));
			code = td_device_start(f.device);
			if (mode == 5) {
				OK(code);
				code = td_device_stop(f.device);
			}
			ck_assert(code == TEDER_DEVICE_LOST && !f.worker && f.released == 1);
			ck_assert(td_device_start(f.device) == TEDER_BAD_STATE);
			ck_assert(td_device_configure(f.device, (td_audio_config){0, 1, 0, 0}) ==
			          TEDER_BAD_STATE);
		}
		uninit(&f);
	}

	fixture f = {0};
	init(&f, false);
	teder_result *r;
	OK(td_backend_enumerate(f.backend, 8192, 272, &r));
	teder_device_info entry = {.struct_size = sizeof(entry), .abi_version = TEDER_ABI_VERSION};
	OK(teder_result_device(r, 0, &entry));
	unsigned char key[20];
	ck_assert(entry.key.bytes == sizeof(key));
	memcpy(key, entry.key.data, sizeof(key));
	teder_result_release(r);
	key[4] = 2;
	ck_assert(open_device(&f, (teder_blob){key, sizeof(key)}, 0) == TEDER_STALE && !f.device);
	key[4] = 1;
	OK(open_device(&f, (teder_blob){key, sizeof(key)}, 44100));
	td_device_info info;
	OK(td_device_get_info(f.device, &info));
	ck_assert(info.rate == 44100 && info.native_rate == 48000 && info.period_frames == 0);
	uninit(&f);
}
END_TEST

typedef struct {
	fixture *f;
	atomic_uint done;
	teder_code code;
} closer;

static void close_device(void *context)
{
	closer *c = context;
	c->code = td_device_close(&c->f->device);
	atomic_store(&c->done, 1);
}

START_TEST(closing)
{
	fixture f = {.gated = true};
	init(&f, false);
	ck_assert(th_gate_init(&f.entered) && th_gate_init(&f.ending));
	OK(open_device(&f, (teder_blob){0}, 0));
	OK(td_device_configure(f.device, (td_audio_config){1, 0.5f, 1, 0}));
	OK(td_device_start(f.device));
	th_gate_await(&f.entered);

	closer c = {.f = &f};
	atomic_init(&c.done, 0);
	th_thread owner;
	ck_assert(th_thread_start(&owner, close_device, &c));
	th_gate_await(&f.ending);
	ck_assert(atomic_load(&c.done) == 0 && f.released == 0);
	ck_assert(td_backend_close(&f.backend) == TEDER_BUSY);

	th_gate_release(&f.ending);
	th_gate_release(&f.entered);
	th_thread_join(&owner);
	OK(c.code);
	ck_assert(atomic_load(&c.done) == 1 && !f.device && f.released == 1 && f.valid);
	th_gate_uninit(&f.entered);
	th_gate_uninit(&f.ending);
	uninit(&f);
}
END_TEST

START_TEST(notifications)
{
	const ma_device_notification_type types[] = {ma_device_notification_type_started,
	                                             ma_device_notification_type_stopped,
	                                             ma_device_notification_type_rerouted,
	                                             ma_device_notification_type_interruption_began,
	                                             ma_device_notification_type_interruption_ended,
	                                             ma_device_notification_type_unlocked};
	ck_assert(!td_device_lost(NULL));
	for (unsigned i = 0; i < sizeof(types) / sizeof(*types); i++) {
		fixture f = {0};
		init(&f, false);
		OK(open_device(&f, (teder_blob){0}, 0));
		notify(f.native, ma_device_notification_type_stopped); /* expected before start */
		ck_assert(!td_device_lost(f.device));
		OK(td_device_configure(f.device, (td_audio_config){1, 0.5f, 0, 0}));
		OK(td_device_start(f.device));
		join(&f);
		uint64_t revision;
		OK(td_device_control(f.device, (td_audio_config){1, 0.5f, 1, 0}, &revision));
		size_t attempts = f.tracker.attempts, bytes = f.tracker.bytes;
		bool lost = i >= 1 && i <= 3;
		notify(f.native, types[i]);
		notify(f.native, types[i]); /* duplicate reports are idempotent */
		ck_assert(td_device_lost(f.device) == lost);
		float pcm[4] = {7, 7, 11, 13};
		f.native->onData(f.native, pcm, NULL, 1);
		td_device_report report;
		OK(td_device_get_report(f.device, &report));
		ck_assert(pcm[0] == (lost ? 0 : 0.125f) && pcm[1] == -pcm[0] && pcm[2] == 11 &&
		          pcm[3] == 13);
		ck_assert(report.generation == 1 && report.pcm_frames == (lost ? 0 : 1) &&
		          report.output_frames == (lost ? 0 : 1) && !report.underrun_frames &&
		          !report.error && !report.final);
		ck_assert(f.tracker.attempts == attempts && f.tracker.bytes == bytes && !f.released);
		if (lost) {
			ck_assert(td_device_start(f.device) == TEDER_DEVICE_LOST);
			ck_assert(td_device_control(f.device, (td_audio_config){1, 1, 1, 0}, &revision) ==
			              TEDER_DEVICE_LOST &&
			          !revision);
		}
		notify(f.native, ma_device_notification_type_interruption_ended);
		notify(f.native, ma_device_notification_type_started);
		ck_assert(td_device_stop(f.device) == (lost ? TEDER_DEVICE_LOST : TEDER_OK));
		if (!lost)
			notify(f.native, ma_device_notification_type_stopped); /* intentional stop */
		ck_assert(td_device_lost(f.device) == lost);
		if (lost) {
			ck_assert(td_device_configure(f.device, (td_audio_config){2, 1, 1, 0}) ==
			          TEDER_BAD_STATE);
			OK(td_device_close(&f.device));
			OK(open_device(&f, (teder_blob){0}, 0));
			ck_assert(!td_device_lost(f.device));
			OK(td_device_configure(f.device, (td_audio_config){1, 0.5f, 1, 0}));
			OK(td_device_start(f.device));
			OK(td_device_stop(f.device));
			OK(td_device_get_report(f.device, &report));
			ck_assert(f.valid && report.pcm_frames == 3 && report.final &&
			          !td_device_lost(f.device));
		}
		uninit(&f);
		ck_assert(f.initialized == (lost ? 2 : 1) && f.released == f.initialized);
	}
}
END_TEST

START_TEST(loss_state)
{
	fixture f = {0};
	init(&f, false);
	OK(open_device(&f, (teder_blob){0}, 0));
	OK(td_device_configure(f.device, (td_audio_config){1, 1, 0, 0}));
	OK(td_device_start(f.device));
	join(&f);
	/* Emulate the pinned worker's missing notification, through real native state
	 * transitions. Callback pointers are changed only after synthetic users join. */
	f.native->onNotification = NULL;
	ck_assert(ma_device_stop(f.native) == MA_SUCCESS);
	f.native->onNotification = td_device_notification;
	ck_assert(ma_device_get_state(f.native) == ma_device_state_stopped);
	td_device_feed *feed = f.native->pUserData;
	ck_assert(!atomic_load(&feed->lost) && td_device_running(f.device));
	ck_assert(td_device_lost(f.device) && atomic_load(&feed->lost));
	ck_assert(td_device_start(f.device) == TEDER_DEVICE_LOST);
	ck_assert(td_device_stop(f.device) == TEDER_DEVICE_LOST);
	ck_assert(td_device_lost(f.device) && !td_device_running(f.device));
	uninit(&f);
	ck_assert(f.initialized == 1 && f.released == 1);
}
END_TEST

START_TEST(loss_startup)
{
	for (unsigned starting = 0; starting < 2; starting++) {
		fixture f = {.loss_on_init = !starting, .loss_on_start = starting};
		init(&f, false);
		teder_code code = open_device(&f, (teder_blob){0}, 0);
		if (starting) {
			OK(code);
			OK(td_device_configure(f.device, (td_audio_config){1, 1, 0, 0}));
			ck_assert(td_device_start(f.device) == TEDER_DEVICE_LOST);
			ck_assert(td_device_start(f.device) == TEDER_BAD_STATE && !f.worker);
		} else
			ck_assert(code == TEDER_DEVICE_LOST && !f.device);
		ck_assert(f.initialized == 1 && f.released == 1);
		uninit(&f);
	}
}
END_TEST

static void delayed_notification(void *context)
{
	fixture *f = context;
	ma_device *borrow = f->native;
	th_gate_hit(&f->notifying);
	notify(borrow, ma_device_notification_type_stopped);
}

START_TEST(loss_closing)
{
	fixture f = {.gated = true, .expect_silence = true};
	init(&f, false);
	ck_assert(th_gate_init(&f.entered) && th_gate_init(&f.ending) && th_gate_init(&f.notifying));
	OK(open_device(&f, (teder_blob){0}, 0));
	OK(td_device_configure(f.device, (td_audio_config){1, 0.5f, 1, 0}));
	OK(td_device_start(f.device));
	th_gate_await(&f.entered);
	f.notification_worker = th_thread_start(&f.notifications, delayed_notification, &f);
	ck_assert(f.notification_worker);
	th_gate_await(&f.notifying);
	notify(f.native, ma_device_notification_type_rerouted);
	ck_assert(td_device_lost(f.device));
	size_t bytes = f.tracker.bytes, attempts = f.tracker.attempts;
	closer c = {.f = &f};
	atomic_init(&c.done, 0);
	th_thread owner;
	ck_assert(th_thread_start(&owner, close_device, &c));
	th_gate_await(&f.ending);
	ck_assert(!atomic_load(&c.done) && !f.released && f.tracker.bytes == bytes &&
	          f.tracker.attempts == attempts);
	ck_assert(td_backend_close(&f.backend) == TEDER_BUSY);
	th_gate_release(&f.ending);
	th_gate_release(&f.entered);
	while (!atomic_load(&f.waiting_notifications))
		pause_for_device();
	ck_assert(!atomic_load(&c.done) && !f.released); /* data returned; notifier still borrows */
	th_gate_release(&f.notifying);
	th_thread_join(&owner);
	OK(c.code);
	ck_assert(!f.device && f.released == 1 && f.valid && !f.notification_worker);
	th_gate_uninit(&f.entered);
	th_gate_uninit(&f.ending);
	th_gate_uninit(&f.notifying);
	uninit(&f);
}
END_TEST

static void repeated_notifications(void *context)
{
	ma_device *device = context;
	for (unsigned i = 0; i < 10000; i++)
		notify(device,
		       i % 2 ? ma_device_notification_type_rerouted
		             : ma_device_notification_type_interruption_ended);
}

START_TEST(loss_race)
{
	fixture f = {0};
	init(&f, false);
	OK(open_device(&f, (teder_blob){0}, 0));
	OK(td_device_configure(f.device, (td_audio_config){1, 1, 0, 0}));
	OK(td_device_start(f.device));
	join(&f);
	uint64_t revision;
	OK(td_device_control(f.device, (td_audio_config){1, 1, 1, 0}, &revision));
	float pcm[2];
	f.native->onData(f.native, pcm, NULL, 1);
	ck_assert(pcm[0] == 0.25f && pcm[1] == -0.25f);
	notify(f.native, ma_device_notification_type_rerouted);
	th_thread notifier;
	ck_assert(th_thread_start(&notifier, repeated_notifications, f.native));
	for (unsigned i = 0; i < 10000; i++) {
		f.native->onData(f.native, pcm, NULL, 1);
		ck_assert(pcm[0] == 0 && pcm[1] == 0);
	}
	th_thread_join(&notifier);
	td_device_report report;
	OK(td_device_get_report(f.device, &report));
	ck_assert(report.source == 1 && report.source_frame == 1 && report.output_frames == 1 &&
	          report.pcm_frames == 1 && !report.underrun_frames && !report.final && !report.error);
	ck_assert(td_device_lost(f.device));
	uninit(&f);
}
END_TEST

START_TEST(unwind)
{
	fixture f = {.reject = true};
	init(&f, false);
#ifdef _WIN32
	DWORD before, after;
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &before));
#endif
	for (unsigned i = 0; i < 16; i++)
		ck_assert(open_device(&f, (teder_blob){0}, 0) == TEDER_DEVICE_LOST && !f.device);
#ifdef _WIN32
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &after));
	ck_assert(before == after);
#endif
	ck_assert(f.initialized == 0 && f.released == 0);
	uninit(&f);
}
END_TEST

START_TEST(native)
{
	fixture f = {0};
	init(&f, true);
	teder_result *r;
	OK(td_backend_enumerate(f.backend, 65536, 272, &r));
	teder_blob key = {0};
#ifndef _WIN32
	/* Headless Linux explicitly selects its enumerated ALSA entry. */
	teder_device_info entry = {.struct_size = sizeof(entry), .abi_version = TEDER_ABI_VERSION};
	OK(teder_result_device(r, 0, &entry));
	key = entry.key;
#endif
	size_t before = f.tracker.attempts;
	OK(open_device(&f, key, 0));
	size_t attempts = f.tracker.attempts - before;
	td_device_info info;
	OK(td_device_get_info(f.device, &info));
	printf("backend=%u rate=%u channels=%u period=%u native-rate=%u native-format=%u\n",
	       info.backend,
	       info.rate,
	       info.channels,
	       info.period_frames,
	       info.native_rate,
	       info.native_format);
	OK(td_device_configure(f.device, (td_audio_config){1, 1, 1, 0}));
	OK(td_device_start(f.device));
	for (unsigned i = 0; i < 5000 && f.generation->references; i++) {
		td_pcm_reap(&f.pool);
		if (f.generation->references)
			pause_for_device();
	}
	OK(td_device_stop(f.device));
	td_device_report report;
	OK(td_device_get_report(f.device, &report));
	ck_assert(report.error == TEDER_OK && report.pcm_frames == 3 &&
	          td_device_callbacks(f.device) > 0);
	ck_assert(!td_device_lost(f.device));

	OK(td_device_close(&f.device));
	teder_result_release(r);
	uninit(&f);
	printf("Native silence submitted; device init allocation sites=%zu\n", attempts);

	for (size_t fail = 1; fail <= attempts; fail++) {
		fixture fault = {0};
		init(&fault, true);
		OK(td_backend_enumerate(fault.backend, 65536, 272, &r));
#ifndef _WIN32
		OK(teder_result_device(r, 0, &entry));
		key = entry.key;
#endif
		fault.tracker.fail_at = fault.tracker.attempts + fail;
		ck_assert(open_device(&fault, key, 0) == TEDER_NO_MEMORY && !fault.device);
		teder_result_release(r);
		uninit(&fault);
	}
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "lifecycle"))
		test = lifecycle;
	else if (!strcmp(argv[1], "controls"))
		test = controls;
	else if (!strcmp(argv[1], "clock-counts"))
		test = clock_counts;
	else if (!strcmp(argv[1], "failures"))
		test = failures;
	else if (!strcmp(argv[1], "closing"))
		test = closing;
	else if (!strcmp(argv[1], "unwind"))
		test = unwind;
	else if (!strcmp(argv[1], "native"))
		test = native;
	else if (!strcmp(argv[1], "notifications"))
		test = notifications;
	else if (!strcmp(argv[1], "loss-state"))
		test = loss_state;
	else if (!strcmp(argv[1], "loss-startup"))
		test = loss_startup;
	else if (!strcmp(argv[1], "loss-closing"))
		test = loss_closing;
	else if (!strcmp(argv[1], "loss-race"))
		test = loss_race;
	else
		return 2;
	Suite *suite = suite_create("device");
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
