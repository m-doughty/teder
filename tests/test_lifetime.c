#include "internal.h"
#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <stdio.h>

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert((call) == TEDER_OK)

static teder_config small(void)
{
	teder_config c = INIT(teder_config);
	c.request_capacity = 2;
	c.event_capacity = 3;
	c.max_input_bytes = 8;
	c.pcm_frames = 4;
	return c;
}

static teder_probe_info info(void)
{
	teder_probe_info p = INIT(teder_probe_info);
	p.codec = TEDER_CODEC_FLAC;
	p.sample_rate = 48000;
	p.channels = 2;
	p.bits_per_sample = 24;
	p.duration_frames = 321;
	p.duration_known = 1;
	p.seekable = 1;
	return p;
}

static teder_result *probe(th_tracker *t)
{
	teder_probe_info p = info();
	td_result_input input = {.kind = TEDER_RESULT_PROBE, .probe = &p};
	teder_result *result = NULL;
	OK(td_result_create(&input, th_allocator(t), 4096, &result));
	return result;
}

START_TEST(engine)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	teder_config c = INIT(teder_config), resolved = INIT(teder_config);
	teder_engine *e = NULL;
	OK(td_engine_create(&c, th_allocator(&t), &e));
	ck_assert(t.attempts == 2 && t.live == 2 && t.bytes == e->owned_bytes);
	OK(teder_get_config(e, &resolved));
	ck_assert(resolved.request_capacity == 32 && resolved.event_capacity == 64);
	ck_assert(resolved.max_input_bytes == 4096 && resolved.pcm_frames == 65536);
	ck_assert(resolved.snapshot_slots == 4 && resolved.snapshot_loans == 16);
	ck_assert(resolved.max_bands == 256 && resolved.max_spans == 16);
	ck_assert(resolved.engine_bytes_limit == UINT64_C(64) * 1024 * 1024);
	ck_assert(resolved.result_bytes_limit == UINT64_C(16) * 1024 * 1024);
	ck_assert(c.request_capacity == 0); /* inputs were copied */
	teder_status s = INIT(teder_status), expected = INIT(teder_status);
	expected.lifecycle = TEDER_LIVE;
	expected.transport = TEDER_EMPTY;
	expected.current.epoch = 1;
	expected.gain = 1.0f;
	OK(th_read_status(e, &s));
	ck_assert(!memcmp(&s, &expected, sizeof(s)));
	teder_engine *before = e;
	ck_assert(teder_destroy(&e) == TEDER_BUSY && e == before);
	for (uint32_t i = 0; i < resolved.request_capacity; i++) {
		ck_assert(e->requests[i].result == NULL);
		ck_assert(e->requests[i].input ==
		          e->requests[0].input + (size_t)i * resolved.max_input_bytes);
	}
	_Alignas(teder_status) uint32_t prefix[2] = {8, TEDER_ABI_VERSION};
	ck_assert(teder_get_status(e, (teder_status *)prefix) == TEDER_ABI_MISMATCH);
	ck_assert(prefix[0] == 8 && prefix[1] == TEDER_ABI_VERSION);
	s.abi_version = 99;
	teder_status untouched = s;
	ck_assert(teder_get_status(e, &s) == TEDER_ABI_MISMATCH && !memcmp(&s, &untouched, sizeof(s)));
	OK(th_close(e));
	OK(th_close(e));
	s.abi_version = TEDER_ABI_VERSION;
	expected.lifecycle = TEDER_CLOSED_STATE;
	OK(th_read_status(e, &s));
	ck_assert(!memcmp(&s, &expected, sizeof(s)));
	OK(teder_get_config(e, &resolved));
	OK(teder_destroy(&e));
	ck_assert_ptr_null(e);
	OK(teder_destroy(&e));
	ck_assert(teder_destroy(NULL) == TEDER_INVALID);
	ck_assert(teder_request_close(NULL) == TEDER_INVALID);
	ck_assert(teder_get_config(NULL, &resolved) == TEDER_INVALID);
	ck_assert(teder_get_status(NULL, &s) == TEDER_INVALID);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

