#include "support/harness.h"
#include "support/engine.h"
#include "source.h"
#include "fixtures/reference.h"
#include <check.h>
#include <stdio.h>

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert((call) == TEDER_OK)
#define RETRY(code, call) \
	do { \
		(code) = (call); \
		if ((code) == TEDER_BUSY || (code) == TEDER_AGAIN) \
			th_yield(); \
	} while ((code) == TEDER_BUSY || (code) == TEDER_AGAIN)
#define A FIXTURE_DIR "/track-a.flac"
#define B FIXTURE_DIR "/track-b.flac"

typedef struct {
	th_tracker tracker;
	th_gate gate, consumer, publication;
	atomic_uint point, request, hit, starts, stops, joins, wait_join, publication_point;
	atomic_uint opened[32], closed[32], reads[32], idle;
	uint32_t fail_stage;
} context;

static void checkpoint(void *p, uint64_t request, unsigned point)
{
	context *c = p;
	if (point == atomic_load(&c->publication_point))
		th_gate_hit(&c->publication);
	if (point == TD_PREPARE_STARTED)
		atomic_fetch_add(&c->starts, 1);
	if (point == TD_PREPARE_STOPPED)
		atomic_fetch_add(&c->stops, 1);
	if (point == TD_DECODE_IDLE)
		atomic_fetch_add(&c->idle, 1);
	if (request < 32) {
		if (point == TD_SOURCE_OPENED)
			atomic_fetch_add(&c->opened[request], 1);
		if (point == TD_SOURCE_CLOSED)
			atomic_fetch_add(&c->closed[request], 1);
		if (point == TD_SOURCE_READ)
			atomic_fetch_add(&c->reads[request], 1);
	}
	unsigned expected = 0;
	if (point == atomic_load(&c->point) && request == atomic_load(&c->request) &&
	    atomic_compare_exchange_strong(&c->hit, &expected, 1))
		th_gate_hit(&c->gate);
}

static void consumer_join(void *p)
{
	context *c = p;
	atomic_fetch_add(&c->joins, 1);
	if (atomic_load(&c->wait_join))
		th_gate_hit(&c->consumer);
}

static bool permit(void *p, uint32_t stage)
{
	return ((context *)p)->fail_stage != stage;
}

static void init(context *c)
{
	memset(c, 0, sizeof(*c));
	atomic_init(&c->point, 0);
	atomic_init(&c->publication_point, 0);
	atomic_init(&c->request, 0);
	atomic_init(&c->hit, 0);
	atomic_init(&c->starts, 0);
	atomic_init(&c->stops, 0);
	atomic_init(&c->joins, 0);
	atomic_init(&c->wait_join, 0);
	atomic_init(&c->idle, 0);
	for (unsigned i = 0; i < 32; i++) {
		atomic_init(&c->opened[i], 0);
		atomic_init(&c->closed[i], 0);
		atomic_init(&c->reads[i], 0);
	}
	ck_assert(th_tracker_init(&c->tracker));
	ck_assert(th_gate_init(&c->gate) && th_gate_init(&c->consumer) &&
	          th_gate_init(&c->publication));
}

static void arm(context *c, uint32_t point, uint32_t request)
{
	atomic_store(&c->point, 0);
	atomic_store(&c->request, request);
	atomic_store(&c->hit, 0);
	atomic_store(&c->point, point);
}

static teder_engine *create(context *c, bool external, uint32_t frames, uint64_t limit)
{
	teder_config config = INIT(teder_config);
	config.pcm_frames = frames;
	config.request_capacity = 2;
	config.engine_bytes_limit = limit;
	td_runtime runtime = {.context = c,
	                      .permit = permit,
	                      .media_checkpoint = checkpoint,
	                      .consumer_join = external ? consumer_join : NULL};
	teder_engine *e = NULL;
	OK(td_engine_create_with(&config, th_allocator(&c->tracker), runtime, &e));
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

static void ack(teder_engine *e, uint64_t id)
{
	teder_code code;
	RETRY(code, teder_request_ack(e, id));
	OK(code);
}

static uint64_t open_path(teder_engine *e, const char *path, uint64_t track)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_open(e, path, (uint32_t)strlen(path), track, 0, &id));
	OK(code);
	return id;
}

static uint64_t seek_to(teder_engine *e, teder_target target, uint64_t frame)
{
	uint64_t id;
	teder_code code;
	RETRY(code, teder_seek(e, &target, frame, &id));
	OK(code);
	return id;
}

static teder_status status(teder_engine *e)
{
	teder_status s = INIT(teder_status);
	OK(th_read_status(e, &s));
	return s;
}

