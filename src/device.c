#include "device.h"
#include <string.h>

struct td_device {
	td_arena *arena;
	td_backend *backend;
	td_backend_loan loan;
	ma_device native;
	td_device_feed feed;
	td_device_info info;
	bool initialized, active, failed;
};

teder_code td_device_plan(size_t *reservation)
{
	if (!reservation)
		return TEDER_INVALID;
	return td_arena_size(sizeof(td_device), reservation) ? TEDER_OK : TEDER_LIMIT;
}

static teder_code outcome(td_device *d, ma_result result)
{
	teder_code code = td_backend_error(d->backend);
	return code != TEDER_OK ? code : result == MA_SUCCESS ? TEDER_OK : TEDER_DEVICE_LOST;
}

teder_code td_device_open(
    td_backend *backend, td_pcm_pool *pool, teder_blob key, uint32_t rate, td_device **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!pool || rate > 1048575)
		return TEDER_INVALID;
	td_backend_loan loan;
	teder_code code = td_backend_borrow(backend, key, &loan);
	if (code != TEDER_OK)
		return code;
	td_arena *arena = td_arena_create(loan.allocator, sizeof(td_device));
	if (!arena) {
		td_backend_return(backend);
		return TEDER_NO_MEMORY;
	}
	td_device *d = td_arena_alloc(arena, 1, sizeof(*d), _Alignof(td_device));
	memset(d, 0, sizeof(*d));
	d->arena = arena;
	d->backend = backend;
	d->loan = loan;
	d->feed.pool = pool;
	d->feed.config.gain = 1;
	atomic_init(&d->feed.calls, 0);
	atomic_init(&d->feed.sequence, 0);
	atomic_init(&d->feed.pcm_frames, 0);
	atomic_init(&d->feed.missing_frames, 0);
	atomic_init(&d->feed.source, 0);
	atomic_init(&d->feed.track, 0);
	atomic_init(&d->feed.source_frame, 0);
	atomic_init(&d->feed.output_frames, 0);
	atomic_init(&d->feed.underrun_frames, 0);
	atomic_init(&d->feed.final, 0);
	atomic_init(&d->feed.error, TEDER_OK);
	atomic_init(&d->feed.control, 0);
	atomic_init(&d->feed.wanted, 0);
	atomic_init(&d->feed.settings, 0);
	ma_device_config config = ma_device_config_init(ma_device_type_playback);
	config.playback.format = ma_format_f32;
	config.playback.channels = 2;
	config.playback.pDeviceID = &d->loan.id;
	config.sampleRate = rate;
	config.dataCallback = td_device_callback;
	config.pUserData = &d->feed;
	config.noPreSilencedOutputBuffer = MA_TRUE;
	config.noFixedSizedCallback = MA_TRUE;
	config.wasapi.noAutoStreamRouting = MA_TRUE;
	ma_result result = ma_device_init(loan.context, &config, &d->native);
	d->initialized = result == MA_SUCCESS;
	code = outcome(d, result);
	if (code == TEDER_OK &&
	    (d->native.playback.format != ma_format_f32 || d->native.playback.channels != 2 ||
	     !d->native.sampleRate || d->native.sampleRate > 1048575))
		code = TEDER_UNSUPPORTED;
	if (code != TEDER_OK) {
		td_device_close(&d);
		return code;
	}
	d->info = (td_device_info){loan.backend,
	                           d->native.sampleRate,
	                           2,
	                           d->native.sampleRate == d->native.playback.internalSampleRate
	                               ? d->native.playback.internalPeriodSizeInFrames
	                               : 0,
	                           d->native.playback.internalSampleRate,
	                           d->native.playback.internalChannels,
	                           (uint32_t)d->native.playback.internalFormat};
	*out = d;
	return TEDER_OK;
}

teder_code td_device_get_info(td_device *d, td_device_info *out)
{
	if (!d || !out)
		return TEDER_INVALID;
	*out = d->info;
	return TEDER_OK;
}

