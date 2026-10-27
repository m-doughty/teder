#include "analysis_pool.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>

static bool lock(td_snapshot_pool *p, bool attempt)
{
#ifdef _WIN32
	if (attempt)
		return TryAcquireSRWLockExclusive(&p->gate) != 0;
	AcquireSRWLockExclusive(&p->gate);
#else
	int code = attempt ? pthread_mutex_trylock(&p->gate) : pthread_mutex_lock(&p->gate);
	if (attempt && code == EBUSY)
		return false;
	if (code)
		abort();
#endif
	return true;
}

static void unlock(td_snapshot_pool *p)
{
#ifdef _WIN32
	ReleaseSRWLockExclusive(&p->gate);
#else
	if (pthread_mutex_unlock(&p->gate))
		abort();
#endif
}

static void finish(td_snapshot_pool *p)
{
	bool last = !p->owned && !p->loans;
	unlock(p);
	/* Any waiter already owns a pin/loan, so the last owner has no waiters. */
	if (last) {
#ifndef _WIN32
		if (pthread_mutex_destroy(&p->gate))
			abort();
#endif
		td_arena_destroy(p->arena);
	}
}

typedef struct {
	size_t capacity, bytes, slots, bands, spans;
} layout;

static teder_code plan(const teder_config *c, layout *l)
{
	teder_code code = td_prefix(c, sizeof(*c));
	if (code != TEDER_OK)
		return code;
	if (!c->snapshot_slots || !c->snapshot_loans || !c->max_bands || !c->max_spans)
		return TEDER_INVALID;
	*l = (layout){.capacity = sizeof(td_snapshot_pool)};
	size_t bands, spans;
	if (!td_size_mul(c->snapshot_slots, c->max_bands, &bands) ||
	    !td_size_mul(c->snapshot_slots, c->max_spans, &spans) ||
	    !td_layout_add(&l->capacity,
	                   c->snapshot_slots,
	                   sizeof(teder_snapshot),
	                   _Alignof(teder_snapshot),
	                   &l->slots) ||
	    !td_layout_add(&l->capacity, bands, sizeof(float), _Alignof(float), &l->bands) ||
	    !td_layout_add(&l->capacity, spans, sizeof(teder_span), _Alignof(teder_span), &l->spans) ||
	    !td_arena_size(l->capacity, &l->bytes))
		return TEDER_LIMIT;
	return TEDER_OK;
}

teder_code td_snapshot_pool_plan(const teder_config *c, size_t *bytes)
{
	if (!bytes)
		return TEDER_INVALID;
	*bytes = 0;
	layout l;
	teder_code code = plan(c, &l);
	if (code == TEDER_OK)
		*bytes = l.bytes;
	return code;
}

teder_code td_snapshot_pool_create(const teder_config *c,
                                   size_t limit,
                                   td_allocator allocator,
                                   td_runtime runtime,
                                   td_snapshot_pool **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!allocator.allocate || !allocator.release)
		return TEDER_INVALID;
	layout l;
	teder_code code = plan(c, &l);
	if (code != TEDER_OK || l.bytes > limit)
		return code != TEDER_OK ? code : TEDER_LIMIT;
	td_arena *arena = td_arena_create(allocator, l.capacity);
	if (!arena)
		return TEDER_NO_MEMORY;
	unsigned char *base = td_arena_alloc(arena, l.capacity, 1, _Alignof(max_align_t));
	memset(base, 0, l.capacity);
	td_snapshot_pool *p = (td_snapshot_pool *)base;
	bool ready = !runtime.permit || runtime.permit(runtime.context, TD_INIT_SNAPSHOT_GATE);
#ifdef _WIN32
	if (ready)
		InitializeSRWLock(&p->gate);
#else
	if (ready)
		ready = pthread_mutex_init(&p->gate, NULL) == 0;
#endif
	if (!ready) {
		td_arena_destroy(arena);
		return TEDER_INTERNAL;
	}
	p->arena = arena;
	p->bytes = l.bytes;
	p->slots = (teder_snapshot *)(base + l.slots);
	p->count = c->snapshot_slots;
	p->loan_limit = c->snapshot_loans;
	p->max_bands = c->max_bands;
	p->max_spans = c->max_spans;
	p->owned = true;
	for (uint32_t i = 0; i < p->count; i++) {
		teder_snapshot *s = &p->slots[i];
		s->header = (td_handle){TD_SNAPSHOT, TEDER_ABI_VERSION};
		s->pool = p;
		s->bands = (float *)(base + l.bands) + (size_t)i * p->max_bands;
		s->spans = (teder_span *)(base + l.spans) + (size_t)i * p->max_spans;
	}
	*out = p;
	return TEDER_OK;
}

