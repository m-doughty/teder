#include "analysis_feed.h"
#include <string.h>

bool td_analysis_plan(size_t *used, uint32_t capacity, td_analysis_layout *out)
{
	if (!used || !out || !capacity || capacity == UINT32_MAX)
		return false;
	size_t end = *used;
	td_analysis_layout l = {.capacity = capacity};
	if (!td_layout_add(&end,
	                   (size_t)capacity + 1,
	                   sizeof(td_analysis_block),
	                   _Alignof(td_analysis_block),
	                   &l.blocks))
		return false;
	*used = end;
	*out = l;
	return true;
}

void td_analysis_init(td_analysis_feed *p, void *storage, const td_analysis_layout *l)
{
	memset(p, 0, sizeof(*p));
	p->blocks = (td_analysis_block *)((unsigned char *)storage + l->blocks);
	p->capacity = l->capacity;
	p->broken = true;
	atomic_init(&p->read, 0);
	atomic_init(&p->write, 0);
	atomic_init(&p->revision, 0);
	atomic_init(&p->dropped_frames, 0);
}

void td_analysis_revision(td_analysis_feed *p, uint64_t revision)
{
	atomic_store_explicit(&p->revision, revision, memory_order_release);
}
