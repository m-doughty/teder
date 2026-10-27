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

static teder_code context_open(teder_engine *e, uint32_t preferred)
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
	code = td_backend_open(e->backend_token,
	                       preferred,
	                       TD_BACKEND_BYTES,
	                       e->allocator,
	                       injected ? api : NULL,
	                       &e->backend);
	if (code == TEDER_OK)
		e->backend_reservation = reservation;
	else {
		td_lock(e);
		e->owned_bytes -= reservation;
		td_unlock(e);
	}
	return code;
}

static void device_close(teder_engine *e)
{
	if (!e->device)
		return;
	td_refill_pause(e);
	/* Save the final callback endpoint before native userdata is released.
	 * Failed stop also joins native users, via uninit in the device owner. */
	if (td_device_running(e->device) || td_device_lost(e->device))
		td_device_stop(e->device);
	td_output_report(e);
	if (td_device_close(&e->device) != TEDER_OK)
		abort();
	td_lock(e);
	e->owned_bytes -= e->device_reservation;
	e->device_reservation = 0;
	td_unlock(e);
}

/* Native uninit is the consumer barrier. The current producer is this worker;
 * preparation can own only unpublished source storage. */
static void invalidate(teder_engine *e, teder_code code)
{
	device_close(e);
	td_refill_pause(e);
	td_lock(e);
	bool exhausted = e->status.current.epoch == UINT64_MAX;
	if (exhausted)
		td_limit_close(e);
	else
		e->status.current.epoch++;
	uint64_t epoch = e->status.current.epoch;
	e->status.output_state = TEDER_OUTPUT_LOST;
	e->status.output_rate = e->status.output_channels = e->status.output_period_frames = 0;
	e->status.backend = 0;
	e->status.output_frame = e->status.latency_frames = 0;
	e->status.underrun_frames = 0;
	e->status.latency_quality = e->status.position_quality = TEDER_QUALITY_UNKNOWN;
	e->status.clock_flags &= TEDER_DURATION_KNOWN;
	e->status.next_source = e->status.next_track = 0;
	e->status.next_state = TEDER_NEXT_NONE;
	e->status.transport = e->status.current.source ? TEDER_PAUSED : TEDER_EMPTY;
	if (!exhausted)
		e->status.last_error = code;
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
		if (e->sources[i].state == TD_JOB_STAGING || e->sources[i].state == TD_JOB_PREPARED)
			atomic_store(&e->sources[i].cancel, 1);
	td_unlock(e);
	if (!exhausted)
		td_sources_suspend(e, epoch);
	if (code != TEDER_OK) {
		teder_event event = {.struct_size = sizeof(event),
		                     .abi_version = TEDER_ABI_VERSION,
		                     .kind = TEDER_EVENT_DEVICE,
		                     .epoch = epoch,
		                     .from_source = e->status.current.source,
		                     .from_track = e->status.track,
		                     .code = code};
		td_emit(e, &event);
	}
}

bool td_output_recover(teder_engine *e)
{
	if (!td_device_lost(e->device))
		return false;
	invalidate(e, TEDER_DEVICE_LOST);
	return true;
}

void td_output_close(teder_engine *e)
{
	device_close(e);
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
	teder_code code = context_open(e, 0);
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
	if (code != TEDER_OK && (created || td_backend_error(e->backend) != TEDER_OK)) {
		if (e->device)
			invalidate(e, code);
		td_output_close(e);
	}
	return code; /* td_complete releases an uncommitted result before terminal */
}

teder_code TEDER_CALL teder_select_device(teder_engine *e,
                                          const uint8_t *key,
                                          uint64_t bytes,
                                          uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code != TEDER_OK)
		return code;
	if ((!key) != (!bytes))
		return TEDER_INVALID;
	if (bytes > e->config.max_input_bytes || bytes > UINT32_MAX)
		return TEDER_LIMIT;
	td_media_input input = {0};
	return td_submit_media(e, TEDER_OP_SELECT_DEVICE, key, (uint32_t)bytes, &input, request);
}