START_TEST(probe_views)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	char key[] = "TITLE", value[] = {'A', 0, (char)0xe2, (char)0x82, (char)0xac};
	uint8_t image[] = {0, 255, 127};
	td_tag tags[] = {{{key, 5}, {value, 5}}, {{key, 5}, {value, 0}}};
	teder_artwork art = INIT(teder_artwork);
	art.picture_type = 3;
	art.width = 12;
	art.height = 8;
	art.depth = 24;
	art.mime = (teder_text){"image/png", 9};
	art.description = (teder_text){"", 0};
	art.encoded = (teder_blob){image, 3};
	teder_probe_info p = info();
	p.tag_count = 2;
	p.artwork_count = 1;
	td_result_input input = {
	    .kind = TEDER_RESULT_PROBE, .probe = &p, .tags = tags, .artwork = &art};
	teder_result *r = NULL;
	OK(td_result_create(&input, th_allocator(&t), 4096, &r));
	ck_assert(t.attempts == 1 && t.live == 1 && t.bytes == r->bytes);
	memset(key, 'x', 5);
	memset(value, 'x', 5);
	memset(image, 4, 3);
	teder_probe_info actual = INIT(teder_probe_info);
	OK(teder_result_probe(r, &actual));
	ck_assert(!memcmp(&actual, &p, sizeof(p)));
	teder_text k, v;
	OK(teder_result_tag(r, 0, &k, &v));
	const unsigned char bytes[] = {'A', 0, 0xe2, 0x82, 0xac};
	ck_assert(k.bytes == 5 && !memcmp(k.data, "TITLE", 5));
	ck_assert(v.bytes == 5 && !memcmp(v.data, bytes, 5));
	OK(teder_result_tag(r, 1, &k, &v));
	ck_assert(!memcmp(k.data, "TITLE", 5));
	ck_assert(v.bytes == 0 && v.data == NULL);
	teder_text saved = k;
	ck_assert(teder_result_tag(r, 2, &k, &v) == TEDER_NOT_FOUND && k.data == saved.data &&
	          k.bytes == saved.bytes);
	ck_assert(teder_result_tag(r, 0, &k, &k) == TEDER_INVALID);
	teder_artwork a = INIT(teder_artwork);
	OK(teder_result_artwork(r, 0, &a));
	ck_assert(a.picture_type == 3 && a.width == 12 && a.height == 8 && a.depth == 24 &&
	          a.colors == 0);
	ck_assert(a.mime.bytes == 9 && !memcmp(a.mime.data, "image/png", 9));
	ck_assert(a.description.bytes == 0 && a.description.data == NULL);
	const uint8_t expected[] = {0, 255, 127};
	ck_assert(a.encoded.bytes == 3 && !memcmp(a.encoded.data, expected, 3));
	teder_artwork old = a;
	ck_assert(teder_result_artwork(r, UINT32_MAX, &a) == TEDER_NOT_FOUND &&
	          !memcmp(&a, &old, sizeof(a)));
	a.struct_size--;
	old = a;
	ck_assert(teder_result_artwork(r, 0, &a) == TEDER_ABI_MISMATCH && !memcmp(&a, &old, sizeof(a)));
	uint32_t count = 99;
	ck_assert(teder_result_device_count(r, &count) == TEDER_BAD_STATE && count == 99);
	OK(teder_result_kind(r, &count));
	ck_assert(count == TEDER_RESULT_PROBE);
	ck_assert(teder_result_kind(r, NULL) == TEDER_INVALID);
	teder_result_release(r);
	teder_result_release(NULL);
	r = probe(&t);
	actual = (teder_probe_info)INIT(teder_probe_info);
	OK(teder_result_probe(r, &actual));
	ck_assert(actual.tag_count == 0 && actual.artwork_count == 0);
	ck_assert(teder_result_tag(r, 0, &k, &v) == TEDER_NOT_FOUND);
	teder_result_release(r);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

