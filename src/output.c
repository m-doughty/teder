#include "internal.h"
#include <stdlib.h>

enum { TD_BACKEND_BYTES = 8 * 1024 * 1024 };
static atomic_uint_fast64_t last_token = 0;

/* Process/library-instance provenance, not authentication. Exhaustion is sticky;
 * neither close nor failed construction can recycle a token from a retained key. */
teder_code td_token_next(atomic_uint_fast64_t *counter, uint64_t *out)
{
	uint_fast64_t value = atomic_load_explicit(counter, memory_order_relaxed);
	for (;;) {
		if (value == UINT64_MAX)
			return TEDER_LIMIT;
		if (atomic_compare_exchange_weak_explicit(
		        counter, &value, value + 1, memory_order_relaxed, memory_order_relaxed)) {
			*out = value + 1;
			return TEDER_OK;
		}
	}
}

teder_code TEDER_CALL teder_enumerate_devices(teder_engine *e, uint64_t *request)
{
	return td_submit(e, TEDER_OP_DEVICES, NULL, 0, request);
}

static teder_code context_open(teder_engine *e)
{
	if (e->backend)
		return TEDER_OK;
	size_t reservation, total;
	teder_code code = td_backend_plan(TD_BACKEND_BYTES, &reservation);
	if (code != TEDER_OK)
		return code;
	if (!e->backend_token) {
		code = td_token_next(&last_token, &e->backend_token);
		if (code != TEDER_OK)
			return code;
	}
	td_lock(e);
	bool fits =
	    td_size_add(e->owned_bytes, reservation, &total) && total <= e->config.engine_bytes_limit;
	if (fits)
		e->owned_bytes = total;
	td_unlock(e);
	if (!fits)
		return TEDER_LIMIT;

	const td_backend_api *api = &e->runtime.backend;
	bool injected = api->init || api->uninit || api->enumerate || api->context;
	code = td_backend_open(
	    e->backend_token, 0, TD_BACKEND_BYTES, e->allocator, injected ? api : NULL, &e->backend);
	if (code == TEDER_OK)
		e->backend_reservation = reservation;
	else {
		td_lock(e);
		e->owned_bytes -= reservation;
		td_unlock(e);
	}
	return code;
}

void td_output_close(teder_engine *e)
{
	if (!e->backend)
		return;
	teder_code code = td_backend_close(&e->backend);
	if (e->backend)
		abort(); /* native users or allocations remain: never publish CLOSED */
	td_lock(e);
	e->owned_bytes -= e->backend_reservation;
	e->backend_reservation = 0;
	if (code != TEDER_OK)
		e->status.last_error = code;
	td_unlock(e);
}

teder_code td_output_enumerate(teder_engine *e, uint32_t slot)
{
	bool created = !e->backend;
	teder_code code = context_open(e);
	if (code != TEDER_OK)
		return code;

	/* Keep the full construction allowance charged until native enumeration and
	 * result copying finish. The context heap has its own retained reservation. */
	td_lock(e);
	uint64_t available = e->config.engine_bytes_limit - e->owned_bytes;
	size_t reservation =
	    (size_t)(available < e->config.result_bytes_limit ? available
	                                                      : e->config.result_bytes_limit);
	e->owned_bytes += reservation;
	td_unlock(e);
	teder_result *result = NULL;
	code = td_backend_enumerate(e->backend, reservation, e->config.max_input_bytes, &result);
	td_lock(e);
	e->owned_bytes -= reservation;
	if (code == TEDER_OK) {
		e->requests[slot].result = result;
		e->owned_bytes += result->bytes;
	}
	td_unlock(e);

	if (code == TEDER_OK) {
		td_media_checkpoint(e, e->requests[slot].info.request, TD_OUTPUT_BEFORE_COMMIT);
		code = td_commit(e, slot);
		if (code == TEDER_OK)
			td_media_checkpoint(e, e->requests[slot].info.request, TD_OUTPUT_AFTER_COMMIT);
	}
	/* A new context is provisional until the first result commits. Existing
	 * healthy contexts survive result failures; a poisoned native heap cannot. */
	if (code != TEDER_OK && (created || td_backend_error(e->backend) != TEDER_OK))
		td_output_close(e);
	return code; /* td_complete releases an uncommitted result before terminal */
}
