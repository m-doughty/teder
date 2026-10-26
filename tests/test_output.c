#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <stdio.h>

#define INIT(type)                                                                                 \
	{                                                                                              \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION                              \
	}
#define OK(call) ck_assert((call) == TEDER_OK)
#define RETRY(code, call)                                                                          \
	do {                                                                                           \
		(code) = (call);                                                                           \
		if ((code) == TEDER_BUSY)                                                                  \
			th_yield();                                                                            \
	} while ((code) == TEDER_BUSY)

typedef struct {
	th_tracker tracker;
	th_gate gate;
	atomic_uint point;
	unsigned opened, closed, enumerated;
	bool reject, enum_error;
	void *native_heap;
#ifdef _WIN32
	DWORD caller, owner;
#else
	pthread_t caller, owner;
#endif
} fixture;

static void hit(fixture *f, unsigned point)
{
	unsigned expected = point;
	if (atomic_compare_exchange_strong(&f->point, &expected, 0))
		th_gate_hit(&f->gate);
}

static void owner(fixture *f)
{
#ifdef _WIN32
	ck_assert(GetCurrentThreadId() == f->owner && f->owner != f->caller);
#else
	ck_assert(pthread_equal(pthread_self(), f->owner) && !pthread_equal(f->owner, f->caller));
#endif
}

static ma_result context_init(const ma_backend *backends,
                              ma_uint32 count,
                              const ma_context_config *config,
                              ma_context *c)
{
	(void)backends;
	ck_assert(count == 4);
	fixture *f = config->pUserData;
#ifdef _WIN32
	f->owner = GetCurrentThreadId();
#else
	f->owner = pthread_self();
#endif
	owner(f);
	hit(f, 1);
	if (f->reject)
		return MA_NO_BACKEND;
	memset(c, 0, sizeof(*c));
	c->backend = ma_backend_wasapi;
	c->pUserData = f;
	c->allocationCallbacks = config->allocationCallbacks;
	f->native_heap = ma_malloc(64, &c->allocationCallbacks);
	if (!f->native_heap)
		return MA_OUT_OF_MEMORY;
	f->opened++;
	return MA_SUCCESS;
}

static ma_result context_uninit(ma_context *c)
{
	fixture *f = c->pUserData;
	owner(f);
	ma_free(f->native_heap, &c->allocationCallbacks);
	f->native_heap = NULL;
	f->closed++;
	return MA_SUCCESS;
}

static ma_result enumerate(ma_context *c, ma_enum_devices_callback_proc callback, void *user)
{
	fixture *f = c->pUserData;
	owner(f);
	hit(f, 2);
	f->enumerated++;
	void *temporary = ma_malloc(32, &c->allocationCallbacks);
	if (!temporary)
		return MA_OUT_OF_MEMORY;
	ma_free(temporary, &c->allocationCallbacks);
	if (f->enum_error)
		return MA_ERROR;
	for (unsigned i = 0; i < 2; i++) {
		ma_device_info info = {0};
		info.id.wasapi[0] = (ma_wchar_win32)('A' + i);
		info.isDefault = i == 0;
		memcpy(info.name, i ? "Beta" : "Alpha", i ? 5 : 6);
		if (!callback(c, ma_device_type_playback, &info, user))
			break;
	}
	return MA_SUCCESS;
}

static void checkpoint(void *p, uint64_t request, unsigned point)
{
	(void)request;
	if (point == TD_OUTPUT_BEFORE_COMMIT)
		hit(p, 3);
	if (point == TD_OUTPUT_AFTER_COMMIT)
		hit(p, 4);
}

static void init(fixture *f)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->point, 0);
	ck_assert(th_tracker_init(&f->tracker) && th_gate_init(&f->gate));
#ifdef _WIN32
	f->caller = GetCurrentThreadId();
#else
	f->caller = pthread_self();
#endif
}

