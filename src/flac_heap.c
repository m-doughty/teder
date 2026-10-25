#include "flac_heap.h"
#include <stdint.h>
#include <string.h>

/* Header alignment preserves malloc's max_align_t guarantee, including when
 * libFLAC overallocates then aligns a separate interior sample pointer. */
typedef union {
	struct { td_flac_heap *owner; size_t bytes; } record;
	max_align_t alignment;
} td_flac_allocation;
static _Thread_local td_flac_heap *current;

teder_code td_flac_heap_init(td_flac_heap *heap, td_allocator allocator, size_t limit)
{
	if (!heap || !allocator.allocate || !allocator.release) return TEDER_INVALID;
	*heap = (td_flac_heap){.allocator = allocator, .limit = limit};
	return TEDER_OK;
}
td_flac_heap *td_flac_scope(td_flac_heap *heap)
{
	td_flac_heap *previous = current;
	current = heap;
	return previous;
}
static void *failed(td_flac_heap *heap, teder_code code)
{
	if (heap && heap->error == TEDER_OK) heap->error = code;
	return NULL;
}
static void *allocate(td_flac_heap *heap, size_t bytes)
{
	if (!heap) return NULL;
	/* C permits a unique allocation for zero; reserve one byte consistently. */
	if (!bytes) bytes = 1;
	if (bytes > SIZE_MAX - sizeof(td_flac_allocation)) return failed(heap, TEDER_LIMIT);
	size_t total = sizeof(td_flac_allocation) + bytes;
	if (total > heap->limit - heap->bytes) return failed(heap, TEDER_LIMIT);
	td_flac_allocation *block = heap->allocator.allocate(heap->allocator.context, total, "flac.heap");
	if (!block) return failed(heap, TEDER_NO_MEMORY);
	block->record.owner = heap; block->record.bytes = total;
	heap->bytes += total; heap->live++;
	if (heap->bytes > heap->peak) heap->peak = heap->bytes;
	return block + 1;
}
void *td_flac_malloc(size_t bytes) { return allocate(current, bytes); }
char *td_flac_strdup(const char *text)
{
	size_t length = strlen(text);
	if (length == SIZE_MAX) return failed(current, TEDER_LIMIT);
	char *copy = allocate(current, length + 1);
	if (copy) memcpy(copy, text, length + 1);
	return copy;
}
void *td_flac_calloc(size_t count, size_t bytes)
{
	if (count && bytes > SIZE_MAX / count) return failed(current, TEDER_LIMIT);
	size_t size = count * bytes;
	void *p = allocate(current, size);
	if (p) memset(p, 0, size ? size : 1);
	return p;
}
void td_flac_free(void *pointer)
{
	if (!pointer) return;
	td_flac_allocation *block = (td_flac_allocation *)pointer - 1;
	td_flac_heap *heap = block->record.owner;
	size_t bytes = block->record.bytes;
	/* Keep it accounted until the backing allocator has actually released it. */
	heap->allocator.release(heap->allocator.context, block);
	heap->bytes -= bytes; heap->live--;
}
void *td_flac_realloc(void *pointer, size_t bytes)
{
	if (!pointer) return td_flac_malloc(bytes);
	if (!bytes) { td_flac_free(pointer); return NULL; }
	td_flac_allocation *block = (td_flac_allocation *)pointer - 1;
	size_t old_size = block->record.bytes - sizeof(*block);
	/* Both allocations exist during copying and count against the SAME owner,
	 * even inside a nested scope. Failure preserves the original allocation.
	 * libFLAC safe_realloc_ may then explicitly free it; that is its decision. */
	void *replacement = allocate(block->record.owner, bytes);
	if (!replacement) return NULL;
	memcpy(replacement, pointer, old_size < bytes ? old_size : bytes);
	td_flac_free(pointer);
	return replacement;
}
