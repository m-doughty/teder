#ifndef TEDER_DEVICE_H
#define TEDER_DEVICE_H
#include "backend.h"
#include <string.h>

typedef struct td_device td_device;
typedef struct {
	uint32_t backend, rate, channels, period_frames;
	uint32_t native_rate, native_channels, native_format;
} td_device_info;
typedef struct {
	uint64_t generation, pcm_frames, missing_frames, control;
	uint64_t source, track, source_frame, output_frames, underrun_frames;
	uint32_t final;
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
/* Generation configuration requires stopped output and quiescent report readers. */
teder_code td_device_configure(td_device *, td_audio_config);
/* Lifecycle owner publishes one control at a time for the configured generation.
 * Completion requires report.control == revision. No native stop or allocation. */
teder_code td_device_control(td_device *, td_audio_config, uint64_t *revision);
teder_code td_device_start(td_device *);
bool td_device_running(td_device *); /* lifecycle-owner observation, not a join */
/* Sticky native loss, including an unexpected stopped native state. Owner or
 * delegated report reader only; excludes concurrent lifecycle changes. This is
 * observation, never proof of callback/notification quiescence. Replace to reset. */
bool td_device_lost(td_device *);
/* Known loss bypasses native stop and uninitializes, returning DEVICE_LOST.
 * The report stays readable until close; this instance cannot restart. */
teder_code td_device_stop(td_device *);
/* One bounded coherent read, including while running; AGAIN retries later.
 * A delegated producer may borrow this with lifecycle/configuration held stable.
 * Quiesce that reader before reconfiguration or device destruction. */
teder_code td_device_get_report(td_device *, td_device_report *);
/* Observation only, modulo 2^32. Never a substitute for native quiescence. */
uint32_t td_device_callbacks(td_device *);
/* Uninit joins native users even after a start/stop failure, then returns the
 * context loan and frees the entire device arena. Clears the owner's pointer. */
teder_code td_device_close(td_device **);

/* Stable userdata in the device arena. Config is owner-written only while
 * quiescent. The callback owns report; live readers use atomic publication. */
typedef struct {
	td_pcm_pool *pool;
	td_audio_config config;
	td_device_report report;
	atomic_uint calls, final, expected_running, lost;
	atomic_int error;
	atomic_uint_fast64_t sequence, pcm_frames, missing_frames, control;
	atomic_uint_fast64_t source, track, source_frame, output_frames, underrun_frames;
	atomic_uint_fast64_t wanted, settings; /* owner publishes; callback takes one bounded read */
	uint64_t applied; /* callback-owned cached settings; initialized at quiescence */
} td_device_feed;
static inline uint64_t td_device_pack(td_audio_config config)
{
	uint32_t bits;
	memcpy(&bits, &config.gain, sizeof(bits));
	return bits | ((uint64_t)config.mute << 32) | ((uint64_t)config.enabled << 33);
}
void td_device_callback(ma_device *, void *, const void *, ma_uint32);
void td_device_notification(const ma_device_notification *);
#endif
