#include "backend.h"
#include "support/harness.h"
#include <check.h>
#include <stdio.h>
#include <string.h>

#define OK(call) ck_assert((call) == TEDER_OK)
typedef struct {
	th_tracker tracker;
	unsigned live, closed, entries, malformed;
	bool fail_init, fail_enum, long_id, default_lookup;
} fixture;

static ma_result
alsa_info(ma_context *c, ma_device_type type, const ma_device_id *id, ma_device_info *info)
{
	(void)c;
	ck_assert(type == ma_device_type_playback);
	/* Pinned ALSA copies only the name; the generic wrapper copies an explicit ID. */
	ck_assert(!id || strcmp(id->alsa, "default") == 0);
	memcpy(info->name, "Default output", 15);
	return MA_SUCCESS;
}

static ma_result fake_init(const ma_backend *backends,
                           ma_uint32 count,
                           const ma_context_config *config,
                           ma_context *c)
{
	ck_assert(count == 1 || count == 4);
	memset(c, 0, sizeof(*c));
	c->allocationCallbacks = config->allocationCallbacks;
	c->pUserData = config->pUserData;
	c->backend = backends[0];
	fixture *f = c->pUserData;
	void *p = ma_malloc(32, &c->allocationCallbacks);
	if (!p)
		return MA_OUT_OF_MEMORY;
	void *q = ma_realloc(p, 64, &c->allocationCallbacks);
	if (!q || f->fail_init) {
		ma_free(q ? q : p, &c->allocationCallbacks);
		return MA_ERROR;
	}
	c->pDeviceInfos = q;
	if (f->default_lookup) {
		c->callbacks.onContextGetDeviceInfo = alsa_info;
		if (ma_mutex_init(&c->deviceInfoLock) != MA_SUCCESS) {
			ma_free(q, &c->allocationCallbacks);
			return MA_ERROR;
		}
	}
	f->live++;
	return MA_SUCCESS;
}

static ma_result fake_uninit(ma_context *c)
{
	fixture *f = c->pUserData;
	ck_assert(f->live == 1);
	f->live--;
	f->closed++;
	if (f->default_lookup)
		ma_mutex_uninit(&c->deviceInfoLock);
	ma_free(c->pDeviceInfos, &c->allocationCallbacks);
	return MA_SUCCESS;
}

static ma_result fake_enum(ma_context *c, ma_enum_devices_callback_proc callback, void *user)
{
	fixture *f = c->pUserData;
	if (f->fail_enum)
		return MA_ERROR;
	ma_device_info info = {0};
	ck_assert(callback(c, ma_device_type_capture, &info, user));
	for (unsigned i = 0; i < f->entries; i++) {
		memset(&info, 0, sizeof(info));
		if (c->backend == ma_backend_wasapi) {
			info.id.wasapi[0] = 0x03a9;
			info.id.wasapi[1] = (ma_wchar_win32)('A' + i % 26);
		} else {
			char *id = c->backend == ma_backend_coreaudio ? info.id.coreaudio
			           : c->backend == ma_backend_alsa    ? info.id.alsa
			                                              : info.id.pulse;
			memcpy(id, "output-A", 9);
			id[7] = (char)('A' + i % 26);
		}
		memcpy(info.name, "Speaker", 8);
		if (f->long_id) {
			if (c->backend == ma_backend_wasapi) {
				for (unsigned j = 0; j < 63; j++)
					info.id.wasapi[j] = 'A';
				info.id.wasapi[63] = 0;
			} else {
				memset(&info.id, 'A', 255);
				info.id.alsa[255] = 0;
			}
		}
		info.isDefault = i == 0;
		if (f->malformed == 1)
			memset(&info.id, 0x41, sizeof(info.id));
		if (f->malformed == 2)
			memset(info.name, 'A', sizeof(info.name));
		if (f->malformed == 3)
			info.name[0] = (char)0xff;
		if (!callback(c, ma_device_type_playback, &info, user))
			break;
	}
	return MA_SUCCESS;
}

static teder_code open_backend(fixture *f, uint64_t token, uint32_t backend, td_backend **out)
{
	td_backend_api api = {fake_init, fake_uninit, fake_enum, f};
	return td_backend_open(token, backend, 4096, th_allocator(&f->tracker), &api, out);
}