static td_pcm_view peek(teder_engine *e, uint64_t epoch)
{
	OK(td_pcm_select(&e->pcm, epoch));
	td_pcm_view v;
	while (td_pcm_peek(&e->pcm, &v) == TEDER_AGAIN) {
		td_signal(e);
		th_yield();
	}
	return v;
}

static void samples(teder_engine *e,
                    teder_target target,
                    uint64_t track,
                    const int32_t *reference,
                    uint32_t start,
                    uint32_t count)
{
	uint32_t received = 0;
	while (received < count) {
		td_pcm_view v = peek(e, target.epoch);
		ck_assert(v.generation->source == target.source && v.generation->track == track);
		ck_assert(v.meta->source_start + v.offset == start + received &&
		          v.meta->output_start + v.offset == received);
		ck_assert(v.frames <= count - received);
		for (uint32_t i = 0; i < v.frames * 2; i++)
			ck_assert(v.samples[i] == reference[(start + received) * 2 + i] / 32768.0f);
		received += v.frames;
		ck_assert((v.meta->flags & TD_PCM_FINAL) == (received == count ? TD_PCM_FINAL : 0));
		OK(td_pcm_consume(&e->pcm, v.frames));
		td_signal(e);
	}
}

static void finish(context *c, teder_engine **e)
{
	OK(th_close(*e));
	OK(teder_destroy(e));
	ck_assert(atomic_load(&c->starts) == 1 && atomic_load(&c->stops) == 1);
	for (unsigned i = 1; i < 32; i++)
		ck_assert(atomic_load(&c->opened[i]) == atomic_load(&c->closed[i]));
	ck_assert(th_tracker_clean(&c->tracker));
	th_tracker_uninit(&c->tracker);
	th_gate_uninit(&c->consumer);
	th_gate_uninit(&c->publication);
	th_gate_uninit(&c->gate);
}

START_TEST(loading)
{
	context c;
	init(&c);
	teder_engine *e = create(&c, true, 4, 0);
	char path[] = A;
	uint64_t id = open_path(e, path, 77);
	memset(path, 'x', sizeof(path) - 1);
	teder_request_info r = terminal(e, id);
	ck_assert(r.outcome == TEDER_OK && r.committed.epoch == 2 && r.committed.source == 1 &&
	          r.track == 77);
	teder_status s = status(e);
	ck_assert(s.transport == TEDER_READY && s.source_rate == 44100 && s.source_channels == 2);
	ck_assert(s.duration_frames == 16 && s.clock_flags == TEDER_DURATION_KNOWN && !s.media_request);
	ck_assert(!s.output_rate && !s.output_channels && !s.source_frame && !s.output_frame &&
	          !s.output_state);
	ack(e, id);
	samples(e, s.current, 77, track_a, 0, 16);
	while (!(status(e).clock_flags & TEDER_DECODE_EOF))
		th_yield();
	uint64_t rejected = 99;
	ck_assert(teder_open(e, "relative", 8, 0, 0, &rejected) == TEDER_INVALID && !rejected);
	ck_assert(teder_open(e, A, sizeof(A) - 1, 0, 2, &rejected) == TEDER_INVALID && !rejected);
	ck_assert(teder_open(e, A, sizeof(A) - 1, 0, 0, NULL) == TEDER_INVALID);
	teder_code code;
	RETRY(code, teder_open(e, A, sizeof(A) - 1, 1, TEDER_OPEN_AUTOPLAY, &id));
	OK(code);
	ck_assert(terminal(e, id).outcome == TEDER_BAD_STATE);
	ack(e, id);
	id = open_path(e, FIXTURE_DIR "/missing.flac", 2);
	ck_assert(terminal(e, id).outcome == TEDER_IO);
	ack(e, id);
	s = status(e);
	ck_assert(s.current.source == 1 && s.current.epoch == 2 && s.transport == TEDER_READY &&
	          s.track == 77);
	id = seek_to(e, s.current, 7);
	r = terminal(e, id);
	ck_assert(r.outcome == TEDER_OK && r.committed.source == 1 && r.committed.epoch == 3);
	ack(e, id);
	samples(e, r.committed, 77, track_a, 7, 9);
	id = seek_to(e, r.committed, 17);
	ck_assert(terminal(e, id).outcome == TEDER_INVALID);
	ack(e, id);
	id = seek_to(e, r.committed, 16);
	r = terminal(e, id);
	ck_assert(r.outcome == TEDER_OK && r.committed.epoch == 4);
	ack(e, id);
	s = status(e);
	ck_assert(s.source_frame == 16 && s.transport == TEDER_ENDED &&
	          s.clock_flags == (TEDER_DURATION_KNOWN | TEDER_DECODE_EOF | TEDER_SUBMIT_EOF));
	teder_target stale = {2, 1};
	RETRY(code, teder_seek(e, &stale, 0, &rejected));
	ck_assert(code == TEDER_STALE && !rejected);
	id = open_path(e, B, 77);
	r = terminal(e, id);
	ck_assert(r.outcome == TEDER_OK && r.committed.source == 2 && r.committed.epoch == 5);
	ack(e, id);
	samples(e, r.committed, 77, track_b, 0, 16);
	finish(&c, &e);
}
END_TEST

