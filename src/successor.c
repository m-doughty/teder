#include "internal.h"
#include "source.h"

teder_code TEDER_CALL teder_prepare_next(teder_engine *e,
                                         const teder_target *target,
                                         uint64_t expected,
                                         const char *path,
                                         uint32_t bytes,
                                         uint64_t track,
                                         uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code != TEDER_OK)
		return code;
	if (!target)
		return TEDER_INVALID;
	if (bytes > e->config.max_input_bytes || bytes > 32767)
		return TEDER_LIMIT;
	code = td_path_validate(path, bytes);
	if (code != TEDER_OK)
		return code;
	td_media_input input = {.target = *target, .track = track, .next = expected};
	return td_submit_media(e, TEDER_OP_PREPARE, path, bytes, &input, request);
}

teder_code TEDER_CALL teder_clear_next(teder_engine *e,
                                       const teder_target *target,
                                       uint64_t expected,
                                       uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	if (!target)
		return TEDER_INVALID;
	td_media_input input = {.target = *target, .next = expected};
	return td_submit_media(e, TEDER_OP_CLEAR_NEXT, NULL, 0, &input, request);
}

teder_code
td_successor_check(teder_engine *e, const td_media_input *input, bool prepare, bool finishing)
{
	teder_status *s = &e->status;
	if (input->target.epoch != s->current.epoch)
		return TEDER_STALE;
	/* Adoption wins over the target advancing at a natural boundary only. */
	if (input->target.source != s->current.source)
		return input->next && input->next == s->current.source ? TEDER_TOO_LATE : TEDER_STALE;
	if (input->next && input->next == s->next_source && s->next_state == TEDER_NEXT_COMMITTED)
		return TEDER_TOO_LATE;
	if (prepare && s->transport == TEDER_ENDED)
		return finishing ? TEDER_TOO_LATE : TEDER_BAD_STATE;
	if (input->next != s->next_source)
		return TEDER_STALE;
	if (prepare && (!s->current.source || s->transport == TEDER_FAILED))
		return TEDER_BAD_STATE;
	return TEDER_OK;
}

void td_successor_drop(teder_engine *e, bool adopted)
{
	for (unsigned i = 0; i < TD_SOURCE_JOBS; i++) {
		td_source_job *j = &e->sources[i];
		if (j->state == TD_JOB_NEXT || (adopted && j->state == TD_JOB_ADOPTED))
			j->state = TD_JOB_RETIRING;
	}
	if (adopted || e->status.next_state != TEDER_NEXT_COMMITTED) {
		e->status.next_source = e->status.next_track = 0;
		e->status.next_state = TEDER_NEXT_NONE;
	}
}

teder_code td_successor_ready(teder_engine *e, td_source_job *j)
{
	td_request_slot *r = &e->requests[j->slot];
	teder_code code = td_successor_check(e, &r->media, true, true);
	if (code == TEDER_OK)
		code = j->outcome;
	if (code == TEDER_OK)
		code = td_commit_locked(e, j->slot);
	if (code != TEDER_OK)
		return code;
	td_successor_drop(e, false);
	j->state = TD_JOB_NEXT;
	td_pcm_generation *g = &j->source->generation;
	e->last_source = e->status.next_source = g->source;
	e->status.next_track = g->track;
	e->status.next_state = TEDER_NEXT_READY;
	r->info.committed = (teder_target){g->generation, g->source};
	r->info.track = g->track;
	return TEDER_OK;
}

void td_successor_clear(teder_engine *e, uint32_t slot)
{
	td_refill_pause(e);
	td_request_slot *r = &e->requests[slot];
	td_media_checkpoint(e, r->info.request, TD_MEDIA_BEFORE_COMMIT);
	td_lock(e);
	teder_code code = td_successor_check(e, &r->media, false, true);
	if (code == TEDER_OK)
		code = td_commit_locked(e, slot);
	if (code == TEDER_OK) {
		td_successor_drop(e, false);
		r->info.committed = e->status.current;
		r->info.track = e->status.track;
	}
	td_unlock(e);
	if (code == TEDER_OK)
		td_media_checkpoint(e, r->info.request, TD_MEDIA_AFTER_COMMIT);
	td_lock(e);
	td_complete(e, slot, code);
	td_unlock(e);
}

/* Called only by the exclusive PCM producer. Future gapless scheduling chooses when to pump the
 * successor; this hook binds its first real queue publication to adoption.
 * No I/O occurs under the gate. The callback acquires only the ready queue. */
teder_code
td_successor_publish(void *context, td_pcm_pool *pool, uint32_t slot, const td_pcm_meta *meta)
{
	td_source_job *j = context;
	teder_engine *e = j->engine;
	td_media_checkpoint(e, j->request, TD_NEXT_BEFORE_PUBLISH);
	td_lock(e);
	unsigned live = TEDER_LIVE;
	teder_code code = TEDER_STALE;
	if (j->state == TD_JOB_NEXT && j->source->generation.generation == e->status.current.epoch &&
	    j->source->generation.source == e->status.next_source) {
		code = TEDER_CANCELLED;
		if (atomic_compare_exchange_strong(&e->lifecycle, &live, TEDER_LIVE)) {
			code = td_pcm_publish(pool, slot, meta);
			if (code == TEDER_OK) {
				j->state = TD_JOB_ADOPTED;
				e->status.next_state = TEDER_NEXT_COMMITTED;
			}
		}
	}
	td_unlock(e);
	if (code == TEDER_OK)
		td_media_checkpoint(e, j->request, TD_NEXT_AFTER_PUBLISH);
	return code;
}
