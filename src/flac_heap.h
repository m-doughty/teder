#ifndef TEDER_FLAC_HEAP_H
#define TEDER_FLAC_HEAP_H
#include "memory.h"
#include "teder.h"

/* Private libFLAC build adapter. One serialized worker owns each heap. Reserve
 * limit bytes from the engine before initialization; keep that reservation and
 * this descriptor alive until all codec objects are deleted and live == 0.
 * The source arena owns the descriptor. These dependency allocations must be
 * individually reusable; libFLAC's free/realloc semantics are preserved.
 * Initialize fresh storage, or reinitialize only after all codec objects are
 * deleted and no allocator call is active. End scopes before storage release.
 * A scope covers EVERY libFLAC entry, including delete. Nesting restores the
 * prior scope. New allocations need a scope; realloc/free keep their recorded
 * owner. No operation falls back to an unaccounted allocator.
 * Allocator callbacks must not reenter this heap. Never use on an audio thread.
 * Check error as well as codec status: libFLAC can retry a failed allocation.
 * Clear error only at a deliberate operation boundary, never during cleanup. */
typedef struct {
	td_allocator allocator;
	size_t limit, bytes, peak, live;
	teder_code error; /* first allocation failure; sticky until explicit reset */
} td_flac_heap;
teder_code td_flac_heap_init(td_flac_heap *, td_allocator, size_t limit);
td_flac_heap *td_flac_scope(td_flac_heap *); /* returns previous thread-local scope */
void *td_flac_malloc(size_t);
void *td_flac_calloc(size_t, size_t);
void *td_flac_realloc(void *, size_t);
char *td_flac_strdup(const char *);
void td_flac_free(void *);
#endif
