#ifndef TEDER_DEVICE_H
#define TEDER_DEVICE_H
#include "backend.h"

typedef struct td_device td_device;
typedef struct {
	uint32_t backend, rate, channels, period_frames;
	uint32_t native_rate, native_channels, native_format;
} td_device_info;
typedef struct {
	uint64_t pcm_frames, missing_frames;
	teder_code error;
} td_device_report;
/* Lifecycle-owner calls only; never on the callback/UI thread. Reserve the
 * device arena plus backend's existing heap reservation before opening.
 * Pool is exclusively consumed by this device while started and must outlive
 * close. No pool clear or generation destruction occurs here. Empty key selects
 * the current default, pinned to its ID; no automatic default-device following.
 * rate=0 negotiates native rate. The caller prepares PCM at info.rate. */
teder_code td_device_plan(size_t *reservation);
teder_code td_device_open(td_backend *, td_pcm_pool *, teder_blob key, uint32_t rate, td_device **);
teder_code td_device_get_info(td_device *, td_device_info *);
/* Configuration is immutable between start and successful stop. Live control
 * publication is not implemented yet; these setters return BUSY while a callback can run. */
teder_code td_device_configure(td_device *, td_audio_config);
teder_code td_device_start(td_device *);
teder_code td_device_stop(td_device *);
teder_code td_device_get_report(td_device *, td_device_report *);
/* Observation only, modulo 2^32. Never a substitute for native quiescence. */
uint32_t td_device_callbacks(td_device *);
/* Uninit joins native users even after a start/stop failure, then returns the
 * context loan and frees the entire device arena. Clears the owner's pointer. */
teder_code td_device_close(td_device **);

/* Stable userdata in the device arena. Config is owner-written only while
 * quiescent; report is callback-written and owner-read only after quiescence. */
typedef struct {
	td_pcm_pool *pool;
	td_audio_config config;
	td_device_report report;
	atomic_uint calls;
} td_device_feed;
void td_device_callback(ma_device *, void *, const void *, ma_uint32);
#endif
