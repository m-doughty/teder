#include "analysis_pool.h"
#include "spectrum.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
	uint64_t source, track;
} identity;
/* One configuration arena: history, identities, spans, FFT and filters die together. */
typedef struct {
	td_arena *arena;
	size_t bytes;
	teder_analysis_config config;
	teder_snapshot_info info;
	teder_span *spans;
	identity *identities;
	float *pcm, *bands;
	td_spectrum *spectrum;
	double rms_power[2];
	float peak[2];
	uint32_t fresh; /* new frames since the last window; excludes retained overlap */
	bool filtered;
} work;

typedef struct {
	size_t capacity, spans, pcm, identities, bands, spectrum;
} layout;

static td_spectrum_config spectrum_config(const teder_analysis_config *c)
{
	return (td_spectrum_config){c->window_frames, c->bands, c->low_hz, c->high_hz, c->floor_db};
}

static teder_code plan(const teder_analysis_config *c, uint32_t spans, layout *l)
{
	*l = (layout){.capacity = sizeof(work)};
	if (!td_layout_add(&l->capacity, spans, sizeof(teder_span), _Alignof(teder_span), &l->spans) ||
	    !td_layout_add(
	        &l->capacity, c->window_frames, 2 * sizeof(float), _Alignof(float), &l->pcm) ||
	    !td_layout_add(
	        &l->capacity, c->window_frames, sizeof(identity), _Alignof(identity), &l->identities) ||
	    !td_layout_add(&l->capacity, c->bands, sizeof(float), _Alignof(float), &l->bands))
		return TEDER_LIMIT;
	td_spectrum_config sc = spectrum_config(c);
	return c->bands ? td_spectrum_plan(&l->capacity, &sc, &l->spectrum) : TEDER_OK;
}

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
	if (c.smoothing_ms < 0 || c.peak_decay_db_per_s < 0)
		return TEDER_INVALID;
	if (!c.window_frames)
		c.window_frames = 2048;
	if (c.window_frames > TD_FFT_MAX || c.bands > e->config.max_bands || c.bands > TD_FFT_BANDS_MAX)
		return TEDER_LIMIT;
	if (c.bands && (c.window_frames < TD_FFT_MIN || (c.window_frames & (c.window_frames - 1))))
		return TEDER_INVALID;
	if (!c.hop_frames)
		c.hop_frames = c.bands ? c.window_frames / 4 : c.window_frames;
	if (c.hop_frames > c.window_frames)
		return TEDER_INVALID;
	if (!c.bands && (c.low_hz || c.high_hz || c.floor_db))
		return TEDER_UNSUPPORTED;
	if (c.bands) {
		if (!c.low_hz)
			c.low_hz = 20;
		if (!c.floor_db)
			c.floor_db = -90;
		if (c.low_hz < 0 || (c.high_hz && c.high_hz <= c.low_hz) || c.floor_db >= 0)
			return TEDER_INVALID;
	}
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
	layout l = {0};
	size_t bytes = 0, total;
	teder_code code = c.enabled ? plan(&c, e->config.max_spans, &l) : TEDER_OK;
	if (code == TEDER_OK && c.enabled && !td_arena_size(l.capacity, &bytes))
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
		td_arena *arena = td_arena_create(e->allocator, l.capacity);
		if (arena) {
			unsigned char *storage = td_arena_alloc(arena, l.capacity, 1, _Alignof(max_align_t));
			candidate = (work *)storage;
			*candidate = (work){.arena = arena,
			                    .bytes = bytes,
			                    .config = c,
			                    .spans = (teder_span *)(storage + l.spans),
			                    .pcm = (float *)(storage + l.pcm),
			                    .identities = (identity *)(storage + l.identities),
			                    .bands = c.bands ? (float *)(storage + l.bands) : NULL};
			if (c.bands) {
				td_spectrum_config sc = spectrum_config(&c);
				code = td_spectrum_init(
				    storage + l.spectrum, l.capacity - l.spectrum, &sc, &candidate->spectrum);
			}
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
	w->info = (teder_snapshot_info){.struct_size = sizeof(w->info),
	                                .abi_version = TEDER_ABI_VERSION,
	                                .channels = 2,
	                                .bands = w->config.bands};
	w->fresh = 0;
	w->filtered = false;
}

