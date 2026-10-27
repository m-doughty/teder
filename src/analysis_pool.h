#ifndef TEDER_ANALYSIS_POOL_H
#define TEDER_ANALYSIS_POOL_H
#include "internal.h"

typedef struct td_snapshot_pool td_snapshot_pool;
struct teder_snapshot {
	td_handle header;
	td_snapshot_pool *pool;
	teder_snapshot_info info;
	float *bands;
	teder_span *spans;
	uint32_t loans;
};

/* The owner pin protects publication/acquisition and all their gate waits.
 * Readers own a loan before touching this pool. No field points into an engine.
 * Payload is immutable while loans exist; only the gate protects loan counts. */
struct td_snapshot_pool {
	td_arena *arena;
	td_mutex gate;
	teder_snapshot *slots, *current;
	size_t bytes;
	uint32_t count, max_bands, max_spans, loan_limit, loans;
	uint64_t sequence, dropped;
	bool owned;
};

teder_code td_snapshot_pool_plan(const teder_config *, size_t *bytes);
teder_code td_snapshot_pool_create(
    const teder_config *, size_t limit, td_allocator, td_runtime, td_snapshot_pool **out);
/* Worker/owner calls only. Input views must remain valid through the copy.
 * Sequence and publication-drop counts are generated here, never taken from input.
 * AGAIN means all slots are pinned; it increments the saturating drop counter. */
teder_code td_snapshot_publish(td_snapshot_pool *,
                               const teder_snapshot_info *,
                               const float *bands,
                               const teder_span *spans);
teder_code td_snapshot_acquire(td_snapshot_pool *, teder_snapshot **out);
void td_snapshot_clear(td_snapshot_pool *); /* invalidate current, retain loans/counters */
/* Quiesce publisher/acquirer first; outstanding readers may release concurrently. */
void td_snapshot_pool_release(td_snapshot_pool **owner);
#endif
