#include "internal.h"
#include "source.h"
#include <stdlib.h>

enum { TD_CODEC_BYTES = 8 * 1024 * 1024 };

void td_media_checkpoint(teder_engine *e, uint64_t request, unsigned point)
{
	if (e->runtime.media_checkpoint)
		e->runtime.media_checkpoint(e->runtime.context, request, point);
}

static void source_checkpoint(void *context, unsigned point)
{
	td_source_job *j = context;
	td_media_checkpoint(j->engine, j->request, point);
}

static td_source_job *active(teder_engine *e)
{
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
		if (e->sources[i].state == TD_JOB_ACTIVE || e->sources[i].state == TD_JOB_SUSPENDED)
			return &e->sources[i];
	return NULL;
}

teder_code TEDER_CALL teder_open(teder_engine *e,
                                 const char *path,
                                 uint32_t bytes,
                                 uint64_t track,
                                 uint32_t flags,
                                 uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code != TEDER_OK)
		return code;
	if (flags & ~TEDER_OPEN_AUTOPLAY)
		return TEDER_INVALID;
	if (bytes > e->config.max_input_bytes || bytes > 32767)
		return TEDER_LIMIT;
	code = td_path_validate(path, bytes);
	if (code != TEDER_OK)
		return code;
	td_media_input input = {.track = track, .flags = flags};
	return td_submit_media(e, TEDER_OP_OPEN, path, bytes, &input, request);
}

teder_code TEDER_CALL teder_seek(teder_engine *e,
                                 const teder_target *target,
                                 uint64_t frame,
                                 uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	if (!target || frame >= (UINT64_C(1) << 36))
		return TEDER_INVALID;
	td_media_input input = {.target = *target, .frame = frame};
	return td_submit_media(e, TEDER_OP_SEEK, NULL, 0, &input, request);
}

/* The gate protects admission/accounting/state; all source calls occur outside it. */
bool td_media_dispatch(teder_engine *e, uint32_t slot)
{
	td_request_slot *r = &e->requests[slot];
	teder_code code = TEDER_OK;
	bool retarget = r->info.operation == TEDER_OP_SELECT_DEVICE ||
	                (r->info.operation == TEDER_OP_TRANSPORT && r->media.flags == TEDER_STOP);
	size_t reservation = 0, total = 0;
	if (r->info.operation != TEDER_OP_OPEN && r->info.operation != TEDER_OP_SEEK && !retarget)
		code = TEDER_UNSUPPORTED;
	else if ((e->status.current.epoch == UINT64_MAX && !retarget) ||
	         (r->info.operation == TEDER_OP_OPEN && e->last_source == UINT64_MAX)) {
		td_complete(e, slot, TEDER_LIMIT);
		td_limit_close(e);
		return false;
	} else if (r->info.operation == TEDER_OP_OPEN && r->media.flags & TEDER_OPEN_AUTOPLAY &&
	           e->status.output_state != TEDER_OUTPUT_READY)
		code = TEDER_BAD_STATE;
	else if (r->info.operation == TEDER_OP_SEEK && e->status.clock_flags & TEDER_DURATION_KNOWN &&
	         r->media.frame > e->status.duration_frames)
		code = TEDER_INVALID;
	else
		code = td_source_plan(r->input_bytes, TD_CODEC_BYTES, &reservation);
	if (code == TEDER_OK &&
	    (!td_size_add(e->owned_bytes, reservation, &total) || total > e->config.engine_bytes_limit))
		code = TEDER_LIMIT;
	if (code != TEDER_OK) {
		td_complete(e, slot, code);
		return false;
	}
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_source_job *j = &e->sources[i];
		if (j->state != TD_JOB_FREE)
			continue;
		if (r->info.operation == TEDER_OP_OPEN)
			r->media.target = e->status.current;
		j->slot = slot;
		j->request = r->info.request;
		j->path_bytes = r->input_bytes;
		j->reservation = reservation;
		e->owned_bytes = total;
		atomic_store(&j->cancel, 0);
		j->state = TD_JOB_STAGING;
		return true;
	}
	td_complete(e, slot, TEDER_BUSY);
	return false;
}

