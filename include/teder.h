#ifndef TEDER_H
#define TEDER_H

/* ABI draft 1; not a released library. See README.md for implemented symbols.
 * Read docs/api-contract.md for states, bounds, threading and ownership.
 * No function invokes application callbacks. No public call is audio-safe.
 */
#include <stdint.h>
#include "teder/export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEDER_ABI_VERSION UINT32_C(1)
typedef int32_t teder_code;
typedef struct teder_engine teder_engine;
typedef struct teder_result teder_result;
typedef struct teder_snapshot teder_snapshot;

/* Constants are not enum-typed ABI parameters. IDs are uint64_t; zero is none. */
enum {
	TEDER_OK = 0, TEDER_AGAIN = 1,
	TEDER_INVALID = -1, TEDER_ABI_MISMATCH = -2, TEDER_LIMIT = -3,
	TEDER_NO_MEMORY = -4, TEDER_BUSY = -5, TEDER_CLOSED = -6,
	TEDER_NOT_FOUND = -7, TEDER_BAD_STATE = -8, TEDER_STALE = -9,
	TEDER_CANCELLED = -10, TEDER_TOO_LATE = -11, TEDER_UNSUPPORTED = -12,
	TEDER_IO = -13, TEDER_CORRUPT = -14, TEDER_DEVICE_LOST = -15,
	TEDER_ENCODING = -16, TEDER_BUFFER_TOO_SMALL = -17, TEDER_INTERNAL = -18
};
enum { TEDER_LIVE = 1, TEDER_CLOSING, TEDER_CLOSED_STATE };
enum {
	TEDER_EMPTY = 1, TEDER_READY, TEDER_PLAYING, TEDER_PAUSED,
	TEDER_DRAINING, TEDER_ENDED, TEDER_FAILED
};
enum { TEDER_OUTPUT_NONE = 0, TEDER_OUTPUT_READY, TEDER_OUTPUT_LOST };
enum { TEDER_NEXT_NONE = 0, TEDER_NEXT_READY, TEDER_NEXT_COMMITTED };
enum { TEDER_QUEUED = 1, TEDER_RUNNING, TEDER_TERMINAL };
enum {
	TEDER_OP_OPEN = 1, TEDER_OP_TRANSPORT, TEDER_OP_SEEK, TEDER_OP_GAIN,
	TEDER_OP_PREPARE, TEDER_OP_CLEAR_NEXT, TEDER_OP_PROBE,
	TEDER_OP_DEVICES, TEDER_OP_SELECT_DEVICE, TEDER_OP_ANALYSIS
};
enum { TEDER_PLAY = 1, TEDER_PAUSE, TEDER_STOP };
enum { TEDER_RESULT_NONE = 0, TEDER_RESULT_PROBE, TEDER_RESULT_DEVICES };
enum { TEDER_QUALITY_UNKNOWN = 0, TEDER_QUALITY_ESTIMATED, TEDER_QUALITY_MEASURED };
enum { TEDER_EVENT_TRANSITION = 1, TEDER_EVENT_UNDERRUN, TEDER_EVENT_DEVICE, TEDER_EVENT_ERROR };
enum { TEDER_OPEN_AUTOPLAY = 1, TEDER_PROBE_ARTWORK = 1 };
enum { TEDER_CODEC_FLAC = 1 };
enum { TEDER_BACKEND_WASAPI = 1, TEDER_BACKEND_COREAUDIO, TEDER_BACKEND_ALSA, TEDER_BACKEND_PULSE };
#define TEDER_CAP_FLAC (UINT64_C(1) << 0)
#define TEDER_CAP_GAPLESS (UINT64_C(1) << 1)
#define TEDER_CAP_ANALYSIS (UINT64_C(1) << 2)
#define TEDER_CAP_ARTWORK (UINT64_C(1) << 3)
#define TEDER_DURATION_KNOWN UINT32_C(1)
#define TEDER_DECODE_EOF UINT32_C(2)
#define TEDER_SUBMIT_EOF UINT32_C(4)
#define TEDER_AUDIBLE_END_ESTIMATED UINT32_C(8)

/* Frozen view/identity records, versioned by ABI 1. Views never own memory.
 * Text is UTF-8 with an explicit byte count; no trailing NUL is promised.
 * A result view lasts until result_release; a snapshot view until its release.
 */
typedef struct teder_text { const char *data; uint32_t bytes; } teder_text;
typedef struct teder_blob { const uint8_t *data; uint64_t bytes; } teder_blob;
typedef struct teder_target { uint64_t epoch, source; } teder_target;
typedef struct teder_span {
	uint64_t output_start, frames, source, track;
} teder_span;

/* For every sized struct: zero it, then set struct_size = sizeof(type) and
 * abi_version = TEDER_ABI_VERSION. ABI 1 requires exact size and version.
 * Reserved fields/unknown input flags must be zero. No packed structs.
 */
