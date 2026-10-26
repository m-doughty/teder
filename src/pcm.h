#ifndef TEDER_PCM_H
#define TEDER_PCM_H
#include "teder.h"
#include "memory.h"
#include <stdatomic.h>
#include <limits.h>
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && UINT_MAX == UINT32_MAX,
               "PCM requires lock-free 32-bit unsigned atomics");

enum { TD_PCM_BLOCK_FRAMES = 1024 };

#define TD_PCM_NONE UINT32_MAX

enum { TD_PCM_FREE, TD_PCM_FILLING, TD_PCM_PUBLISHED, TD_PCM_READING, TD_PCM_RETURNED };

enum { TD_PCM_DISCONTINUITY = 1, TD_PCM_BOUNDARY = 2, TD_PCM_FINAL = 4 };

/* One descriptor allocated inside each source arena. Retire owns that entire
 * arena, including deferred dependency destructors. Identities are immutable.
 * Only the producer changes references/retiring; callback reads identities.
 * Before retirement, stop all decoder/converter uses of that arena too. */
typedef struct {
	td_arena *arena;
	uint64_t generation, source, track;
	uint32_t references;
	bool retiring;
} td_pcm_generation;

typedef struct {
	uint64_t source_start, source_frames, output_start;
	uint32_t frames, flags;
} td_pcm_meta;

typedef struct {
	float *samples;
	uint32_t capacity;
	atomic_uint state;
	td_pcm_generation *owner;
	td_pcm_meta meta;
} td_pcm_block;

/* SPSC: writer owns write; reader owns read. capacity+1 cells hold capacity
 * entries. Each release/acquire pair transfers the cell and its block. */
typedef struct {
	uint32_t *cells, capacity;
	atomic_uint read, write;
} td_pcm_queue;

typedef struct {
	td_pcm_block *blocks;
	uint32_t count, *free_slots, free_count;
	td_pcm_queue ready, returned;
	uint64_t last_generation; /* producer only */
	uint64_t selected;        /* consumer only, initially no generation */
	uint32_t reading, offset; /* consumer only */
	bool stopped, closed;     /* stopped: consumer; closed: both roles quiescent */
} td_pcm_pool;

typedef struct {
	size_t blocks, free_slots, ready, returned, samples;
	uint32_t count;
} td_pcm_layout;

typedef struct {
	const float *samples;
	const td_pcm_meta *meta;
	const td_pcm_generation *generation;
	uint32_t offset, frames;
} td_pcm_view;

bool td_pcm_plan(size_t *used, uint32_t frames, td_pcm_layout *);
void td_pcm_init(td_pcm_pool *, void *base, const td_pcm_layout *, uint32_t frames);
teder_code td_pcm_generation_init(
    td_pcm_generation *, td_arena *, uint64_t generation, uint64_t source, uint64_t track);
teder_code
td_pcm_retire(td_pcm_generation **); /* marks retiring, BUSY while borrowed; producer only */
/* Producer-only. A write borrow ends at publish/abort. Reap returned blocks
 * before expecting them to become free. No operation grows a pool. */
teder_code td_pcm_acquire(td_pcm_pool *, td_pcm_generation *, uint32_t *slot);
teder_code td_pcm_publish(td_pcm_pool *, uint32_t slot, const td_pcm_meta *);
teder_code td_pcm_abort(td_pcm_pool *, uint32_t slot);
uint32_t td_pcm_reap(td_pcm_pool *);
/* Consumer-only. Select at a callback boundary after prior view use ends.
 * Views end on consume/select/stop/next peek; never retain one across a call.
 * Peek skips older generations, holds newer ones, and visits at most count
 * blocks. Output is unchanged on error. No callback operation frees or locks. */
teder_code td_pcm_select(td_pcm_pool *, uint64_t generation);
teder_code td_pcm_peek(td_pcm_pool *, td_pcm_view *);
teder_code td_pcm_consume(td_pcm_pool *, uint32_t frames);
teder_code
td_pcm_stop(td_pcm_pool *); /* returns CLOSED once currently queued blocks are returned */
/* Exclusive final cleanup after BOTH roles and all view users are joined or
 * otherwise quiescent. Only here may both queue cursors be reset. No restart. */
void td_pcm_clear(td_pcm_pool *);
bool td_pcm_push(td_pcm_queue *, uint32_t slot);
bool td_pcm_pop(td_pcm_queue *, uint32_t *slot);
#endif
