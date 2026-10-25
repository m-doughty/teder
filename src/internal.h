#ifndef TEDER_INTERNAL_H
#define TEDER_INTERNAL_H
#include "teder.h"
#include "memory.h"
#include "pcm.h"
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

enum { TD_ENGINE = 0x5444454e, TD_RESULT = 0x54445245 };
typedef struct { uint32_t kind, version; } td_handle;
/* Reading a prefix does not certify an address or a released handle. */
static inline teder_code td_prefix(const void *p, size_t size)
{
	uint32_t prefix[2];
	if (!p) return TEDER_INVALID;
	memcpy(prefix, p, sizeof(prefix));
	return prefix[0] == size && prefix[1] == TEDER_ABI_VERSION ? TEDER_OK : TEDER_ABI_MISMATCH;
}
static inline teder_code td_handle_check(const void *p, uint32_t kind)
{
	td_handle header;
	if (!p) return TEDER_INVALID;
	memcpy(&header, p, sizeof(header));
	if (header.version != TEDER_ABI_VERSION) return TEDER_ABI_MISMATCH;
	return header.kind == kind ? TEDER_OK : TEDER_BAD_STATE;
}
typedef struct { teder_text key, value; } td_tag;
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
	teder_request_info info;
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
enum { TD_INIT_LOCK = 1, TD_INIT_WAKE, TD_INIT_THREAD, TD_INIT_PUBLISH };
enum { TD_STARTED = 1, TD_STOPPED };
typedef struct {
	void *context;
	bool (*permit)(void *, uint32_t stage);
	void (*checkpoint)(void *, uint32_t point);
	teder_code (*execute)(void *, teder_engine *, uint32_t slot);
} td_runtime;
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
	td_thread thread;
	atomic_uint lifecycle;
	uint64_t last_request, last_completion;
	uint32_t event_head, event_count;
};
/* Private seams use the same implementation as public creation. Allocator
 * contexts outlive all resulting arenas; no global allocator replacement.
 * Input structs and their views remain valid and immutable throughout creation. */
teder_code td_engine_create(const teder_config *, td_allocator, teder_engine **);
teder_code td_engine_create_with(const teder_config *, td_allocator, td_runtime, teder_engine **);
teder_code td_result_create(const td_result_input *, td_allocator, uint64_t limit, teder_result **);
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
/* Internal producers validate typed inputs before this bounded byte-copy seam.
 * Public calls use try-lock; the worker blocks only for short bookkeeping. */
teder_code td_submit(teder_engine *, uint32_t operation, const void *, uint32_t bytes, uint64_t *request);
teder_code td_commit(teder_engine *, uint32_t slot);
teder_code td_build_result(teder_engine *, uint32_t slot, const td_result_input *);
teder_code td_emit(teder_engine *, const teder_event *);
void td_limit_close(teder_engine *); /* gate held */
#endif
