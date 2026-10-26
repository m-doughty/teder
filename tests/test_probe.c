#include "metadata.h"
#include "source.h"
#include "support/harness.h"
#include "support/engine.h"
#include "support/metadata.h"
#include <check.h>
#define PATH OUTPUT_DIR "/probe-meta.flac"
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

typedef struct {
	th_tracker tracker;
	th_gate gate;
	atomic_uint point, starts, stops, opened, closed;
	unsigned fail_stage;
} fixture;

static void checkpoint(void *context, uint64_t request, unsigned point)
{
	(void)request;
	fixture *f = context;
	if (point == TD_PROBE_STARTED)
		atomic_fetch_add(&f->starts, 1);
	if (point == TD_PROBE_STOPPED)
		atomic_fetch_add(&f->stops, 1);
	if (point == TD_PROBE_OPENED)
		atomic_fetch_add(&f->opened, 1);
	if (point == TD_PROBE_CLOSED)
		atomic_fetch_add(&f->closed, 1);
	unsigned expected = point;
	if (atomic_compare_exchange_strong(&f->point, &expected, 0))
		th_gate_hit(&f->gate);
}

static bool permit(void *context, uint32_t stage)
{
	return ((fixture *)context)->fail_stage != stage;
}

static void init(fixture *f, unsigned point)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->point, point);
	atomic_init(&f->starts, 0);
	atomic_init(&f->stops, 0);
	atomic_init(&f->opened, 0);
	atomic_init(&f->closed, 0);
	ck_assert(th_tracker_init(&f->tracker) && th_gate_init(&f->gate));
	th_metadata_fixture data;
	th_metadata_make(&data, 5, 2, 1);
	th_metadata_write(&data, PATH, data.bytes);
}

static teder_code create(fixture *f, teder_engine **e)
{
	teder_config c = INIT(teder_config);
	c.request_capacity = 3;
	c.pcm_frames = 2048;
	td_runtime runtime = {.context = f, .permit = permit, .media_checkpoint = checkpoint};
	return td_engine_create_with(&c, th_allocator(&f->tracker), runtime, e);
}

static void dispose(fixture *f)
{
	ck_assert(atomic_load(&f->starts) == atomic_load(&f->stops));
	ck_assert(atomic_load(&f->opened) == atomic_load(&f->closed));
	ck_assert(th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
	th_gate_uninit(&f->gate);
	th_metadata_remove(PATH);
}

static size_t owned(teder_engine *e)
{
	td_lock(e);
	size_t bytes = e->owned_bytes;
	td_unlock(e);
	return bytes;
}

static uint64_t probe(teder_engine *e, uint32_t flags)
{
	uint64_t id;
	teder_code code;
	char path[] = PATH;
	RETRY(code, teder_probe(e, path, sizeof(path) - 1, flags, &id));
	OK(code);
	memset(path, 'x', sizeof(path)); /* admitted input must already be copied */
	return id;
}

static teder_request_info terminal(teder_engine *e, uint64_t id)
{
	teder_request_info r = INIT(teder_request_info);
	for (;;) {
		teder_code code = teder_request_get(e, id, &r);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && r.state == TEDER_TERMINAL)
			return r;
		th_yield();
	}
}

static void ack(teder_engine *e, uint64_t id)
{
	teder_code code;
	RETRY(code, teder_request_ack(e, id));
	OK(code);
}

static teder_result *take(teder_engine *e, uint64_t id)
{
	teder_result *r;
	teder_code code;
	RETRY(code, teder_request_take_result(e, id, &r));
	OK(code);
	return r;
}

static void verify(teder_result *r, unsigned pictures)
{
	teder_probe_info p = INIT(teder_probe_info);
	OK(teder_result_probe(r, &p));
	ck_assert(p.codec == TEDER_CODEC_FLAC && p.sample_rate == 44100 && p.channels == 2 &&
	          p.bits_per_sample == 16 && p.duration_known && p.duration_frames == 16 &&
	          p.seekable && p.tag_count == 5 && p.artwork_count == pictures);
	teder_text key, value;
	OK(teder_result_tag(r, 1, &key, &value));
	ck_assert(key.bytes == 6 && !memcmp(key.data, "ARTIST", 6));
	ck_assert(value.bytes == 3 && !memcmp(value.data, "two", 3));
	OK(teder_result_tag(r, 3, &key, &value));
	ck_assert(value.bytes == 3 && !memcmp(value.data, "a\0b", 3));
	if (pictures) {
		teder_artwork art = INIT(teder_artwork);
		OK(teder_result_artwork(r, 1, &art));
		ck_assert(art.picture_type == 4 && art.width == 321 && art.height == 201 &&
		          art.encoded.bytes == 5 && !memcmp(art.encoded.data, "\0\1\xff\0\3", 5));
	}
}