START_TEST(snapshot)
{
	for (unsigned backend = 1; backend <= 4; backend++) {
		fixture f = {.entries = 2};
		ck_assert(th_tracker_init(&f.tracker));
		td_backend *b;
		OK(open_backend(&f, UINT64_C(0x102030405060708), backend, &b));
		teder_result *r;
		OK(td_backend_enumerate(b, 8192, 272, &r));
		uint32_t count;
		OK(teder_result_device_count(r, &count));
		ck_assert(count == 2);
		for (unsigned i = 0; i < count; i++) {
			teder_device_info info = {.struct_size = sizeof(info),
			                          .abi_version = TEDER_ABI_VERSION};
			OK(teder_result_device(r, i, &info));
			ck_assert(info.backend == backend && info.is_default == (i == 0));
			ck_assert(info.name.bytes == 7 && !memcmp(info.name.data, "Speaker", 7));
			ck_assert(info.key.bytes == (backend == 1 ? 22 : 25));
			unsigned char key[272];
			memcpy(key, info.key.data, (size_t)info.key.bytes);
			ck_assert(!memcmp(key, "TDK1\x08\x07\x06\x05\x04\x03\x02\x01", 12));
			ma_device_id id;
			OK(td_backend_key(b, (teder_blob){key, info.key.bytes}, &id));
			if (backend == 1)
				ck_assert(id.wasapi[0] == 0x03a9 && id.wasapi[1] == 'A' + i && !id.wasapi[2]);
			else {
				const char *s = backend == 2 ? id.coreaudio : backend == 3 ? id.alsa : id.pulse;
				ck_assert(!memcmp(s, "output-", 7) && s[7] == (char)('A' + i) && !s[8]);
			}
		}
		teder_result *empty;
		f.long_id = true;
		OK(td_backend_enumerate(b, 8192, 272, &empty));
		teder_device_info longest = {.struct_size = sizeof(longest),
		                             .abi_version = TEDER_ABI_VERSION};
		OK(teder_result_device(empty, 0, &longest));
		ck_assert(longest.key.bytes == (backend == 1 ? 144 : 272));
		ma_device_id decoded;
		OK(td_backend_key(b, longest.key, &decoded));
		teder_result_release(empty);
		f.entries = 0;
		OK(td_backend_enumerate(b, 8192, 272, &empty));
		OK(teder_result_device_count(empty, &count));
		ck_assert(count == 0);
		teder_result_release(empty);
		OK(td_backend_close(&b));
		ck_assert(!b && f.live == 0 && f.closed == 1 && f.tracker.live == 1);
		teder_device_info info = {.struct_size = sizeof(info), .abi_version = TEDER_ABI_VERSION};
		OK(teder_result_device(r, 0, &info));
		ck_assert(!memcmp(info.name.data, "Speaker", 7) && info.backend == backend);
		teder_result_release(r);
		ck_assert(th_tracker_clean(&f.tracker));
		th_tracker_uninit(&f.tracker);
	}
}
END_TEST

START_TEST(default_id)
{
	fixture f = {.default_lookup = true};
	ck_assert(th_tracker_init(&f.tracker));
	td_backend *b;
	OK(open_backend(&f, 42, TEDER_BACKEND_ALSA, &b));
	td_backend_loan loan;
	OK(td_backend_borrow(b, (teder_blob){0}, &loan));
	ck_assert_str_eq(loan.id.alsa, "default");
	ck_assert(td_backend_close(&b) == TEDER_BUSY);
	td_backend_return(b);
	OK(td_backend_close(&b));
	ck_assert(th_tracker_clean(&f.tracker) && f.live == 0 && f.closed == 1);
	th_tracker_uninit(&f.tracker);
}
END_TEST