START_TEST(devices)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	char name[] = "Device";
	uint8_t key[] = {0, 2, 255};
	teder_device_info d = INIT(teder_device_info);
	d.backend = TEDER_BACKEND_ALSA;
	d.is_default = 1;
	d.name = (teder_text){name, 6};
	d.key = (teder_blob){key, 3};
	td_result_input input = {.kind = TEDER_RESULT_DEVICES, .device_count = 1, .devices = &d};
	teder_result *r;
	OK(td_result_create(&input, th_allocator(&t), 4096, &r));
	name[0] = 'X';
	key[1] = 4;
	teder_device_info out = INIT(teder_device_info);
	OK(teder_result_device(r, 0, &out));
	ck_assert(out.backend == TEDER_BACKEND_ALSA && out.is_default == 1);
	ck_assert(out.name.bytes == 6 && !memcmp(out.name.data, "Device", 6));
	const uint8_t expected[] = {0, 2, 255};
	ck_assert(out.key.bytes == 3 && !memcmp(out.key.data, expected, 3));
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	ck_assert(count == 1);
	OK(teder_result_kind(r, &count));
	ck_assert(count == TEDER_RESULT_DEVICES);
	teder_device_info before = out;
	ck_assert(teder_result_device(r, 1, &out) == TEDER_NOT_FOUND &&
	          !memcmp(&before, &out, sizeof(out)));
	teder_probe_info p = INIT(teder_probe_info), old = p;
	ck_assert(teder_result_probe(r, &p) == TEDER_BAD_STATE && !memcmp(&p, &old, sizeof(p)));
	teder_text text = {0};
	teder_artwork art = INIT(teder_artwork);
	ck_assert(teder_result_tag(r, 0, &text, NULL) == TEDER_INVALID);
	ck_assert(teder_result_artwork(r, 0, &art) == TEDER_BAD_STATE);
	teder_result_release(r);
	input.device_count = 0;
	input.devices = NULL;
	OK(td_result_create(&input, th_allocator(&t), 4096, &r));
	OK(teder_result_device_count(r, &count));
	ck_assert(count == 0);
	ck_assert(teder_result_device(r, 0, &out) == TEDER_NOT_FOUND);
	teder_result_release(r);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