static void dispose(fixture *f)
{
	ck_assert(f->opened == f->closed && !f->native_heap && th_tracker_clean(&f->tracker));
	th_gate_uninit(&f->gate);
	th_tracker_uninit(&f->tracker);
}

static teder_engine *create(fixture *f, teder_config config, bool native)
{
	td_runtime runtime = {.context = f,
	                      .media_checkpoint = checkpoint,
	                      .backend = {context_init, context_uninit, enumerate, f}};
	teder_engine *e;
	OK(td_engine_create_with(
	    &config, th_allocator(&f->tracker), native ? (td_runtime){0} : runtime, &e));
	return e;
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

static uint64_t submit(teder_engine *e)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_enumerate_devices(e, &id));
	OK(code);
	ck_assert(id != 0);
	return id;
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
	ack(e, id);
	return r;
}

static teder_device_info entry(teder_result *r, unsigned index)
{
	teder_device_info d = INIT(teder_device_info);
	OK(teder_result_device(r, index, &d));
	return d;
}

static size_t owned(teder_engine *e)
{
	td_lock(e);
	size_t bytes = e->owned_bytes;
	td_unlock(e);
	return bytes;
}

static void close_engine(teder_engine **e)
{
	OK(th_close(*e));
	OK(teder_destroy(e));
}

START_TEST(snapshot)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	size_t base = owned(e);
	uint64_t id = submit(e);
	teder_request_info done = terminal(e, id);
	ck_assert(done.outcome == TEDER_OK && done.operation == TEDER_OP_DEVICES &&
	          done.result_kind == TEDER_RESULT_DEVICES && done.committed.epoch == 0);
	teder_result *r = take(e, id);
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	ck_assert(count == 2);
	teder_device_info a = entry(r, 0), b = entry(r, 1);
	ck_assert(a.backend == TEDER_BACKEND_WASAPI && a.is_default == 1 && b.is_default == 0);
	ck_assert(a.name.bytes == 5 && !memcmp(a.name.data, "Alpha", 5));
	ck_assert(b.name.bytes == 4 && !memcmp(b.name.data, "Beta", 4));
	ck_assert(a.key.bytes == 20 && b.key.bytes == 20 && !memcmp(a.key.data, "TDK1", 4));
	ck_assert(((const unsigned char *)a.key.data)[16] == 'A' &&
	          ((const unsigned char *)b.key.data)[16] == 'B');
	size_t context_bytes = owned(e);
	ck_assert(context_bytes == base + e->backend_reservation && f.opened == 1);

	id = submit(e);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ck_assert(owned(e) > context_bytes);
	ack(e, id); /* untaken result dies with acknowledgement */
	ck_assert(owned(e) == context_bytes && f.opened == 1 && f.enumerated == 2);
	teder_status status = INIT(teder_status);
	OK(th_read_status(e, &status));
	ck_assert(status.output_state == TEDER_OUTPUT_NONE && status.backend == 0 &&
	          status.current.epoch == 1 && status.current.source == 0 && status.media_request == 0);

	close_engine(&e);
	ck_assert(f.closed == 1 && f.tracker.live == 1);
	teder_device_info retained = entry(r, 0);
	ck_assert(retained.name.bytes == 5 && !memcmp(retained.name.data, "Alpha", 5));
	ck_assert(retained.key.bytes == a.key.bytes &&
	          !memcmp(retained.key.data, a.key.data, a.key.bytes));
	teder_result_release(r);
	dispose(&f);
}
END_TEST