void td_prepare_run(teder_engine *e)
{
	td_media_checkpoint(e, 0, TD_PREPARE_STARTED);
	for (;;) {
		td_lock(e);
		td_source_job *j = NULL;
		for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
			if (e->sources[i].state == TD_JOB_STAGING)
				j = &e->sources[i];
		bool closing = atomic_load(&e->lifecycle) != TEDER_LIVE;
		if (!j) {
			td_unlock(e);
			if (closing)
				break;
			if (ma_event_wait(&e->prepare_wake) != MA_SUCCESS)
				abort();
			continue;
		}
		td_unlock(e);
		td_media_checkpoint(e, j->request, TD_PREPARE_JOB);
		td_lock(e);
		closing = atomic_load(&e->lifecycle) != TEDER_LIVE;
		td_request_slot *r = &e->requests[j->slot];
		bool selected = r->info.operation == TEDER_OP_SELECT_DEVICE;
		bool stopped = r->info.operation == TEDER_OP_TRANSPORT;
		bool retarget = selected || stopped;
		bool seek = r->info.operation == TEDER_OP_SEEK || retarget;
		td_source_config c = {.generation = r->media.target.epoch + (retarget ? 0 : 1),
		                      .source = seek ? r->media.target.source : e->last_source + 1,
		                      .track = seek ? e->status.track : r->media.track,
		                      .output_rate = e->status.output_rate,
		                      .codec_bytes = TD_CODEC_BYTES,
		                      .reservation = j->reservation,
		                      .cancel = &j->cancel,
		                      .lifecycle = &e->lifecycle,
		                      .context = j,
		                      .checkpoint = source_checkpoint};
		uint64_t frame = r->media.frame;
		td_unlock(e);
		teder_code code =
		    closing ? TEDER_CANCELLED
		            : td_source_open(
		                  (const char *)r->input, j->path_bytes, &c, e->allocator, &j->source);
		/* A fresh decoder is already at zero. Native seek rejects empty FLAC;
		 * priming instead validates its EOF and preserves whole-stream MD5. */
		if (code == TEDER_OK && seek && frame)
			code = td_source_seek(j->source, frame);
		if (code == TEDER_OK)
			do {
				code = td_source_prime(j->source);
			} while (code == TEDER_AGAIN);
		td_lock(e);
		j->outcome = code;
		j->state = TD_JOB_PREPARED;
		td_unlock(e);
		td_signal(e);
	}
	td_media_checkpoint(e, 0, TD_PREPARE_STOPPED);
}

static bool retire(teder_engine *e, td_source_job *j)
{
	teder_code code = td_source_retire(&j->source);
	if (j->source) {
		if (code != TEDER_BUSY)
			abort();
		return false;
	}
	td_lock(e);
	e->owned_bytes -= j->reservation;
	j->reservation = 0;
	j->state = TD_JOB_FREE;
	if (code != TEDER_OK)
		e->status.last_error = code;
	td_unlock(e);
	return true;
}

