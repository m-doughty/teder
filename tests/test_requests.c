#include "support/harness.h"
#include "support/engine.h"
#include <check.h>
#include <stdio.h>

#define INIT(type) {.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION}
#define OK(call) ck_assert((call) == TEDER_OK)
#define RETRY(code, call) \
	do { \
		(code) = (call); \
		if ((code) == TEDER_BUSY || (code) == TEDER_AGAIN) \
			th_yield(); \
	} while ((code) == TEDER_BUSY || (code) == TEDER_AGAIN)

typedef struct {
	uint32_t fail_stage;
	atomic_uint starts, stops;
	th_gate *startup, *before, *after;
	bool result;
	uint32_t calls;
	uint64_t ids[16];
	uint8_t copied[16][8];
} context;

static bool permit(void *p, uint32_t stage)
{
	return ((context *)p)->fail_stage != stage;
}

static void checkpoint(void *p, uint32_t point)
{
	context *c = p;
	if (point == TD_STARTED) {
		atomic_fetch_add(&c->starts, 1);
		if (c->startup)
			th_gate_hit(c->startup);
	} else
		atomic_fetch_add(&c->stops, 1);
}

static teder_code execute(void *p, teder_engine *e, uint32_t slot)
{
	context *c = p;
	td_request_slot *s = &e->requests[slot];
	if (c->calls == 16 || s->input_bytes > 8)
		return TEDER_INTERNAL;
	c->ids[c->calls] = s->info.request;
	if (s->input_bytes)
		memcpy(c->copied[c->calls], s->input, s->input_bytes);
	c->calls++;
	if (c->result) {
		teder_probe_info probe = INIT(teder_probe_info);
		probe.codec = TEDER_CODEC_FLAC;
		probe.sample_rate = 48000;
		probe.channels = 2;
		probe.bits_per_sample = 24;
		td_result_input input = {.kind = TEDER_RESULT_PROBE, .probe = &probe};
		teder_code code = td_build_result(e, slot, &input);
		if (code != TEDER_OK)
			return code;
	}
	if (c->before)
		th_gate_hit(c->before);
	teder_code code = td_commit(e, slot);
	if (c->after)
		th_gate_hit(c->after);
	return code;
}

static teder_config config(void)
{
	teder_config c = INIT(teder_config);
	c.request_capacity = 3;
	c.event_capacity = 2;
	c.max_input_bytes = 8;
	c.pcm_frames = 4;
	return c;
}

static teder_engine *create(th_tracker *t, context *ctx)
{
	teder_config c = config();
	teder_engine *e = NULL;
	td_runtime runtime = {
	    .context = ctx, .permit = permit, .checkpoint = checkpoint, .execute = execute};
	OK(td_engine_create_with(&c, th_allocator(t), runtime, &e));
	return e;
}

static teder_code submit(teder_engine *e, const void *data, uint32_t bytes, uint64_t *id)
{
	teder_code code;
	RETRY(code, td_submit(e, TEDER_OP_PROBE, data, bytes, id));
	return code;
}

static teder_request_info terminal(teder_engine *e, uint64_t id)
{
	teder_request_info out = INIT(teder_request_info);
	for (;;) {
		teder_code code = teder_request_get(e, id, &out);
		ck_assert(code == TEDER_OK || code == TEDER_AGAIN);
		if (code == TEDER_OK && out.state == TEDER_TERMINAL)
			return out;
		th_yield();
	}
}

static void ack(teder_engine *e, uint64_t id)
{
	teder_code code;
	RETRY(code, teder_request_ack(e, id));
	OK(code);
}

static void finish(teder_engine **e, th_tracker *t, context *c)
{
	OK(th_close(*e));
	OK(teder_destroy(e));
	ck_assert(atomic_load(&c->starts) == 1 && atomic_load(&c->stops) == 1);
	ck_assert(th_tracker_clean(t));
	th_tracker_uninit(t);
}

