#ifndef TEDER_ANALYSIS_FEED_H
#define TEDER_ANALYSIS_FEED_H
#include "memory.h"
#include "teder.h"
#include <stdatomic.h>
#include <limits.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && UINT_MAX == UINT32_MAX && ATOMIC_LONG_LOCK_FREE == 2 &&
                   ATOMIC_LLONG_LOCK_FREE == 2,
               "Analysis feed requires lock-free cursor/counter atomics");
enum { TD_ANALYSIS_FRAMES = 256, TD_ANALYSIS_DISCONTINUITY = 1 };
/* One source per record; adjacent records describe every track span. Source and
 * track zero identify inserted silence. Samples are post-gain interleaved stereo. */
typedef struct {
	uint64_t epoch, revision, output_start, source, track;
	uint32_t rate, frames, flags;
	float samples[TD_ANALYSIS_FRAMES * 2];
} td_analysis_block;

typedef struct {
	td_analysis_block *blocks;
	uint32_t capacity; /* capacity+1 cells; one remains empty */
	atomic_uint read, write;
	atomic_uint_fast64_t revision, dropped_frames;
	/* Producer-only continuity state. Owner must quiesce both roles to reset. */
	uint64_t epoch, last_revision, next_frame;
	uint32_t rate;
	bool broken;
} td_analysis_feed;

typedef struct {
	size_t blocks;
	uint32_t capacity;
} td_analysis_layout;
/* Append checked storage to the owner's arena plan; no separate feed lifetime. */
bool td_analysis_plan(size_t *used, uint32_t capacity, td_analysis_layout *);
void td_analysis_init(td_analysis_feed *, void *storage, const td_analysis_layout *);
/* Owner publishes zero to disable, a nonzero revision to enable. An in-flight
 * callback may finish its old revision; consumer filters revisions explicitly.
 * Each re-enable/reconfiguration uses a new revision, even without a callback gap. */
void td_analysis_revision(td_analysis_feed *, uint64_t revision);

typedef struct {
	td_analysis_feed *feed;
	uint32_t read, write, rate;
	uint64_t epoch, revision, start, cursor, frames;
	bool dropped, discontinuity;
} td_analysis_write;
/* One producer transaction per callback. Append copies but does not publish.
 * Finish publishes the complete interval, or drops it whole when capacity is
 * exhausted. Omitting finish aborts staging and marks the next record discontinuous.
 * Work visits at most capacity records; no allocation, lock, wake or reclamation. */
td_analysis_write
td_analysis_begin(td_analysis_feed *, uint64_t epoch, uint32_t rate, uint64_t start, bool enabled);
void td_analysis_append(
    td_analysis_write *, const float *, uint32_t frames, uint64_t source, uint64_t track);
void td_analysis_finish(td_analysis_write *);
/* Sole consumer. Peek borrows a record until consume; no source/PCM pointers
 * escape. Stop/join producer, consumer and all view users before arena release.
 * Disable does not discard queued data or permit resetting either cursor.
 * Device replacement transfers the producer role only after old dispatch stops. */
teder_code td_analysis_peek(td_analysis_feed *, const td_analysis_block **);
teder_code td_analysis_consume(td_analysis_feed *);
#endif
