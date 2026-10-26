#ifndef TEDER_BACKEND_H
#define TEDER_BACKEND_H
#include "audio_callback.h"
#include "miniaudio.h"

/* Backend allocator calls may come from native worker threads. This heap is
 * bounded, individually reusable, and never entered by teder's audio callback.
 * Reserve limit plus the owner arena before creation. error is sticky; inspect
 * counters only under lock or after all native users are quiescent. Backing
 * allocator callbacks must not reenter this heap. */
typedef struct {
	td_allocator allocator;
	ma_mutex lock;
	size_t limit, bytes, peak, live;
	teder_code error;
} td_backend_heap;
teder_code td_backend_heap_init(td_backend_heap *, td_allocator, size_t limit);
ma_allocation_callbacks td_backend_allocator(td_backend_heap *);
teder_code td_backend_heap_error(td_backend_heap *);
teder_code td_backend_heap_uninit(td_backend_heap *);

/* Private dependency seam; never installed. Failed init must unwind its own
 * partial state; successful init transfers ownership until uninit. Context and
 * allocator userdata outlive uninit and every native worker it joins. */
typedef struct {
	ma_result (*init)(const ma_backend *, ma_uint32, const ma_context_config *, ma_context *);
	ma_result (*uninit)(ma_context *);
	ma_result (*enumerate)(ma_context *, ma_enum_devices_callback_proc, void *);
	void *context;
} td_backend_api;
typedef struct td_backend td_backend;

/* One non-audio lifecycle thread serializes these calls, including open/close
 * on the same thread (COM). token is a nonzero engine-instance identity, unique
 * across live/stale keys; its generation belongs to the engine. preferred=0 chooses
 * the platform's enabled native backends. No null/custom fallback is enabled.
 * NULL api selects miniaudio; non-NULL is a private test seam. */
teder_code td_backend_plan(size_t heap_limit, size_t *reservation);
teder_code td_backend_open(uint64_t token,
                           uint32_t preferred,
                           size_t heap_limit,
                           td_allocator,
                           const td_backend_api *,
                           td_backend **);
teder_code
td_backend_enumerate(td_backend *, uint64_t result_limit, uint32_t key_limit, teder_result **);
/* Decode only validated backend-specific bytes; no native access, pointer or
 * OS-handle deserialization. Output is unchanged on failure. */
teder_code td_backend_key(td_backend *, teder_blob, ma_device_id *);
teder_code td_backend_close(td_backend **);
#endif
