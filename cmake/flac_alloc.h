#ifndef TEDER_FLAC_ALLOC_REDIRECT_H
#define TEDER_FLAC_ALLOC_REDIRECT_H
/* Force-include only in the private pinned libFLAC target. Parse the CRT
 * declarations before rewriting calls; never rewrite consumer/OS allocators. */
#ifdef HAVE_CONFIG_H
/* Feature-test macros must precede the first libc header, including this one. */
#include <config.h>
#endif
#include <stdlib.h>
#include <string.h>
#include "flac_heap.h"
#define malloc td_flac_malloc
#define calloc td_flac_calloc
#define realloc td_flac_realloc
#define free td_flac_free
#define strdup td_flac_strdup
#endif
