#ifndef TEDER_MEMORY_H
#define TEDER_MEMORY_H
#include <stddef.h>
#include <stdbool.h>

/* Private allocator seam. Context and literal site names outlive the region.
 * Callers serialize region access and prove borrower quiescence before reset.
 * Callbacks return storage aligned for max_align_t; no over-aligned objects. */
typedef struct {
	void *context;
	void *(*allocate)(void *, size_t, const char *site);
	void (*release)(void *, void *);
} td_allocator;
typedef struct td_arena td_arena;
bool td_size_add(size_t a, size_t b, size_t *out);
bool td_size_mul(size_t a, size_t b, size_t *out);
td_arena *td_arena_create(td_allocator allocator, size_t capacity);
void *td_arena_alloc(td_arena *, size_t count, size_t size, size_t alignment);
/* LIFO cleanup runs while every region byte remains valid. Destructors must
 * not reenter the arena. Failed registration leaves ownership with caller. */
bool td_arena_defer(td_arena *, void (*destroy)(void *), void *object);
void td_arena_reset(td_arena *);
void td_arena_destroy(td_arena *);
#endif