START_TEST(cancellation)
{
	for (unsigned after = 0; after < 2; after++) {
		context c;
		init(&c);
		teder_engine *e = create(&c, true, 4, 0);
		arm(&c, after ? TD_MEDIA_AFTER_COMMIT : TD_MEDIA_BEFORE_COMMIT, 1);
		uint64_t id = open_path(e, A, 44);
		th_gate_await(&c.gate);
		teder_code code;
		RETRY(code, teder_request_cancel(e, id));
		OK(code);
		th_gate_release(&c.gate);
		teder_request_info r = terminal(e, id);
		ck_assert(r.outcome == (after ? TEDER_OK : TEDER_CANCELLED));
		ack(e, id);
		if (after) {
			/* Reuse the acknowledged slot; its cancellation cannot reach the old source. */
			arm(&c, TD_SOURCE_READ, 2);
			id = open_path(e, B, 55);
			th_gate_await(&c.gate);
			RETRY(code, teder_request_cancel(e, id));
			OK(code);
			th_gate_release(&c.gate);
			ck_assert(terminal(e, id).outcome == TEDER_CANCELLED);
			ack(e, id);
			samples(e, r.committed, 44, track_a, 0, 16);
		} else
			ck_assert(status(e).transport == TEDER_EMPTY && !status(e).current.source);
		finish(&c, &e);
	}
	context c;
	init(&c);
	teder_engine *e = create(&c, true, 4, 0);
	uint64_t id = open_path(e, A, 3);
	teder_request_info r = terminal(e, id);
	ack(e, id);
	arm(&c, TD_MEDIA_BEFORE_COMMIT, 2);
	id = seek_to(e, r.committed, 7);
	th_gate_await(&c.gate);
	teder_code code;
	RETRY(code, teder_request_cancel(e, id));
	OK(code);
	th_gate_release(&c.gate);
	ck_assert(terminal(e, id).outcome == TEDER_CANCELLED);
	ack(e, id);
	ck_assert(status(e).current.epoch == 2);
	samples(e, r.committed, 3, track_a, 0, 16);
	finish(&c, &e);
}
END_TEST

START_TEST(progress)
{
	context c;
	init(&c);
	teder_engine *e = create(&c, true, 4, 0);
	uint64_t id = open_path(e, A, 4);
	teder_request_info old = terminal(e, id);
	ack(e, id);
	arm(&c, TD_SOURCE_READ, 2);
	id = open_path(e, B, 5);
	th_gate_await(&c.gate);
	teder_status s = status(e);
	ck_assert(s.media_request == id && s.current.source == 1 && s.transport == TEDER_READY);
	uint64_t rejected = 9;
	ck_assert(teder_open(e, A, sizeof(A) - 1, 0, 0, &rejected) == TEDER_BUSY && !rejected);
	samples(e, old.committed, 4, track_a, 0, 16); /* blocked preparer cannot starve decoding */
	th_gate_release(&c.gate);
	teder_request_info next = terminal(e, id);
	ck_assert(next.outcome == TEDER_OK);
	ack(e, id);
	arm(&c, TD_DECODE_IDLE, 0);
	td_signal(e);
	th_gate_await(&c.gate);
	unsigned reads = atomic_load(&c.reads[2]);
	ma_mutex_lock(&c.tracker.lock);
	size_t attempts = c.tracker.attempts;
	ma_mutex_unlock(&c.tracker.lock);
	ck_assert(e->pcm.free_count == 0); /* producer is held at its wait boundary */
	th_gate_release(&c.gate);
	for (unsigned i = 0; i < 32; i++) {
		s = status(e);
		ck_assert(s.current.source == 2);
	}
	ck_assert(atomic_load(&c.reads[2]) == reads);
	ma_mutex_lock(&c.tracker.lock);
	ck_assert(c.tracker.attempts == attempts);
	ma_mutex_unlock(&c.tracker.lock);
	samples(e, next.committed, 5, track_b, 0, 16);
	finish(&c, &e);
}
END_TEST

