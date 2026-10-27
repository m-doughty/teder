#ifndef TEDER_INTERNAL_H
#define TEDER_INTERNAL_H
#include "teder.h"
#include "memory.h"
#include "pcm.h"
#include "backend.h"
#include "device.h"
#include "miniaudio.h"
#include <stdatomic.h>
#include <string.h>
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "close requires lock-free unsigned atomics");
#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK td_mutex;
typedef HANDLE td_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t td_mutex;
typedef pthread_t td_thread;
#endif

enum { TD_ENGINE = 0x5444454e, TD_RESULT = 0x54445245, TD_SNAPSHOT = 0x5444534e };

typedef struct {
	uint32_t kind, version;
} td_handle;

/* Reading a prefix does not certify an address or a released handle. */
static inline teder_code td_prefix(const void *p, size_t size)
{
	uint32_t prefix[2];
	if (!p)
		return TEDER_INVALID;
	memcpy(prefix, p, sizeof(prefix));
	return prefix[0] == size && prefix[1] == TEDER_ABI_VERSION ? TEDER_OK : TEDER_ABI_MISMATCH;
}

static inline teder_code td_handle_check(const void *p, uint32_t kind)
{
	td_handle header;
	if (!p)
		return TEDER_INVALID;
	memcpy(&header, p, sizeof(header));
	if (header.version != TEDER_ABI_VERSION)
		return TEDER_ABI_MISMATCH;
	return header.kind == kind ? TEDER_OK : TEDER_BAD_STATE;
}

typedef struct {
	teder_text key, value;
} td_tag;

typedef struct {
	uint32_t kind, device_count;
	const teder_probe_info *probe;
	const td_tag *tags;
	const teder_artwork *artwork;
	const teder_device_info *devices;
} td_result_input;

struct teder_result {
	td_handle header;
	td_arena *arena;
	size_t bytes;
	uint32_t kind, device_count;
	teder_probe_info probe;
	td_tag *tags;
	teder_artwork *artwork;
	teder_device_info *devices;
};

typedef struct {
	teder_target target;
	uint64_t frame, track, next;
	uint32_t flags;
	float gain;
} td_media_input;

typedef struct {
	teder_request_info info;
	td_media_input media;
	teder_result *result;
	uint8_t *input;
	uint32_t input_bytes;
	uint64_t completed;
	bool cancelled, committed;
} td_request_slot;

/* Private dependency/test seam, never installed or exposed to callers.
 * Context survives destroy; execute borrows a RUNNING slot until return, calls
 * td_commit before any durable effect, and retires temporary work before return.
 * No operation or checkpoint runs while the engine gate is held. */
enum {
	TD_INIT_LOCK = 1,
	TD_INIT_WAKE,
	TD_INIT_THREAD,
	TD_INIT_PUBLISH,
	TD_INIT_PREPARE_WAKE,
	TD_INIT_PREPARE_THREAD,
	TD_INIT_REFILL_WAKE,
	TD_INIT_REFILL_IDLE,
	TD_INIT_REFILL_THREAD,
	TD_INIT_PROBE_WAKE,
	TD_INIT_PROBE_THREAD,
	TD_INIT_SNAPSHOT_GATE
};

enum { TD_STARTED = 1, TD_STOPPED };

enum {
	TD_PREPARE_STARTED = 10,
	TD_PREPARE_STOPPED,
	TD_MEDIA_BEFORE_COMMIT,
	TD_MEDIA_AFTER_COMMIT,
	TD_DECODE_IDLE,
	TD_OUTPUT_BEFORE_COMMIT,
	TD_OUTPUT_AFTER_COMMIT,
	TD_SELECT_BEFORE_COMMIT,
	TD_SELECT_AFTER_COMMIT,
	TD_PREPARE_JOB,
	TD_REFILL_STARTED,
	TD_REFILL_STOPPED,
	TD_REFILL_PUMP,
	TD_REFILL_PARKED,
	TD_REFILL_PAUSING,
	TD_REFILL_RESUMED,
	TD_TRANSPORT_BEFORE_COMMIT,
	TD_TRANSPORT_PUBLISHED,
	TD_NEXT_BEFORE_PUBLISH,
	TD_NEXT_AFTER_PUBLISH,
	TD_PROBE_STARTED,
	TD_PROBE_STOPPED,
	TD_PROBE_OPENED,
	TD_PROBE_READ,
	TD_PROBE_CLOSED,
	TD_PROBE_BEFORE_COMMIT,
	TD_PROBE_AFTER_COMMIT
};

typedef struct {
	void *context;
	bool (*permit)(void *, uint32_t stage);
	void (*checkpoint)(void *, uint32_t point);
	teder_code (*execute)(void *, teder_engine *, uint32_t slot);
	void (*media_checkpoint)(void *, uint64_t request, unsigned point);
	/* Optional private consumer: one endpoint, joined here before PCM clear.
	 * Without it, control owns the idle endpoint until a native device starts. */
	void (*consumer_join)(void *);
	/* Zero uses native backends; otherwise all functions are required.
	 * The descriptor is copied; its caller-owned context outlives destroy. */
	td_backend_api backend;
} td_runtime;

enum {
	TD_JOB_FREE,
	TD_JOB_STAGING,
	TD_JOB_PREPARED,
	TD_JOB_ACTIVE,
	TD_JOB_RETIRING,
	TD_JOB_SUSPENDED,
	TD_JOB_NEXT,
	TD_JOB_ADOPTED,
	TD_SOURCE_JOBS = 3
};

typedef struct {
	teder_engine *engine;
	struct td_source *source;
	atomic_uint cancel;
	size_t reservation;
	uint64_t request;
	uint32_t state, slot, path_bytes;
	teder_code outcome;
} td_source_job;

