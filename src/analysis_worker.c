#include "analysis_pool.h"
#include <math.h>
#include <stdlib.h>

/* One configuration arena: accumulator and bounded spans die together. No PCM
 * history is needed for the initial nonoverlapping, rectangular meter windows. */
typedef struct {
	td_arena *arena;
	size_t bytes;
	uint32_t window;
	teder_snapshot_info info;
	teder_span *spans;
	double squares[2];
	bool invalid;
} work;

static uint64_t add(uint64_t a, uint64_t b)
{
	return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

uint64_t td_analysis_dropped(teder_engine *e)
{
	return add(e->analysis_discarded,
	           atomic_load_explicit(&e->analysis.dropped_frames, memory_order_relaxed));
}

void td_analysis_invalidate(teder_engine *e)
{
	td_snapshot_clear(e->snapshots);
}

void td_analysis_signal(teder_engine *e)
{
	if (ma_event_signal(&e->analysis_wake) != MA_SUCCESS)
		abort();
}

teder_code TEDER_CALL teder_configure_analysis(teder_engine *e,
                                               const teder_analysis_config *input,
                                               uint64_t *request)
{
	if (!request)
		return TEDER_INVALID;
	*request = 0;
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code == TEDER_OK)
		code = td_prefix(input, sizeof(*input));
	if (code != TEDER_OK)
		return code;
	teder_analysis_config c = *input;
	if (c.enabled > 1 || c.reserved || !isfinite(c.low_hz) || !isfinite(c.high_hz) ||
	    !isfinite(c.floor_db) || !isfinite(c.smoothing_ms) || !isfinite(c.peak_decay_db_per_s))
		return TEDER_INVALID;
	if (!c.window_frames)
		c.window_frames = 2048;
	if (!c.hop_frames)
		c.hop_frames = c.window_frames;
	if (c.window_frames > 65536 || c.bands > e->config.max_bands)
		return TEDER_LIMIT;
	if (c.bands || c.hop_frames != c.window_frames || c.low_hz || c.high_hz || c.floor_db ||
	    c.smoothing_ms || c.peak_decay_db_per_s)
		return TEDER_UNSUPPORTED;
	return td_submit(e, TEDER_OP_ANALYSIS, &c, sizeof(c), request);
}

teder_code TEDER_CALL teder_snapshot_acquire(teder_engine *e, teder_snapshot **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	teder_code code = td_handle_check(e, TD_ENGINE);
	if (code != TEDER_OK)
		return code;
	if (atomic_load(&e->lifecycle) != TEDER_LIVE)
		return TEDER_CLOSED;
	if (!td_try_lock(e))
		return TEDER_AGAIN;
	code = atomic_load(&e->lifecycle) != TEDER_LIVE ? TEDER_CLOSED
	       : !atomic_load(&e->analysis.revision)    ? TEDER_AGAIN
	                                                : td_snapshot_acquire(e->snapshots, out);
	td_unlock(e);
	return code;
}

static void release(teder_engine *e, work *w)
{
	if (!w)
		return;
	size_t bytes = w->bytes;
	td_arena_destroy(w->arena);
	td_lock(e);
	e->owned_bytes -= bytes;
	td_unlock(e);
}

static work *configure(teder_engine *e, uint32_t slot, work *current)
{
	td_request_slot *r = &e->requests[slot];
	teder_analysis_config c;
	memcpy(&c, r->input, sizeof(c)); /* input storage is byte-aligned, owned until terminal */
	work *candidate = NULL;
	size_t capacity = sizeof(work), spans = 0, bytes = 0, total;
	teder_code code = TEDER_OK;
	if (c.enabled &&
	    (!td_layout_add(
	         &capacity, e->config.max_spans, sizeof(teder_span), _Alignof(teder_span), &spans) ||
	     !td_arena_size(capacity, &bytes)))
		code = TEDER_LIMIT;
	td_lock(e);
	if (r->cancelled)
		code = TEDER_CANCELLED;
	if (code == TEDER_OK &&
	    (!td_size_add(e->owned_bytes, bytes, &total) || total > e->config.engine_bytes_limit))
		code = TEDER_LIMIT;
	if (code == TEDER_OK)
		e->owned_bytes = total; /* old and candidate owners coexist until commitment */
	td_unlock(e);
	if (code == TEDER_OK && c.enabled) {
		td_arena *arena = td_arena_create(e->allocator, capacity);
		if (arena) {
			unsigned char *storage = td_arena_alloc(arena, capacity, 1, _Alignof(max_align_t));
			candidate = (work *)storage;
			*candidate = (work){.arena = arena,
			                    .bytes = bytes,
			                    .window = c.window_frames,
			                    .spans = (teder_span *)(storage + spans)};
		} else {
			td_lock(e);
			e->owned_bytes -= bytes;
			td_unlock(e);
			code = TEDER_NO_MEMORY;
		}
	}
	if (code == TEDER_OK)
		td_media_checkpoint(e, r->info.request, TD_ANALYSIS_BEFORE_COMMIT);
	td_lock(e);
	if (code == TEDER_OK && e->analysis_version == UINT64_MAX)
		code = TEDER_LIMIT;
	if (code == TEDER_OK)
		code = td_commit_locked(e, slot);
	if (code == TEDER_OK) {
		e->analysis_version++;
		td_analysis_revision(&e->analysis, c.enabled ? e->analysis_version : 0);
		td_analysis_invalidate(e);
	}
	td_unlock(e);
	if (code == TEDER_OK) {
		td_media_checkpoint(e, r->info.request, TD_ANALYSIS_AFTER_COMMIT);
		release(e, current);
		current = candidate;
	} else
		release(e, candidate);
	td_lock(e);
	td_complete(e, slot, code);
	td_unlock(e);
	return current;
}