static void prepared(teder_engine *e, td_source_job *j)
{
	uint32_t slot = j->slot;
	td_media_checkpoint(e, j->request, TD_MEDIA_BEFORE_COMMIT);
	td_lock(e);
	td_request_slot *r = &e->requests[slot];
	teder_code code = j->outcome;
	bool selected = r->info.operation == TEDER_OP_SELECT_DEVICE;
	bool stopped = r->info.operation == TEDER_OP_TRANSPORT;
	bool retarget = selected || stopped;
	bool seek = r->info.operation == TEDER_OP_SEEK || retarget;
	bool autoplay = r->info.operation == TEDER_OP_OPEN && r->media.flags & TEDER_OPEN_AUTOPLAY;
	if (code == TEDER_OK &&
	    (j->source->generation.generation != e->status.current.epoch + (retarget ? 0 : 1) ||
	     (seek && (r->media.target.epoch != e->status.current.epoch ||
	               r->media.target.source != e->status.current.source))))
		code = TEDER_STALE;
	if (code == TEDER_OK && autoplay && e->status.output_state != TEDER_OUTPUT_READY)
		code = TEDER_BAD_STATE;
	bool play = !retarget && (autoplay || (seek && (e->status.transport == TEDER_PLAYING ||
	                                                e->status.transport == TEDER_DRAINING)));
	if (code == TEDER_OK && !retarget)
		code = td_commit_locked(e, slot);
	td_unlock(e);
	if (code == TEDER_OK)
		code = td_output_quiesce(e);
	td_lock(e);
	if (code == TEDER_OK) {
		td_source_job *old = active(e);
		if (old)
			old->state = TD_JOB_RETIRING;
		j->state = TD_JOB_ACTIVE;
		td_source *s = j->source;
		e->status.current = (teder_target){s->generation.generation, s->generation.source};
		if (!seek)
			e->last_source = s->generation.source;
		e->status.track = s->generation.track;
		e->status.source_frame = s->start;
		e->status.output_frame = 0;
		e->status.source_rate = s->info.sample_rate;
		e->status.source_channels = s->info.channels;
		e->status.duration_frames = s->info.duration_frames;
		e->status.clock_flags =
		    (s->info.duration_known ? TEDER_DURATION_KNOWN : 0) | (s->done ? TEDER_DECODE_EOF : 0);
		e->status.position_quality = TEDER_QUALITY_UNKNOWN;
		e->status.last_error = TEDER_OK;
		if (selected)
			e->status.transport = TEDER_PAUSED;
		else if (stopped || play || !seek || e->status.transport == TEDER_FAILED ||
		         e->status.transport == TEDER_ENDED)
			e->status.transport = TEDER_READY; /* native start must succeed first */
		if (!retarget && seek && s->done) {
			e->status.transport = TEDER_ENDED;
			e->status.clock_flags |= TEDER_SUBMIT_EOF;
		}
		e->status.next_source = e->status.next_track = 0;
		e->status.next_state = TEDER_NEXT_NONE;
		r->info.committed = e->status.current;
		r->info.track = e->status.track;
	}
	td_unlock(e);
	if (code == TEDER_OK) {
		/* With no external consumer, control may discard obsolete queued PCM.
		 * An attached endpoint selects generations at its own callback boundary. */
		if (!e->runtime.consumer_join) {
			td_pcm_view unused;
			td_pcm_select(&e->pcm, j->source->generation.generation);
			td_pcm_peek(&e->pcm, &unused);
		}
		td_media_checkpoint(e, j->request, TD_MEDIA_AFTER_COMMIT);
		if (play) {
			while (td_decode_pump(e))
				; /* bounded pool prefill, or validated EOF; no callback owns it yet */
			td_source *s = j->source;
			code = s->error;
			bool empty = s->done && s->output == 0;
			if (code == TEDER_OK && !empty)
				code = td_output_start(e);
			td_lock(e);
			if (code == TEDER_OK) {
				e->status.transport = empty ? TEDER_ENDED : TEDER_PLAYING;
				if (empty)
					e->status.clock_flags |= TEDER_SUBMIT_EOF;
			} else if (e->status.output_state == TEDER_OUTPUT_READY) {
				e->status.transport = TEDER_FAILED;
				e->status.last_error = code;
			}
			td_unlock(e);
		}
	} else if (!retire(e, j))
		abort(); /* uncommitted source has no PCM references */
	td_lock(e);
	if (retarget && code != TEDER_OK) {
		e->status.transport =
		    stopped || e->status.output_state == TEDER_OUTPUT_READY ? TEDER_FAILED : TEDER_PAUSED;
		e->status.last_error = code;
	}
	td_complete(e, slot, code);
	td_unlock(e);
}