START_TEST(cancellation)
{
	for (unsigned mode = 0; mode < 4; mode++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		config.request_capacity = 2;
		teder_engine *e = create(&f, config, false);
		if (mode == 3) {
			uint64_t warm = submit(e);
			ck_assert(terminal(e, warm).outcome == TEDER_OK);
			ack(e, warm);
		}
		atomic_store(&f.point, mode == 0 ? 1 : mode == 2 ? 4 : 3);
		uint64_t id = submit(e);
		th_gate_await(&f.gate);
		teder_result *r = NULL;
		ck_assert(teder_request_take_result(e, id, &r) == TEDER_BUSY && !r);
		OK(teder_request_cancel(e, id));
		uint64_t next = submit(e), rejected = 99;
		ck_assert(teder_enumerate_devices(e, &rejected) == TEDER_LIMIT && rejected == 0);

		th_gate_release(&f.gate);
		teder_request_info done = terminal(e, id);
		ck_assert(done.outcome == (mode == 2 ? TEDER_OK : TEDER_CANCELLED));
		ck_assert(done.result_kind == (mode == 2 ? TEDER_RESULT_DEVICES : TEDER_RESULT_NONE));
		ck_assert(terminal(e, next).outcome == TEDER_OK);
		ack(e, id);
		ack(e, next);
		ck_assert(f.opened == (mode < 2 ? 2 : 1));
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

START_TEST(failures)
{
	size_t base = 0, reservation;
	OK(td_backend_plan(8 * 1024 * 1024, &reservation));
	for (unsigned mode = 0; mode < 10; mode++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		if (mode == 4 || mode == 5)
			config.engine_bytes_limit = base + reservation - (mode == 4);
		if (mode == 6)
			config.result_bytes_limit = 1;
		if (mode == 7)
			config.max_input_bytes = 19;
		f.reject = mode == 8;
		f.enum_error = mode == 9;
		teder_engine *e = create(&f, config, false);
		size_t original = owned(e);
		if (mode == 0)
			base = original;
		if (mode < 4)
			f.tracker.fail_at = f.tracker.attempts + mode + 1;
		uint64_t id = submit(e);
		teder_code expected = mode < 4    ? TEDER_NO_MEMORY
		                      : mode < 8  ? TEDER_LIMIT
		                      : mode == 8 ? TEDER_UNSUPPORTED
		                                  : TEDER_DEVICE_LOST;
		ck_assert(terminal(e, id).outcome == expected);
		ck_assert(!e->backend && owned(e) == original && f.tracker.bytes == original);
		ack(e, id);
		if (mode < 4) {
			f.tracker.fail_at = 0;
			id = submit(e);
			ck_assert(terminal(e, id).outcome == TEDER_OK);
			ack(e, id);
		}
		close_engine(&e);
		dispose(&f);
	}

	/* Existing healthy contexts survive result allocation failure. A failed
	 * dependency allocation poisons the context, which must be rebuilt. */
	for (unsigned fail = 1; fail <= 2; fail++) {
		fixture f;
		init(&f);
		teder_config config = INIT(teder_config);
		teder_engine *e = create(&f, config, false);
		uint64_t id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_OK);
		ack(e, id);
		uint64_t token = e->backend_token;
		f.tracker.fail_at = f.tracker.attempts + fail;
		id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_NO_MEMORY);
		ck_assert((e->backend != NULL) == (fail == 2));
		ack(e, id);
		f.tracker.fail_at = 0;
		id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_OK && e->backend_token == token);
		close_engine(&e); /* also frees the final untaken result */
		dispose(&f);
	}
}
END_TEST

