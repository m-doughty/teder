#include "pcm.h"
#include <assert.h>
#include <string.h>

bool td_pcm_plan(size_t *used, uint32_t frames, td_pcm_layout *out)
{
	if (!used || !out || !frames)
		return false;
	td_pcm_layout plan = {0};
	size_t end = *used;
	plan.count = (frames - 1) / TD_PCM_BLOCK_FRAMES + 1;
	if (!td_layout_add(
	        &end, plan.count, sizeof(td_pcm_block), _Alignof(td_pcm_block), &plan.blocks) ||
	    !td_layout_add(&end, plan.count, sizeof(uint32_t), _Alignof(uint32_t), &plan.free_slots) ||
	    !td_layout_add(
	        &end, (size_t)plan.count + 1, sizeof(uint32_t), _Alignof(uint32_t), &plan.ready) ||
	    !td_layout_add(
	        &end, (size_t)plan.count + 1, sizeof(uint32_t), _Alignof(uint32_t), &plan.returned) ||
	    !td_layout_add(&end, frames, 2 * sizeof(float), _Alignof(float), &plan.samples))
		return false;
	*used = end;
	*out = plan;
	return true;
}

static void queue_init(td_pcm_queue *q, uint32_t *cells, uint32_t count)
{
	q->cells = cells;
	q->capacity = count;
	atomic_init(&q->read, 0);
	atomic_init(&q->write, 0);
}

void td_pcm_init(td_pcm_pool *p, void *storage, const td_pcm_layout *layout, uint32_t frames)
{
	unsigned char *base = storage;
	memset(p, 0, sizeof(*p));
	p->reading = TD_PCM_NONE;
	p->count = layout->count;
	p->free_count = p->count;
	p->blocks = (td_pcm_block *)(base + layout->blocks);
	p->free_slots = (uint32_t *)(base + layout->free_slots);
	queue_init(&p->ready, (uint32_t *)(base + layout->ready), p->count);
	queue_init(&p->returned, (uint32_t *)(base + layout->returned), p->count);
	float *samples = (float *)(base + layout->samples);
	for (uint32_t i = 0; i < p->count; i++) {
		td_pcm_block *b = &p->blocks[i];
		memset(b, 0, sizeof(*b));
		size_t start = (size_t)i * TD_PCM_BLOCK_FRAMES;
		b->samples = samples + start * 2;
		b->capacity =
		    frames - start < TD_PCM_BLOCK_FRAMES ? (uint32_t)(frames - start) : TD_PCM_BLOCK_FRAMES;
		atomic_init(&b->state, TD_PCM_FREE);
		p->free_slots[i] = p->count - i - 1;
	}
}

static void reclaim(td_pcm_pool *p, uint32_t slot)
{
	td_pcm_block *b = &p->blocks[slot];
	assert(b->owner && b->owner->references && p->free_count < p->count);
	b->owner->references--;
	b->owner = NULL;
	atomic_store_explicit(&b->state, TD_PCM_FREE, memory_order_relaxed);
	p->free_slots[p->free_count++] = slot;
}

uint32_t td_pcm_reap(td_pcm_pool *p)
{
	uint32_t slot, count = 0;
	while (count < p->count && td_pcm_pop(&p->returned, &slot)) {
		assert(atomic_load_explicit(&p->blocks[slot].state, memory_order_relaxed) ==
		       TD_PCM_RETURNED);
		reclaim(p, slot);
		count++;
	}
	return count;
}

teder_code td_pcm_acquire(td_pcm_pool *p, td_pcm_generation *g, uint32_t *slot)
{
	if (!slot)
		return TEDER_INVALID;
	*slot = TD_PCM_NONE;
	if (!p || !g)
		return TEDER_INVALID;
	if (p->closed)
		return TEDER_CLOSED;
	if (g->retiring)
		return TEDER_STALE;
	if (g->references == UINT32_MAX)
		return TEDER_LIMIT;
	if (!p->free_count)
		return TEDER_AGAIN;
	uint32_t index = p->free_slots[--p->free_count];
	td_pcm_block *b = &p->blocks[index];
	b->owner = g;
	b->meta = (td_pcm_meta){0};
	g->references++;
	atomic_store_explicit(&b->state, TD_PCM_FILLING, memory_order_relaxed);
	*slot = index;
	return TEDER_OK;
}

teder_code td_pcm_abort(td_pcm_pool *p, uint32_t slot)
{
	if (!p || slot >= p->count)
		return TEDER_INVALID;
	if (atomic_load_explicit(&p->blocks[slot].state, memory_order_relaxed) != TD_PCM_FILLING)
		return TEDER_BAD_STATE;
	reclaim(p, slot);
	return TEDER_OK;
}

teder_code td_pcm_publish(td_pcm_pool *p, uint32_t slot, const td_pcm_meta *meta)
{
	if (!p || !meta || slot >= p->count)
		return TEDER_INVALID;
	td_pcm_block *b = &p->blocks[slot];
	if (atomic_load_explicit(&b->state, memory_order_relaxed) != TD_PCM_FILLING)
		return TEDER_BAD_STATE;
	if (!meta->frames || meta->frames > b->capacity ||
	    meta->flags & ~(TD_PCM_DISCONTINUITY | TD_PCM_BOUNDARY | TD_PCM_FINAL))
		return TEDER_INVALID;
	if (meta->source_frames > UINT64_MAX - meta->source_start ||
	    meta->frames > UINT64_MAX - meta->output_start)
		return TEDER_LIMIT;
	if (b->owner->retiring || b->owner->generation < p->last_generation)
		return TEDER_STALE;
	b->meta = *meta;
	uint64_t generation = b->owner->generation;
	atomic_store_explicit(&b->state, TD_PCM_PUBLISHED, memory_order_relaxed);
	if (!td_pcm_push(&p->ready, slot)) {
		atomic_store_explicit(&b->state, TD_PCM_FILLING, memory_order_relaxed);
		return TEDER_AGAIN;
	}
	p->last_generation = generation;
	return TEDER_OK;
}

void td_pcm_clear(td_pcm_pool *p)
{
	/* Exclusive quiescence also covers abandoned filling and partial blocks. */
	p->free_count = 0;
	for (uint32_t i = 0; i < p->count; i++) {
		if (p->blocks[i].owner)
			reclaim(p, i);
		else
			p->free_slots[p->free_count++] = i;
	}
	atomic_store(&p->ready.read, 0);
	atomic_store(&p->ready.write, 0);
	atomic_store(&p->returned.read, 0);
	atomic_store(&p->returned.write, 0);
	p->reading = TD_PCM_NONE;
	p->offset = 0;
	p->closed = true;
}
