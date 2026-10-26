#include "internal.h"
#include "source.h"

teder_code TEDER_CALL teder_transport(teder_engine *e,
                                      const teder_target *target,
                                      uint32_t action,
                                      uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	if (!target || action < TEDER_PLAY || action > TEDER_STOP)
		return TEDER_INVALID;
	td_media_input input = {.target = *target, .flags = action};
	return td_submit_media(e, TEDER_OP_TRANSPORT, NULL, 0, &input, request);
}

teder_code TEDER_CALL teder_set_gain(teder_engine *e, float gain, uint32_t mute, uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	if (!(gain >= 0 && gain <= 1) || mute > 1)
		return TEDER_INVALID;
	td_media_input input = {.gain = gain, .flags = mute};
	return td_submit_media(e, TEDER_OP_GAIN, NULL, 0, &input, request);
}

/* Control owns the producer here. Keep filling while waiting for the callback;
 * no callback wake or native lifecycle call is needed to acknowledge a control. */
static teder_code applied(teder_engine *e, uint64_t revision)
{
	for (;;) {
		td_device_report report;
		teder_code code = td_device_get_report(e->device, &report);
		if (code != TEDER_OK && code != TEDER_AGAIN)
			return code;
		if (code == TEDER_OK && report.control == revision)
			return report.error;
		if (atomic_load(&e->lifecycle) != TEDER_LIVE) {
			code = td_output_quiesce(e);
			if (code != TEDER_OK)
				return code;
			code = td_device_get_report(e->device, &report);
			if (code != TEDER_OK)
				return code;
			return report.control == revision ? report.error : TEDER_CANCELLED;
		}
		bool progress = td_pcm_reap(&e->pcm) != 0;
		progress = td_decode_pump(e) || progress;
		if (!progress)
			td_worker_interval();
	}
}

void td_transport(teder_engine *e, uint32_t slot)
{
	if (e->requests[slot].info.operation == TEDER_OP_TRANSPORT &&
	    e->requests[slot].media.flags == TEDER_STOP) {
		td_stop(e, slot);
		return;
	}
	td_refill_pause(e);
	td_output_report(e);
	td_media_checkpoint(e, e->requests[slot].info.request, TD_TRANSPORT_BEFORE_COMMIT);
	td_lock(e);
	td_request_slot *r = &e->requests[slot];
	bool gain = r->info.operation == TEDER_OP_GAIN;
	bool play = !gain && r->media.flags == TEDER_PLAY;
	uint32_t state = e->status.transport;
	bool running = state == TEDER_PLAYING || state == TEDER_DRAINING;
	bool noop = gain   ? r->media.gain == e->status.gain && r->media.flags == e->status.mute
	            : play ? running
	                   : state == TEDER_PAUSED || state == TEDER_READY;
	teder_code code = TEDER_OK;
	if (!gain && (r->media.target.epoch != e->status.current.epoch ||
	              r->media.target.source != e->status.current.source))
		code = TEDER_STALE;
	else if (!gain && (state == TEDER_EMPTY || state == TEDER_ENDED || state == TEDER_FAILED ||
	                   (play && e->status.output_state != TEDER_OUTPUT_READY)))
		code = TEDER_BAD_STATE;
	if (code == TEDER_OK)
		code = td_commit_locked(e, slot);
	if (code == TEDER_OK && !gain) {
		r->info.committed = e->status.current;
		r->info.track = e->status.track;
	}
	td_audio_config config = {e->status.current.epoch,
	                          gain ? r->media.gain : e->status.gain,
	                          gain ? running : play,
	                          gain ? r->media.flags : e->status.mute};
	td_unlock(e);
	if (code != TEDER_OK || noop)
		goto complete;
	if (td_device_running(e->device)) {
		uint64_t revision;
		code = td_device_control(e->device, config, &revision);
		if (code == TEDER_OK) {
			td_media_checkpoint(e, r->info.request, TD_TRANSPORT_PUBLISHED);
			code = applied(e, revision);
		}
	} else if (play) {
		e->prefill = true;
		while (td_decode_pump(e))
			;
		e->prefill = false;
		bool empty = false;
		for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
			if (e->sources[i].state == TD_JOB_ACTIVE) {
				td_source *s = e->sources[i].source;
				code = s->error;
				empty = s->done && !s->output;
			}
		if (e->status.transport == TEDER_FAILED)
			code = e->status.last_error;
		if (e->status.next_state == TEDER_NEXT_COMMITTED)
			empty = false;
		if (code == TEDER_OK && !empty)
			code = td_output_start(e);
		if (code == TEDER_OK && empty) {
			td_lock(e);
			e->status.clock_flags |= TEDER_SUBMIT_EOF;
			td_unlock(e);
		}
	}
	if (code == TEDER_OK) {
		td_output_report(e);
		td_lock(e);
		if (gain) {
			e->status.gain = config.gain;
			e->status.mute = config.mute;
		} else if (e->status.transport != TEDER_FAILED) {
			e->status.transport = e->status.clock_flags & TEDER_SUBMIT_EOF   ? TEDER_ENDED
			                      : !play                                    ? TEDER_PAUSED
			                      : e->status.clock_flags & TEDER_DECODE_EOF ? TEDER_DRAINING
			                                                                 : TEDER_PLAYING;
		}
		td_unlock(e);
	}
complete:
	td_lock(e);
	td_complete(e, slot, code);
	td_unlock(e);
}