START_TEST(admission)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	th_gate start;
	ck_assert(th_gate_init(&start));
	context ctx = {.startup = &start};
	teder_engine *e = create(&t, &ctx);
	th_gate_await(&start);
	size_t bytes = t.bytes, attempts = t.attempts;
	uint8_t payload[] = {4, 5, 6};
	uint64_t ids[4];
	for (unsigned i = 0; i < 3; i++) {
		OK(submit(e, payload, 3, &ids[i]));
		ck_assert(ids[i] == i + 1);
	}
	memset(payload, 9, sizeof(payload));
	ck_assert(submit(e, payload, 3, &ids[3]) == TEDER_LIMIT && ids[3] == 0);
	ck_assert(t.attempts == attempts && t.bytes == bytes);
	teder_request_info out = INIT(teder_request_info), expected = INIT(teder_request_info);
	expected.request = 1;
	expected.operation = TEDER_OP_PROBE;
	expected.state = TEDER_QUEUED;
	teder_code code;
	RETRY(code, teder_request_get(e, 1, &out));
	OK(code);
	ck_assert(!memcmp(&out, &expected, sizeof(out)));
	ck_assert(teder_request_ack(e, 1) == TEDER_BUSY);
	teder_result *r = (teder_result *)e;
	ck_assert(teder_request_take_result(e, 1, &r) == TEDER_BUSY && !r);
	RETRY(code, teder_request_cancel(e, 2));
	OK(code);
	RETRY(code, teder_request_cancel(e, 2));
	OK(code);
	th_gate_release(&start);
	ck_assert(terminal(e, 3).outcome == TEDER_OK);
	ck_assert(ctx.calls == 2 && ctx.ids[0] == 1 && ctx.ids[1] == 3);
	const uint8_t original[] = {4, 5, 6};
	ck_assert(!memcmp(ctx.copied[0], original, 3));
	ck_assert(submit(e, NULL, 0, &ids[3]) == TEDER_LIMIT && ids[3] == 0);
	RETRY(code, teder_request_poll(e, &out));
	OK(code);
	ck_assert(out.request == 1);
	RETRY(code, teder_request_poll(e, &out));
	OK(code);
	ck_assert(out.request == 1);
	RETRY(code, teder_request_cancel(e, 1));
	ck_assert(code == TEDER_TOO_LATE);
	ack(e, 1);
	RETRY(code, teder_request_poll(e, &out));
	OK(code);
	ck_assert(out.request == 2 && out.outcome == TEDER_CANCELLED &&
	          out.result_kind == TEDER_RESULT_NONE);
	ack(e, 2);
	ack(e, 3);
	OK(submit(e, NULL, 0, &ids[3]));
	ck_assert(ids[3] == 4);
	ck_assert(terminal(e, 4).outcome == TEDER_OK);
	for (uint64_t next = 5; next <= 6; next++) {
		OK(submit(e, NULL, 0, &ids[0]));
		ck_assert(ids[0] == next);
		terminal(e, next);
	}
	/* Close must also work when every slot retains an unread outcome. */
	OK(th_close(e));
	OK(teder_request_poll(e, &out));
	ck_assert(out.request == 4);
	ack(e, 4);
	ack(e, 5);
	ack(e, 6);
	expected = out;
	ck_assert(teder_request_poll(e, &out) == TEDER_AGAIN && !memcmp(&out, &expected, sizeof(out)));
	ck_assert(teder_request_get(e, 1, &out) == TEDER_NOT_FOUND);
	ck_assert(teder_request_cancel(e, 0) == TEDER_NOT_FOUND);
	ck_assert(teder_request_ack(e, 4) == TEDER_NOT_FOUND);
	finish(&e, &t, &ctx);
	th_gate_uninit(&start);
}
END_TEST

