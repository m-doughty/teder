#include "internal.h"
#include <stdlib.h>

/* Keep the control queue ordered while preparation returns its borrow. Waiting
 * uses the existing completion/close event; no stopped-output polling is needed. */
static bool preparation_finished(teder_engine *e)
{
	for (;;) {
		if (atomic_load(&e->lifecycle) != TEDER_LIVE)
			return false; /* shutdown joins before completing the retained requests */
		td_lock(e);
		bool pending = false;
		for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
			pending |=
			    e->sources[i].state == TD_JOB_STAGING || e->sources[i].state == TD_JOB_PREPARED;
		td_unlock(e);
		if (!pending)
			return true;
		if (td_decode_step(e))
			continue;
		td_media_checkpoint(e, 0, TD_PREPARATION_WAIT);
		if (ma_event_wait(&e->wake) != MA_SUCCESS)
			abort();
	}
}

void td_stop(teder_engine *e, uint32_t slot)
{
	td_refill_pause(e);
	td_media_checkpoint(e, e->requests[slot].info.request, TD_TRANSPORT_BEFORE_COMMIT);
	td_lock(e);
	td_request_slot *r = &e->requests[slot];
	bool loaded = e->status.current.source != 0;
	teder_code code = TEDER_OK;
	if (r->media.target.epoch != e->status.current.epoch ||
	    r->media.target.source != e->status.current.source)
		code = TEDER_STALE;
	else if (e->runtime.consumer_join)
		code = TEDER_BAD_STATE; /* the private test endpoint cannot be stolen */
	else if (loaded && e->status.current.epoch == UINT64_MAX)
		code = TEDER_LIMIT;
	if (code == TEDER_OK)
		code = td_commit_locked(e, slot);
	if (code != TEDER_OK) {
		td_complete(e, slot, code);
		if (code == TEDER_LIMIT)
			td_limit_close(e);
		td_unlock(e);
		return;
	}
	uint64_t previous = e->status.media_request;
	for (uint32_t i = 0; previous && i < e->config.request_capacity; i++) {
		td_request_slot *pending = &e->requests[i];
		if (pending->info.request != previous)
			continue;
		pending->cancelled = true;
		if (pending->info.state == TEDER_QUEUED)
			td_complete(e, i, TEDER_CANCELLED);
	}
	for (unsigned i = 0; previous && i < TD_SOURCE_JOBS; i++)
		if (e->sources[i].request == previous &&
		    (e->sources[i].state == TD_JOB_STAGING || e->sources[i].state == TD_JOB_PREPARED))
			atomic_store(&e->sources[i].cancel, 1);
	e->status.media_request = r->info.request; /* reserve through rewind completion */
	e->status.next_source = e->status.next_track = 0;
	e->status.next_state = TEDER_NEXT_NONE;
	td_unlock(e);
	if (loaded)
		code = td_output_quiesce(e);
	td_lock(e);
	/* Failed native stop already uninitialized and invalidated the stream. */
	if (loaded && code == TEDER_OK) {
		e->status.current.epoch++;
		td_analysis_invalidate(e);
	}
	if (loaded && code == TEDER_OK) {
		e->status.transport = TEDER_READY;
		e->status.source_frame = e->status.output_frame = 0;
		e->status.underrun_frames = 0;
		e->status.clock_flags &= TEDER_DURATION_KNOWN;
		e->status.position_quality = TEDER_QUALITY_UNKNOWN;
	}
	r->info.committed = e->status.current;
	r->info.track = e->status.track;
	uint64_t epoch = e->status.current.epoch;
	td_unlock(e);
	if (loaded && code == TEDER_OK)
		td_sources_suspend(e, epoch);
	if (!preparation_finished(e))
		return;
	if (loaded && code == TEDER_OK) {
		td_media_retarget(e, slot);
		preparation_finished(e);
		return; /* staged rewind or shutdown owns terminal completion */
	}
	td_lock(e);
	if (code != TEDER_OK && e->status.output_state != TEDER_OUTPUT_LOST) {
		e->status.transport = TEDER_FAILED;
		e->status.last_error = code;
	}
	td_complete(e, slot, code);
	td_unlock(e);
}
