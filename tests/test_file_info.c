/* A valid FLAC opens, then one native file-information call fails. Native file errors
 * are IO, never the UNSUPPORTED of a file that is not regular. This executable links a
 * copy of the source objects built with the file-information fault hook. Every native
 * file-information call after the opened checkpoint consults the hook: fstat on POSIX,
 * GetFileType then GetFileInformationByHandle on Windows. Each one is failed in turn. */
#include "file.h"
#include "source.h"
#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#endif
#define TRACK FIXTURE_DIR "/track-a.flac"
#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert_msg((call) == TEDER_OK, "%s failed", #call)
#define RETRY(code, call) \
	do { \
		(code) = (call); \
		if ((code) == TEDER_BUSY) \
			th_yield(); \
	} while ((code) == TEDER_BUSY)
#ifdef _WIN32
enum { INFO_CALLS = 2 }; /* GetFileType, GetFileInformationByHandle */
#else
enum { INFO_CALLS = 1 }; /* fstat */
#endif

/* Opens are sequential, and each consults the hook only on the thread that just reported
 * its opened checkpoint, so plain atomics order the hook against that checkpoint. */
static atomic_uint fault_at, consulted, injected;

bool td_file_fault(void)
{
	if (atomic_fetch_add(&consulted, 1) != atomic_load(&fault_at))
		return false;
	atomic_fetch_add(&injected, 1);
#ifdef _WIN32
	SetLastError(ERROR_IO_DEVICE);
#else
	errno = EIO;
#endif
	return true;
}

typedef struct {
	th_tracker tracker;
	atomic_uint arm, opened, closed;
} fixture;

static void checkpoint(void *context, uint64_t request, unsigned point)
{
	(void)request;
	fixture *f = context;
	if (point == TD_SOURCE_OPENED || point == TD_PROBE_OPENED) {
		atomic_fetch_add(&f->opened, 1);
		atomic_store(&consulted, 0);
		atomic_store(&fault_at, atomic_load(&f->arm)); /* applies to this open only */
	}
	if (point == TD_SOURCE_CLOSED || point == TD_PROBE_CLOSED)
		atomic_fetch_add(&f->closed, 1);
}

static teder_engine *create(fixture *f)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->arm, UINT_MAX);
	atomic_init(&f->opened, 0);
	atomic_init(&f->closed, 0);
	atomic_init(&fault_at, UINT_MAX);
	atomic_init(&consulted, 0);
	atomic_init(&injected, 0);
	ck_assert(th_tracker_init(&f->tracker));
	teder_config c = INIT(teder_config);
	td_runtime runtime = {.context = f, .media_checkpoint = checkpoint};
	teder_engine *e;
	OK(td_engine_create_with(&c, th_allocator(&f->tracker), runtime, &e));
	return e;
}

static teder_code finish(teder_engine *e, uint64_t id)
{
	teder_request_info r = INIT(teder_request_info);
	for (;;) {
		teder_code code = teder_request_get(e, id, &r);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && r.state == TEDER_TERMINAL)
			break;
		th_yield();
	}
	teder_code code;
	RETRY(code, teder_request_ack(e, id));
	OK(code);
	return r.outcome;
}

/* Probe or open the fixture, failing the given native call after the file opened.
 * UINT_MAX fails none. Returns the outcome; counts this request's hook consultations. */
static teder_code submit(fixture *f, teder_engine *e, bool open, unsigned fail, unsigned *calls)
{
	atomic_store(&f->arm, fail);
	unsigned before = atomic_load(&injected), opened = atomic_load(&f->opened);
	uint64_t id;
	teder_code code;
	if (open)
		RETRY(code, teder_open(e, TRACK, sizeof(TRACK) - 1, 77, 0, &id));
	else
		RETRY(code, teder_probe(e, TRACK, sizeof(TRACK) - 1, 0, &id));
	OK(code);
	code = finish(e, id);
	*calls = atomic_load(&consulted);
	ck_assert_msg(atomic_load(&f->opened) == opened + 1 &&
	                  atomic_load(&injected) == before + (fail == UINT_MAX ? 0 : 1),
	              "%s: %u opens, %u faults injected",
	              open ? "open" : "probe",
	              atomic_load(&f->opened) - opened,
	              atomic_load(&injected) - before);
	return code;
}

static void run(bool open)
{
	fixture f;
	teder_engine *e = create(&f);
	unsigned calls;
	teder_code plain = submit(&f, e, open, UINT_MAX, &calls);
	ck_assert_msg(plain == TEDER_OK && calls == INFO_CALLS,
	              "%s without a fault: outcome %d, %u native information calls",
	              open ? "open" : "probe",
	              plain,
	              calls);
	for (unsigned call = 0; call < INFO_CALLS; call++) {
		teder_code faulted = submit(&f, e, open, call, &calls);
		ck_assert_msg(faulted == TEDER_IO && calls == call + 1,
		              "%s with native information call %u failed: outcome %d (IO is %d), "
		              "%u calls",
		              open ? "open" : "probe",
		              call,
		              faulted,
		              TEDER_IO,
		              calls);
	}
	ck_assert(submit(&f, e, open, UINT_MAX, &calls) == TEDER_OK && calls == INFO_CALLS);
	OK(th_close(e));
	OK(teder_destroy(&e));
	/* Each faulted open closed its file; the rest closed when retired or closed. */
	ck_assert(atomic_load(&f.opened) == INFO_CALLS + 2 &&
	          atomic_load(&f.closed) == atomic_load(&f.opened));
	ck_assert(th_tracker_clean(&f.tracker));
	th_tracker_uninit(&f.tracker);
}

START_TEST(info_probe)
{
	run(false);
}
END_TEST

START_TEST(info_open)
{
	run(true);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "probe"))
		test = info_probe;
	else if (!strcmp(argv[1], "open"))
		test = info_open;
	else
		return 2;
	Suite *suite = suite_create("file-info");
	TCase *tc = tcase_create(argv[1]);
	tcase_add_test(tc, test);
	suite_add_tcase(suite, tc);
	SRunner *runner = srunner_create(suite);
	srunner_set_fork_status(runner, CK_NOFORK);
	srunner_run_all(runner, CK_NORMAL);
	int failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? 1 : 0;
}
