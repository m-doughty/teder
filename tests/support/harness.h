#ifndef TEDER_TEST_HARNESS_H
#define TEDER_TEST_HARNESS_H
#include "memory.h"
#include "miniaudio.h"
#include <check.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#else
#include <pthread.h>
#endif

/* Test-only, bounded bookkeeping; never installed or used by a real callback.
 * Inspect counters/configure fail_at only while allocator users are quiescent. */
typedef struct {
	ma_mutex lock;

	struct {
		void *pointer;
		size_t size;
		const char *site;
	} blocks[128];

	size_t attempts, fail_at, live, bytes, peak, errors;
	const char *failed_site;
} th_tracker;

bool th_tracker_init(th_tracker *);
bool th_tracker_clean(th_tracker *);
void th_tracker_uninit(th_tracker *); /* requires all blocks released */
void *th_alloc(void *, size_t, const char *);
void th_free(void *, void *);
td_allocator th_allocator(th_tracker *);
ma_allocation_callbacks th_ma_allocator(th_tracker *);

/* One-shot rendezvous: worker hits; controller awaits then releases.
 * Destroy only after joining all users. No timeout frees borrowed resources. */
typedef struct {
	ma_event arrived, release;
} th_gate;

bool th_gate_init(th_gate *);
void th_gate_hit(th_gate *);
void th_gate_await(th_gate *);
void th_gate_release(th_gate *);
void th_gate_uninit(th_gate *);

typedef struct {
#ifdef _WIN32
	HANDLE handle;
#else
	pthread_t handle;
#endif
	void (*run)(void *);
	void *context;
} th_thread;

bool th_thread_start(th_thread *, void (*)(void *), void *);
void th_thread_join(th_thread *);

typedef size_t (*th_render)(void *, float *, size_t frames);

typedef enum { TH_OK, TH_STOPPED, TH_BUSY, TH_CAPACITY, TH_DEVICE, TH_INVALID } th_status;

typedef struct {
	ma_mutex lock;
	float *pcm; /* caller-owned, capacity * channels floats; outlives all steps */
	size_t capacity, channels, frames, underruns;
	bool stopped, active, device_error;
	th_render render;
	void *context;
} th_sink;

bool th_sink_init(th_sink *, float *, size_t capacity, size_t channels, th_render, void *);
th_status th_sink_step(th_sink *, size_t frames, bool force_underrun);
bool th_sink_stop(th_sink *); /* false means a callback still borrows storage */
void th_sink_device_error(th_sink *);
bool th_sink_uninit(th_sink *); /* refuses while a callback is active */
bool th_clock_advance(uint64_t *nanoseconds, uint64_t delta);
bool th_pcm_equal(const float *, const float *, size_t frames, size_t channels);

/* miniaudio's context setup loads COM's entry points on Windows, and its worker thread
 * for synchronous backends calls them. A fake context replaces that setup, so it calls
 * this after clearing the context to supply them. Elsewhere this does nothing. Inline,
 * so only the executables that call it import COM. */
static inline void th_context_com(ma_context *c)
{
#ifdef _WIN32
	c->win32.CoInitialize = (ma_proc)CoInitialize;
	c->win32.CoInitializeEx = (ma_proc)CoInitializeEx;
	c->win32.CoUninitialize = (ma_proc)CoUninitialize;
#else
	(void)c;
#endif
}

/* Check runs every case without fork. Its failure path longjmps out of the test, then
 * reuses that stack to collect the result while engine, device and helper threads may
 * still write the abandoned frame's fixtures, gates and trackers. These replacements
 * report a failed check from the failing frame and end the process there, so storage
 * any thread borrows stays live until exit. Passing checks are unchanged. The
 * deprecated fail macros would bypass them and are removed. */
_Noreturn void th_check_failed(const char *file, int line, const char *expr);
_Noreturn void
th_check_failed_msg(const char *file, int line, const char *expr, const char *format, ...)
    CK_ATTRIBUTE_FORMAT(printf, 4, 5);
#undef ck_assert
#define ck_assert(expr) \
	((expr) ? _mark_point(__FILE__, __LINE__) \
	        : th_check_failed(__FILE__, __LINE__, "Assertion '" #expr "' failed"))
#undef ck_assert_msg
#define ck_assert_msg(expr, ...) \
	((expr) \
	     ? _mark_point(__FILE__, __LINE__) \
	     : th_check_failed_msg(__FILE__, __LINE__, "Assertion '" #expr "' failed", __VA_ARGS__))
#undef ck_abort
#define ck_abort() th_check_failed(__FILE__, __LINE__, "Failed")
#undef ck_abort_msg
#define ck_abort_msg(...) th_check_failed_msg(__FILE__, __LINE__, "Failed", __VA_ARGS__)
#undef fail_unless
#undef fail_if
#undef fail
#endif