START_TEST(retirement)
{
	context c;
	init(&c);
	teder_engine *e = create(&c, true, 2048, 0);
	size_t baseline = e->owned_bytes, reservation;
	OK(td_source_plan(sizeof(A) - 1, 8 * 1024 * 1024, &reservation));
	uint64_t id = open_path(e, A, 9);
	teder_request_info r = terminal(e, id);
	ack(e, id);
	td_pcm_view v = peek(e, r.committed.epoch);
	const float *borrow = v.samples;
	id = open_path(e, B, 10);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	while (!(status(e).clock_flags & TEDER_DECODE_EOF))
		th_yield();
	id = open_path(e, A, 11);
	ck_assert(terminal(e, id).outcome == TEDER_OK);
	ack(e, id);
	uint64_t rejected = 99;
	ck_assert(teder_open(e, B, sizeof(B) - 1, 12, 0, &rejected) == TEDER_BUSY && !rejected);
	td_lock(e);
	ck_assert(e->owned_bytes == baseline + 3 * reservation);
	td_unlock(e);
	/* Two retired generations still own their borrowed/queued PCM. */
	ck_assert(v.generation->source == 1 && v.generation->track == 9 &&
	          borrow[0] == -8000 / 32768.0f);
	atomic_store(&c.wait_join, 1);
	OK(teder_request_close(e));
	th_gate_await(&c.consumer);
	ck_assert(status(e).lifecycle == TEDER_CLOSING);
	ck_assert(teder_destroy(&e) == TEDER_BUSY);
	ck_assert(borrow[0] == -8000 / 32768.0f && v.meta->frames == 16);
	/* End the borrow before allowing the consumer-quiescence handshake to finish. */
	OK(td_pcm_consume(&e->pcm, v.frames));
	th_gate_release(&c.consumer);
	OK(th_close(e));
	ck_assert(e->owned_bytes == baseline);
	finish(&c, &e);
	ck_assert(atomic_load(&c.joins) == 1);
}
END_TEST

START_TEST(failures)
{
	for (unsigned seeking = 0; seeking < 2; seeking++) {
		size_t count = 0;
		for (size_t n = 0; n <= count; n++) {
			context c;
			init(&c);
			teder_engine *e = create(&c, false, 32, 0);
			teder_target target = {0};
			if (seeking) {
				uint64_t first = open_path(e, A, 1);
				teder_request_info r = terminal(e, first);
				OK(r.outcome);
				target = r.committed;
				ack(e, first);
				while (!(status(e).clock_flags & TEDER_DECODE_EOF))
					th_yield();
			}
			ma_mutex_lock(&c.tracker.lock);
			size_t before = c.tracker.attempts;
			c.tracker.fail_at = n ? before + n : 0;
			ma_mutex_unlock(&c.tracker.lock);
			uint64_t id = seeking ? seek_to(e, target, 7) : open_path(e, A, 1);
			teder_request_info r = terminal(e, id);
			if (r.outcome == TEDER_OK) {
				while (status(e).transport != TEDER_FAILED &&
				       !(status(e).clock_flags & TEDER_DECODE_EOF))
					th_yield();
				ck_assert(status(e).last_error == (n ? TEDER_NO_MEMORY : TEDER_OK));
			} else
				ck_assert(r.outcome == TEDER_NO_MEMORY);
			if (n && seeking && r.outcome != TEDER_OK) {
				teder_status s = status(e);
				ck_assert(s.current.epoch == 2 && s.current.source == 1 &&
				          s.transport == TEDER_READY);
			}
			if (!n) {
				ma_mutex_lock(&c.tracker.lock);
				count = c.tracker.attempts - before;
				ma_mutex_unlock(&c.tracker.lock);
				ck_assert(count > 5);
			}
			ack(e, id);
			finish(&c, &e);
		}
		printf("asynchronous %s fail-at-1..%u: zero retained resources\n",
		       seeking ? "seek" : "open",
		       (unsigned)count);
	}
	context c;
	init(&c);
	teder_engine *e = create(&c, false, 4, 0);
	td_lock(e);
	e->config.engine_bytes_limit = e->owned_bytes;
	td_unlock(e);
	uint64_t id = open_path(e, A, 1);
	ck_assert(terminal(e, id).outcome == TEDER_LIMIT);
	ack(e, id);
	ck_assert(c.tracker.attempts == 1 && !atomic_load(&c.opened[1]));
	finish(&c, &e);
	const unsigned stages[] = {
	    TD_INIT_PREPARE_WAKE, TD_INIT_PREPARE_THREAD, TD_INIT_THREAD, TD_INIT_PUBLISH};
	for (unsigned i = 0; i < 4; i++) {
		unsigned stage = stages[i];
		init(&c);
		c.fail_stage = stage;
		teder_config config = INIT(teder_config);
		td_runtime runtime = {.context = &c, .permit = permit, .media_checkpoint = checkpoint};
		ck_assert(td_engine_create_with(&config, th_allocator(&c.tracker), runtime, &e) ==
		              TEDER_INTERNAL &&
		          !e);
		unsigned started = stage == TD_INIT_THREAD || stage == TD_INIT_PUBLISH ? 1 : 0;
		ck_assert(atomic_load(&c.starts) == started && atomic_load(&c.stops) == started &&
		          th_tracker_clean(&c.tracker));
		th_tracker_uninit(&c.tracker);
		th_gate_uninit(&c.gate);
		th_gate_uninit(&c.consumer);
		th_gate_uninit(&c.publication);
	}
}
END_TEST

