#include "metadata.h"
#include "source.h"
#include <stdlib.h>

teder_code TEDER_CALL
teder_probe(teder_engine *e, const char *path, uint32_t bytes, uint32_t flags, uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code != TEDER_OK)
		return code;
	if (flags & ~TEDER_PROBE_ARTWORK)
		return TEDER_INVALID;
	if (bytes > e->config.max_input_bytes || bytes > 32767)
		return TEDER_LIMIT;
	code = td_path_validate(path, bytes);
	if (code != TEDER_OK)
		return code;
	td_media_input input = {.flags = flags};
	return td_submit_media(e, TEDER_OP_PROBE, path, bytes, &input, request);
}

void td_probe_signal(teder_engine *e)
{
	if (ma_event_signal(&e->probe_wake) != MA_SUCCESS)
		abort();
}

static void file_checkpoint(void *context, unsigned point)
{
	teder_engine *e = context;
	/* Only this worker changes its request ID, under the gate. */
	td_media_checkpoint(e, e->probe_request, TD_PROBE_OPENED + point - TD_FILE_OPENED);
}

/* Gate held. Leave room for one maximum-path source preparation, including its
 * codec cap. A probe cannot take that headroom, even while its I/O is blocked. */
static teder_code reserve(teder_engine *e, td_request_slot *r, td_metadata_config *c, size_t *total)
{
	size_t headroom, needed;
	uint32_t path = e->config.max_input_bytes < 32767 ? e->config.max_input_bytes : 32767;
	teder_code code = td_metadata_plan(r->input_bytes, &c->work_bytes);
	if (code == TEDER_OK)
		code = td_source_plan(path, 8 * 1024 * 1024, &headroom);
	if (code != TEDER_OK)
		return code;
	uint64_t available = e->config.engine_bytes_limit - e->owned_bytes;
	if (!td_size_add(c->work_bytes, headroom, &needed) || available <= needed)
		return TEDER_LIMIT;
	available -= needed;
	c->result_bytes =
	    available < e->config.result_bytes_limit ? available : e->config.result_bytes_limit;
	if (c->result_bytes > SIZE_MAX || !td_size_add(c->work_bytes, (size_t)c->result_bytes, total) ||
	    !td_size_add(e->owned_bytes, *total, &needed))
		return TEDER_LIMIT;
	e->owned_bytes = needed;
	return TEDER_OK;
}

void td_probe_run(teder_engine *e)
{
	td_media_checkpoint(e, 0, TD_PROBE_STARTED);
	for (;;) {
		td_lock(e);
		if (atomic_load(&e->lifecycle) != TEDER_LIVE) {
			td_unlock(e);
			break;
		}
		uint32_t slot = e->config.request_capacity;
		if (!e->runtime.execute)
			for (uint32_t i = 0; i < e->config.request_capacity; i++) {
				td_request_slot *r = &e->requests[i];
				if (r->info.state == TEDER_QUEUED && r->info.operation == TEDER_OP_PROBE &&
				    (slot == e->config.request_capacity ||
				     r->info.request < e->requests[slot].info.request))
					slot = i;
			}
		if (slot == e->config.request_capacity) {
			td_unlock(e);
			if (ma_event_wait(&e->probe_wake) != MA_SUCCESS)
				abort();
			continue;
		}
		td_request_slot *r = &e->requests[slot];
		r->info.state = TEDER_RUNNING;
		e->probe_request = r->info.request;
		atomic_store(&e->probe_cancel, 0);
		td_metadata_config config = {.flags = r->media.flags,
		                             .file = {.cancel = &e->probe_cancel,
		                                      .lifecycle = &e->lifecycle,
		                                      .context = e,
		                                      .checkpoint = file_checkpoint}};
		size_t reservation = 0;
		teder_code code = r->cancelled ? TEDER_CANCELLED : reserve(e, r, &config, &reservation);
		/* A failed size calculation never acquired a reservation. */
		if (code != TEDER_OK)
			reservation = 0;
		td_unlock(e);
		teder_result *result = NULL;
		if (code == TEDER_OK)
			code = td_metadata_read(
			    (const char *)r->input, r->input_bytes, &config, e->allocator, &result);
		if (code == TEDER_OK)
			td_media_checkpoint(e, e->probe_request, TD_PROBE_BEFORE_COMMIT);
		td_lock(e);
		if (code == TEDER_OK)
			code = td_commit_locked(e, slot);
		if (code == TEDER_OK) {
			r->result = result;
			e->owned_bytes -= reservation - result->bytes;
			td_unlock(e);
			td_media_checkpoint(e, e->probe_request, TD_PROBE_AFTER_COMMIT);
		} else {
			td_unlock(e);
			teder_result_release(result);
			td_lock(e);
			e->owned_bytes -= reservation;
			td_unlock(e);
		}
		td_lock(e);
		td_complete(e, slot, code);
		e->probe_request = 0;
		td_unlock(e);
	}
	td_media_checkpoint(e, 0, TD_PROBE_STOPPED);
}