START_TEST(failures)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	teder_config c = small();
	teder_engine *e = NULL;
	t.fail_at = 1;
	ck_assert(td_engine_create(&c, th_allocator(&t), &e) == TEDER_NO_MEMORY && e == NULL);
	ck_assert_str_eq(t.failed_site, "arena.chunk");
	ck_assert(t.live == 0 && t.bytes == 0);
	printf("engine fail-at=1 site=%s expected=NO_MEMORY live=%zu bytes=%zu\n",
	       t.failed_site,
	       t.live,
	       t.bytes);
	t.fail_at = 0;
	OK(td_engine_create(&c, th_allocator(&t), &e));
	size_t baseline = t.bytes;
	teder_result *r = NULL;
	teder_probe_info p = info();
	td_result_input in = {.kind = TEDER_RESULT_PROBE, .probe = &p};
	t.fail_at = t.attempts + 1;
	ck_assert(td_result_create(&in, th_allocator(&t), 4096, &r) == TEDER_NO_MEMORY && r == NULL);
	ck_assert(t.live == 2 && t.bytes == baseline);
	printf("result fail-at=%zu site=%s expected=NO_MEMORY live=%zu bytes=%zu baseline=%zu\n",
	       t.fail_at,
	       t.failed_site,
	       t.live,
	       t.bytes,
	       baseline);
	t.fail_at = 0;
	size_t attempts = t.attempts;
	ck_assert(td_result_create(&in, th_allocator(&t), 0, &r) == TEDER_LIMIT && r == NULL);
	ck_assert(t.attempts == attempts);
	c.engine_bytes_limit = baseline - 1;
	teder_engine *other = e;
	ck_assert(td_engine_create(&c, th_allocator(&t), &other) == TEDER_LIMIT && other == NULL);
	c.engine_bytes_limit = baseline;
	OK(td_engine_create(&c, th_allocator(&t), &other));
	OK(th_close(other));
	OK(teder_destroy(&other));
	c = small();
	c.request_capacity = UINT32_MAX;
	c.max_input_bytes = UINT32_MAX;
	ck_assert(td_engine_create(&c, th_allocator(&t), &other) == TEDER_LIMIT && other == NULL);
	_Alignas(teder_config) uint32_t prefix[] = {8, TEDER_ABI_VERSION};
	ck_assert(teder_create((const teder_config *)prefix, &other) == TEDER_ABI_MISMATCH &&
	          other == NULL);
	ck_assert(teder_create(NULL, &other) == TEDER_INVALID && other == NULL);
	ck_assert(teder_create(&c, NULL) == TEDER_INVALID);
	const unsigned char invalid[][4] = {{0xc0, 0xaf},
	                                    {0xed, 0xa0, 0x80},
	                                    {0xf4, 0x90, 0x80, 0x80},
	                                    {0xe2, 0x82},
	                                    {0x80},
	                                    {0xe0, 0x80, 0x80},
	                                    {0xf0, 0x80, 0x80, 0x80}};
	const uint32_t lengths[] = {2, 3, 4, 2, 1, 3, 4};
	td_tag tag = {{"K", 1}, {NULL, 0}};
	in.tags = &tag;
	p.tag_count = 1;
	for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
		tag.value = (teder_text){(const char *)invalid[i], lengths[i]};
		ck_assert(td_result_create(&in, th_allocator(&t), 4096, &r) == TEDER_ENCODING && r == NULL);
	}
	const unsigned char valid[] = {
	    0xc2, 0x80, 0xdf, 0xbf, 0xf0, 0x90, 0x80, 0x80, 0xf4, 0x8f, 0xbf, 0xbf};
	tag.value = (teder_text){(const char *)valid, sizeof(valid)};
	OK(td_result_create(&in, th_allocator(&t), 4096, &r));
	teder_text key, text;
	OK(teder_result_tag(r, 0, &key, &text));
	ck_assert(text.bytes == sizeof(valid) && !memcmp(text.data, valid, sizeof(valid)));
	teder_result_release(r);
	r = NULL;
	tag.value = (teder_text){NULL, 1};
	ck_assert(td_result_create(&in, th_allocator(&t), 4096, &r) == TEDER_INVALID);
	tag.value = (teder_text){"x", UINT32_MAX};
	ck_assert(td_result_create(&in, th_allocator(&t), 4096, &r) == TEDER_LIMIT);
	p.tag_count = UINT32_MAX;
	ck_assert(td_result_create(&in, th_allocator(&t), 4096, &r) == TEDER_LIMIT);
	p.tag_count = 0;
	p.abi_version++;
	ck_assert(td_result_create(&in, th_allocator(&t), 4096, &r) == TEDER_ABI_MISMATCH);
	p = info();
	teder_artwork art = INIT(teder_artwork);
	p.artwork_count = 1;
	in.artwork = &art;
	art.reserved = 1;
	ck_assert(td_result_create(&in, th_allocator(&t), 4096, &r) == TEDER_INVALID);
	art.reserved = 0;
	art.encoded = (teder_blob){(const uint8_t *)"x", UINT64_MAX};
	ck_assert(td_result_create(&in, th_allocator(&t), UINT64_MAX, &r) == TEDER_LIMIT);
	r = probe(&t);
	size_t bytes = r->bytes;
	teder_result_release(r);
	r = NULL;
	p = info();
	in = (td_result_input){.kind = TEDER_RESULT_PROBE, .probe = &p};
	ck_assert(td_result_create(&in, th_allocator(&t), bytes - 1, &r) == TEDER_LIMIT);
	OK(td_result_create(&in, th_allocator(&t), bytes, &r));
	ck_assert(teder_get_status((teder_engine *)r, NULL) == TEDER_BAD_STATE);
	r->header.version++;
	uint32_t value = 77;
	ck_assert(teder_result_kind(r, &value) == TEDER_ABI_MISMATCH && value == 77);
	r->header.version--;
	teder_result_release(r);
	ck_assert(t.bytes == baseline && t.live == 2);
	OK(th_close(e));
	OK(teder_destroy(&e));
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

