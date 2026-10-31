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
 * across live/stale keys; the engine assigns it on first backend open. preferred=0 chooses
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
/* Pure validation before native access. backend=0 accepts any supported backend. */
teder_code td_backend_key_decode(uint64_t token, uint32_t backend, teder_blob, ma_device_id *);
typedef struct {
	ma_context *context;
	ma_device_id id;
	td_allocator allocator;
	uint32_t backend;
} td_backend_loan;
/* Lifecycle-owner only. Empty key resolves the current default to a fixed ID.
 * A successful borrow prevents context close until device uninit and return. */
teder_code td_backend_borrow(td_backend *, teder_blob key, td_backend_loan *);
void td_backend_return(td_backend *);
teder_code td_backend_error(td_backend *);
teder_code td_backend_close(td_backend **);
#endif
