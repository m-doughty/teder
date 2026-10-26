#include "support/harness.h"
#include "support/engine.h"
#include "source.h"
#include <FLAC/stream_encoder.h>
#include <check.h>
#include <stdio.h>

#define INIT(type) \
	{ \
		.struct_size = sizeof(type), .abi_version = TEDER_ABI_VERSION \
	}
#define OK(call) ck_assert((call) == TEDER_OK)
#define PATH OUTPUT_DIR "/refill-long.flac"
enum { FRAMES = 32769 };

typedef struct {
	th_tracker tracker;
	th_gate native, pump;
	atomic_uint block, starts, stops, closed, joins, pumps, race;
	bool consumer;
	unsigned fail_stage, contexts, releases;
} fixture;

static bool permit(void *p, uint32_t stage)
{
	return ((fixture *)p)->fail_stage != stage;
}

static void checkpoint(void *p, uint64_t request, unsigned point)
{
	(void)request;
	fixture *f = p;
	if (point == TD_REFILL_STARTED)
		atomic_fetch_add(&f->starts, 1);
	if (point == TD_REFILL_STOPPED)
		atomic_fetch_add(&f->stops, 1);
	if (point == TD_SOURCE_CLOSED)
		atomic_fetch_add(&f->closed, 1);
	if (point == TD_REFILL_PUMP)
		atomic_fetch_add(&f->pumps, 1);
	unsigned phase = 1;
	if (point == TD_REFILL_PAUSING && atomic_compare_exchange_strong(&f->race, &phase, 2))
		atomic_store(&f->block, TD_REFILL_PARKED);
	phase = 2;
	if (point == TD_REFILL_RESUMED && atomic_compare_exchange_strong(&f->race, &phase, 3))
		th_gate_hit(&f->native);
	unsigned expected = point;
	if (atomic_compare_exchange_strong(&f->block, &expected, 0))
		th_gate_hit(&f->pump);
}

static ma_result context_init(const ma_backend *backends,
                              ma_uint32 count,
                              const ma_context_config *config,
                              ma_context *c)
{
	(void)backends;
	ck_assert(count == 4);
	memset(c, 0, sizeof(*c));
	c->backend = ma_backend_wasapi;
	c->pUserData = config->pUserData;
	((fixture *)c->pUserData)->contexts++;
	return MA_SUCCESS;
}

static ma_result context_uninit(ma_context *c)
{
	((fixture *)c->pUserData)->releases++;
	return MA_SUCCESS;
}

static ma_result enumerate(ma_context *c, ma_enum_devices_callback_proc callback, void *user)
{
	(void)callback;
	(void)user;
	th_gate_hit(&((fixture *)c->pUserData)->native);
	return MA_SUCCESS; /* a valid empty snapshot, after the native call is released */
}

static void consumer_join(void *p)
{
	atomic_fetch_add(&((fixture *)p)->joins, 1);
}

static void init(fixture *f)
{
	memset(f, 0, sizeof(*f));
	atomic_init(&f->block, 0);
	atomic_init(&f->starts, 0);
	atomic_init(&f->stops, 0);
	atomic_init(&f->closed, 0);
	atomic_init(&f->joins, 0);
	atomic_init(&f->pumps, 0);
	atomic_init(&f->race, 0);
	f->consumer = true;
	ck_assert(th_tracker_init(&f->tracker));
	ck_assert(th_gate_init(&f->native) && th_gate_init(&f->pump));
}

static teder_code create(fixture *f, teder_engine **e)
{
	teder_config c = INIT(teder_config);
	c.pcm_frames = 2048;
	td_runtime runtime = {.context = f,
	                      .permit = permit,
	                      .media_checkpoint = checkpoint,
	                      .consumer_join = f->consumer ? consumer_join : NULL,
	                      .backend = {context_init, context_uninit, enumerate, f}};
	return td_engine_create_with(&c, th_allocator(&f->tracker), runtime, e);
}