START_TEST(validation)
{
	fixture f = {.entries = 1};
	ck_assert(th_tracker_init(&f.tracker));
	td_backend *b;
	OK(open_backend(&f, 42, 1, &b));
	teder_result *r;
	OK(td_backend_enumerate(b, 8192, 272, &r));
	teder_device_info info = {.struct_size = sizeof(info), .abi_version = TEDER_ABI_VERSION};
	OK(teder_result_device(r, 0, &info));
	unsigned char key[272];
	memcpy(key, info.key.data, (size_t)info.key.bytes);
	ma_device_id id, saved;
	memset(&saved, 0x55, sizeof(saved));
	const unsigned positions[] = {0, 4, 12, 13, 14, 15, 20};
	for (unsigned i = 0; i < sizeof(positions) / sizeof(*positions); i++) {
		unsigned index = positions[i];
		unsigned char original = key[index];
		key[index] ^= 0x80;
		id = saved;
		ck_assert(td_backend_key(b, (teder_blob){key, info.key.bytes}, &id) ==
		          (index == 4 ? TEDER_STALE : TEDER_INVALID));
		ck_assert(!memcmp(&id, &saved, sizeof(id)));
		key[index] = original;
	}
	ck_assert(td_backend_key(b, (teder_blob){key, 15}, &id) == TEDER_INVALID);
	ck_assert(td_backend_key(b, (teder_blob){key, 273}, &id) == TEDER_INVALID);
	ck_assert(td_backend_key(b, (teder_blob){NULL, 22}, &id) == TEDER_INVALID);
	ck_assert(td_backend_key(b, (teder_blob){key, 22}, NULL) == TEDER_INVALID);
	teder_result_release(r);
	/* Copied keys survive the result; a different engine token is stale. */
	OK(td_backend_key(b, (teder_blob){key, 22}, &id));
	OK(td_backend_close(&b));
	OK(open_backend(&f, 43, 1, &b));
	ck_assert(td_backend_key(b, (teder_blob){key, 22}, &id) == TEDER_STALE);
	for (unsigned malformed = 1; malformed <= 3; malformed++) {
		f.malformed = malformed;
		ck_assert(td_backend_enumerate(b, 8192, 272, &r) ==
		          (malformed == 3 ? TEDER_ENCODING : TEDER_INVALID));
		ck_assert_ptr_null(r);
	}
	f.malformed = 0;
	ck_assert(td_backend_enumerate(b, 8192, 21, &r) == TEDER_LIMIT && !r);
	ck_assert(td_backend_enumerate(b, 1, 272, &r) == TEDER_LIMIT && !r);
	f.entries = 129;
	ck_assert(td_backend_enumerate(b, 65536, 272, &r) == TEDER_LIMIT && !r);
	f.entries = 128;
	OK(td_backend_enumerate(b, 65536, 272, &r));
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	ck_assert(count == 128);
	teder_result_release(r);
	f.fail_enum = true;
	ck_assert(td_backend_enumerate(b, 8192, 272, &r) == TEDER_DEVICE_LOST && !r);
	OK(td_backend_close(&b));
	ck_assert(th_tracker_clean(&f.tracker));
	th_tracker_uninit(&f.tracker);
}
END_TEST

START_TEST(failures)
{
	for (unsigned fail = 1; fail <= 4; fail++) {
		fixture f = {.entries = 1};
		ck_assert(th_tracker_init(&f.tracker));
		f.tracker.fail_at = fail;
		td_backend *b;
		teder_code code = open_backend(&f, 1, 1, &b);
		if (fail <= 3)
			ck_assert(code == TEDER_NO_MEMORY && !b && !f.live && !f.closed);
		else {
			OK(code);
			teder_result *r;
			ck_assert(td_backend_enumerate(b, 8192, 272, &r) == TEDER_NO_MEMORY && !r);
			OK(td_backend_close(&b));
			ck_assert(f.closed == 1);
		}
		ck_assert(th_tracker_clean(&f.tracker));
		th_tracker_uninit(&f.tracker);
	}
	fixture f = {.fail_init = true};
	ck_assert(th_tracker_init(&f.tracker));
	td_backend *b;
	ck_assert(open_backend(&f, 1, 1, &b) == TEDER_DEVICE_LOST && !b && !f.closed);
	td_backend_api api = {fake_init, fake_uninit, fake_enum, &f};
	ck_assert(td_backend_open(1, 1, 0, th_allocator(&f.tracker), &api, &b) == TEDER_LIMIT && !b);
	ck_assert(open_backend(&f, 0, 1, &b) == TEDER_INVALID && !b);
	ck_assert(open_backend(&f, 1, 5, &b) == TEDER_INVALID && !b);
	size_t size;
	ck_assert(td_backend_plan(SIZE_MAX, &size) == TEDER_LIMIT);
	ck_assert(th_tracker_clean(&f.tracker));
	th_tracker_uninit(&f.tracker);
}
END_TEST

typedef struct {
	ma_allocation_callbacks allocator;
	bool valid;
} heap_job;

static void allocate_many(void *context)
{
	heap_job *job = context;
	ma_allocation_callbacks *a = &job->allocator;
	job->valid = true;
	for (unsigned i = 0; i < 1000; i++) {
		void *p = ma_malloc(64, a);
		if (!p) {
			job->valid = false;
			return;
		}
		memset(p, 0x34, 64);
		void *q = ma_realloc(p, 128, a);
		if (!q) {
			ma_free(p, a);
			job->valid = false;
			return;
		}
		for (unsigned j = 0; j < 64; j++)
			if (((unsigned char *)q)[j] != 0x34)
				job->valid = false;
		ma_free(q, a);
	}
}