START_TEST(identity)
{
	context c;
	init(&c);
	teder_engine *e = create(&c, false, 32, 0);
	arm(&c, TD_MEDIA_BEFORE_COMMIT, 1);
	uint64_t id = open_path(e, A, 1);
	th_gate_await(&c.gate);
	/* Model an independent stream mutation while preparation is outside the gate. */
	td_lock(e);
	e->status.current.epoch++;
	td_unlock(e);
	th_gate_release(&c.gate);
	teder_request_info r = terminal(e, id);
	ck_assert(r.outcome == TEDER_STALE && !r.committed.source && !r.committed.epoch);
	ack(e, id);
	ck_assert(status(e).transport == TEDER_EMPTY && status(e).current.epoch == 2);
	id = open_path(e, A, 2);
	r = terminal(e, id);
	ck_assert(r.outcome == TEDER_OK && r.committed.epoch == 3 && r.committed.source == 1);
	ack(e, id);
	while (!(status(e).clock_flags & TEDER_DECODE_EOF))
		th_yield();
	FILE *input = fopen(A, "rb"), *output = fopen(OUTPUT_DIR "/decode-corrupt.flac", "wb");
	ck_assert(input && output);
	unsigned char data[4096];
	size_t bytes = fread(data, 1, sizeof(data), input);
	ck_assert(bytes > 4 && feof(input));
	ck_assert(!fclose(input));
	data[bytes - 1] ^= 1;
	ck_assert(fwrite(data, 1, bytes, output) == bytes && !fclose(output));
	id = open_path(e, OUTPUT_DIR "/decode-corrupt.flac", 3);
	ck_assert(terminal(e, id).outcome == TEDER_CORRUPT);
	ack(e, id);
	id = open_path(e, FIXTURE_DIR "/track-a.pcm", 3);
	ck_assert(terminal(e, id).outcome == TEDER_UNSUPPORTED);
	ack(e, id);
	teder_status saved = status(e);
	ck_assert(saved.current.epoch == 3 && saved.current.source == 1 && saved.track == 2 &&
	          saved.transport == TEDER_READY);
	data[bytes - 1] ^= 1;
	data[21] &= 0xf0;
	memset(data + 22, 0, 4);
	output = fopen(OUTPUT_DIR "/decode-unknown.flac", "wb");
	ck_assert(output && fwrite(data, 1, bytes, output) == bytes && !fclose(output));
	arm(&c, TD_MEDIA_AFTER_COMMIT, (uint32_t)id + 1);
	id = open_path(e, OUTPUT_DIR "/decode-unknown.flac", 5);
	th_gate_await(&c.gate);
	saved = status(e);
	ck_assert(saved.duration_frames == 0 && saved.clock_flags == 0);
	th_gate_release(&c.gate);
	OK(terminal(e, id).outcome);
	ack(e, id);
	while (!(status(e).clock_flags & TEDER_DECODE_EOF))
		th_yield();
	saved = status(e);
	ck_assert(saved.duration_frames == 16 && (saved.clock_flags & TEDER_DURATION_KNOWN));
	ma_mutex_lock(&c.tracker.lock);
	size_t attempts = c.tracker.attempts;
	ma_mutex_unlock(&c.tracker.lock);
	size_t reservation;
	OK(td_source_plan(sizeof(A) - 1, 8 * 1024 * 1024, &reservation));
	td_lock(e);
	e->config.engine_bytes_limit = e->owned_bytes + reservation - 1;
	td_unlock(e);
	id = open_path(e, A, 4);
	ck_assert(terminal(e, id).outcome == TEDER_LIMIT);
	ack(e, id);
	ma_mutex_lock(&c.tracker.lock);
	ck_assert(c.tracker.attempts == attempts);
	ma_mutex_unlock(&c.tracker.lock);
	ck_assert(status(e).current.epoch == saved.current.epoch && !atomic_load(&c.opened[id]));
	finish(&c, &e);
	for (unsigned which = 0; which < 2; which++) {
		init(&c);
		e = create(&c, false, 4, 0);
		td_lock(e);
		if (which)
			e->last_source = UINT64_MAX;
		else
			e->status.current.epoch = UINT64_MAX;
		td_unlock(e);
		id = open_path(e, A, 1);
		r = terminal(e, id);
		ck_assert(r.outcome == TEDER_LIMIT && !r.committed.source);
		OK(th_close(e));
		ck_assert(status(e).last_error == TEDER_LIMIT && !atomic_load(&c.opened[id]));
		finish(&c, &e);
	}
}
END_TEST