void td_output_select(teder_engine *e, uint32_t slot)
{
	td_request_slot *r = &e->requests[slot];
	teder_blob key = {r->input_bytes ? r->input : NULL, r->input_bytes};
	ma_device_id id;
	bool exhausted = false;
	bool created = !e->backend;
	size_t reservation = 0, total;
	teder_code code = key.bytes ? td_backend_key_decode(e->backend_token, 0, key, &id) : TEDER_OK;
	if (code == TEDER_OK && e->runtime.consumer_join)
		code = TEDER_BAD_STATE; /* a private external consumer cannot share the endpoint */
	if (code == TEDER_OK)
		code = context_open(e, key.bytes ? ((const unsigned char *)key.data)[12] : 0);
	if (code == TEDER_OK && key.bytes)
		code = td_backend_key(e->backend, key, &id);
	if (code == TEDER_OK)
		code = td_device_plan(&reservation);
	if (code == TEDER_OK)
		td_media_checkpoint(e, r->info.request, TD_SELECT_BEFORE_COMMIT);
	td_lock(e);
	if (code == TEDER_OK && e->status.current.epoch == UINT64_MAX) {
		code = TEDER_LIMIT;
		exhausted = true;
	}
	if (code == TEDER_OK &&
	    (!td_size_add(e->owned_bytes, reservation, &total) || total > e->config.engine_bytes_limit))
		code = TEDER_LIMIT;
	if (code == TEDER_OK)
		code = td_commit_locked(e, slot);
	if (code == TEDER_OK)
		e->owned_bytes = total;
	td_unlock(e);
	if (code != TEDER_OK) {
		if (created)
			td_output_close(e);
		goto complete;
	}

	invalidate(e, TEDER_OK);
	td_lock(e);
	r->info.committed = e->status.current;
	r->info.track = e->status.track;
	td_unlock(e);
	td_media_checkpoint(e, r->info.request, TD_SELECT_AFTER_COMMIT);
	code = atomic_load(&e->lifecycle) == TEDER_LIVE
	           ? td_device_open(e->backend, &e->pcm, key, 0, &e->device)
	           : TEDER_CANCELLED;
	/* The old native producer was joined by invalidate before opening this one. */
	if (code == TEDER_OK) {
		code = td_device_analysis(e->device, &e->analysis);
		if (code != TEDER_OK && td_device_close(&e->device) != TEDER_OK)
			abort();
	}
	td_device_info info;
	if (code == TEDER_OK && td_device_get_info(e->device, &info) != TEDER_OK)
		abort();
	td_lock(e);
	if (code == TEDER_OK) {
		e->device_reservation = reservation;
		e->status.output_state = TEDER_OUTPUT_READY;
		e->status.output_rate = info.rate;
		e->status.output_channels = info.channels;
		e->status.output_period_frames = info.period_frames;
		e->status.backend = info.backend;
	} else {
		e->owned_bytes -= reservation;
		e->status.last_error = code;
	}
	bool loaded = e->status.current.source != 0;
	teder_event event = {.struct_size = sizeof(event),
	                     .abi_version = TEDER_ABI_VERSION,
	                     .kind = TEDER_EVENT_DEVICE,
	                     .epoch = e->status.current.epoch,
	                     .from_source = e->status.current.source,
	                     .from_track = e->status.track,
	                     .code = code};
	td_unlock(e);
	if (code != TEDER_OK && (created || td_backend_error(e->backend) != TEDER_OK))
		td_output_close(e);
	td_emit(e, &event);
	if (code == TEDER_OK && loaded) {
		td_media_retarget(e, slot);
		return; /* preparation owns this RUNNING slot until its terminal outcome */
	}
complete:
	td_lock(e);
	td_complete(e, slot, code);
	if (exhausted)
		td_limit_close(e);
	td_unlock(e);
}

/* Source commitment runs with refill paused, outside the bookkeeping gate. */
teder_code td_output_quiesce(teder_engine *e)
{
	if (td_output_recover(e))
		return TEDER_DEVICE_LOST;
	if (!td_device_running(e->device))
		return TEDER_OK;
	teder_code code = td_device_stop(e->device);
	if (code != TEDER_OK)
		invalidate(e, code);
	else
		td_output_report(e);
	return code;
}

teder_code td_output_start(teder_engine *e)
{
	if (atomic_load(&e->lifecycle) != TEDER_LIVE)
		return TEDER_CANCELLED;
	td_lock(e);
	td_audio_config config = {e->status.current.epoch, e->status.gain, 1, e->status.mute};
	td_unlock(e);
	teder_code code = td_device_configure(e->device, config);
	if (code == TEDER_OK)
		code = td_device_start(e->device);
	if (code != TEDER_OK)
		invalidate(e, code);
	return code;
}

bool td_output_report(teder_engine *e)
{
	if (!e->device)
		return false;
	td_device_report report;
	teder_code code = td_device_get_report(e->device, &report);
	if (code == TEDER_AGAIN)
		return false;
	td_lock(e);
	if (code != TEDER_OK)
		report = (td_device_report){.generation = e->status.current.epoch, .error = code};
	teder_event transition;
	bool transitioned = td_transition_report(e, &report, &transition);
	bool current = report.generation == e->status.current.epoch &&
	               (!report.source ||
	                (report.source == e->status.current.source && report.track == e->status.track));
	uint64_t missing = 0;
	if (current && report.error == TEDER_OK) {
		e->status.output_frame = report.output_frames;
		if (report.source) {
			e->status.source_frame = report.source_frame;
			e->status.position_quality = e->status.source_rate == e->status.output_rate
			                                 ? TEDER_QUALITY_MEASURED
			                                 : TEDER_QUALITY_ESTIMATED;
		}
		missing = report.underrun_frames - e->status.underrun_frames;
		e->status.underrun_frames = report.underrun_frames;
	}
	bool failed = current && report.error != TEDER_OK && e->status.transport != TEDER_FAILED;
	bool ended = current && report.error == TEDER_OK && report.final &&
	             (e->status.transport == TEDER_PLAYING || e->status.transport == TEDER_DRAINING);
	bool draining = current && !failed && !ended && e->status.transport == TEDER_PLAYING &&
	                (e->status.clock_flags & TEDER_DECODE_EOF);
	if (failed) {
		e->status.transport = TEDER_FAILED;
		e->status.last_error = report.error;
	} else if (ended) {
		e->status.clock_flags |= TEDER_SUBMIT_EOF;
		e->status.transport = TEDER_ENDED;
	} else if (draining)
		e->status.transport = TEDER_DRAINING;
	teder_event event = {.struct_size = sizeof(event),
	                     .abi_version = TEDER_ABI_VERSION,
	                     .kind = TEDER_EVENT_ERROR,
	                     .epoch = e->status.current.epoch,
	                     .from_source = e->status.current.source,
	                     .from_track = e->status.track,
	                     .code = report.error};
	teder_event underrun = {.struct_size = sizeof(underrun),
	                        .abi_version = TEDER_ABI_VERSION,
	                        .kind = TEDER_EVENT_UNDERRUN,
	                        .epoch = e->status.current.epoch,
	                        .output_frame = e->status.output_frame,
	                        .from_source = e->status.current.source,
	                        .from_track = e->status.track,
	                        .missing_frames = missing};
	td_unlock(e);
	if (transitioned)
		td_emit(e, &transition);
	if (failed)
		td_emit(e, &event);
	if (missing)
		td_emit(e, &underrun);
	return transitioned || failed || ended || draining;
}