static void reset(work *w)
{
	w->info = (teder_snapshot_info){
	    .struct_size = sizeof(w->info), .abi_version = TEDER_ABI_VERSION, .channels = 2};
	w->squares[0] = w->squares[1] = 0;
	w->invalid = false;
}

static void publish(teder_engine *e, work *w)
{
	for (unsigned ch = 0; ch < 2; ch++)
		w->info.rms[ch] = (float)sqrt(w->squares[ch] / w->info.frames);
	td_media_checkpoint(e, 0, TD_ANALYSIS_BEFORE_PUBLISH);
	td_lock(e);
	/* Epoch mutation and configuration commitment use this same gate. */
	if (atomic_load(&e->lifecycle) == TEDER_LIVE && w->info.epoch == e->status.current.epoch &&
	    w->info.config_revision == atomic_load(&e->analysis.revision)) {
		if (w->invalid) {
			e->analysis_discarded = add(e->analysis_discarded, w->info.frames);
			td_snapshot_drop(e->snapshots);
		} else {
			w->info.dropped_frames = td_analysis_dropped(e);
			teder_code code = td_snapshot_publish(e->snapshots, &w->info, NULL, w->spans);
			if (code != TEDER_OK && code != TEDER_AGAIN)
				td_limit_close(e); /* publication counters must never wrap */
		}
		e->status.analysis_sequence = e->snapshots->sequence;
		e->status.analysis_dropped_publications = e->snapshots->dropped;
	}
	td_unlock(e);
	reset(w);
}

static void consume(teder_engine *e, work *w, const td_analysis_block *b)
{
	td_media_checkpoint(e, 0, TD_ANALYSIS_BLOCK);
	td_lock(e);
	bool current =
	    b->epoch == e->status.current.epoch && b->revision == atomic_load(&e->analysis.revision);
	td_unlock(e);
	if (!current)
		return; /* intentional epoch/configuration invalidation is not pressure loss */
	bool same = w->info.epoch == b->epoch && w->info.config_revision == b->revision;
	if (w->info.frames && (!same || w->info.output_rate != b->rate ||
	                       w->info.output_start + w->info.frames != b->output_start ||
	                       (b->flags & TD_ANALYSIS_DISCONTINUITY))) {
		if (same) {
			td_lock(e);
			e->analysis_discarded = add(e->analysis_discarded, w->info.frames);
			td_unlock(e);
		}
		reset(w);
	}
	for (uint32_t offset = 0; offset < b->frames;) {
		if (!w->info.frames) {
			reset(w);
			w->info.epoch = b->epoch;
			w->info.config_revision = b->revision;
			w->info.output_start = b->output_start + offset;
			w->info.output_rate = b->rate;
		}
		uint32_t count = w->window - (uint32_t)w->info.frames;
		if (count > b->frames - offset)
			count = b->frames - offset;
		uint32_t n = w->info.span_count;
		if (n && w->spans[n - 1].source == b->source && w->spans[n - 1].track == b->track)
			w->spans[n - 1].frames += count;
		else if (n < e->config.max_spans)
			w->spans[w->info.span_count++] =
			    (teder_span){b->output_start + offset, count, b->source, b->track};
		else
			w->invalid = true;
		for (uint32_t i = offset; i < offset + count; i++)
			for (unsigned ch = 0; ch < 2; ch++) {
				float sample = b->samples[2 * i + ch];
				if (!isfinite(sample)) {
					w->invalid = true;
					continue;
				}
				float magnitude = fabsf(sample);
				if (magnitude > w->info.peak[ch])
					w->info.peak[ch] = magnitude;
				w->squares[ch] += (double)sample * sample;
			}
		w->info.frames += count;
		offset += count;
		if (w->info.frames == w->window)
			publish(e, w);
	}
}

void td_analysis_run(teder_engine *e)
{
	work *w = NULL;
	td_media_checkpoint(e, 0, TD_ANALYSIS_STARTED);
	for (;;) {
		td_lock(e);
		if (atomic_load(&e->lifecycle) != TEDER_LIVE) {
			td_analysis_revision(&e->analysis, 0);
			td_unlock(e);
			break;
		}
		uint32_t slot = e->config.request_capacity;
		if (!e->runtime.execute)
			for (uint32_t i = 0; i < e->config.request_capacity; i++)
				if (e->requests[i].info.operation == TEDER_OP_ANALYSIS &&
				    e->requests[i].info.state == TEDER_QUEUED) {
					slot = i; /* admission permits only one pending configuration */
					e->requests[i].info.state = TEDER_RUNNING;
					break;
				}
		td_unlock(e);
		if (slot != e->config.request_capacity) {
			w = configure(e, slot, w);
			continue;
		}
		const td_analysis_block *block;
		if (w && td_analysis_peek(&e->analysis, &block) == TEDER_OK) {
			consume(e, w, block);
			td_analysis_consume(&e->analysis);
		} else if (w)
			td_worker_sleep(5); /* callbacks never wake or block on this worker */
		else if (ma_event_wait(&e->analysis_wake) != MA_SUCCESS)
			abort();
	}
	release(e, w);
	td_media_checkpoint(e, 0, TD_ANALYSIS_STOPPED);
}
