#include "backend.h"
#include <string.h>

typedef union {
	struct {
		td_backend_heap *owner;
		size_t bytes;
	} record;
	max_align_t alignment;
} allocation;

static void *allocate(size_t bytes, void *context)
{
	td_backend_heap *h = context;
	if (!bytes)
		bytes = 1;
	ma_mutex_lock(&h->lock);
	allocation *p = NULL;
	size_t total;
	if (h->error != TEDER_OK) {
		/* A failed operation must be unwound, not recovered behind our budget. */
	} else if (!td_size_add(sizeof(*p), bytes, &total) || total > h->limit - h->bytes)
		h->error = TEDER_LIMIT;
	else {
		p = h->allocator.allocate(h->allocator.context, total, "backend.heap");
		if (!p)
			h->error = TEDER_NO_MEMORY;
		else {
			p->record.owner = h;
			p->record.bytes = total;
			h->bytes += total;
			h->live++;
			if (h->bytes > h->peak)
				h->peak = h->bytes;
		}
	}
	ma_mutex_unlock(&h->lock);
	return p ? p + 1 : NULL;
}

static void release(void *pointer, void *context)
{
	(void)context;
	if (!pointer)
		return;
	allocation *p = (allocation *)pointer - 1;
	td_backend_heap *h = p->record.owner;
	size_t bytes = p->record.bytes;
	ma_mutex_lock(&h->lock);
	h->allocator.release(h->allocator.context, p);
	h->bytes -= bytes;
	h->live--;
	ma_mutex_unlock(&h->lock);
}

static void *resize(void *pointer, size_t bytes, void *context)
{
	if (!pointer)
		return allocate(bytes, context);
	if (!bytes) {
		release(pointer, context);
		return NULL;
	}
	allocation *p = (allocation *)pointer - 1;
	size_t old = p->record.bytes - sizeof(*p);
	void *copy = allocate(bytes, p->record.owner);
	if (copy) {
		memcpy(copy, pointer, old < bytes ? old : bytes);
		release(pointer, context);
	}
	return copy;
}

teder_code td_backend_heap_init(td_backend_heap *h, td_allocator allocator, size_t limit)
{
	if (!h || !allocator.allocate || !allocator.release)
		return TEDER_INVALID;
	*h = (td_backend_heap){.allocator = allocator, .limit = limit};
	return ma_mutex_init(&h->lock) == MA_SUCCESS ? TEDER_OK : TEDER_INTERNAL;
}

ma_allocation_callbacks td_backend_allocator(td_backend_heap *h)
{
	return (ma_allocation_callbacks){h, allocate, resize, release};
}

teder_code td_backend_heap_error(td_backend_heap *h)
{
	ma_mutex_lock(&h->lock);
	teder_code code = h->error;
	ma_mutex_unlock(&h->lock);
	return code;
}

teder_code td_backend_heap_uninit(td_backend_heap *h)
{
	/* Caller has joined every allocator user. Retain storage if any borrow remains. */
	if (h->live)
		return TEDER_BUSY;
	ma_mutex_uninit(&h->lock);
	return TEDER_OK;
}