START_TEST(cancellation)
{
	for (unsigned committed = 0; committed < 2; committed++) {
		th_tracker t;
		ck_assert(th_tracker_init(&t));
		th_gate gate;
		ck_assert(th_gate_init(&gate));
		context ctx = {.before = committed ? NULL : &gate, .after = committed ? &gate : NULL};
		teder_engine *e = create(&t, &ctx);
		uint64_t first, second, third;
		OK(submit(e, NULL, 0, &first));
		th_gate_await(&gate);
		OK(submit(e, NULL, 0, &second));
		OK(submit(e, NULL, 0, &third));
		teder_code code;
		RETRY(code, teder_request_cancel(e, first));
		OK(code);
		OK(teder_request_close(e));
		OK(teder_request_close(e));
		teder_status s = INIT(teder_status);
		OK(th_read_status(e, &s));
		ck_assert(s.lifecycle == TEDER_CLOSING);
		teder_engine *saved = e;
		ck_assert(teder_destroy(&e) == TEDER_BUSY && e == saved);
		uint64_t rejected = 99;
		ck_assert(submit(e, NULL, 0, &rejected) == TEDER_CLOSED && rejected == 0);
		teder_request_info out = INIT(teder_request_info);
		RETRY(code, teder_request_get(e, first, &out));
		OK(code);
		ck_assert(out.state == TEDER_RUNNING);
		ck_assert(teder_request_ack(e, first) == TEDER_BUSY);
		th_gate_release(&gate);
		OK(th_close(e));
		ck_assert(terminal(e, first).outcome == (committed ? TEDER_OK : TEDER_CANCELLED));
		ck_assert(terminal(e, second).outcome == TEDER_CANCELLED && ctx.calls == 1);
		ck_assert(terminal(e, third).outcome == TEDER_CANCELLED);
		ack(e, first);
		ack(e, second);
		ack(e, third);
		finish(&e, &t, &ctx);
		th_gate_uninit(&gate);
	}
}
END_TEST

START_TEST(results)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	context ctx = {.result = true};
	teder_engine *e = create(&t, &ctx);
	size_t baseline = t.bytes;
	uint64_t id;
	OK(submit(e, NULL, 0, &id));
	teder_request_info out = terminal(e, id);
	ck_assert(out.outcome == TEDER_OK && out.result_kind == TEDER_RESULT_PROBE && t.live == 3);
	teder_result *r = NULL;
	teder_code code;
	RETRY(code, teder_request_take_result(e, id, &r));
	OK(code);
	ck_assert(e->owned_bytes == baseline);
	teder_result *again = r;
	RETRY(code, teder_request_take_result(e, id, &again));
	ck_assert(code == TEDER_NOT_FOUND && !again);
	ck_assert(terminal(e, id).result_kind == TEDER_RESULT_PROBE);
	ack(e, id);
	OK(submit(e, NULL, 0, &id));
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	ck_assert(t.live == 3 &&
	          e->owned_bytes == baseline); /* detached result plus engine and snapshot arenas */
	t.fail_at = t.attempts + 1;
	OK(submit(e, NULL, 0, &id));
	ck_assert(terminal(e, id).outcome == TEDER_NO_MEMORY && e->owned_bytes == baseline);
	ack(e, id);
	t.fail_at = 0;
	e->config.engine_bytes_limit = baseline;
	size_t attempts = t.attempts;
	OK(submit(e, NULL, 0, &id));
	ck_assert(terminal(e, id).outcome == TEDER_LIMIT);
	ack(e, id);
	ck_assert(t.attempts == attempts && e->owned_bytes == baseline);
	e->config.engine_bytes_limit = UINT64_C(64) * 1024 * 1024;
	th_gate gate;
	ck_assert(th_gate_init(&gate));
	ctx.before = &gate;
	OK(submit(e, NULL, 0, &id));
	th_gate_await(&gate);
	ck_assert(t.live == 4);
	RETRY(code, teder_request_cancel(e, id));
	OK(code);
	th_gate_release(&gate);
	ck_assert(terminal(e, id).outcome == TEDER_CANCELLED && t.live == 3 &&
	          e->owned_bytes == baseline);
	ack(e, id);
	OK(th_close(e));
	OK(teder_destroy(&e));
	ck_assert(t.live == 1);
	teder_probe_info probe = INIT(teder_probe_info);
	OK(teder_result_probe(r, &probe));
	ck_assert(probe.sample_rate == 48000);
	teder_result_release(r);
	ck_assert(th_tracker_clean(&t));
	th_tracker_uninit(&t);
	th_gate_uninit(&gate);
	ck_assert(atomic_load(&ctx.starts) == 1 && atomic_load(&ctx.stops) == 1);
}
END_TEST

