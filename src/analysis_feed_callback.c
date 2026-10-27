#include "analysis_feed.h"
#include <string.h>

static uint32_t next(const td_analysis_feed *p, uint32_t index)
{
	return index == p->capacity ? 0 : index + 1;
}

td_analysis_write
td_analysis_begin(td_analysis_feed *p, uint64_t epoch, uint32_t rate, uint64_t start, bool enabled)
{
	td_analysis_write w = {0};
	if (!p)
		return w;
	uint64_t revision = atomic_load_explicit(&p->revision, memory_order_acquire);
	bool broken = p->broken;
	p->broken = true; /* aborted, disabled and paused calls also break continuity */
	if (!enabled || !revision || !epoch || !rate)
		return w;
	w.feed = p;
	w.read = atomic_load_explicit(&p->read, memory_order_acquire);
	w.write = atomic_load_explicit(&p->write, memory_order_relaxed);
	w.epoch = epoch;
	w.revision = revision;
	w.rate = rate;
	w.start = w.cursor = start;
	w.discontinuity = broken || p->epoch != epoch || p->last_revision != revision ||
	                  p->rate != rate || p->next_frame != start;
	return w;
}

void td_analysis_append(
    td_analysis_write *w, const float *pcm, uint32_t frames, uint64_t source, uint64_t track)
{
	if (!w->feed || !frames)
		return;
	td_analysis_feed *p = w->feed;
	w->frames = w->frames > UINT64_MAX - frames ? UINT64_MAX : w->frames + frames;
	if (w->cursor > UINT64_MAX - frames) {
		w->dropped = true;
		return; /* output-time exhaustion never publishes a wrapped interval */
	}
	uint64_t start = w->cursor;
	w->cursor += frames;
	if (w->dropped)
		return;
	while (frames) {
		uint32_t end = next(p, w->write);
		if (end == w->read) {
			w->dropped = true;
			return;
		}
		uint32_t count = frames < TD_ANALYSIS_FRAMES ? frames : TD_ANALYSIS_FRAMES;
		td_analysis_block *b = &p->blocks[w->write];
		b->epoch = w->epoch;
		b->revision = w->revision;
		b->rate = w->rate;
		b->output_start = start;
		b->source = source;
		b->track = track;
		b->frames = count;
		b->flags = w->discontinuity ? TD_ANALYSIS_DISCONTINUITY : 0;
		memcpy(b->samples, pcm, (size_t)count * 2 * sizeof(*pcm));
		w->discontinuity = false;
		w->write = end;
		pcm += (size_t)count * 2;
		start += count;
		frames -= count;
	}
}

void td_analysis_finish(td_analysis_write *w)
{
	if (!w->feed)
		return;
	td_analysis_feed *p = w->feed;
	if (w->dropped) {
		uint64_t count = w->frames;
		uint64_t before = atomic_load_explicit(&p->dropped_frames, memory_order_relaxed);
		atomic_store_explicit(&p->dropped_frames,
		                      before > UINT64_MAX - count ? UINT64_MAX : before + count,
		                      memory_order_relaxed);
	} else if (w->cursor != w->start) {
		p->epoch = w->epoch;
		p->last_revision = w->revision;
		p->rate = w->rate;
		p->next_frame = w->cursor;
		p->broken = false;
		atomic_store_explicit(&p->write, w->write, memory_order_release);
	}
	w->feed = NULL;
}

teder_code td_analysis_peek(td_analysis_feed *p, const td_analysis_block **out)
{
	if (!p || !out)
		return TEDER_INVALID;
	*out = NULL;
	uint32_t read = atomic_load_explicit(&p->read, memory_order_relaxed);
	if (read == atomic_load_explicit(&p->write, memory_order_acquire))
		return TEDER_AGAIN;
	*out = &p->blocks[read];
	return TEDER_OK;
}

teder_code td_analysis_consume(td_analysis_feed *p)
{
	if (!p)
		return TEDER_INVALID;
	uint32_t read = atomic_load_explicit(&p->read, memory_order_relaxed);
	if (read == atomic_load_explicit(&p->write, memory_order_acquire))
		return TEDER_AGAIN;
	atomic_store_explicit(&p->read, next(p, read), memory_order_release);
	return TEDER_OK;
}