START_TEST(closing)
{
	const unsigned points[] = {TD_SOURCE_OPENED, TD_SOURCE_READ, TD_MEDIA_BEFORE_COMMIT};
	for (unsigned i = 0; i < 3; i++) {
		context c;
		init(&c);
		arm(&c, points[i], 1);
		teder_engine *e = create(&c, false, 4, 0);
		uint64_t id = open_path(e, A, 1);
		th_gate_await(&c.gate);
		OK(teder_request_close(e));
		ck_assert(status(e).lifecycle == TEDER_CLOSING && teder_destroy(&e) == TEDER_BUSY);
		th_gate_release(&c.gate);
		ck_assert(terminal(e, id).outcome == TEDER_CANCELLED);
		finish(&c, &e);
	}
}
END_TEST

static uint64_t prepare(teder_engine *e, const char *path, uint64_t track)
{
	teder_status s = status(e);
	uint64_t id;
	teder_code code;
	RETRY(
	    code,
	    teder_prepare_next(e, &s.current, s.next_source, path, (uint32_t)strlen(path), track, &id));
	OK(code);
	return id;
}

static uint64_t clear(teder_engine *e)
{
	teder_status s = status(e);
	uint64_t id;
	teder_code code;
	RETRY(code, teder_clear_next(e, &s.current, s.next_source, &id));
	OK(code);
	return id;
}

static void done(teder_engine *e, uint64_t id, teder_code expected)
{
	ck_assert_msg(
	    terminal(e, id).outcome == expected, "request %u expected %d", (unsigned)id, expected);
	ack(e, id);
}

static teder_engine *with_next(context *c, bool external)
{
	init(c);
	teder_engine *e = create(c, external, 32, 0);
	done(e, open_path(e, A, 77), TEDER_OK);
	done(e, prepare(e, A, 77), TEDER_OK);
	teder_status s = status(e);
	ck_assert(s.current.epoch == 2 && s.current.source == 1 && s.track == 77 &&
	          s.next_source == 2 && s.next_track == 77 && s.next_state == TEDER_NEXT_READY);
	return e;
}

START_TEST(next_basic)
{
	context c;
	teder_engine *e = with_next(&c, true);
	teder_status original = status(e);
	uint64_t rejected = 99;
	teder_code code;
	RETRY(code, teder_prepare_next(e, &original.current, 0, A, sizeof(A) - 1, 0, &rejected));
	ck_assert(code == TEDER_STALE && !rejected);
	ck_assert(teder_prepare_next(e, NULL, 2, A, sizeof(A) - 1, 0, &rejected) == TEDER_INVALID);
	ck_assert(teder_prepare_next(e, &original.current, 2, "relative", 8, 0, &rejected) ==
	          TEDER_INVALID);
	ck_assert(teder_clear_next(e, NULL, 2, &rejected) == TEDER_INVALID && !rejected);
	size_t reservation;
	OK(td_source_plan(sizeof(A) - 1, 8 * 1024 * 1024, &reservation));
	td_lock(e);
	size_t ceiling = e->owned_bytes + reservation;
	td_unlock(e);
	for (unsigned i = 0; i < 8; i++) {
		uint64_t id = prepare(e, A, 77);
		teder_request_info r = terminal(e, id);
		ck_assert(r.outcome == TEDER_OK && r.committed.epoch == 2 && r.committed.source == 3 + i &&
		          r.track == 77);
		ack(e, id);
		teder_status s = status(e);
		ck_assert(s.next_state == TEDER_NEXT_READY && s.next_source == 3 + i &&
		          s.current.source == 1 && s.current.epoch == 2 && !s.output_frame);
		td_lock(e);
		ck_assert(e->owned_bytes <= ceiling);
		td_unlock(e);
		/* Never poll events: retained notifications cannot own source arenas. */
	}
	done(e, clear(e), TEDER_OK);
	ck_assert(status(e).next_state == TEDER_NEXT_NONE && !status(e).next_source);
	done(e, clear(e), TEDER_OK); /* zero/none is an acknowledged no-op */
	samples(e, original.current, 77, track_a, 0, 16);
	finish(&c, &e);
}
END_TEST