typedef struct teder_config {
	uint32_t struct_size, abi_version;
	uint32_t request_capacity, event_capacity, max_input_bytes, pcm_frames;
	uint32_t snapshot_slots, snapshot_loans, max_bands, max_spans;
	uint64_t engine_bytes_limit, result_bytes_limit;
} teder_config;
typedef struct teder_build_info {
	uint32_t struct_size, abi_version;
	uint64_t capabilities, backend_mask;
	uint32_t version_major, version_minor, version_patch, reserved;
} teder_build_info;
typedef struct teder_status {
	uint32_t struct_size, abi_version;
	uint32_t lifecycle, transport, output_state, clock_flags;
	teder_target current;
	uint64_t track, source_frame, duration_frames, output_frame;
	uint32_t source_rate, source_channels, output_rate, output_channels;
	uint32_t backend, latency_quality;
	uint64_t latency_frames;
	float gain;
	uint32_t mute, output_period_frames, reserved2;
	uint64_t media_request, next_source, next_track;
	uint32_t next_state, position_quality;
	uint64_t underrun_frames, analysis_dropped_frames, analysis_dropped_publications;
	uint64_t analysis_sequence, event_sequence, events_dropped;
	teder_code last_error;
	uint32_t reserved;
} teder_status;
typedef struct teder_request_info {
	uint32_t struct_size, abi_version;
	uint64_t request;
	uint32_t operation, state;
	teder_code outcome;
	uint32_t result_kind;
	teder_target committed;
	uint64_t track;
	uint32_t detail_bytes, detail_truncated;
	char detail[160];
} teder_request_info;
typedef struct teder_event {
	uint32_t struct_size, abi_version;
	uint64_t sequence, epoch, output_frame;
	uint32_t kind, reserved;
	uint64_t from_source, from_track, to_source, to_track, missing_frames;
	teder_code code;
	uint32_t reserved2;
} teder_event;
typedef struct teder_probe_info {
	uint32_t struct_size, abi_version;
	uint32_t codec, sample_rate, channels, bits_per_sample;
	uint64_t duration_frames;
	uint32_t duration_known, seekable, tag_count, artwork_count;
} teder_probe_info;
typedef struct teder_artwork {
	uint32_t struct_size, abi_version;
	uint32_t picture_type, width, height, depth, colors, reserved;
	teder_text mime, description;
	teder_blob encoded;
} teder_artwork;
typedef struct teder_device_info {
	uint32_t struct_size, abi_version;
	uint32_t backend, is_default;
	teder_text name;
	teder_blob key;
} teder_device_info;
typedef struct teder_analysis_config {
	uint32_t struct_size, abi_version;
	uint32_t enabled, bands, window_frames, hop_frames;
	float low_hz, high_hz, floor_db, smoothing_ms, peak_decay_db_per_s;
	uint32_t reserved;
} teder_analysis_config;
typedef struct teder_snapshot_info {
	uint32_t struct_size, abi_version;
	uint64_t sequence, epoch, output_start, frames, config_revision;
	uint32_t output_rate, channels, bands, span_count;
	uint64_t dropped_frames, dropped_publications;
	float peak[2], rms[2];
} teder_snapshot_info;

/* Build info is a caller-buffer copy, with no library-owned output pointers.
 * Backend mask bit n means backend ID n; capabilities promise tested features.
 */
TEDER_API teder_code TEDER_CALL teder_get_build_info(teder_build_info *out);
/* Zero config limits select documented build defaults; query resolved values.
 * Create reserves engine storage but does no file/device I/O. Failure: *out=NULL.
 * See README for implemented services; future workers share this engine owner.
 */
TEDER_API teder_code TEDER_CALL teder_create(const teder_config *config, teder_engine **out);
TEDER_API teder_code TEDER_CALL teder_get_config(teder_engine *engine, teder_config *out);
TEDER_API teder_code TEDER_CALL teder_get_status(teder_engine *engine, teder_status *out);
/* Close bypasses command capacity; repeated close on a live handle is safe.
 * Destroy requires CLOSED and exclusive access; once workers are implemented,
 * it joins the quiescent lifecycle worker before releasing storage.
 * Run away from the UI; success clears *engine.
 * Failure preserves it. NULL *engine is a successful no-op, not stale detection.
 */
TEDER_API teder_code TEDER_CALL teder_request_close(teder_engine *engine);
TEDER_API teder_code TEDER_CALL teder_destroy(teder_engine **engine);

/* Submission copies all inputs. OK means admitted, not performed. On rejection
 * *request=0 and no ownership transfers. Each admitted ID has one terminal result.
 * Targets are copied and checked again at commitment. Track labels may repeat.
 */
TEDER_API teder_code TEDER_CALL teder_open(teder_engine *engine,
	const char *path, uint32_t bytes, uint64_t track, uint32_t flags, uint64_t *request);