START_TEST(events)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	context ctx = {0};
	teder_engine *e = create(&t, &ctx);
	uint64_t id;
	OK(submit(e, NULL, 0, &id));
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	teder_event event = INIT(teder_event);
	event.kind = TEDER_EVENT_UNDERRUN;
	for (unsigned i = 1; i <= 3; i++) {
		event.missing_frames = i * 10;
		OK(td_emit(e, &event));
	}
	teder_status status = INIT(teder_status);
	OK(th_read_status(e, &status));
	ck_assert(status.event_sequence == 3 && status.events_dropped == 1);
	teder_event out = INIT(teder_event);
	teder_code code;
	RETRY(code, teder_event_poll(e, &out));
	OK(code);
	ck_assert(out.sequence == 2 && out.missing_frames == 20);
	RETRY(code, teder_event_poll(e, &out));
	OK(code);
	ck_assert(out.sequence == 3 && out.missing_frames == 30);
	ck_assert(terminal(e, id).outcome == TEDER_OK); /* telemetry cannot evict outcomes */
	td_lock(e);
	e->status.events_dropped = UINT64_MAX;
	td_unlock(e);
	for (unsigned i = 0; i < 3; i++)
		OK(td_emit(e, &event));
	OK(th_read_status(e, &status));
	ck_assert(status.events_dropped == UINT64_MAX);
	td_lock(e);
	e->status.event_sequence = UINT64_MAX;
	td_unlock(e);
	ck_assert(td_emit(e, &event) == TEDER_LIMIT);
	OK(th_close(e));
	OK(teder_get_status(e, &status));
	ck_assert(status.last_error == TEDER_LIMIT && status.event_sequence == UINT64_MAX);
	ack(e, id);
	finish(&e, &t, &ctx);
	ck_assert(th_tracker_init(&t));
	ctx = (context){0};
	e = create(&t, &ctx);
	td_lock(e);
	e->last_request = UINT64_MAX - 1;
	td_unlock(e);
	OK(submit(e, NULL, 0, &id));
	ck_assert(id == UINT64_MAX);
	terminal(e, id);
	uint64_t rejected = 7;
	ck_assert(submit(e, NULL, 0, &rejected) == TEDER_LIMIT && !rejected);
	OK(th_close(e));
	OK(teder_get_status(e, &status));
	ck_assert(status.last_error == TEDER_LIMIT);
	finish(&e, &t, &ctx);
}
END_TEST

