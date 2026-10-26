#include "internal.h"
#include "source.h"

/* The active source is fully decoded. Keep its final PCM immutable and append
 * the successor through the same producer/queue; never restart the device. */
bool td_transition_pump(teder_engine *e, td_source_job *previous)
{
	td_lock(e);
	td_source_job *next = NULL;
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_source_job *j = &e->sources[i];
		if (j->state == TD_JOB_ADOPTED ||
		    (j->state == TD_JOB_NEXT && (e->prefill || e->status.transport == TEDER_PLAYING ||
		                                 e->status.transport == TEDER_DRAINING)))
			next = j;
	}
	td_unlock(e);
	if (!next || next->source->error != TEDER_OK || (next->source->done && next->source->published))
		return false;
	td_source *s = next->source, *old = previous->source;
	if (s->slot == TD_PCM_NONE && !e->pcm.free_count)
		return false;
	teder_code code = TEDER_LIMIT;
	if (old->config.output_start <= UINT64_MAX - old->output) {
		s->config.output_start = old->config.output_start + old->output;
		code = td_source_pump(s, &e->pcm);
	}
	if (code >= 0 || code == TEDER_CLOSED)
		return true;
	td_lock(e);
	bool adopted = next->state == TD_JOB_ADOPTED;
	if (adopted)
		e->status.transport = TEDER_FAILED;
	else
		td_successor_drop(e, false);
	e->status.last_error = code;
	teder_event event = {.struct_size = sizeof(event),
	                     .abi_version = TEDER_ABI_VERSION,
	                     .kind = TEDER_EVENT_ERROR,
	                     .epoch = e->status.current.epoch,
	                     .from_source = e->status.current.source,
	                     .from_track = e->status.track,
	                     .to_source = s->generation.source,
	                     .to_track = s->generation.track,
	                     .code = code};
	td_unlock(e);
	td_emit(e, &event);
	return true;
}

/* The coherent device report proves the successor was submitted. Its source
 * arena retains the first boundary index even if several callbacks ran first. */
bool td_transition_report(teder_engine *e, const td_device_report *report, teder_event *event)
{
	if (report->generation != e->status.current.epoch)
		return false;
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_source_job *j = &e->sources[i];
		if (j->state != TD_JOB_ADOPTED || j->source->generation.source != report->source ||
		    j->source->generation.track != report->track)
			continue;
		td_source *s = j->source;
		uint64_t first = atomic_load_explicit(&s->generation.first_output, memory_order_acquire);
		if (first > report->output_frames)
			return false;
		*event = (teder_event){.struct_size = sizeof(*event),
		                       .abi_version = TEDER_ABI_VERSION,
		                       .kind = TEDER_EVENT_TRANSITION,
		                       .epoch = report->generation,
		                       .output_frame = first,
		                       .from_source = e->status.current.source,
		                       .from_track = e->status.track,
		                       .to_source = report->source,
		                       .to_track = report->track};
		for (unsigned k = 0; k < TD_SOURCE_JOBS; k++)
			if (e->sources[k].state == TD_JOB_ACTIVE)
				e->sources[k].state = TD_JOB_RETIRING;
		j->state = TD_JOB_ACTIVE;
		e->status.current.source = report->source;
		e->status.track = report->track;
		e->status.source_rate = s->info.sample_rate;
		e->status.source_channels = s->info.channels;
		e->status.duration_frames = s->info.duration_frames;
		e->status.clock_flags =
		    (s->info.duration_known ? TEDER_DURATION_KNOWN : 0) | (s->done ? TEDER_DECODE_EOF : 0);
		e->status.next_state = TEDER_NEXT_NONE;
		e->status.next_source = e->status.next_track = 0;
		if (e->status.transport == TEDER_DRAINING && !s->done)
			e->status.transport = TEDER_PLAYING;
		return true;
	}
	return false;
}
