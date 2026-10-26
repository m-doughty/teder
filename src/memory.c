#include "memory.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct td_cleanup {
	struct td_cleanup *next;
	void (*destroy)(void *);
	void *object;
} td_cleanup;

struct td_arena {
	td_allocator allocator;
	size_t capacity, used;
	td_cleanup *cleanup;
	max_align_t alignment;
	unsigned char data[];
};

_Static_assert(offsetof(td_arena, data) % _Alignof(max_align_t) == 0, "arena data alignment");

bool td_size_add(size_t a, size_t b, size_t *out)
{
	if (b > SIZE_MAX - a)
		return false;
	*out = a + b;
	return true;
}

bool td_size_mul(size_t a, size_t b, size_t *out)
{
	if (a && b > SIZE_MAX / a)
		return false;
	*out = a * b;
	return true;
}

bool td_layout_add(size_t *used, size_t count, size_t size, size_t alignment, size_t *offset)
{
	size_t bytes, start, end;
	if (!alignment || (alignment & (alignment - 1)) || alignment > _Alignof(max_align_t) ||
	    !td_size_mul(count, size, &bytes) || !td_size_add(*used, alignment - 1, &start))
		return false;
	start &= ~(alignment - 1);
	if (!td_size_add(start, bytes, &end))
		return false;
	*offset = start;
	*used = end;
	return true;
}

bool td_arena_size(size_t capacity, size_t *total)
{
	return capacity && td_size_add(sizeof(td_arena), capacity, total);
}

static void *libc_allocate(void *context, size_t bytes, const char *site)
{
	(void)context;
	(void)site;
	return malloc(bytes);
}

static void libc_release(void *context, void *pointer)
{
	(void)context;
	free(pointer);
}

td_allocator td_libc_allocator(void)
{
	return (td_allocator){NULL, libc_allocate, libc_release};
}

td_arena *td_arena_create(td_allocator allocator, size_t capacity)
{
	size_t total;
	td_arena *arena;
	if (!allocator.allocate || !allocator.release || !td_arena_size(capacity, &total))
		return NULL;
	arena = allocator.allocate(allocator.context, total, "arena.chunk");
	if (!arena)
		return NULL;
	arena->allocator = allocator;
	arena->capacity = capacity;
	arena->used = 0;
	arena->cleanup = NULL;
	return arena;
}

void *td_arena_alloc(td_arena *arena, size_t count, size_t size, size_t alignment)
{
	size_t aligned, end;
	if (!arena || !count || !size)
		return NULL;
	end = arena->used;
	if (!td_layout_add(&end, count, size, alignment, &aligned) || end > arena->capacity)
		return NULL;
	arena->used = end;
	return arena->data + aligned;
}

bool td_arena_defer(td_arena *arena, void (*destroy)(void *), void *object)
{
	td_cleanup *entry;
	if (!destroy)
		return false;
	entry = td_arena_alloc(arena, 1, sizeof(*entry), _Alignof(td_cleanup));
	if (!entry)
		return false;
	*entry = (td_cleanup){arena->cleanup, destroy, object};
	arena->cleanup = entry;
	return true;
}

void td_arena_reset(td_arena *arena)
{
	if (!arena)
		return;
	while (arena->cleanup) {
		td_cleanup *entry = arena->cleanup;
		arena->cleanup = entry->next;
		entry->destroy(entry->object);
	}
	arena->used = 0;
}

void td_arena_destroy(td_arena *arena)
{
	if (!arena)
		return;
	td_arena_reset(arena);
	arena->allocator.release(arena->allocator.context, arena);
}
