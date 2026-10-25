#include "pcm.h"

static unsigned next(const td_pcm_queue *q, unsigned index) { return index == q->capacity ? 0 : index + 1; }
bool td_pcm_push(td_pcm_queue *q, uint32_t slot)
{
	unsigned write = atomic_load_explicit(&q->write, memory_order_relaxed), end = next(q, write);
	if (end == atomic_load_explicit(&q->read, memory_order_acquire)) return false;
	q->cells[write] = slot;
	atomic_store_explicit(&q->write, end, memory_order_release);
	return true;
}
bool td_pcm_pop(td_pcm_queue *q, uint32_t *slot)
{
	unsigned read = atomic_load_explicit(&q->read, memory_order_relaxed);
	if (read == atomic_load_explicit(&q->write, memory_order_acquire)) return false;
	*slot = q->cells[read];
	atomic_store_explicit(&q->read, next(q, read), memory_order_release);
	return true;
}
static teder_code return_block(td_pcm_pool *p)
{
	td_pcm_block *b = &p->blocks[p->reading];
	atomic_store_explicit(&b->state, TD_PCM_RETURNED, memory_order_relaxed);
	if (!td_pcm_push(&p->returned, p->reading)) {
		/* Capacity equals the whole pool, so valid ownership makes this
		 * unreachable. Retain the consumed block safely if the invariant fails. */
		atomic_store_explicit(&b->state, TD_PCM_READING, memory_order_relaxed);
		return TEDER_AGAIN;
	}
	p->reading = TD_PCM_NONE; p->offset = 0;
	return TEDER_OK;
}
teder_code td_pcm_select(td_pcm_pool *p, uint64_t generation)
{
	if (!p || !generation) return TEDER_INVALID;
	if (p->closed || p->stopped) return TEDER_CLOSED;
	if (generation < p->selected) return TEDER_STALE;
	p->selected = generation;
	return TEDER_OK;
}
teder_code td_pcm_peek(td_pcm_pool *p, td_pcm_view *out)
{
	if (!p || !out) return TEDER_INVALID;
	if (p->closed) return TEDER_CLOSED;
	for (uint32_t visited = 0; visited < p->count; visited++) {
		if (p->reading == TD_PCM_NONE) {
			if (!td_pcm_pop(&p->ready, &p->reading)) return p->stopped ? TEDER_CLOSED : TEDER_AGAIN;
			p->offset = 0;
			atomic_store_explicit(&p->blocks[p->reading].state, TD_PCM_READING, memory_order_relaxed);
		}
		td_pcm_block *b = &p->blocks[p->reading];
		if (p->stopped || b->owner->generation < p->selected || p->offset == b->meta.frames) {
			p->offset = b->meta.frames;
			teder_code code = return_block(p);
			if (code != TEDER_OK) return code;
			continue;
		}
		if (b->owner->generation > p->selected) return TEDER_AGAIN;
		*out = (td_pcm_view){b->samples + (size_t)p->offset * 2, &b->meta, b->owner,
			p->offset, b->meta.frames - p->offset};
		return TEDER_OK;
	}
	return TEDER_AGAIN;
}
teder_code td_pcm_consume(td_pcm_pool *p, uint32_t frames)
{
	if (!p || !frames) return TEDER_INVALID;
	if (p->closed || p->stopped) return TEDER_CLOSED;
	if (p->reading == TD_PCM_NONE) return TEDER_BAD_STATE;
	td_pcm_block *b = &p->blocks[p->reading];
	if (b->owner->generation != p->selected) return TEDER_STALE;
	if (frames > b->meta.frames - p->offset) return TEDER_INVALID;
	p->offset += frames;
	return p->offset == b->meta.frames ? return_block(p) : TEDER_OK;
}
teder_code td_pcm_stop(td_pcm_pool *p)
{
	if (!p) return TEDER_INVALID;
	p->stopped = true;
	td_pcm_view unused;
	return td_pcm_peek(p, &unused);
}