teder_code td_device_configure(td_device *d, td_audio_config config)
{
	if (!d || !(config.gain >= 0 && config.gain <= 1) || config.enabled > 1 || config.mute > 1 ||
	    (config.enabled && !config.generation))
		return TEDER_INVALID;
	if (d->active)
		return TEDER_BUSY;
	if (d->failed)
		return TEDER_BAD_STATE;
	if (config.generation && config.generation < d->feed.pool->selected)
		return TEDER_STALE;
	if (config.generation != d->feed.config.generation) {
		d->feed.report = (td_device_report){.generation = config.generation};
		atomic_store(&d->feed.sequence, 0);
		atomic_store(&d->feed.pcm_frames, 0);
		atomic_store(&d->feed.missing_frames, 0);
		atomic_store(&d->feed.source, 0);
		atomic_store(&d->feed.track, 0);
		atomic_store(&d->feed.source_frame, 0);
		atomic_store(&d->feed.output_frames, 0);
		atomic_store(&d->feed.underrun_frames, 0);
		atomic_store(&d->feed.final, 0);
		atomic_store(&d->feed.error, TEDER_OK);
	}
	d->feed.config = config;
	d->feed.applied = td_device_pack(config);
	d->feed.report.control = 0;
	atomic_store(&d->feed.control, 0);
	atomic_store(&d->feed.wanted, 0);
	atomic_store(&d->feed.settings, d->feed.applied);
	return TEDER_OK;
}

teder_code td_device_control(td_device *d, td_audio_config config, uint64_t *revision)
{
	if (!revision)
		return TEDER_INVALID;
	*revision = 0;
	if (!d || !(config.gain >= 0 && config.gain <= 1) || config.mute > 1 || config.enabled > 1)
		return TEDER_INVALID;
	if (!d->active || d->failed)
		return TEDER_BAD_STATE;
	if (config.generation != d->feed.config.generation)
		return TEDER_STALE;
	uint64_t before = atomic_load(&d->feed.wanted);
	if (before >= UINT64_MAX - 1)
		return TEDER_LIMIT;
	/* All fields are atomic; the callback retains its previous settings if
	 * this bounded read overlaps publication. Revisions never wrap. */
	atomic_store(&d->feed.wanted, before + 1);
	atomic_store(&d->feed.settings, td_device_pack(config));
	atomic_store(&d->feed.wanted, before + 2);
	*revision = before + 2;
	return TEDER_OK;
}

/* Failed native start/stop can leave backend state uncertain. Uninit is the
 * terminal quiescence operation; never allow config reuse after that failure. */
static teder_code failed(td_device *d, teder_code code)
{
	ma_device_uninit(&d->native);
	d->initialized = false;
	d->active = false;
	d->failed = true;
	return code;
}

teder_code td_device_start(td_device *d)
{
	if (!d)
		return TEDER_INVALID;
	if (d->failed)
		return TEDER_BAD_STATE;
	if (d->active)
		return TEDER_OK;
	d->active = true; /* start may render its initial buffer before returning */
	teder_code code = outcome(d, ma_device_start(&d->native));
	return code == TEDER_OK ? code : failed(d, code);
}

bool td_device_running(td_device *d)
{
	return d && d->active;
}

teder_code td_device_stop(td_device *d)
{
	if (!d)
		return TEDER_INVALID;
	if (d->failed)
		return TEDER_BAD_STATE;
	if (!d->active)
		return TEDER_OK;
	teder_code code = outcome(d, ma_device_stop(&d->native));
	if (code != TEDER_OK)
		return failed(d, code);
	d->active = false;
	return TEDER_OK;
}

teder_code td_device_get_report(td_device *d, td_device_report *out)
{
	if (!d || !out)
		return TEDER_INVALID;
	uint64_t before = atomic_load(&d->feed.sequence);
	if (before == UINT64_MAX)
		return TEDER_LIMIT;
	if (before & 1)
		return TEDER_AGAIN;
	td_device_report report = {.generation = d->feed.config.generation,
	                           .source = atomic_load(&d->feed.source),
	                           .track = atomic_load(&d->feed.track),
	                           .source_frame = atomic_load(&d->feed.source_frame),
	                           .output_frames = atomic_load(&d->feed.output_frames),
	                           .underrun_frames = atomic_load(&d->feed.underrun_frames),
	                           .pcm_frames = atomic_load(&d->feed.pcm_frames),
	                           .control = atomic_load(&d->feed.control),
	                           .missing_frames = atomic_load(&d->feed.missing_frames),
	                           .final = atomic_load(&d->feed.final),
	                           .error = atomic_load(&d->feed.error)};
	if (before != atomic_load(&d->feed.sequence))
		return TEDER_AGAIN;
	*out = report;
	return TEDER_OK;
}

uint32_t td_device_callbacks(td_device *d)
{
	return atomic_load_explicit(&d->feed.calls, memory_order_acquire);
}

teder_code td_device_close(td_device **owner)
{
	if (!owner || !*owner)
		return TEDER_INVALID;
	td_device *d = *owner;
	if (d->initialized)
		ma_device_uninit(&d->native);
	td_backend_return(d->backend);
	td_arena_destroy(d->arena);
	*owner = NULL;
	return TEDER_OK;
}