typedef struct {
	const teder_result *result;
	th_gate gate;
	bool valid;
} reader;

static void read_result(void *p)
{
	reader *r = p;
	th_gate_hit(&r->gate);
	r->valid = true;
	for (int i = 0; i < 100; i++) {
		teder_probe_info out = INIT(teder_probe_info);
		if (teder_result_probe(r->result, &out) != TEDER_OK || out.duration_frames != 321 ||
		    out.sample_rate != 48000)
			r->valid = false;
	}
}

START_TEST(ownership)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	teder_config c = small();
	teder_engine *e;
	OK(td_engine_create(&c, th_allocator(&t), &e));
	size_t engine_bytes = t.bytes;
	teder_result *r = probe(&t), *second = probe(&t);
	size_t result_bytes = r->bytes;
	OK(td_engine_attach_result(e, 0, &r));
	ck_assert_ptr_null(r);
	ck_assert(td_engine_attach_result(e, 0, &second) == TEDER_BUSY && second != NULL);
	ck_assert(td_engine_attach_result(e, 2, &second) == TEDER_NOT_FOUND && second != NULL);
	e->config.result_bytes_limit = result_bytes - 1;
	ck_assert(td_engine_attach_result(e, 1, &second) == TEDER_LIMIT && second != NULL);
	e->config.result_bytes_limit = result_bytes;
	uint64_t limit = e->config.engine_bytes_limit;
	e->config.engine_bytes_limit = e->owned_bytes;
	ck_assert(td_engine_attach_result(e, 1, &second) == TEDER_LIMIT && second != NULL);
	e->config.engine_bytes_limit = limit;
	OK(td_engine_attach_result(e, 1, &second));
	ck_assert(e->owned_bytes == engine_bytes + 2 * result_bytes);
	OK(td_engine_take_result(e, 0, &r));
	ck_assert(e->owned_bytes == engine_bytes + result_bytes);
	ck_assert(td_engine_take_result(e, 0, &second) == TEDER_NOT_FOUND && second == NULL);
	reader readers[2] = {{.result = r}, {.result = r}};
	th_thread threads[2];
	unsigned initialized = 0, started = 0;
	for (; initialized < 2; initialized++)
		if (!th_gate_init(&readers[initialized].gate))
			break;
	if (initialized == 2)
		for (; started < 2; started++)
			if (!th_thread_start(&threads[started], read_result, &readers[started]))
				break;
	for (unsigned i = 0; i < started; i++)
		th_gate_await(&readers[i].gate);
	teder_code closed = th_close(e), destroyed = teder_destroy(&e);
	bool accounted = t.bytes == result_bytes && t.live == 1;
	for (unsigned i = 0; i < started; i++)
		th_gate_release(&readers[i].gate);
	for (unsigned i = 0; i < started; i++)
		th_thread_join(&threads[i]);
	for (unsigned i = 0; i < initialized; i++)
		th_gate_uninit(&readers[i].gate);
	ck_assert(closed == TEDER_OK && destroyed == TEDER_OK && e == NULL && accounted);
	teder_result_release(r);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
	ck_assert(initialized == 2 && started == 2 && readers[0].valid && readers[1].valid);
	ck_assert(th_tracker_init(&t));
	OK(td_engine_create(&c, th_allocator(&t), &e));
	r = probe(&t);
	second = probe(&t);
	OK(td_engine_attach_result(e, 0, &r));
	OK(th_close(e));
	ck_assert(td_engine_attach_result(e, 1, &second) == TEDER_CLOSED && second != NULL);
	OK(td_engine_take_result(e, 0, &r));
	OK(teder_destroy(&e));
	teder_result_release(r);
	teder_result_release(second);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "engine"))
		test = engine;
	else if (!strcmp(argv[1], "probe"))
		test = probe_views;
	else if (!strcmp(argv[1], "devices"))
		test = devices;
	else if (!strcmp(argv[1], "failures"))
		test = failures;
	else if (!strcmp(argv[1], "ownership"))
		test = ownership;
	else
		return 2;
	Suite *suite = suite_create("lifetime");
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