START_TEST(identity)
{
	atomic_uint_fast64_t counter;
	atomic_init(&counter, UINT64_MAX - 1);
	uint64_t value = 0;
	OK(td_token_next(&counter, &value));
	ck_assert(value == UINT64_MAX);
	ck_assert(td_token_next(&counter, &value) == TEDER_LIMIT && value == UINT64_MAX);
	ck_assert(atomic_load(&counter) == UINT64_MAX);

	fixture a, b;
	init(&a);
	init(&b);
	teder_config config = INIT(teder_config);
	teder_engine *first = create(&a, config, false), *second = create(&b, config, false);
	uint64_t one = submit(first), two = submit(second);
	ck_assert(terminal(first, one).outcome == TEDER_OK &&
	          terminal(second, two).outcome == TEDER_OK);
	teder_result *r = take(first, one), *s = take(second, two);
	teder_device_info x = entry(r, 0), y = entry(s, 0);
	ck_assert(first->backend_token && second->backend_token &&
	          first->backend_token != second->backend_token);
	ck_assert(
	    memcmp((const unsigned char *)x.key.data + 4, (const unsigned char *)y.key.data + 4, 8));
	close_engine(&first);
	close_engine(&second);

	first = create(&a, config, false);
	one = submit(first);
	ck_assert(terminal(first, one).outcome == TEDER_OK);
	teder_result *later = take(first, one);
	y = entry(later, 0);
	ck_assert(
	    memcmp((const unsigned char *)x.key.data + 4, (const unsigned char *)y.key.data + 4, 8));
	close_engine(&first);
	teder_result_release(r);
	teder_result_release(s);
	teder_result_release(later);
	dispose(&a);
	dispose(&b);
}
END_TEST

START_TEST(closing)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, false);
	size_t base = owned(e);
	atomic_store(&f.point, 2);
	uint64_t id = submit(e);
	th_gate_await(&f.gate);
	uint64_t queued = submit(e);
	OK(teder_request_close(e));
	teder_status status = INIT(teder_status);
	OK(th_read_status(e, &status));
	ck_assert(status.lifecycle == TEDER_CLOSING && f.closed == 0);
	ck_assert(teder_destroy(&e) == TEDER_BUSY && e);

	th_gate_release(&f.gate);
	OK(th_close(e));
	ck_assert(terminal(e, id).outcome == TEDER_CANCELLED &&
	          terminal(e, queued).outcome == TEDER_CANCELLED);
	ck_assert(owned(e) == base && f.opened == 1 && f.closed == 1 && !e->backend);
	uint64_t rejected = 99;
	ck_assert(teder_enumerate_devices(e, &rejected) == TEDER_CLOSED && rejected == 0);
	OK(teder_destroy(&e));
	dispose(&f);
}
END_TEST

START_TEST(native)
{
	fixture f;
	init(&f);
	teder_config config = INIT(teder_config);
	teder_engine *e = create(&f, config, true);
	size_t before = f.tracker.attempts;
	uint64_t id = submit(e);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	size_t attempts = f.tracker.attempts - before;
	teder_result *r = take(e, id);
	uint32_t count;
	OK(teder_result_device_count(r, &count));
	printf("Public native enumeration: devices=%u allocations=%zu\n", count, attempts);
	close_engine(&e);
	for (uint32_t i = 0; i < count; i++) {
		teder_device_info d = entry(r, i);
		ck_assert(d.backend >= TEDER_BACKEND_WASAPI && d.backend <= TEDER_BACKEND_PULSE &&
		          d.key.bytes >= 17 && d.key.bytes <= 272 && d.is_default <= 1);
		printf("device=%u backend=%u default=%u name=%.*s\n",
		       i,
		       d.backend,
		       d.is_default,
		       (int)d.name.bytes,
		       d.name.data);
	}
	teder_result_release(r);
	dispose(&f);

	for (size_t fail = 1; fail <= attempts; fail++) {
		init(&f);
		e = create(&f, config, true);
		size_t base = owned(e);
		f.tracker.fail_at = f.tracker.attempts + fail;
		id = submit(e);
		ck_assert(terminal(e, id).outcome == TEDER_NO_MEMORY);
		ck_assert(owned(e) == base && !e->backend);
		close_engine(&e);
		dispose(&f);
	}
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "snapshot"))
		test = snapshot;
	else if (!strcmp(argv[1], "cancellation"))
		test = cancellation;
	else if (!strcmp(argv[1], "failures"))
		test = failures;
	else if (!strcmp(argv[1], "identity"))
		test = identity;
	else if (!strcmp(argv[1], "closing"))
		test = closing;
	else if (!strcmp(argv[1], "native"))
		test = native;
	else
		return 2;
	Suite *suite = suite_create("output");
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