START_TEST(next_failures)
{
	size_t count = 0;
	for (size_t n = 0; n <= count; n++) {
		context c;
		teder_engine *e = with_next(&c, true);
		/* Hold the control/producer after all active allocation has finished. */
		arm(&c, TD_DECODE_IDLE, 0);
		td_signal(e);
		th_gate_await(&c.gate);
		ma_mutex_lock(&c.tracker.lock);
		size_t before = c.tracker.attempts;
		c.tracker.fail_at = n ? before + n : 0;
		ma_mutex_unlock(&c.tracker.lock);
		uint64_t id = prepare(e, FIXTURE_DIR "/mono-24.flac", 88);
		th_gate_release(&c.gate);
		teder_request_info r = terminal(e, id);
		ck_assert(r.outcome == (n ? TEDER_NO_MEMORY : TEDER_OK));
		if (!n) {
			ma_mutex_lock(&c.tracker.lock);
			count = c.tracker.attempts - before;
			ma_mutex_unlock(&c.tracker.lock);
			ck_assert(count > 5);
		}
		ack(e, id);
		teder_status s = status(e);
		ck_assert(s.current.source == 1 && s.current.epoch == 2 && s.track == 77 &&
		          s.next_source == (n ? 2 : 3) && s.next_track == (n ? 77 : 88));
		samples(e, s.current, 77, track_a, 0, 16);
		finish(&c, &e);
	}
	printf("successor replacement fail-at-1..%u: active and prior successor preserved\n",
	       (unsigned)count);
	context c;
	teder_engine *e = with_next(&c, true);
	done(e, prepare(e, FIXTURE_DIR "/missing.flac", 88), TEDER_IO);
	done(e, prepare(e, FIXTURE_DIR "/track-a.pcm", 88), TEDER_UNSUPPORTED);
	td_lock(e);
	e->config.engine_bytes_limit = e->owned_bytes;
	td_unlock(e);
	done(e, prepare(e, B, 88), TEDER_LIMIT);
	ck_assert(status(e).next_source == 2 && status(e).next_track == 77);
	samples(e, status(e).current, 77, track_a, 0, 16);
	finish(&c, &e);
}
END_TEST

START_TEST(next_cancellation)
{
	for (unsigned action = 0; action < 3; action++)
		for (unsigned after = 0; after < 2; after++) {
			context c;
			teder_engine *e = with_next(&c, true);
			arm(&c, after ? TD_MEDIA_AFTER_COMMIT : TD_MEDIA_BEFORE_COMMIT, 3);
			uint64_t id = action == 1 ? clear(e) : prepare(e, B, 88);
			th_gate_await(&c.gate);
			teder_code code;
			if (action == 2)
				OK(teder_request_close(e));
			else {
				RETRY(code, teder_request_cancel(e, id));
				OK(code);
			}
			th_gate_release(&c.gate);
			done(e, id, after ? TEDER_OK : TEDER_CANCELLED);
			if (action != 2) {
				teder_status s = status(e);
				ck_assert(s.next_source == (after ? (action == 1 ? 0 : 3) : 2));
				samples(e, s.current, 77, track_a, 0, 16);
			}
			finish(&c, &e);
		}
}
END_TEST

START_TEST(next_progress)
{
	context c;
	init(&c);
	teder_engine *e = create(&c, true, 4, 0);
	done(e, open_path(e, A, 77), TEDER_OK);
	arm(&c, TD_SOURCE_READ, 2);
	uint64_t id = prepare(e, B, 88);
	th_gate_await(&c.gate);
	teder_status s = status(e);
	ck_assert(s.media_request == id && s.next_state == TEDER_NEXT_NONE);
	samples(e, s.current, 77, track_a, 0, 16); /* active decode runs during blocked preparation */
	th_gate_release(&c.gate);
	done(e, id, TEDER_OK);
	ck_assert(status(e).next_source == 2);
	finish(&c, &e);
	for (unsigned action = 0; action < 3; action++) {
		e = with_next(&c, false);
		if (action == 0)
			id = seek_to(e, status(e).current, 7);
		else if (action == 1)
			id = open_path(e, B, 88);
		else {
			s = status(e);
			teder_code code;
			RETRY(code, teder_transport(e, &s.current, TEDER_STOP, &id));
			OK(code);
		}
		done(e, id, TEDER_OK);
		s = status(e);
		ck_assert(s.next_state == TEDER_NEXT_NONE && !s.next_source && !s.next_track &&
		          s.current.epoch == 3 && s.current.source == (action == 1 ? 3 : 1));
		finish(&c, &e);
	}
}
END_TEST

typedef struct {
	teder_engine *engine;
	td_source *source;
	teder_code code;
} publisher;

