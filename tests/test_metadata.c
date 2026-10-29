#include "metadata.h"
#include "flac_heap.h"
#include "support/harness.h"
#include "support/engine.h"
#include "support/metadata.h"
#include <check.h>
#define PATH OUTPUT_DIR "/metadata-test.flac"
#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert_msg((call) == TEDER_OK, "%s failed", #call)

static td_metadata_config config(const char *path)
{
	td_metadata_config c = {
	    .flags = TEDER_PROBE_ARTWORK, .result_bytes = 16 * 1024 * 1024, .file = {.read_size = 3}};
	OK(td_metadata_plan((uint32_t)strlen(path), &c.work_bytes));
	return c;
}

static teder_code
read_meta(const char *path, td_metadata_config c, td_allocator a, teder_result **r)
{
	return td_metadata_read(path, (uint32_t)strlen(path), &c, a, r);
}

static void text_is(teder_text t, const char *value, unsigned length)
{
	ck_assert(t.bytes == length);
	if (length)
		ck_assert(t.data && !memcmp(t.data, value, length));
	else
		ck_assert_ptr_null(t.data);
}

static void verify(teder_result *r, unsigned pictures)
{
	uint32_t kind;
	OK(teder_result_kind(r, &kind));
	ck_assert(kind == TEDER_RESULT_PROBE);
	teder_probe_info p = INIT(teder_probe_info);
	OK(teder_result_probe(r, &p));
	ck_assert(p.codec == TEDER_CODEC_FLAC && p.sample_rate == 44100 && p.channels == 2 &&
	          p.bits_per_sample == 16 && p.duration_frames == 16 && p.duration_known &&
	          p.seekable && p.tag_count == 5 && p.artwork_count == pictures);
	const char *keys[] = {"ARTIST", "ARTIST", "EMPTY", "NOTE", "TITLE"};
	const char *values[] = {"one", "two", "", "a\0b", "\xd7\xa9"};
	const unsigned lengths[] = {3, 3, 0, 3, 2};
	for (unsigned i = 0; i < 5; i++) {
		teder_text key, value;
		OK(teder_result_tag(r, i, &key, &value));
		text_is(key, keys[i], (unsigned)strlen(keys[i]));
		text_is(value, values[i], lengths[i]);
	}
	for (unsigned i = 0; i < pictures; i++) {
		teder_artwork art = INIT(teder_artwork);
		OK(teder_result_artwork(r, i, &art));
		ck_assert(art.picture_type == 3 + i && art.width == 320 + i && art.height == 200 + i &&
		          art.depth == 24 && !art.colors && !art.reserved && art.encoded.bytes == 5 &&
		          !memcmp(art.encoded.data, "\0\1\xff\0\3", 5));
		text_is(art.mime, "image/png", 9);
		text_is(art.description, "\xd7\xa9", 2);
	}
	teder_artwork art = INIT(teder_artwork);
	ck_assert(teder_result_artwork(r, pictures, &art) == TEDER_NOT_FOUND);
}