TEDER_API teder_code TEDER_CALL teder_transport(teder_engine *engine,
	const teder_target *target, uint32_t action, uint64_t *request);
TEDER_API teder_code TEDER_CALL teder_seek(teder_engine *engine,
	const teder_target *target, uint64_t source_frame, uint64_t *request);
TEDER_API teder_code TEDER_CALL teder_set_gain(teder_engine *engine,
	float linear_gain, uint32_t mute, uint64_t *request);
/* expected_next=0 requires no successor. Replacement preserves the old successor
 * until the new one is ready. Commitment of the old successor wins with TOO_LATE.
 */
TEDER_API teder_code TEDER_CALL teder_prepare_next(teder_engine *engine,
	const teder_target *target, uint64_t expected_next, const char *path,
	uint32_t bytes, uint64_t track, uint64_t *request);
TEDER_API teder_code TEDER_CALL teder_clear_next(teder_engine *engine,
	const teder_target *target, uint64_t expected_next, uint64_t *request);
TEDER_API teder_code TEDER_CALL teder_probe(teder_engine *engine,
	const char *path, uint32_t bytes, uint32_t flags, uint64_t *request);
TEDER_API teder_code TEDER_CALL teder_enumerate_devices(teder_engine *engine, uint64_t *request);
/* NULL key with zero bytes selects the current default. Otherwise key is copied
 * from this engine's enumeration. Keys retain no result pointer or OS handle.
 */
TEDER_API teder_code TEDER_CALL teder_select_device(teder_engine *engine,
	const uint8_t *key, uint64_t bytes, uint64_t *request);
TEDER_API teder_code TEDER_CALL teder_configure_analysis(teder_engine *engine,
	const teder_analysis_config *config, uint64_t *request);

/* Poll returns the oldest terminal, unacknowledged ID without removing it.
 * AGAIN means none available. Get also inspects queued/running requests.
 * Cancel is capacity-independent intent; a racing commit may still succeed.
 * Take transfers a result exactly once. Ack discards any untaken result and
 * releases the slot; neither operation changes an already taken result.
 */
TEDER_API teder_code TEDER_CALL teder_request_poll(teder_engine *engine, teder_request_info *out);
TEDER_API teder_code TEDER_CALL teder_request_get(teder_engine *engine, uint64_t request, teder_request_info *out);
TEDER_API teder_code TEDER_CALL teder_request_cancel(teder_engine *engine, uint64_t request);
TEDER_API teder_code TEDER_CALL teder_request_take_result(teder_engine *engine, uint64_t request, teder_result **out);
TEDER_API teder_code TEDER_CALL teder_request_ack(teder_engine *engine, uint64_t request);
/* Event poll consumes one copy. AGAIN means empty; sequence gaps expose loss. */
TEDER_API teder_code TEDER_CALL teder_event_poll(teder_engine *engine, teder_event *out);

/* Results own immutable storage independent of the engine. Accessors borrow it.
 * Wrong-kind access returns BAD_STATE; out-of-range index returns NOT_FOUND.
 * All indices are zero-based; repeated tags retain their file order.
 * Tag key/value outputs must be distinct; identical pointers return INVALID.
 */
TEDER_API teder_code TEDER_CALL teder_result_kind(const teder_result *result, uint32_t *out);
TEDER_API teder_code TEDER_CALL teder_result_probe(const teder_result *result, teder_probe_info *out);
TEDER_API teder_code TEDER_CALL teder_result_tag(const teder_result *result, uint32_t index, teder_text *key, teder_text *value);
TEDER_API teder_code TEDER_CALL teder_result_artwork(const teder_result *result, uint32_t index, teder_artwork *out);
TEDER_API teder_code TEDER_CALL teder_result_device_count(const teder_result *result, uint32_t *out);
TEDER_API teder_code TEDER_CALL teder_result_device(const teder_result *result, uint32_t index, teder_device_info *out);
TEDER_API void TEDER_CALL teder_result_release(teder_result *result);

/* Acquire returns one bounded reference to an immutable slot and its pool.
 * AGAIN: no current data or brief contention. LIMIT: loan budget exhausted.
 * Each successful acquire needs one release, even if addresses compare equal.
 * Loans survive engine destruction. Release must not dereference the engine.
 */
TEDER_API teder_code TEDER_CALL teder_snapshot_acquire(teder_engine *engine, teder_snapshot **out);
TEDER_API teder_code TEDER_CALL teder_snapshot_get_info(const teder_snapshot *snapshot, teder_snapshot_info *out);
TEDER_API teder_code TEDER_CALL teder_snapshot_bands(const teder_snapshot *snapshot, const float **out, uint32_t *count);
TEDER_API teder_code TEDER_CALL teder_snapshot_spans(const teder_snapshot *snapshot, const teder_span **out, uint32_t *count);
TEDER_API void TEDER_CALL teder_snapshot_release(teder_snapshot *snapshot);

#ifdef __cplusplus
}
#endif
#endif
