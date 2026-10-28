#ifndef TEDER_FUZZ_ENGINE_H
#define TEDER_FUZZ_ENGINE_H
#include "internal.h"
#include <pthread.h>

/* Process heap for one engine iteration, shared by workers and fake native
 * threads. It never returns NULL except for one optional injected failure. */
typedef struct {
	pthread_mutex_t lock;
	size_t live, bytes, peak, attempts, fail_at;
	bool failed;
} fz_heap;

void fz_heap_init(fz_heap *, size_t fail_at); /* fail_at=0 disables injection */
td_allocator fz_heap_allocator(fz_heap *);
bool fz_heap_failed(fz_heap *);
void fz_heap_end(fz_heap *); /* aborts unless every allocation was released */

/* Deterministic fake miniaudio backend. One lifecycle thread owns every native
 * call. While started, a render thread consumes only frames the harness credits.
 * Devices 'A' (default, 44100 Hz) and 'B' (22050 Hz) render float stereo. */
enum { FZ_WORKERS = 5 };
typedef struct {
	pthread_t owner, host, render;
	bool owned, rendering, fail_init, fail_start, trace;
	ma_device *native;
	void *native_heap;
	unsigned period, contexts, closed, devices, releases, live_devices;
	atomic_uint stop, lose, credit, active; /* active: render thread consuming credit */
	atomic_uint_fast64_t rendered, callbacks;
	atomic_uint entries[FZ_WORKERS], exits[FZ_WORKERS];
	atomic_uint sources_opened, sources_closed, probes_opened, probes_closed;
} fz_platform;

void fz_platform_init(fz_platform *, uint8_t flags);
td_runtime fz_platform_runtime(fz_platform *);
void fz_platform_credit(fz_platform *, unsigned frames); /* host thread */
void fz_platform_refill(fz_platform *, unsigned frames); /* credit only after the last is used */
void fz_platform_lose(fz_platform *);                    /* next rendered step reports loss */
void fz_platform_end(fz_platform *); /* after destroy: workers, files and devices balanced */
#endif
