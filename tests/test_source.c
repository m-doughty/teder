#include "source.h"
#include "support/harness.h"
#include "fixtures/reference.h"
#include <FLAC/stream_encoder.h>
#include <check.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define OK(x) ck_assert((x) == TEDER_OK)
typedef struct { th_tracker tracker; td_arena *arena; td_pcm_pool pool; unsigned opened, closed, reads; } fixture;
static void checkpoint(void *ctx, unsigned point)
{
	fixture *f = ctx;
	if (point == TD_SOURCE_OPENED) f->opened++;
	if (point == TD_SOURCE_CLOSED) f->closed++;
	if (point == TD_SOURCE_READ) f->reads++;
}
static void init(fixture *f, uint32_t frames)
{
	memset(f, 0, sizeof(*f)); ck_assert(th_tracker_init(&f->tracker));
	size_t size = 0; td_pcm_layout layout; ck_assert(td_pcm_plan(&size, frames, &layout));
	f->arena = td_arena_create(th_allocator(&f->tracker), size); ck_assert_ptr_nonnull(f->arena);
	td_pcm_init(&f->pool, td_arena_alloc(f->arena, size, 1, _Alignof(max_align_t)), &layout, frames);
	OK(td_pcm_select(&f->pool, 17));
}
static void cleanup(fixture *f, td_source **s)
{
	td_pcm_clear(&f->pool); OK(td_source_retire(s));
	ck_assert(f->opened == f->closed); td_arena_destroy(f->arena);
	ck_assert(th_tracker_clean(&f->tracker)); th_tracker_uninit(&f->tracker);
}
static td_source_config config(fixture *f, const char *path, uint32_t rate)
{
	td_source_config c = {.generation = 17, .source = 29, .track = 41, .output_start = 101,
		.output_rate = rate, .codec_bytes = 4*1024*1024, .context = f, .checkpoint = checkpoint};
	OK(td_source_plan((uint32_t)strlen(path), c.codec_bytes, &c.reservation)); return c;
}
static teder_code open_source(fixture *f, const char *path, uint32_t rate, td_source **s)
{
	td_source_config c = config(f, path, rate);
	return td_source_open(path, (uint32_t)strlen(path), &c, th_allocator(&f->tracker), s);
}
static int32_t sample(uint64_t i, unsigned c, unsigned bits)
{ return (int32_t)(((int64_t)((i+c*3)%15) - 7) * (INT64_C(1) << (bits-4))); }
static void make(const char *name, unsigned bits, unsigned rate, unsigned channels, unsigned frames, unsigned block)
{
	td_flac_heap heap; OK(td_flac_heap_init(&heap, td_libc_allocator(), 64*1024*1024));
	td_flac_heap *previous = td_flac_scope(&heap);
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new(); ck_assert_ptr_nonnull(e);
	ck_assert(FLAC__stream_encoder_set_streamable_subset(e, false));
	ck_assert(FLAC__stream_encoder_set_channels(e, channels) && FLAC__stream_encoder_set_bits_per_sample(e, bits));
	ck_assert(FLAC__stream_encoder_set_sample_rate(e, rate) && FLAC__stream_encoder_set_blocksize(e, block));
	ck_assert(FLAC__stream_encoder_set_total_samples_estimate(e, frames));
	ck_assert(FLAC__stream_encoder_init_file(e, name, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	int32_t pcm[256*8];
	for (unsigned offset = 0; offset < frames;) {
		unsigned count = frames-offset < 256 ? frames-offset : 256;
		for (unsigned i = 0; i < count; i++) for (unsigned c = 0; c < channels; c++) pcm[i*channels+c] = sample(offset+i, c, bits);
		ck_assert(FLAC__stream_encoder_process_interleaved(e, pcm, count)); offset += count;
	}
	ck_assert(FLAC__stream_encoder_finish(e)); FLAC__stream_encoder_delete(e); td_flac_scope(previous);
	ck_assert(heap.error == TEDER_OK && heap.live == 0 && heap.bytes == 0);
}
typedef void (*verify)(uint64_t, const float *, void *);
static uint64_t drain(fixture *f, td_source *s, verify check, void *ctx)
{
	uint64_t frames = 0, source_end = s->start; bool final = false; unsigned guard = 0;
	for (;;) {
		ck_assert(++guard < 1000000);
		teder_code code = td_source_pump(s, &f->pool);
		ck_assert_msg(code == TEDER_OK || code == TEDER_AGAIN || code == TEDER_CLOSED, "pump code=%d", code);
		td_pcm_view v;
		while (td_pcm_peek(&f->pool, &v) == TEDER_OK) {
			ck_assert(!final && v.generation->generation == 17 && v.generation->source == 29 && v.generation->track == 41);
			ck_assert(v.meta->output_start + v.offset == 101 + frames);
			if (!v.offset) { ck_assert(v.meta->source_start == source_end); source_end += v.meta->source_frames; }
			uint32_t count = v.frames < 7 ? v.frames : 7;
			for (unsigned i = 0; i < count; i++) if (check) check(frames+i, v.samples+i*2, ctx);
			frames += count; final = (v.meta->flags & TD_PCM_FINAL) && count == v.frames;
			if (!(v.meta->flags & TD_PCM_FINAL)) ck_assert(v.meta->frames == 1024 || f->pool.count == 1);
			OK(td_pcm_consume(&f->pool, count));
		}
		if (code == TEDER_CLOSED) break;
	}
	ck_assert((frames == 0 || final) && source_end == s->decoded);
	return frames;
}
static void literal(uint64_t i, const float *pcm, void *ctx)
{
	const int32_t *expected = ctx;
	for (unsigned c = 0; c < 2; c++) ck_assert(pcm[c] == (float)expected[i*2+c]/32768.0f);
}
static void mapped(uint64_t i, const float *pcm, void *ctx)
{
	unsigned channels = *(unsigned *)ctx;
	/* Independent explicit FLAC speaker table; LFE has zero weight. */
	static const double w[8][8][2] = {
		{{1,1}}, {{1,0},{0,1}}, {{1,0},{0,1},{0.7071067811865475,0.7071067811865475}},
		{{1,0},{0,1},{0.7071067811865475,0},{0,0.7071067811865475}},
		{{1,0},{0,1},{0.7071067811865475,0.7071067811865475},{0.7071067811865475,0},{0,0.7071067811865475}},
		{{1,0},{0,1},{0.7071067811865475,0.7071067811865475},{0,0},{0.7071067811865475,0},{0,0.7071067811865475}},
		{{1,0},{0,1},{0.7071067811865475,0.7071067811865475},{0,0},{0.5,0.5},{0.7071067811865475,0},{0,0.7071067811865475}},
		{{1,0},{0,1},{0.7071067811865475,0.7071067811865475},{0,0},{0.7071067811865475,0},{0,0.7071067811865475},{0.7071067811865475,0},{0,0.7071067811865475}}};
	for (unsigned side = 0; side < 2; side++) {
		double value = 0, norm = 0;
		for (unsigned c = 0; c < channels; c++) { value += (double)sample(i,c,4)/8 * w[channels-1][c][side]; norm += w[channels-1][c][side]; }
		ck_assert(fabs(pcm[side] - value/norm) < 0.000001);
	}
}
START_TEST(reference)
{
	fixture f; td_source *s = NULL; init(&f, 31);
	OK(open_source(&f, FIXTURE_DIR "/track-a.flac", 44100, &s));
	ck_assert(s->info.duration_frames == 16 && s->info.duration_known && s->info.seekable && s->info.bits_per_sample == 16);
	ck_assert(drain(&f,s,literal,(void *)track_a) == 16); cleanup(&f,&s);
	const unsigned rates[] = {1,8000,22050,44100,48000,96000,384000,1048575};
	for (unsigned channels = 1; channels <= 8; channels++) {
		char path[1024]; snprintf(path,sizeof(path), OUTPUT_DIR "/format-%u.flac",channels);
		make(path,channels*4,rates[channels-1],channels,3073,1024);
		init(&f,3072); td_source_config c = config(&f,path,rates[channels-1]); c.read_size = 5;
		OK(td_source_open(path,(uint32_t)strlen(path),&c,th_allocator(&f.tracker),&s));
		ck_assert(s->info.channels == channels && s->info.bits_per_sample == channels*4 && s->info.sample_rate == rates[channels-1]);
		ck_assert(drain(&f,s,mapped,&channels) == 3073); ck_assert(f.tracker.peak <= c.reservation + 32768); cleanup(&f,&s);
	}
	make(OUTPUT_DIR "/large.flac",32,48000,2,65536,65535); init(&f,2048);
	OK(open_source(&f,OUTPUT_DIR "/large.flac",48000,&s)); unsigned channels = 2;
	ck_assert(drain(&f,s,mapped,&channels) == 65536); cleanup(&f,&s);
	channels = 1;
	for (unsigned bits = 4; bits <= 32; bits++) {
		make(OUTPUT_DIR "/depth.flac",bits,44100,1,16,16); init(&f,19);
		OK(open_source(&f,OUTPUT_DIR "/depth.flac",44100,&s));
		ck_assert(s->info.bits_per_sample == bits && drain(&f,s,mapped,&channels) == 16); cleanup(&f,&s);
	}
	const char *unicode = OUTPUT_DIR "/m\xc3\xbasica \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d.flac";
	make(unicode,16,44100,1,16,16); init(&f,19); OK(open_source(&f,unicode,44100,&s));
	ck_assert(drain(&f,s,mapped,&channels) == 16); cleanup(&f,&s);
	int32_t endpoints[] = {INT32_MIN,INT32_MAX}; const int32_t *planes[] = {endpoints}; float converted[4];
	td_downmix(converted,planes,2,1,32);
	ck_assert(converted[0] == -1 && converted[1] == -1 && converted[2] == 1 && converted[3] == 1);
}
END_TEST
static void collect(uint64_t i, const float *pcm, void *ctx) { float *out = ctx; out[i*2] = pcm[0]; out[i*2+1] = pcm[1]; }
START_TEST(conversion)
{
	/* Independent binary64 Butterworth/interpolation reference; generator lives
	 * in fixtures/conversion_reference.py. Right channel is exactly antisymmetric. */
	static const float up[] = {-0.04094626455f,-0.0260894719f,-0.00735579755f,0.008179460252f,0.02109879309f,0.03616974303f,0.05300392303f,0.06838969104f,0.08258750842f,0.09776064209f,0.1137339885f,0.1290505057f,0.1438280368f,0.1590479343f,0.1746308554f,0.1887845811f,0.1936437193f,0.1692182742f,0.09912730051f,0.009039844883f,-0.03782188738f,-0.01730656985f,0.017204101f,0.0132319389f,-0.00930889522f,-0.009217704921f,0.005627328048f,0.00628239033f,-0.003607361489f,-0.004251240744f,0.002377476964f,0.002868860826f};
	static const float down[] = {-0.07722943774f,-0.0684246029f,0.02713067635f,0.06513578667f,0.1416162903f,0.1716223316f,0.05848782661f,-0.04445810882f};
	float a[128] = {0}, b[128] = {0};
	for (unsigned direction = 0; direction < 2; direction++) {
		unsigned rate = direction ? 22050 : 88200; uint64_t expected = direction ? 8 : 32;
		for (unsigned pass = 0; pass < 2; pass++) {
			fixture f; td_source *s = NULL; init(&f,pass ? 3 : 64);
			OK(open_source(&f,FIXTURE_DIR "/track-a.flac",rate,&s));
			ck_assert(drain(&f,s,collect,pass ? b : a) == expected); cleanup(&f,&s);
		}
		for (unsigned i = 0; i < expected*2; i++) { ck_assert(isfinite(a[i]) && fabsf(a[i]-b[i]) < 0.000001f); }
		const float *reference = direction ? down : up;
		for (unsigned i = 0; i < expected; i++) { ck_assert(fabsf(a[i*2]-reference[i]) < 0.000001f); ck_assert(a[i*2+1] == -a[i*2]); }
	}
	for (unsigned pass = 0; pass < 2; pass++) {
		fixture f; td_source *s = NULL; init(&f,pass ? 3 : 64); OK(open_source(&f,FIXTURE_DIR "/track-a.flac",48000,&s));
		ck_assert(drain(&f,s,collect,pass ? b : a) == 18); cleanup(&f,&s);
	}
	for (unsigned i = 0; i < 36; i++) ck_assert(isfinite(a[i]) && fabsf(a[i]-b[i]) < 0.000001f);
	uint64_t n = 77; ck_assert(!td_rate_frames(UINT64_MAX,1048575,1,true,&n) && n == 77);
	ck_assert(td_rate_frames(16,48000,44100,true,&n) && n == 18);
}
END_TEST
static void copy_changed(const char *to, unsigned change)
{
	unsigned char bytes[1024]; FILE *in = fopen(FIXTURE_DIR "/track-a.flac","rb"); ck_assert_ptr_nonnull(in);
	size_t n = fread(bytes,1,sizeof(bytes),in); ck_assert(fclose(in) == 0);
	if (change == 1) n--; /* truncated frame */
	if (change == 2) bytes[n-4] ^= 1; /* CRC failure */
	if (change == 3) bytes[26] ^= 1; /* wrong PCM MD5 */
	if (change == 4) { bytes[21] &= 0xf0; memset(bytes+22,0,4); } /* unknown duration */
	if (change == 5) bytes[n++] = 42; /* trailing garbage */
	FILE *out = fopen(to,"wb"); ck_assert_ptr_nonnull(out); ck_assert(fwrite(bytes,1,n,out) == n); ck_assert(fclose(out) == 0);
}
START_TEST(errors)
{
	fixture f; td_source *s = NULL; init(&f,64);
	td_source_config c = config(&f,FIXTURE_DIR "/track-a.flac",44100); size_t baseline = f.tracker.bytes;
	ck_assert(td_source_open("relative",8,&c,th_allocator(&f.tracker),&s) == TEDER_INVALID);
	ck_assert(td_path_validate("/bad\xc0\xaf",6) == TEDER_ENCODING);
	ck_assert(td_path_validate("/bad\xed\xa0\x80",7) == TEDER_ENCODING);
	ck_assert(td_path_validate("/x\0x",4) == TEDER_INVALID);
	ck_assert(open_source(&f,OUTPUT_DIR "/absent.flac",44100,&s) == TEDER_IO);
	c.reservation--; ck_assert(td_source_open(FIXTURE_DIR "/track-a.flac",(uint32_t)strlen(FIXTURE_DIR "/track-a.flac"),&c,th_allocator(&f.tracker),&s) == TEDER_LIMIT);
	ck_assert(f.tracker.bytes == baseline); cleanup(&f,&s);
	for (unsigned i = 1; i <= 5; i++) {
		copy_changed(OUTPUT_DIR "/broken.flac",i); init(&f,64); OK(open_source(&f,OUTPUT_DIR "/broken.flac",44100,&s));
		if (i == 4) { ck_assert(!s->info.duration_known && s->info.duration_frames == 0); ck_assert(drain(&f,s,literal,(void *)track_a) == 16); }
		else { teder_code code = TEDER_AGAIN; for (unsigned j = 0; j < 100 && code >= 0; j++) code = td_source_pump(s,&f.pool); ck_assert(code == TEDER_CORRUPT); }
		cleanup(&f,&s);
	}
	init(&f,64); OK(open_source(&f,FIXTURE_DIR "/track-a.flac",44100,&s));
	ck_assert(td_source_seek(s,17) == TEDER_INVALID); OK(td_source_seek(s,7));
	ck_assert(drain(&f,s,literal,(void *)(track_a+14)) == 9); ck_assert(td_source_seek(s,0) == TEDER_BAD_STATE); cleanup(&f,&s);
	init(&f,64); OK(open_source(&f,FIXTURE_DIR "/track-a.flac",44100,&s)); OK(td_source_seek(s,16));
	ck_assert(drain(&f,s,NULL,NULL) == 0); cleanup(&f,&s);
}
END_TEST
START_TEST(failures)
{
	for (unsigned seeking = 0; seeking < 2; seeking++) {
		size_t count = 0;
		for (size_t fail = 0; fail <= count; fail++) {
			fixture f; td_source *s = NULL; init(&f,64); f.tracker.attempts = 0; f.tracker.fail_at = fail;
			teder_code code = open_source(&f,FIXTURE_DIR "/track-a.flac",88200,&s);
			if (code == TEDER_OK) {
				if (seeking) code = td_source_seek(s,1);
				for (unsigned i = 0; i < 1000 && code >= 0; i++) code = td_source_pump(s,&f.pool);
			}
			if (!fail) { ck_assert(code == TEDER_CLOSED); count = f.tracker.attempts; ck_assert(count > 5); }
			else ck_assert(code == TEDER_NO_MEMORY);
			cleanup(&f,&s);
		}
		printf("source open/seek/decode failures 1..%zu: zero retained resources\n",count);
	}
}
END_TEST
typedef struct { fixture *f; th_gate gate; atomic_uint cancel; td_source *source; teder_code result; unsigned pause_at; } job;
static void gate_read(void *ctx, unsigned point)
{
	job *j = ctx; checkpoint(j->f,point);
	if (point == TD_SOURCE_READ && j->f->reads == j->pause_at) th_gate_hit(&j->gate);
}
static void open_worker(void *ctx)
{
	job *j = ctx; const char *path = FIXTURE_DIR "/track-a.flac"; td_source_config c = config(j->f,path,44100);
	if (j->source) { j->result = td_source_seek(j->source,7); return; }
	c.cancel = &j->cancel; c.context = j; c.checkpoint = gate_read;
	j->result = td_source_open(path,(uint32_t)strlen(path),&c,th_allocator(&j->f->tracker),&j->source);
}
START_TEST(cancellation)
{
	fixture f; init(&f,1); job j = {.f = &f, .pause_at = 1}; atomic_init(&j.cancel,0); ck_assert(th_gate_init(&j.gate)); th_thread thread;
	ck_assert(th_thread_start(&thread,open_worker,&j)); th_gate_await(&j.gate);
	ck_assert(f.opened == 1 && f.closed == 0); atomic_store(&j.cancel,1); th_gate_release(&j.gate); th_thread_join(&thread);
	ck_assert(j.result == TEDER_CANCELLED && !j.source && f.closed == 1); th_gate_uninit(&j.gate); cleanup(&f,&j.source);
	init(&f,1); atomic_store(&j.cancel,0); ck_assert(th_gate_init(&j.gate));
	OK(open_source(&f,FIXTURE_DIR "/track-a.flac",44100,&j.source)); j.pause_at = f.reads+1;
	j.source->config.cancel = &j.cancel; j.source->config.context = &j; j.source->config.checkpoint = gate_read;
	ck_assert(th_thread_start(&thread,open_worker,&j)); th_gate_await(&j.gate);
	atomic_store(&j.cancel,1); th_gate_release(&j.gate); th_thread_join(&thread);
	ck_assert(j.result == TEDER_CANCELLED && f.closed == 0); cleanup(&f,&j.source); th_gate_uninit(&j.gate);
	init(&f,1); td_source *s = NULL; atomic_uint cancel; atomic_init(&cancel,0);
	td_source_config c = config(&f,FIXTURE_DIR "/track-a.flac",44100); c.cancel = &cancel;
	OK(td_source_open(FIXTURE_DIR "/track-a.flac",(uint32_t)strlen(FIXTURE_DIR "/track-a.flac"),&c,th_allocator(&f.tracker),&s));
	for (unsigned i = 0; i < 20 && !s->published; i++) ck_assert(td_source_pump(s,&f.pool) >= 0);
	ck_assert(s->published); unsigned reads = f.reads; size_t allocations = f.tracker.attempts;
	for (unsigned i = 0; i < 20; i++) ck_assert(td_source_pump(s,&f.pool) == TEDER_AGAIN);
	ck_assert(f.reads == reads && f.tracker.attempts == allocations);
	td_pcm_view view; OK(td_pcm_peek(&f.pool,&view)); atomic_store(&cancel,1);
	ck_assert(td_source_pump(s,&f.pool) == TEDER_CANCELLED); ck_assert(td_source_retire(&s) == TEDER_BUSY);
	ck_assert(f.closed == 1 && s->heap.bytes == 0 && !s->decoder && view.generation->track == 41);
	ck_assert(view.samples[0] == (float)track_a[0]/32768.0f); OK(td_pcm_consume(&f.pool,1)); OK(td_source_retire(&s)); cleanup(&f,&s);
}
END_TEST
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1],"reference")) test = reference;
	else if (!strcmp(argv[1],"conversion")) test = conversion;
	else if (!strcmp(argv[1],"errors")) test = errors;
	else if (!strcmp(argv[1],"failures")) test = failures;
	else if (!strcmp(argv[1],"cancellation")) test = cancellation;
	else return 2;
	Suite *suite = suite_create("source"); TCase *tc = tcase_create(argv[1]); tcase_add_test(tc,test); suite_add_tcase(suite,tc);
	SRunner *runner = srunner_create(suite); srunner_set_fork_status(runner,CK_NOFORK); srunner_run_all(runner,CK_NORMAL);
	int failed = srunner_ntests_failed(runner); srunner_free(runner); return failed ? 1 : 0;
}