static void publish_next(void *p)
{
	publisher *pub = p;
	do {
		pub->code = td_source_pump(pub->source, &pub->engine->pcm);
	} while (pub->code == TEDER_AGAIN);
}

START_TEST(next_adoption)
{
	for (unsigned action = 0; action < 4; action++) {
		context c;
		teder_engine *e = with_next(&c, true);
		teder_status s = status(e);
		samples(e, s.current, 77, track_a, 0, 16);
		uint64_t id = 0;
		arm(&c, action == 1 ? TD_MEDIA_BEFORE_COMMIT : TD_DECODE_IDLE, action == 1 ? 3 : 0);
		if (action == 1)
			id = prepare(e, B, 88); /* replacement staged before old successor adoption */
		else
			td_signal(e);
		th_gate_await(&c.gate);
		/* Control has paused refill. Borrow its exclusive producer for the real
		 * source pump; production boundary scheduling is not exercised here. */
		publisher pub = {.engine = e};
		td_lock(e);
		for (unsigned i = 0; i < TD_SOURCE_JOBS; i++)
			if (e->sources[i].state == TD_JOB_NEXT)
				pub.source = e->sources[i].source;
		td_unlock(e);
		ck_assert(pub.source);
		if (action == 0)
			id = clear(e); /* admitted while READY, loses at worker commitment */
		atomic_store(&c.publication_point,
		             action == 2 ? TD_NEXT_BEFORE_PUBLISH : TD_NEXT_AFTER_PUBLISH);
		th_thread worker;
		ck_assert(th_thread_start(&worker, publish_next, &pub));
		th_gate_await(&c.publication);
		td_pcm_view held = {0};
		if (action != 2) {
			s = status(e);
			ck_assert(s.next_state == TEDER_NEXT_COMMITTED && s.next_source == 2 &&
			          s.current.source == 1 && s.current.epoch == 2);
			held = peek(e, 2);
			ck_assert(held.generation->source == 2 && held.generation->track == 77 &&
			          held.frames == 16);
			for (unsigned i = 0; i < 32; i++)
				ck_assert(held.samples[i] == track_a[i] / 32768.0f);
		}
		if (action == 0) {
			uint64_t rejected = 99;
			teder_code code;
			code = teder_clear_next(e, &s.current, 2, &rejected);
			ck_assert(code == TEDER_BUSY && !rejected);
		} else if (action >= 2) {
			atomic_store(&c.wait_join, 1);
			OK(teder_request_close(e));
			ck_assert(teder_destroy(&e) == TEDER_BUSY);
		}
		th_gate_release(&c.publication);
		th_thread_join(&worker);
		ck_assert(pub.code == (action == 2 ? TEDER_CANCELLED : TEDER_OK));
		th_gate_release(&c.gate);
		if (action <= 1)
			done(e, id, TEDER_TOO_LATE);
		if (action == 1) {
			ck_assert(atomic_load(&c.closed[3]) == 1 && !atomic_load(&c.closed[2]));
		}
		if (action == 0) {
			uint64_t rejected = 99;
			teder_code code;
			RETRY(code, teder_clear_next(e, &s.current, 2, &rejected));
			ck_assert(code == TEDER_TOO_LATE && !rejected);
			teder_target stale = {s.current.epoch - 1, s.current.source};
			RETRY(code, teder_clear_next(e, &stale, 2, &rejected));
			ck_assert(code == TEDER_STALE && !rejected);
		}
		if (action >= 2)
			th_gate_await(&c.consumer);
		if (action != 2) {
			ck_assert(!atomic_load(&c.closed[2]));
			ck_assert(held.samples[0] == track_a[0] / 32768.0f);
			OK(td_pcm_consume(&e->pcm, held.frames));
		}
		if (action >= 2)
			th_gate_release(&c.consumer);
		finish(&c, &e);
	}
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "loading"))
		test = loading;
	else if (!strcmp(argv[1], "cancellation"))
		test = cancellation;
	else if (!strcmp(argv[1], "progress"))
		test = progress;
	else if (!strcmp(argv[1], "retirement"))
		test = retirement;
	else if (!strcmp(argv[1], "failures"))
		test = failures;
	else if (!strcmp(argv[1], "closing"))
		test = closing;
	else if (!strcmp(argv[1], "identity"))
		test = identity;
	else if (!strcmp(argv[1], "next-basic"))
		test = next_basic;
	else if (!strcmp(argv[1], "next-failures"))
		test = next_failures;
	else if (!strcmp(argv[1], "next-cancellation"))
		test = next_cancellation;
	else if (!strcmp(argv[1], "next-progress"))
		test = next_progress;
	else if (!strcmp(argv[1], "next-adoption"))
		test = next_adoption;
	else
		return 2;
	Suite *suite = suite_create("decode");
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