START_TEST(results)
{
	fixture f;
	init(&f, 0);
	teder_engine *e;
	OK(create(&f, &e));
	size_t base = owned(e);
	teder_result *results[2];
	for (unsigned i = 0; i < 2; i++) {
		uint64_t id = probe(e, i);
		teder_request_info r = terminal(e, id);
		ck_assert(r.operation == TEDER_OP_PROBE && r.outcome == TEDER_OK &&
		          r.result_kind == TEDER_RESULT_PROBE && !r.committed.epoch &&
		          !r.committed.source && !r.track);
		results[i] = take(e, id);
		ck_assert(owned(e) == base);
		teder_result *again = results[i];
		teder_code code;
		RETRY(code, teder_request_take_result(e, id, &again));
		ck_assert(code == TEDER_NOT_FOUND && !again);
		ack(e, id);
	}
	ck_assert(results[0] != results[1]);
	uint64_t id = probe(e, 1);
	OK(terminal(e, id).outcome);
	ck_assert(owned(e) == base + results[1]->bytes);
	ack(e, id); /* untaken result released and accounted */
	ck_assert(owned(e) == base);
	const char *paths[] = {FIXTURE_DIR "/track-a.flac", FIXTURE_DIR "/track-b.flac"};
	for (unsigned i = 0; i < 2; i++) {
		teder_code code;
		RETRY(code, teder_open(e, paths[i], (uint32_t)strlen(paths[i]), 42 + i, 0, &id));
		OK(code);
		teder_request_info r = terminal(e, id);
		ck_assert(r.outcome == TEDER_OK && r.committed.source == i + 1 &&
		          r.committed.epoch == i + 2 && r.track == 42 + i);
		ack(e, id);
	}
	OK(th_close(e));
	OK(teder_destroy(&e));
	ck_assert(f.tracker.live == 2 && f.tracker.bytes == results[0]->bytes + results[1]->bytes);
	for (unsigned i = 0; i < 2; i++) {
		verify(results[i], 2 * i);
		teder_result_release(results[i]);
	}
	dispose(&f);
}
END_TEST