static bool measure(work *w, uint32_t max_spans)
{
	w->info.span_count = 0;
	for (uint32_t i = 0; i < w->config.window_frames; i++) {
		uint32_t n = w->info.span_count;
		identity id = w->identities[i];
		if (n && w->spans[n - 1].source == id.source && w->spans[n - 1].track == id.track)
			w->spans[n - 1].frames++;
		else if (n < max_spans)
			w->spans[w->info.span_count++] =
			    (teder_span){w->info.output_start + i, 1, id.source, id.track};
		else
			return false;
	}
	td_spectrum_values v = {0};
	if (w->spectrum) {
		if (td_spectrum_measure(
		        w->spectrum, w->pcm, w->config.window_frames, w->info.output_rate, &v) != TEDER_OK)
			return false;
	} else {
		double squares[2] = {0};
		for (uint32_t i = 0; i < w->config.window_frames; i++)
			for (unsigned ch = 0; ch < 2; ch++) {
				float sample = w->pcm[2 * i + ch];
				if (!isfinite(sample))
					return false;
				v.peak[ch] = fmaxf(v.peak[ch], fabsf(sample));
				squares[ch] += (double)sample * sample;
			}
		for (unsigned ch = 0; ch < 2; ch++)
			v.rms[ch] = (float)sqrt(squares[ch] / w->config.window_frames);
	}
	double seconds = (double)w->config.hop_frames / w->info.output_rate;
	double alpha = w->filtered && w->config.smoothing_ms > 0
	                   ? -expm1(-seconds * 1000 / w->config.smoothing_ms)
	                   : 1;
	double decay = w->filtered && w->config.peak_decay_db_per_s > 0
	                   ? pow(10, -seconds * w->config.peak_decay_db_per_s / 20)
	                   : 0;
	for (unsigned ch = 0; ch < 2; ch++) {
		double power = (double)v.rms[ch] * v.rms[ch];
		w->rms_power[ch] = alpha == 1 ? power : (1 - alpha) * w->rms_power[ch] + alpha * power;
		w->peak[ch] = fmaxf(v.peak[ch], (float)(w->peak[ch] * decay));
		w->info.rms[ch] = alpha == 1 ? v.rms[ch] : (float)sqrt(w->rms_power[ch]);
		w->info.peak[ch] = w->peak[ch];
	}
	for (uint32_t i = 0; i < w->config.bands; i++)
		w->bands[i] =
		    alpha == 1 ? v.bands[i] : (float)((1 - alpha) * w->bands[i] + alpha * v.bands[i]);
	w->filtered = true;
	return true;
}

static void publish(teder_engine *e, work *w)
{
	bool valid = measure(w, e->config.max_spans);
	if (!valid)
		w->filtered = false;
	td_media_checkpoint(e, 0, TD_ANALYSIS_BEFORE_PUBLISH);
	td_lock(e);
	/* Epoch mutation and configuration commitment use this same gate. */
	if (atomic_load(&e->lifecycle) == TEDER_LIVE && w->info.epoch == e->status.current.epoch &&
	    w->info.config_revision == atomic_load(&e->analysis.revision)) {
		if (!valid) {
			e->analysis_discarded = add(e->analysis_discarded, w->fresh);
			td_snapshot_drop(e->snapshots);
		} else {
			w->info.dropped_frames = td_analysis_dropped(e);
			teder_code code = td_snapshot_publish(e->snapshots, &w->info, w->bands, w->spans);
			if (code != TEDER_OK && code != TEDER_AGAIN)
				td_limit_close(e);
		}
		e->status.analysis_sequence = e->snapshots->sequence;
		e->status.analysis_dropped_publications = e->snapshots->dropped;
	}
	td_unlock(e);
	uint32_t hop = w->config.hop_frames, keep = w->config.window_frames - hop;
	memmove(w->pcm, w->pcm + 2 * hop, (size_t)keep * 2 * sizeof(float));
	memmove(w->identities, w->identities + hop, (size_t)keep * sizeof(identity));
	w->info.output_start += hop;
	w->info.frames = keep;
	w->fresh = 0;
}

static void consume(teder_engine *e, work *w, const td_analysis_block *b)
{
	td_media_checkpoint(e, 0, TD_ANALYSIS_BLOCK);
	td_lock(e);
	bool current =
	    b->epoch == e->status.current.epoch && b->revision == atomic_load(&e->analysis.revision);
	td_unlock(e);
	if (!current)
		return;
	bool same = w->info.epoch == b->epoch && w->info.config_revision == b->revision;
	if (w->info.epoch && (!same || w->info.output_rate != b->rate ||
	                      w->info.output_start + w->info.frames != b->output_start ||
	                      (b->flags & TD_ANALYSIS_DISCONTINUITY))) {
		if (same) {
			td_lock(e);
			e->analysis_discarded = add(e->analysis_discarded, w->fresh);
			td_unlock(e);
		}
		reset(w);
	}
	if (!w->info.epoch) {
		reset(w);
		w->info.epoch = b->epoch;
		w->info.config_revision = b->revision;
		w->info.output_start = b->output_start;
		w->info.output_rate = b->rate;
	}
	for (uint32_t offset = 0; offset < b->frames;) {
		if (atomic_load(&e->lifecycle) != TEDER_LIVE)
			return;
		uint32_t at = (uint32_t)w->info.frames;
		uint32_t count = w->config.window_frames - at;
		if (count > b->frames - offset)
			count = b->frames - offset;
		memcpy(w->pcm + 2 * at, b->samples + 2 * offset, (size_t)count * 2 * sizeof(float));
		for (uint32_t i = at; i < at + count; i++)
			w->identities[i] = (identity){b->source, b->track};
		w->info.frames += count;
		w->fresh += count;
		offset += count;
		if (w->info.frames == w->config.window_frames)
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