static teder_code validate(td_snapshot_pool *p,
                           const teder_snapshot_info *info,
                           const float *bands,
                           const teder_span *spans)
{
	teder_code code = td_prefix(info, sizeof(*info));
	if (code != TEDER_OK)
		return code;
	if (info->bands > p->max_bands || info->span_count > p->max_spans)
		return TEDER_LIMIT;
	if (!info->epoch || !info->config_revision || !info->output_rate || info->channels != 2 ||
	    !info->frames || info->frames > UINT64_MAX - info->output_start ||
	    (info->bands && !bands) || !info->span_count || !spans)
		return TEDER_INVALID;
	for (unsigned i = 0; i < 2; i++)
		if (!isfinite(info->peak[i]) || info->peak[i] < 0 || !isfinite(info->rms[i]) ||
		    info->rms[i] < 0)
			return TEDER_INVALID;
	for (uint32_t i = 0; i < info->bands; i++)
		if (!isfinite(bands[i]))
			return TEDER_INVALID;
	uint64_t cursor = info->output_start, remaining = info->frames;
	for (uint32_t i = 0; i < info->span_count; i++) {
		if (spans[i].output_start != cursor || !spans[i].frames || spans[i].frames > remaining ||
		    (!spans[i].source && spans[i].track))
			return TEDER_INVALID;
		cursor += spans[i].frames;
		remaining -= spans[i].frames;
	}
	return remaining ? TEDER_INVALID : TEDER_OK;
}

teder_code td_snapshot_publish(td_snapshot_pool *p,
                               const teder_snapshot_info *info,
                               const float *bands,
                               const teder_span *spans)
{
	if (!p)
		return TEDER_INVALID;
	teder_code code = validate(p, info, bands, spans);
	if (code != TEDER_OK)
		return code;
	lock(p, false);
	assert(p->owned);
	if (p->sequence == UINT64_MAX) {
		unlock(p);
		return TEDER_LIMIT;
	}
	teder_snapshot *s = NULL;
	for (uint32_t i = 0; i < p->count; i++)
		if (!p->slots[i].loans) {
			s = &p->slots[i];
			break;
		}
	if (!s) {
		if (p->dropped != UINT64_MAX)
			p->dropped++;
		unlock(p);
		return TEDER_AGAIN;
	}
	s->info = *info;
	s->info.sequence = ++p->sequence;
	s->info.dropped_publications = p->dropped;
	if (info->bands)
		memcpy(s->bands, bands, (size_t)info->bands * sizeof(*bands));
	memcpy(s->spans, spans, (size_t)info->span_count * sizeof(*spans));
	p->current = s;
	unlock(p);
	return TEDER_OK;
}

teder_code td_snapshot_acquire(td_snapshot_pool *p, teder_snapshot **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!p)
		return TEDER_INVALID;
	if (!lock(p, true))
		return TEDER_AGAIN;
	assert(p->owned);
	teder_code code = !p->current                 ? TEDER_AGAIN
	                  : p->loans == p->loan_limit ? TEDER_LIMIT
	                                              : TEDER_OK;
	if (code == TEDER_OK) {
		p->current->loans++;
		p->loans++;
		*out = p->current;
	}
	unlock(p);
	return code;
}

void td_snapshot_drop(td_snapshot_pool *p)
{
	lock(p, false);
	assert(p->owned);
	if (p->dropped != UINT64_MAX)
		p->dropped++;
	unlock(p);
}

void td_snapshot_clear(td_snapshot_pool *p)
{
	lock(p, false);
	assert(p->owned);
	p->current = NULL;
	unlock(p);
}

void td_snapshot_pool_release(td_snapshot_pool **owner)
{
	if (!owner || !*owner)
		return;
	td_snapshot_pool *p = *owner;
	lock(p, false);
	assert(p->owned);
	p->owned = false;
	p->current = NULL;
	*owner = NULL;
	finish(p);
}

teder_code TEDER_CALL teder_snapshot_get_info(const teder_snapshot *s, teder_snapshot_info *out)
{
	teder_code code = td_handle_check(s, TD_SNAPSHOT);
	if (code == TEDER_OK)
		code = td_prefix(out, sizeof(*out));
	if (code == TEDER_OK)
		*out = s->info;
	return code;
}

teder_code TEDER_CALL teder_snapshot_bands(const teder_snapshot *s,
                                           const float **out,
                                           uint32_t *count)
{
	if (!out || !count)
		return TEDER_INVALID;
	teder_code code = td_handle_check(s, TD_SNAPSHOT);
	if (code == TEDER_OK) {
		*out = s->info.bands ? s->bands : NULL;
		*count = s->info.bands;
	}
	return code;
}

teder_code TEDER_CALL teder_snapshot_spans(const teder_snapshot *s,
                                           const teder_span **out,
                                           uint32_t *count)
{
	if (!out || !count)
		return TEDER_INVALID;
	teder_code code = td_handle_check(s, TD_SNAPSHOT);
	if (code == TEDER_OK) {
		*out = s->info.span_count ? s->spans : NULL;
		*count = s->info.span_count;
	}
	return code;
}

void TEDER_CALL teder_snapshot_release(teder_snapshot *s)
{
	if (!s)
		return;
	assert(td_handle_check(s, TD_SNAPSHOT) == TEDER_OK);
	td_snapshot_pool *p = s->pool;
	lock(p, false);
	assert(s->loans && p->loans);
	s->loans--;
	p->loans--;
	finish(p);
}
