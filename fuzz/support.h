#ifndef TEDER_FUZZ_SUPPORT_H
#define TEDER_FUZZ_SUPPORT_H
#include "memory.h"
#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FZ_CHECK(condition) \
	do { \
		if (!(condition)) { \
			fz_trace_flush(); \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
			abort(); \
		} \
	} while (0)
/* Triage trace: formatted into a bounded in-memory log so that worker threads are
 * not serialized on stderr, then written by fz_trace_flush (and any FZ_CHECK). */
void fz_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
void fz_trace_flush(void);
#define FZ_INIT(type) {.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION}
enum { FZ_HEADER = 16, FZ_MAX_INPUT = 65536 + FZ_HEADER };
/* One synchronous iteration. Its allocator context outlives all library owners. */
typedef struct {
	char path[64];
	uint8_t control[FZ_HEADER];
	size_t live, bytes, peak;
	unsigned opened, closed, reads, cancel_at;
	atomic_uint cancel;
} fz_input;
bool fz_begin(fz_input *, const uint8_t *, size_t);
void fz_remove(fz_input *);
void fz_end(fz_input *);
td_allocator fz_allocator(fz_input *);
void fz_checkpoint(void *, unsigned);
uint32_t fz_u32(const uint8_t *);
void fz_touch(const void *, size_t);
#endif