START_TEST(admission)
{
	fixture f;
	init(&f, TD_PROBE_STARTED);
	teder_engine *e;
	OK(create(&f, &e));
	th_gate_await(&f.gate);
	uint64_t id = 99;
	ck_assert(teder_probe(e, PATH, sizeof(PATH) - 1, 2, &id) == TEDER_INVALID && !id);
	ck_assert(teder_probe(e, "relative", 8, 0, &id) == TEDER_INVALID && !id);
	ck_assert(teder_probe(e, "/a\0b", 4, 0, &id) == TEDER_INVALID && !id);
	ck_assert(teder_probe(e, "/\xff", 2, 0, &id) == TEDER_ENCODING && !id);
	ck_assert(teder_probe(e, PATH, 32768, 0, &id) == TEDER_LIMIT && !id);
	uint64_t ids[3];
	for (unsigned i = 0; i < 3; i++)
		ids[i] = probe(e, 0);
	td_lock(e); /* Contention can precede the full-pool decision. */
	teder_code code = teder_probe(e, PATH, sizeof(PATH) - 1, 0, &id);
	td_unlock(e);
	ck_assert(code == TEDER_BUSY && !id);
	RETRY(code, teder_probe(e, PATH, sizeof(PATH) - 1, 0, &id));
	ck_assert(code == TEDER_LIMIT && !id);
	ck_assert(teder_request_ack(e, ids[0]) == TEDER_BUSY);
	RETRY(code, teder_request_cancel(e, ids[1]));
	OK(code);
	ck_assert(f.tracker.attempts == 1 && f.tracker.live == 1 && !atomic_load(&f.opened));
	th_gate_release(&f.gate);
	for (unsigned i = 0; i < 3; i++) {
		ck_assert(terminal(e, ids[i]).outcome == (i == 1 ? TEDER_CANCELLED : TEDER_OK));
		ack(e, ids[i]);
	}
	id = probe(e, 0);
	ck_assert(id == 4 && terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	const char *missing = OUTPUT_DIR "/absent-probe.flac";
	RETRY(code, teder_probe(e, missing, (uint32_t)strlen(missing), 0, &id));
	OK(code);
	ck_assert(id == 5 && terminal(e, id).outcome == TEDER_IO);
	ack(e, id);
	OK(th_close(e));
	ck_assert(teder_probe(e, PATH, sizeof(PATH) - 1, 0, &id) == TEDER_CLOSED && !id);
	OK(teder_destroy(&e));
	dispose(&f);
}
END_TEST

START_TEST(cancellation)
{
	const unsigned points[] = {TD_PROBE_READ, TD_PROBE_BEFORE_COMMIT, TD_PROBE_AFTER_COMMIT};
	for (unsigned closing = 0; closing < 2; closing++)
		for (unsigned i = 0; i < 3; i++) {
			fixture f;
			init(&f, points[i]);
			teder_engine *e;
			OK(create(&f, &e));
			size_t base = owned(e);
			uint64_t first = probe(e, 1);
			th_gate_await(&f.gate);
			uint64_t second = probe(e, 0);
			teder_code code;
			ck_assert(teder_request_ack(e, first) == TEDER_BUSY);
			if (closing)
				OK(teder_request_close(e));
			else {
				RETRY(code, teder_request_cancel(e, first));
				OK(code);
			}
			ck_assert(teder_destroy(&e) == TEDER_BUSY && e);
			th_gate_release(&f.gate);
			ck_assert(terminal(e, first).outcome == (i == 2 ? TEDER_OK : TEDER_CANCELLED));
			if (i == 2) {
				teder_result *r = take(e, first);
				verify(r, 2);
				teder_result_release(r);
			}
			ck_assert(terminal(e, second).outcome == (closing ? TEDER_CANCELLED : TEDER_OK));
			ack(e, first);
			ack(e, second);
			ck_assert(owned(e) == base);
			OK(th_close(e));
			OK(teder_destroy(&e));
			dispose(&f);
		}
}
END_TEST

START_TEST(limits)
{
	size_t result_bytes = 0;
	for (unsigned mode = 0; mode < 5; mode++) {
		fixture f;
		init(&f, 0);
		teder_engine *e;
		OK(create(&f, &e));
		size_t base = owned(e), work, headroom;
		OK(td_metadata_plan(sizeof(PATH) - 1, &work));
		OK(td_source_plan(e->config.max_input_bytes, 8 * 1024 * 1024, &headroom));
		td_lock(e);
		if (mode == 0)
			e->config.engine_bytes_limit = base + work + headroom;
		else if (mode >= 3)
			e->config.engine_bytes_limit = base + work + headroom + result_bytes - (mode == 4);
		else
			e->config.result_bytes_limit = mode == 1 ? 1 : 4096;
		td_unlock(e);
		uint64_t id = probe(e, 1);
		ck_assert(terminal(e, id).outcome == (mode == 2 || mode == 3 ? TEDER_OK : TEDER_LIMIT));
		if (mode == 2 || mode == 3) {
			teder_result *r = take(e, id);
			if (mode == 2)
				result_bytes = r->bytes;
			ck_assert(r->bytes == result_bytes && owned(e) == base);
			verify(r, 2);
			teder_result_release(r);
		}
		ack(e, id);
		ck_assert(owned(e) == base && f.tracker.live == 1);
		ck_assert(f.tracker.peak <= e->config.engine_bytes_limit);
		if (!mode)
			ck_assert(f.tracker.attempts == 1 && !atomic_load(&f.opened));
		OK(th_close(e));
		OK(teder_destroy(&e));
		dispose(&f);
	}
}
END_TEST

START_TEST(failures)
{
	size_t count = 0;
	for (size_t fail = 0; fail <= count; fail++) {
		fixture f;
		init(&f, 0);
		teder_engine *e;
		OK(create(&f, &e));
		size_t base = owned(e);
		f.tracker.fail_at = fail ? f.tracker.attempts + fail : 0;
		uint64_t id = probe(e, 1);
		ck_assert(terminal(e, id).outcome == (fail ? TEDER_NO_MEMORY : TEDER_OK));
		if (!fail)
			count = f.tracker.attempts - 1;
		ack(e, id);
		ck_assert(owned(e) == base && f.tracker.live == 1);
		OK(th_close(e));
		OK(teder_destroy(&e));
		dispose(&f);
	}
	ck_assert(count == 32);
}
END_TEST

START_TEST(initialization)
{
	fixture warm;
	init(&warm, 0);
	teder_engine *e;
	OK(create(&warm, &e));
	OK(th_close(e));
	OK(teder_destroy(&e));
	dispose(&warm);
#ifdef _WIN32
	DWORD before, after;
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &before));
#endif
	const unsigned stages[] = {
	    TD_INIT_PROBE_WAKE, TD_INIT_PROBE_THREAD, TD_INIT_THREAD, TD_INIT_PUBLISH};
	for (unsigned i = 0; i < 4; i++) {
		fixture f;
		init(&f, 0);
		f.fail_stage = stages[i];
		ck_assert(create(&f, &e) == TEDER_INTERNAL && !e);
		ck_assert(atomic_load(&f.starts) == (i >= 2 ? 1 : 0));
		dispose(&f);
	}
#ifdef _WIN32
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &after) && after == before);
#endif
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = !strcmp(argv[1], "results")          ? results
	                    : !strcmp(argv[1], "admission")      ? admission
	                    : !strcmp(argv[1], "cancellation")   ? cancellation
	                    : !strcmp(argv[1], "limits")         ? limits
	                    : !strcmp(argv[1], "failures")       ? failures
	                    : !strcmp(argv[1], "initialization") ? initialization
	                                                         : NULL;
	if (!test)
		return 2;
	Suite *suite = suite_create("probe");
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