bool td_decode_step(teder_engine *e)
{
	bool progress = false;
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_source_job *j = &e->sources[i];
		td_lock(e);
		uint32_t state = j->state;
		td_unlock(e);
		if (state == TD_JOB_PREPARED) {
			prepared(e, j);
			progress = true;
		}
	}
	td_pcm_reap(&e->pcm);
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_lock(e);
		bool retiring = e->sources[i].state == TD_JOB_RETIRING;
		td_unlock(e);
		if (retiring)
			progress |= retire(e, &e->sources[i]);
	}
	return td_decode_pump(e) || progress;
}

bool td_decode_pump(teder_engine *e)
{
	td_lock(e);
	td_source_job *j = active(e);
	td_unlock(e);
	if (!j || j->state == TD_JOB_SUSPENDED || j->source->error != TEDER_OK || j->source->done)
		return false;
	td_source *s = j->source;
	if (s->slot == TD_PCM_NONE && !e->pcm.free_count)
		return false;
	teder_code code = td_source_pump(s, &e->pcm);
	td_lock(e);
	e->status.duration_frames = s->info.duration_frames;
	e->status.clock_flags =
	    (s->info.duration_known ? TEDER_DURATION_KNOWN : 0) | (s->done ? TEDER_DECODE_EOF : 0);
	bool failed = code < 0 && code != TEDER_CLOSED && atomic_load(&e->lifecycle) == TEDER_LIVE;
	if (failed) {
		e->status.transport = TEDER_FAILED;
		e->status.last_error = code;
	}
	teder_event event = {.struct_size = sizeof(event),
	                     .abi_version = TEDER_ABI_VERSION,
	                     .kind = TEDER_EVENT_ERROR,
	                     .epoch = e->status.current.epoch,
	                     .from_source = e->status.current.source,
	                     .from_track = e->status.track,
	                     .code = code};
	td_unlock(e);
	if (failed)
		td_emit(e, &event);
	return true;
}

void td_sources_close(teder_engine *e)
{
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
		if (e->sources[i].state != TD_JOB_FREE && !retire(e, &e->sources[i]))
			abort();
}

/* The lifecycle owner has joined the native consumer. Return old generations
 * through the existing queues; do not reset live cursors or reuse descriptors. */
void td_sources_suspend(teder_engine *e, uint64_t epoch)
{
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_source_job *j = &e->sources[i];
		td_lock(e);
		if (j->state == TD_JOB_ACTIVE)
			j->state = TD_JOB_SUSPENDED;
		bool owned = j->state == TD_JOB_SUSPENDED || j->state == TD_JOB_RETIRING;
		td_unlock(e);
		if (owned && j->source->slot != TD_PCM_NONE) {
			if (td_pcm_abort(&e->pcm, j->source->slot) != TEDER_OK)
				abort();
			j->source->slot = TD_PCM_NONE;
		}
	}
	td_pcm_view unused;
	if (td_pcm_select(&e->pcm, epoch) != TEDER_OK || td_pcm_peek(&e->pcm, &unused) != TEDER_AGAIN)
		abort();
	td_pcm_reap(&e->pcm);
}

void td_media_retarget(teder_engine *e, uint32_t slot)
{
	td_lock(e);
	td_source_job *old = active(e);
	td_request_slot *r = &e->requests[slot];
	/* The device open has finished borrowing the copied key. Reuse this bounded
	 * slot for the retained source path until preparation returns. */
	memcpy(r->input, old->source->path, old->path_bytes);
	r->input_bytes = old->path_bytes;
	r->media.target = e->status.current;
	r->media.frame = e->status.source_frame;
	bool dispatched = td_media_dispatch(e, slot);
	if (!dispatched) {
		e->status.transport = TEDER_FAILED;
		e->status.last_error = r->info.outcome;
	}
	td_unlock(e);
	if (dispatched)
		td_prepare_signal(e);
}