struct teder_engine {
	td_handle header;
	td_arena *arena;
	teder_config config;
	teder_status status;
	size_t owned_bytes;
	td_request_slot *requests;
	teder_event *events;
	td_pcm_pool pcm;
	td_allocator allocator;
	td_runtime runtime;
	td_mutex gate;
	ma_event wake;
	ma_event prepare_wake;
	ma_event refill_wake, refill_idle;
	ma_event probe_wake;
	atomic_uint refill_enabled, refill_stop;
	atomic_uint probe_cancel;
	uint64_t probe_request; /* gate protected; worker borrows its RUNNING slot */
	td_thread thread;
	td_thread prepare_thread;
	td_thread refill_thread;
	td_thread probe_thread;
	td_source_job sources[TD_SOURCE_JOBS];
	uint64_t last_source;
	bool prefill;        /* exclusive producer: PLAY may commit a ready successor */
	td_backend *backend; /* lifecycle worker only; context outlives devices */
	size_t backend_reservation;
	td_device *device;
	size_t device_reservation;
	uint64_t backend_token;
	atomic_uint lifecycle;
	uint64_t last_request, last_completion;
	uint32_t event_head, event_count;
};

/* Private seams use the same implementation as public creation. Allocator
 * contexts outlive all resulting arenas; no global allocator replacement.
 * Input structs and their views remain valid and immutable throughout creation. */
teder_code td_engine_create(const teder_config *, td_allocator, teder_engine **);
void td_probe_run(teder_engine *);
void td_probe_signal(teder_engine *);
teder_code td_engine_create_with(const teder_config *, td_allocator, td_runtime, teder_engine **);
teder_code td_result_create(const td_result_input *, td_allocator, uint64_t limit, teder_result **);
bool td_utf8(const unsigned char *, size_t bytes);
/* Slot ownership only, not request admission/state/ID validation.
 * Attach consumes *owner only on success. Take clears the slot exactly once.
 * Producers must budget construction before allocation; attach is a second
 * check at transfer. Hold the engine gate (or prove the worker idle). */
teder_code td_engine_attach_result(teder_engine *, uint32_t slot, teder_result **owner);
teder_code td_engine_take_result(teder_engine *, uint32_t slot, teder_result **out);
bool td_try_lock(teder_engine *);
void td_lock(teder_engine *);
void td_unlock(teder_engine *);
teder_code td_control_start(teder_engine *);
void td_control_join(teder_engine *);
void td_control_dispose(teder_engine *);
void td_signal(teder_engine *);
void td_prepare_signal(teder_engine *);
void td_prepare_run(teder_engine *);
/* Control holds the producer unless it has resumed refill. Pause acknowledges
 * the last pump/reap before control touches source or producer-owned state. */
void td_refill_run(teder_engine *);
void td_worker_interval(void);
void td_refill_pause(teder_engine *);
void td_refill_stop(teder_engine *); /* pause, request exit, then caller joins */
void td_refill_resume(teder_engine *);
bool td_decode_pump(teder_engine *); /* current producer only */
bool td_decode_step(teder_engine *); /* control, refill paused */
teder_code td_output_enumerate(teder_engine *, uint32_t slot);
void td_output_close(teder_engine *); /* lifecycle worker: device before context */
void td_output_select(teder_engine *, uint32_t slot);
void td_transport(teder_engine *, uint32_t slot);
void td_stop(teder_engine *, uint32_t slot);
/* Successor bookkeeping requires the gate; retirement additionally requires
 * exclusive producer ownership. Adopted PCM retains its generation references. */
teder_code td_successor_check(teder_engine *, const td_media_input *, bool prepare, bool finishing);
teder_code td_successor_ready(teder_engine *, td_source_job *);
void td_successor_clear(teder_engine *, uint32_t slot);
void td_successor_drop(teder_engine *, bool adopted);
bool td_transition_pump(teder_engine *, td_source_job *);
bool td_transition_report(teder_engine *, const td_device_report *, teder_event *); /* gate held */
teder_code td_successor_publish(void *, td_pcm_pool *, uint32_t, const td_pcm_meta *);
teder_code td_output_quiesce(teder_engine *); /* control, producer already paused */
bool td_output_recover(teder_engine *);       /* control, refill paused; joins lost native users */
teder_code td_output_start(teder_engine *);
bool td_output_report(teder_engine *);                   /* current producer; no native control */
void td_sources_suspend(teder_engine *, uint64_t epoch); /* native consumer quiescent */
void td_media_retarget(teder_engine *, uint32_t slot);
teder_code td_token_next(atomic_uint_fast64_t *, uint64_t *);
void td_sources_close(teder_engine *);            /* prep joined and consumer quiescent */
bool td_media_dispatch(teder_engine *, uint32_t); /* gate held; never performs I/O */
teder_code td_submit_media(
    teder_engine *, uint32_t, const void *, uint32_t, const td_media_input *, uint64_t *);
void td_complete(teder_engine *, uint32_t, teder_code); /* gate held */
void td_media_checkpoint(teder_engine *, uint64_t, unsigned);
/* Internal producers validate typed inputs before this bounded byte-copy seam.
 * Public calls use try-lock; the worker blocks only for short bookkeeping. */
teder_code
td_submit(teder_engine *, uint32_t operation, const void *, uint32_t bytes, uint64_t *request);
teder_code td_commit(teder_engine *, uint32_t slot);
teder_code td_commit_locked(teder_engine *, uint32_t slot);
teder_code td_build_result(teder_engine *, uint32_t slot, const td_result_input *);
teder_code td_emit(teder_engine *, const teder_event *);
void td_limit_close(teder_engine *); /* gate held */
#endif