START_TEST(heap)
{
	th_tracker tracker;
	ck_assert(th_tracker_init(&tracker));
	td_backend_heap h;
	OK(td_backend_heap_init(&h, th_allocator(&tracker), 4096));
	ma_allocation_callbacks a = td_backend_allocator(&h);
	heap_job jobs[2] = {{a, false}, {a, false}};
	th_thread first, second;
	ck_assert(th_thread_start(&first, allocate_many, &jobs[0]));
	ck_assert(th_thread_start(&second, allocate_many, &jobs[1]));
	th_thread_join(&first);
	th_thread_join(&second);
	ck_assert(jobs[0].valid && jobs[1].valid);
	ck_assert(h.live == 0 && h.bytes == 0 && tracker.attempts == 4000);
	unsigned char *p = ma_malloc(16, &a);
	ck_assert_ptr_nonnull(p);
	ck_assert((uintptr_t)p % _Alignof(max_align_t) == 0);
	memset(p, 0x42, 16);
	h.limit = h.bytes + 1; /* Quiescent owner: force realloc-overlap rejection. */
	ck_assert_ptr_null(ma_realloc(p, 32, &a));
	ck_assert(td_backend_heap_error(&h) == TEDER_LIMIT && h.live == 1);
	for (unsigned i = 0; i < 16; i++)
		ck_assert(p[i] == 0x42);
	ck_assert(td_backend_heap_uninit(&h) == TEDER_BUSY);
	ma_free(p, &a);
	ck_assert_ptr_null(ma_malloc(1, &a)); /* sticky until owner destruction */
	OK(td_backend_heap_uninit(&h));
	OK(td_backend_heap_init(&h, th_allocator(&tracker), 4096));
	ck_assert_ptr_null(ma_malloc(SIZE_MAX, &a));
	ck_assert(h.error == TEDER_LIMIT && !h.live);
	OK(td_backend_heap_uninit(&h));
	ck_assert(th_tracker_clean(&tracker));
	th_tracker_uninit(&tracker);
}
END_TEST

START_TEST(unwind)
{
	ma_backend disabled = ma_backend_null;
	ma_context context;
	/* Warm OS/COM caches before the regression's exact handle baseline. */
	for (unsigned i = 0; i < 16; i++)
		ck_assert(ma_context_init(&disabled, 1, NULL, &context) == MA_NO_BACKEND);
#ifdef _WIN32
	DWORD before, after;
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &before));
#endif
	for (unsigned i = 0; i < 16; i++)
		ck_assert(ma_context_init(&disabled, 1, NULL, &context) == MA_NO_BACKEND);
#ifdef _WIN32
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &after));
	ck_assert(before == after);
#endif
}
END_TEST

START_TEST(native)
{
	th_tracker tracker;
	ck_assert(th_tracker_init(&tracker));
	td_backend *b;
	teder_code code = td_backend_open(1, 0, 2 * 1024 * 1024, th_allocator(&tracker), NULL, &b);
	if (code != TEDER_OK) {
		printf("Native context unavailable: code=%d\n", code);
		ck_assert(th_tracker_clean(&tracker));
		th_tracker_uninit(&tracker);
	}
	OK(code);
	size_t attempts = tracker.attempts;
	teder_result *r;
	OK(td_backend_enumerate(b, 65536, 272, &r));
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	printf("Native playback devices=%u\n", count);
	for (uint32_t i = 0; i < count; i++) {
		teder_device_info info = {.struct_size = sizeof(info), .abi_version = TEDER_ABI_VERSION};
		OK(teder_result_device(r, i, &info));
		ma_device_id id;
		OK(td_backend_key(b, info.key, &id));
		printf("device=%u backend=%u default=%u key-bytes=%llu\n",
		       i,
		       info.backend,
		       info.is_default,
		       (unsigned long long)info.key.bytes);
	}
	OK(td_backend_close(&b));
	teder_result_release(r);
	ck_assert(th_tracker_clean(&tracker));
	for (size_t fail = 1; fail <= attempts; fail++) {
		tracker.fail_at = tracker.attempts + fail;
		code = td_backend_open(1, 0, 2 * 1024 * 1024, th_allocator(&tracker), NULL, &b);
		ck_assert(code == TEDER_NO_MEMORY && !b);
		ck_assert(th_tracker_clean(&tracker));
	}
	printf("Native context allocation failures checked=%zu\n", attempts);
	th_tracker_uninit(&tracker);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "native"))
		test = native;
	else if (!strcmp(argv[1], "snapshot"))
		test = snapshot;
	else if (!strcmp(argv[1], "validation"))
		test = validation;
	else if (!strcmp(argv[1], "default-id"))
		test = default_id;
	else if (!strcmp(argv[1], "failures"))
		test = failures;
	else if (!strcmp(argv[1], "heap"))
		test = heap;
	else if (!strcmp(argv[1], "unwind"))
		test = unwind;
	else
		return 2;
	Suite *suite = suite_create("backend");
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