START_TEST(reference)
{
	const char *path = OUTPUT_DIR "/m\xc3\xbasica-\xd7\xa9.flac";
	th_metadata_fixture f;
	th_metadata_make(&f, 5, 2, 1);
	th_metadata_write(&f, path, f.bytes);
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	td_metadata_config c = config(path);
	teder_result *r, *without;
	td_flac_heap outer;
	OK(td_flac_heap_init(&outer, td_libc_allocator(), 4096));
	td_flac_heap *previous = td_flac_scope(&outer);
	OK(read_meta(path, c, th_allocator(&t), &r));
	void *check_scope = td_flac_malloc(7);
	ck_assert(check_scope && outer.live == 1);
	td_flac_free(check_scope);
	td_flac_scope(previous);
	ck_assert(t.live == 1 && t.bytes == r->bytes && t.peak <= c.work_bytes + c.result_bytes);
	c.flags = 0;
	OK(read_meta(path, c, th_allocator(&t), &without));
	ck_assert(without != r && t.live == 2);
	th_metadata_remove(path); /* Native file and parser chain have already closed. */
	teder_config engine_config = INIT(teder_config);
	teder_engine *engine;
	OK(td_engine_create(&engine_config, th_allocator(&t), &engine));
	OK(th_close(engine));
	OK(teder_destroy(&engine));
	verify(r, 2);
	verify(without, 0);
	teder_result_release(without);
	teder_result_release(r);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

static void expect(const th_metadata_fixture *f, td_metadata_config c, teder_code expected)
{
	th_metadata_write(f, PATH, f->bytes);
	teder_result *r = (teder_result *)(uintptr_t)1;
	teder_code code = read_meta(PATH, c, td_libc_allocator(), &r);
	ck_assert_msg(code == expected, "metadata outcome %d, expected %d", code, expected);
	if (expected != TEDER_OK)
		ck_assert_ptr_null(r);
	teder_result_release(r);
}

START_TEST(validation)
{
	th_metadata_fixture f;
	th_metadata_make(&f, 0, 0, 0);
	th_metadata_write(&f, PATH, f.bytes);
	td_metadata_config c = config(PATH);
	teder_result *r;
	OK(read_meta(PATH, c, td_libc_allocator(), &r));
	teder_probe_info p = INIT(teder_probe_info);
	OK(teder_result_probe(r, &p));
	ck_assert(!p.tag_count && !p.artwork_count && p.duration_frames == 16);
	teder_result_release(r);
	f.bytes = 42;
	f.data[4] = 128;
	f.data[21] &= 0xf0;
	memset(f.data + 22, 0, 4);
	th_metadata_write(&f, PATH, f.bytes);
	OK(read_meta(PATH, c, td_libc_allocator(), &r));
	OK(teder_result_probe(r, &p));
	ck_assert(!p.tag_count && !p.artwork_count && !p.duration_known && !p.duration_frames);
	teder_result_release(r);
	for (unsigned mutation = 0; mutation < 7; mutation++) {
		th_metadata_make(&f, 5, 1, 0);
		teder_code expected = TEDER_CORRUPT;
		if (mutation == 0)
			f.data[f.comment + 4 + 4 + 4 + 4 + 4] = '='; /* empty key */
		if (mutation == 1)
			f.data[f.comment + 3]--; /* surplus comment byte */
		if (mutation == 2)
			f.data[f.picture + 3]--; /* picture length disagrees with parsed fields */
		if (mutation == 3)
			f.data[f.description] = 0; /* C-string truncation must be rejected */
		if (mutation == 4 || mutation == 5) {
			f.data[mutation == 4 ? f.description : f.comment + 27] = 0xff;
			expected = TEDER_ENCODING;
		}
		if (mutation == 6) {
			f.data[0] = 'X';
			expected = TEDER_UNSUPPORTED;
		}
		expect(&f, c, expected);
	}
	th_metadata_make(&f, 5, 1, 0);
	for (size_t bytes = 0; bytes < f.bytes; bytes++) {
		th_metadata_write(&f, PATH, bytes);
		ck_assert(read_meta(PATH, c, td_libc_allocator(), &r) == TEDER_CORRUPT && !r);
	}
	c.flags = 2;
	ck_assert(read_meta(PATH, c, td_libc_allocator(), &r) == TEDER_INVALID && !r);
	c = config(PATH);
	ck_assert(td_metadata_read("relative", 8, &c, td_libc_allocator(), &r) == TEDER_INVALID && !r);
	ck_assert(read_meta(OUTPUT_DIR "/\xff.flac", c, td_libc_allocator(), &r) == TEDER_ENCODING &&
	          !r);
	ck_assert(read_meta(OUTPUT_DIR "/absent-metadata.flac", c, td_libc_allocator(), &r) ==
	              TEDER_IO &&
	          !r);
}
END_TEST

START_TEST(limits)
{
	th_metadata_fixture f;
	td_metadata_config c = config(PATH);
	th_metadata_make(&f, TD_METADATA_TAGS, 0, 0);
	expect(&f, c, TEDER_OK);
	th_metadata_make(&f, TD_METADATA_TAGS + 1, 0, 0);
	expect(&f, c, TEDER_LIMIT);
	th_metadata_make(&f, 0, TD_METADATA_ARTWORK, 0);
	expect(&f, c, TEDER_OK);
	th_metadata_make(&f, 0, TD_METADATA_ARTWORK + 1, 0);
	expect(&f, c, TEDER_LIMIT);
	th_metadata_make(&f, 0, 0, TD_METADATA_BLOCKS - 2);
	expect(&f, c, TEDER_OK);
	th_metadata_make(&f, 0, 0, TD_METADATA_BLOCKS - 1);
	expect(&f, c, TEDER_LIMIT);
	th_metadata_make(&f, 5, 1, 0);
	c.work_bytes = 1;
	expect(&f, c, TEDER_LIMIT);
	c = config(PATH);
	c.work_bytes -= 8 * 1024 * 1024; /* no room for the codec after scratch allocation */
	expect(&f, c, TEDER_LIMIT);
	c = config(PATH);
	c.result_bytes = 1;
	expect(&f, c, TEDER_LIMIT);
}
END_TEST

START_TEST(failures)
{
	th_metadata_fixture f;
	th_metadata_make(&f, 5, 2, 1);
	th_metadata_write(&f, PATH, f.bytes);
	size_t count = 0;
	for (size_t fail = 0; fail <= count; fail++) {
		th_tracker t;
		ck_assert(th_tracker_init(&t));
		t.fail_at = fail;
		teder_result *r;
		teder_code code = read_meta(PATH, config(PATH), th_allocator(&t), &r);
		ck_assert_msg(code == (fail ? TEDER_NO_MEMORY : TEDER_OK),
		              "fail-at-%u: outcome %d",
		              (unsigned)fail,
		              code);
		if (!fail)
			count = t.attempts;
		else
			ck_assert_ptr_null(r);
		teder_result_release(r);
		ck_assert(th_tracker_clean(&t));
		th_tracker_uninit(&t);
	}
	printf("metadata allocation failures 1..%zu release all owners\n", count);
}
END_TEST

typedef struct {
	th_tracker tracker;
	th_gate gate;
	atomic_uint cancel, lifecycle;
	unsigned stop_at, opened, closed, reads;
	teder_code outcome;
	teder_result *result;
} cancellation_context;

static void checkpoint(void *context, unsigned point)
{
	cancellation_context *c = context;
	if (point == TD_FILE_OPENED)
		c->opened++;
	if (point == TD_FILE_CLOSED)
		c->closed++;
	if (point == TD_FILE_READ && ++c->reads != 10)
		return; /* gate after chain/iterator/current STREAMINFO allocations */
	if (point == c->stop_at) {
		c->stop_at = 0;
		th_gate_hit(&c->gate);
	}
}

static void read_worker(void *context)
{
	cancellation_context *c = context;
	td_metadata_config config_value = config(PATH);
	config_value.file = (td_file_config){3, &c->cancel, &c->lifecycle, c, checkpoint};
	c->outcome = read_meta(PATH, config_value, th_allocator(&c->tracker), &c->result);
}

START_TEST(cancellation)
{
	th_metadata_fixture f;
	th_metadata_make(&f, 5, 2, 1);
	th_metadata_write(&f, PATH, f.bytes);
	for (unsigned point = TD_FILE_OPENED; point <= TD_FILE_CLOSED; point++) {
		cancellation_context c = {.stop_at = point};
		ck_assert(th_tracker_init(&c.tracker) && th_gate_init(&c.gate));
		atomic_init(&c.cancel, 0);
		atomic_init(&c.lifecycle, TEDER_LIVE);
		th_thread worker;
		ck_assert(th_thread_start(&worker, read_worker, &c));
		th_gate_await(&c.gate);
		ck_assert(c.tracker.live == (point == TD_FILE_OPENED ? 1 : point == TD_FILE_READ ? 5 : 2));
		atomic_store(point == TD_FILE_CLOSED ? &c.lifecycle : &c.cancel,
		             point == TD_FILE_CLOSED ? TEDER_CLOSING : 1);
		th_gate_release(&c.gate);
		th_thread_join(&worker);
		ck_assert(c.outcome == TEDER_CANCELLED && !c.result && c.opened == 1 && c.closed == 1);
		ck_assert(th_tracker_clean(&c.tracker));
		th_tracker_uninit(&c.tracker);
		th_gate_uninit(&c.gate);
	}
	cancellation_context jobs[2] = {{.stop_at = TD_FILE_READ}, {.stop_at = TD_FILE_READ}};
	th_thread workers[2];
	for (unsigned i = 0; i < 2; i++) {
		ck_assert(th_tracker_init(&jobs[i].tracker) && th_gate_init(&jobs[i].gate));
		atomic_init(&jobs[i].cancel, 0);
		atomic_init(&jobs[i].lifecycle, TEDER_LIVE);
		ck_assert(th_thread_start(&workers[i], read_worker, &jobs[i]));
		th_gate_await(&jobs[i].gate);
	}
	atomic_store(&jobs[1].cancel, 1);
	for (unsigned i = 0; i < 2; i++) {
		th_gate_release(&jobs[i].gate);
		th_thread_join(&workers[i]);
		ck_assert(jobs[i].outcome == (i ? TEDER_CANCELLED : TEDER_OK));
		if (!i)
			verify(jobs[i].result, 2);
		else
			ck_assert_ptr_null(jobs[i].result);
		teder_result_release(jobs[i].result);
		ck_assert(jobs[i].opened == 1 && jobs[i].closed == 1 && th_tracker_clean(&jobs[i].tracker));
		th_tracker_uninit(&jobs[i].tracker);
		th_gate_uninit(&jobs[i].gate);
	}
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "reference"))
		test = reference;
	else if (!strcmp(argv[1], "validation"))
		test = validation;
	else if (!strcmp(argv[1], "limits"))
		test = limits;
	else if (!strcmp(argv[1], "failures"))
		test = failures;
	else if (!strcmp(argv[1], "cancellation"))
		test = cancellation;
	else
		return 2;
	Suite *suite = suite_create("metadata");
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