START_TEST(initialization)
{
	for (unsigned stage = TD_INIT_LOCK; stage <= TD_INIT_PUBLISH; stage++) {
		th_tracker t;
		ck_assert(th_tracker_init(&t));
		context ctx = {.fail_stage = stage};
		teder_config c = config();
		td_runtime runtime = {
		    .context = &ctx, .permit = permit, .checkpoint = checkpoint, .execute = execute};
		teder_engine *e = NULL;
		ck_assert(td_engine_create_with(&c, th_allocator(&t), runtime, &e) == TEDER_INTERNAL && !e);
		unsigned expected = stage == TD_INIT_PUBLISH ? 1 : 0;
		ck_assert(atomic_load(&ctx.starts) == expected && atomic_load(&ctx.stops) == expected);
		ck_assert(t.attempts == 2 && th_tracker_clean(&t));
		printf("init stage=%u started=%u joined=%u live=%zu bytes=%zu\n",
		       stage,
		       expected,
		       expected,
		       t.live,
		       t.bytes);
		th_tracker_uninit(&t);
	}
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	th_gate start;
	ck_assert(th_gate_init(&start));
	context ctx = {.startup = &start};
	teder_engine *e = create(&t, &ctx);
	th_gate_await(&start);
	uint64_t id;
	OK(submit(e, NULL, 0, &id));
	OK(teder_request_close(e));
	ck_assert(teder_destroy(&e) == TEDER_BUSY);
	th_gate_release(&start);
	OK(th_close(e));
	ck_assert(terminal(e, id).outcome == TEDER_CANCELLED && !ctx.calls);
	finish(&e, &t, &ctx);
	th_gate_uninit(&start);
	for (unsigned i = 0; i < 32; i++) {
		ck_assert(th_tracker_init(&t));
		ctx = (context){0};
		e = create(&t, &ctx);
		finish(&e, &t, &ctx);
	}
	printf("worker baseline=0 after-destroy=0; 32 immediate-close/join cycles\n");
}
END_TEST

typedef struct {
	teder_engine *engine;
	th_gate gate;
} held;

static void hold(void *p)
{
	held *h = p;
	td_lock(h->engine);
	th_gate_hit(&h->gate);
	td_unlock(h->engine);
}

START_TEST(contention)
{
	th_tracker t;
	ck_assert(th_tracker_init(&t));
	context ctx = {0};
	teder_engine *e = create(&t, &ctx);
	uint64_t id = 99;
	ck_assert(td_submit(e, TEDER_OP_PROBE, NULL, 1, &id) == TEDER_INVALID && !id);
	ck_assert(td_submit(e, TEDER_OP_PROBE, "x", 9, &id) == TEDER_LIMIT && !id);
	teder_request_info out = INIT(teder_request_info);
	out.abi_version++;
	ck_assert(teder_request_get(e, 1, &out) == TEDER_ABI_MISMATCH);
	ck_assert(teder_request_take_result(NULL, 1, NULL) == TEDER_INVALID);
	held h = {.engine = e};
	ck_assert(th_gate_init(&h.gate));
	th_thread thread;
	ck_assert(th_thread_start(&thread, hold, &h));
	th_gate_await(&h.gate);
	out = (teder_request_info)INIT(teder_request_info);
	teder_request_info before = out;
	ck_assert(teder_request_get(e, 1, &out) == TEDER_AGAIN && !memcmp(&out, &before, sizeof(out)));
	ck_assert(td_submit(e, TEDER_OP_PROBE, NULL, 0, &id) == TEDER_BUSY && !id);
	ck_assert(teder_request_cancel(e, 1) == TEDER_BUSY);
	ck_assert(teder_request_ack(e, 1) == TEDER_BUSY);
	teder_result *r = (teder_result *)e;
	ck_assert(teder_request_take_result(e, 1, &r) == TEDER_BUSY && !r);
	teder_status status = INIT(teder_status), saved = status;
	ck_assert(teder_get_status(e, &status) == TEDER_AGAIN &&
	          !memcmp(&status, &saved, sizeof(status)));
	OK(teder_request_close(e));
	ck_assert(teder_destroy(&e) == TEDER_BUSY);
	th_gate_release(&h.gate);
	th_thread_join(&thread);
	th_gate_uninit(&h.gate);
	finish(&e, &t, &ctx);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "admission"))
		test = admission;
	else if (!strcmp(argv[1], "cancellation"))
		test = cancellation;
	else if (!strcmp(argv[1], "results"))
		test = results;
	else if (!strcmp(argv[1], "events"))
		test = events;
	else if (!strcmp(argv[1], "initialization"))
		test = initialization;
	else if (!strcmp(argv[1], "contention"))
		test = contention;
	else
		return 2;
	Suite *suite = suite_create("requests");
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