static void dispose(fixture *f)
{
	ck_assert(atomic_load(&f->starts) == atomic_load(&f->stops));
	ck_assert(f->contexts == f->releases && th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
	th_gate_uninit(&f->native);
	th_gate_uninit(&f->pump);
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

static uint64_t open_track(teder_engine *e)
{
	uint64_t id;
	teder_code code;
	do {
		code = teder_open(e, PATH, sizeof(PATH) - 1, 42, 0, &id);
		if (code == TEDER_BUSY)
			th_yield();
	} while (code == TEDER_BUSY);
	OK(code);
	return id;
}

static void make_track(void)
{
	td_flac_heap heap;
	OK(td_flac_heap_init(&heap, td_libc_allocator(), 8 * 1024 * 1024));
	td_flac_heap *previous = td_flac_scope(&heap);
	FLAC__StreamEncoder *encoder = FLAC__stream_encoder_new();
	ck_assert(encoder);
	ck_assert(FLAC__stream_encoder_set_channels(encoder, 2));
	ck_assert(FLAC__stream_encoder_set_bits_per_sample(encoder, 16));
	ck_assert(FLAC__stream_encoder_set_sample_rate(encoder, 44100));
	ck_assert(FLAC__stream_encoder_set_blocksize(encoder, 256));
	ck_assert(FLAC__stream_encoder_set_total_samples_estimate(encoder, FRAMES));
	ck_assert(FLAC__stream_encoder_init_file(encoder, PATH, NULL, NULL) ==
	          FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	int32_t samples[512];
	for (unsigned frame = 0; frame < FRAMES;) {
		unsigned count = FRAMES - frame < 256 ? FRAMES - frame : 256;
		for (unsigned i = 0; i < count; i++) {
			samples[2 * i] = (int32_t)((frame + i) % 2048) - 1024;
			samples[2 * i + 1] = 1024 - (int32_t)((frame + i) % 2048);
		}
		ck_assert(FLAC__stream_encoder_process_interleaved(encoder, samples, count));
		frame += count;
	}
	ck_assert(FLAC__stream_encoder_finish(encoder));
	FLAC__stream_encoder_delete(encoder);
	td_flac_scope(previous);
	ck_assert(heap.error == TEDER_OK && heap.live == 0 && heap.bytes == 0);
}

/* No producer wake, even on an empty queue. Native control can remain blocked
 * for this entire stream, which is sixteen times the engine's PCM capacity. */
static void drain(teder_engine *e, teder_target target)
{
	OK(td_pcm_select(&e->pcm, target.epoch));
	unsigned frame = 0, calls = 0;
	const unsigned sizes[] = {1, 31, 257, 7};
	while (frame < FRAMES) {
		td_pcm_view view;
		teder_code code;
		while ((code = td_pcm_peek(&e->pcm, &view)) == TEDER_AGAIN)
			th_yield();
		OK(code);
		ck_assert(view.generation->source == target.source && view.generation->track == 42);
		ck_assert(view.meta->source_start + view.offset == frame);
		unsigned count = sizes[calls++ % 4];
		if (count > view.frames)
			count = view.frames;
		ck_assert(frame + count <= FRAMES);
		float pcm[514];
		td_audio_result result;
		td_audio_config config = {.generation = target.epoch, .gain = 1, .enabled = 1};
		OK(td_audio_render(&e->pcm, config, pcm, 514, count, &result));
		ck_assert(result.pcm_frames == count && result.missing_frames == 0);
		for (unsigned i = 0; i < count; i++) {
			float expected = ((int32_t)((frame + i) % 2048) - 1024) / 32768.0f;
			ck_assert(pcm[2 * i] == expected && pcm[2 * i + 1] == -expected);
		}
		frame += count;
	}
	td_pcm_view unused;
	ck_assert(td_pcm_peek(&e->pcm, &unused) == TEDER_AGAIN);
}

START_TEST(native_progress)
{
	make_track();
	fixture f;
	init(&f);
	teder_engine *e = NULL;
	OK(create(&f, &e));
	teder_request_info opened = terminal(e, open_track(e));
	OK(opened.outcome);
	uint64_t id;
	teder_code code;
	do {
		code = teder_enumerate_devices(e, &id);
	} while (code == TEDER_BUSY);
	OK(code);
	th_gate_await(&f.native);
	drain(e, opened.committed);
	teder_status s = INIT(teder_status);
	do {
		OK(th_read_status(e, &s));
	} while (!(s.clock_flags & TEDER_DECODE_EOF));
	ck_assert(s.current.source == opened.committed.source && s.duration_frames == FRAMES &&
	          s.clock_flags == (TEDER_DURATION_KNOWN | TEDER_DECODE_EOF));
	/* Raw consumption does not invent a public playback clock or terminal EOF. */
	ck_assert(s.transport == TEDER_READY && s.source_frame == 0 && s.output_frame == 0);
	th_gate_release(&f.native);
	OK(terminal(e, id).outcome);
	OK(th_close(e));
	OK(teder_destroy(&e));
	ck_assert(atomic_load(&f.joins) == 1 && atomic_load(&f.closed) == 1);
	dispose(&f);
}
END_TEST

START_TEST(handoff)
{
	make_track();
	for (unsigned closing = 0; closing < 2; closing++) {
		fixture f;
		init(&f);
		teder_engine *e = NULL;
		OK(create(&f, &e));
		teder_request_info first = terminal(e, open_track(e));
		OK(first.outcome);
		atomic_store(&f.block, TD_REFILL_PUMP);
		th_gate_await(&f.pump);
		uint64_t id = 0;
		if (closing)
			OK(teder_request_close(e));
		else
			id = open_track(e);
		teder_status s = INIT(teder_status);
		OK(th_read_status(e, &s));
		ck_assert(s.current.epoch == first.committed.epoch && atomic_load(&f.closed) == 0);
		if (closing)
			ck_assert(s.lifecycle == TEDER_CLOSING && teder_destroy(&e) == TEDER_BUSY);
		else
			ck_assert(s.media_request == id);
		th_gate_release(&f.pump);
		if (!closing) {
			teder_request_info next = terminal(e, id);
			OK(next.outcome);
			ck_assert(next.committed.epoch == first.committed.epoch + 1 &&
			          next.committed.source == first.committed.source + 1);
			drain(e, next.committed);
		}
		OK(th_close(e));
		OK(teder_destroy(&e));
		ck_assert(atomic_load(&f.closed) == (closing ? 1 : 2));
		dispose(&f);
	}
}
END_TEST

START_TEST(idle)
{
	make_track();
	fixture f;
	init(&f);
	f.consumer = false;
	teder_engine *e = NULL;
	OK(create(&f, &e));
	OK(terminal(e, open_track(e)).outcome);
	atomic_store(&f.block, TD_DECODE_IDLE);
	td_signal(e);
	th_gate_await(&f.pump);
	ck_assert(e->pcm.free_count == 0 && !atomic_load(&e->refill_enabled));
	ck_assert(atomic_load(&f.pumps) == 0); /* full, ready source needs no polling */
	th_gate_release(&f.pump);
	uint64_t id;
	teder_code code;
	do {
		code = teder_enumerate_devices(e, &id);
	} while (code == TEDER_BUSY);
	OK(code);
	th_gate_await(&f.native);
	ck_assert(!atomic_load(&e->refill_enabled) && atomic_load(&f.pumps) == 0);
	th_gate_release(&f.native);
	OK(terminal(e, id).outcome);
	OK(th_close(e));
	OK(teder_destroy(&e));
	ck_assert(atomic_load(&f.starts) == 1 && atomic_load(&f.stops) == 1 &&
	          atomic_load(&f.pumps) == 0 && atomic_load(&f.joins) == 0);
	dispose(&f);
}
END_TEST

START_TEST(close_resume)
{
	make_track();
	fixture f;
	init(&f);
	teder_engine *e = NULL;
	OK(create(&f, &e));
	OK(terminal(e, open_track(e)).outcome);
	/* Arm before a pause revokes the producer, then hold the worker after its
	 * acknowledgement and control after the next resume has been issued. */
	atomic_store(&f.race, 1);
	td_signal(e);
	th_gate_await(&f.pump);
	th_gate_await(&f.native);
	ck_assert(atomic_load(&e->refill_enabled) == 1);
	OK(teder_request_close(e));
	th_gate_release(&f.pump);
	th_gate_release(&f.native);
	puts("close after acknowledged pause and pending resume");
	fflush(stdout);
	OK(th_close(e));
	OK(teder_destroy(&e));
	ck_assert(atomic_load(&f.closed) == 1 && atomic_load(&f.joins) == 1);
	dispose(&f);
}
END_TEST

START_TEST(initialization)
{
#ifdef _WIN32
	/* Warm the CRT/thread paths before checking native event/thread ownership. */
	fixture warm;
	init(&warm);
	teder_engine *engine = NULL;
	OK(create(&warm, &engine));
	OK(th_close(engine));
	OK(teder_destroy(&engine));
	dispose(&warm);
	DWORD before, after;
	ck_assert(GetProcessHandleCount(GetCurrentProcess(), &before));
#endif
	const unsigned stages[] = {TD_INIT_REFILL_WAKE,
	                           TD_INIT_REFILL_IDLE,
	                           TD_INIT_REFILL_THREAD,
	                           TD_INIT_THREAD,
	                           TD_INIT_PUBLISH};
	for (unsigned i = 0; i < sizeof(stages) / sizeof(*stages); i++) {
		fixture f;
		init(&f);
		f.fail_stage = stages[i];
		teder_engine *e = NULL;
		ck_assert(create(&f, &e) == TEDER_INTERNAL && !e);
		unsigned expected = stages[i] == TD_INIT_THREAD || stages[i] == TD_INIT_PUBLISH;
		ck_assert(atomic_load(&f.starts) == expected && atomic_load(&f.stops) == expected);
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
	const TTest *test = !strcmp(argv[1], "native-progress")  ? native_progress
	                    : !strcmp(argv[1], "handoff")        ? handoff
	                    : !strcmp(argv[1], "idle")           ? idle
	                    : !strcmp(argv[1], "initialization") ? initialization
	                                                         : NULL;
	if (!strcmp(argv[1], "close-resume"))
		test = close_resume;
	if (!test)
		return 2;
	Suite *suite = suite_create("refill");
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
