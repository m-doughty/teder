#include "flac_heap.h"
#include "support/harness.h"
#include "fixtures/reference.h"
#include <FLAC/stream_decoder.h>
#include <FLAC/metadata.h>
#include <check.h>
#include <stdio.h>
#include <string.h>
#define OK(call) ck_assert((call) == TEDER_OK)

START_TEST(semantics)
{
	th_tracker t; td_flac_heap a, b;
	ck_assert(th_tracker_init(&t));
	OK(td_flac_heap_init(&a, th_allocator(&t), 4096));
	OK(td_flac_heap_init(&b, th_allocator(&t), 4096));
	ck_assert_ptr_null(td_flac_malloc(8));
	ck_assert_ptr_null(FLAC__stream_decoder_new()); /* no ambient allocation */
	ck_assert_ptr_null(td_flac_scope(&a));
	unsigned char *p = td_flac_calloc(8, 1);
	ck_assert_ptr_nonnull(p); ck_assert((uintptr_t)p % _Alignof(max_align_t) == 0);
	for (size_t i = 0; i < 8; i++) ck_assert(p[i] == 0);
	memset(p, 0x5a, 8); size_t bytes = a.bytes, attempts = t.attempts;
	ck_assert(bytes == t.bytes && a.live == 1);
	a.limit = bytes;
	ck_assert_ptr_null(td_flac_malloc(1));
	ck_assert_ptr_null(td_flac_realloc(p, 16));
	ck_assert(a.error == TEDER_LIMIT && a.bytes == bytes && t.attempts == attempts);
	for (size_t i = 0; i < 8; i++) ck_assert(p[i] == 0x5a);
	a.limit = 4096; a.error = TEDER_OK; t.fail_at = t.attempts + 1;
	ck_assert_ptr_null(td_flac_realloc(p, 16));
	ck_assert(a.error == TEDER_NO_MEMORY && a.bytes == bytes);
	ck_assert_ptr_null(td_flac_calloc(SIZE_MAX, 2));
	ck_assert(a.error == TEDER_NO_MEMORY); /* retain first error */
	t.fail_at = 0; a.error = TEDER_OK;
	ck_assert_ptr_eq(td_flac_scope(&b), &a);
	p = td_flac_realloc(p, 16); ck_assert_ptr_nonnull(p);
	for (size_t i = 0; i < 8; i++) ck_assert(p[i] == 0x5a);
	ck_assert(b.bytes == 0 && a.bytes == bytes + 8 && a.peak == bytes * 2 + 8);
	p = td_flac_realloc(p, 4); ck_assert_ptr_nonnull(p);
	for (size_t i = 0; i < 4; i++) ck_assert(p[i] == 0x5a);
	ck_assert(a.bytes == bytes - 4 && a.live == 1);
	ck_assert_ptr_null(td_flac_realloc(p, 0));
	ck_assert(a.live == 0 && a.bytes == 0);
	ck_assert_ptr_eq(td_flac_scope(&a), &b);
	ck_assert_ptr_null(td_flac_malloc(SIZE_MAX)); ck_assert(a.error == TEDER_LIMIT);
	a.error = TEDER_OK;
	ck_assert_ptr_null(td_flac_calloc(SIZE_MAX, 2)); ck_assert(a.error == TEDER_LIMIT);
	p = td_flac_realloc(NULL, 0); ck_assert_ptr_nonnull(p); td_flac_free(p);
	p = td_flac_calloc(0, SIZE_MAX); ck_assert_ptr_nonnull(p); ck_assert(*p == 0);
	td_flac_free(p); td_flac_free(NULL);
	a.error = TEDER_OK;
	FLAC__StreamMetadata *picture = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);
	ck_assert_ptr_nonnull(picture);
	ck_assert(FLAC__metadata_object_picture_set_mime_type(picture, "image/png", true));
	ck_assert_str_eq(picture->data.picture.mime_type, "image/png");
	FLAC__metadata_object_delete(picture);
	ck_assert(a.error == TEDER_OK);
	ck_assert_ptr_eq(td_flac_scope(NULL), &a);
	ck_assert(a.bytes == 0 && a.live == 0 && th_tracker_clean(&t)); th_tracker_uninit(&t);
}
END_TEST

typedef struct {
	unsigned char bytes[4096]; size_t size, cursor;
	unsigned frames, comments, errors;
	bool exact;
} input;
static void load(input *in)
{
	*in = (input){.exact = true};
	FILE *file = fopen(FIXTURE_DIR "/track-a.flac", "rb"); ck_assert_ptr_nonnull(file);
	in->size = fread(in->bytes, 1, sizeof(in->bytes), file);
	ck_assert(!ferror(file) && feof(file) && in->size > 42 && in->size + 22 < sizeof(in->bytes));
	ck_assert(fclose(file) == 0);
	/* Insert a legal one-point SEEKTABLE to exercise decoder realloc, including
	 * safe_realloc_'s free-on-failure policy, alongside temporary comment data. */
	memmove(in->bytes + 64, in->bytes + 42, in->size - 42);
	memset(in->bytes + 42, 0, 22); in->bytes[42] = 3; in->bytes[45] = 18; in->bytes[63] = 16;
	in->size += 22;
}
static FLAC__StreamDecoderReadStatus read_bytes(const FLAC__StreamDecoder *d, FLAC__byte *out, size_t *bytes, void *ctx)
{
	(void)d; input *in = ctx;
	size_t count = in->size - in->cursor;
	if (count > *bytes) count = *bytes;
	if (count > 5) count = 5; /* legal short reads */
	memcpy(out, in->bytes + in->cursor, count); in->cursor += count; *bytes = count;
	return count ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}
static FLAC__StreamDecoderWriteStatus write_pcm(const FLAC__StreamDecoder *d, const FLAC__Frame *f,
	const FLAC__int32 *const samples[], void *ctx)
{
	(void)d; input *in = ctx;
	if (f->header.channels != 2 || f->header.bits_per_sample != 16 || f->header.sample_rate != 44100 ||
		f->header.blocksize > 16 - in->frames) { in->exact = false; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
	for (unsigned i = 0; i < f->header.blocksize; i++) for (unsigned c = 0; c < 2; c++)
		if (samples[c][i] != track_a[(in->frames + i) * 2 + c]) in->exact = false;
	in->frames += f->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void metadata(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *ctx)
{
	(void)d; input *in = ctx;
	if (m->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
		in->comments++;
		if (m->data.vorbis_comment.num_comments != 1 || m->data.vorbis_comment.comments[0].length != 13 ||
			memcmp(m->data.vorbis_comment.comments[0].entry, "TITLE=track-a", 13)) in->exact = false;
	}
}
static void decoder_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus error, void *ctx)
{
	(void)d; (void)error; ((input *)ctx)->errors++;
}
static bool decode(input *in)
{
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	if (!d) return false;
	bool opened = FLAC__stream_decoder_set_md5_checking(d, true) &&
		FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_VORBIS_COMMENT) &&
		FLAC__stream_decoder_init_stream(d, read_bytes, NULL, NULL, NULL, NULL, write_pcm, metadata, decoder_error, in) == FLAC__STREAM_DECODER_INIT_STATUS_OK;
	bool processed = opened && FLAC__stream_decoder_process_until_end_of_stream(d);
	bool finished = opened && FLAC__stream_decoder_finish(d);
	FLAC__stream_decoder_delete(d);
	return processed && finished && in->exact && in->frames == 16 && in->comments == 1 && in->errors == 0;
}
static void *persistent_fail(void *context, size_t bytes, const char *site)
{
	th_tracker *t = context;
	if (t->fail_at && t->attempts >= t->fail_at) t->fail_at = t->attempts + 1;
	return th_alloc(t, bytes, site);
}
START_TEST(failures)
{
	th_tracker t; td_flac_heap heap; input in;
	ck_assert(th_tracker_init(&t)); load(&in);
	OK(td_flac_heap_init(&heap, th_allocator(&t), 1024 * 1024));
	ck_assert_ptr_null(td_flac_scope(&heap)); ck_assert(decode(&in));
	size_t attempts = t.attempts, peak = heap.peak;
	ck_assert(attempts == 15 && heap.error == TEDER_OK && heap.live == 0 && heap.bytes == 0 && th_tracker_clean(&t));
	for (size_t fail = 1; fail <= attempts; fail++) {
		load(&in); t.attempts = 0; t.fail_at = fail;
		OK(td_flac_heap_init(&heap, th_allocator(&t), 1024 * 1024));
		bool success = decode(&in);
		/* Pinned md5.c:508 retries its final buffer allocation with malloc.
		 * A recovered one-shot failure still leaves our first error visible. */
		ck_assert(success == (fail == 15)); ck_assert(heap.error == TEDER_NO_MEMORY);
		ck_assert(heap.live == 0 && heap.bytes == 0 && th_tracker_clean(&t));
		ck_assert_str_eq(t.failed_site, "flac.heap");
		load(&in); t.attempts = 0; t.fail_at = fail;
		OK(td_flac_heap_init(&heap, (td_allocator){&t, persistent_fail, th_free}, 1024 * 1024));
		ck_assert(!decode(&in)); ck_assert(heap.error == TEDER_NO_MEMORY);
		ck_assert(heap.live == 0 && heap.bytes == 0 && th_tracker_clean(&t));
	}
	t.fail_at = 0;
	/* A measured peak is a test boundary, not a production codec size estimate. */
	for (size_t limit = 0; limit <= peak; limit += peak) {
		load(&in); OK(td_flac_heap_init(&heap, th_allocator(&t), limit));
		ck_assert(decode(&in) == (limit == peak));
		ck_assert(heap.error == (limit == peak ? TEDER_OK : TEDER_LIMIT));
		ck_assert(heap.peak <= limit && heap.bytes == 0 && heap.live == 0 && th_tracker_clean(&t));
	}
	load(&in); OK(td_flac_heap_init(&heap, th_allocator(&t), peak - 1));
	ck_assert(!decode(&in)); ck_assert(heap.error == TEDER_LIMIT && th_tracker_clean(&t));
	td_flac_scope(NULL); th_tracker_uninit(&t);
	printf("decoder fail-at-1..%zu: exact PCM/metadata baseline, zero live allocations after every unwind; peak=%zu\n", attempts, peak);
}
END_TEST

typedef struct { th_gate gate; td_flac_heap heap; th_tracker tracker; bool valid; } job;
static void worker(void *ctx)
{
	job *j = ctx;
	j->valid = td_flac_scope(&j->heap) == NULL;
	/* Both threads remain inside distinct scopes before either decoder runs. */
	th_gate_hit(&j->gate);
	for (unsigned i = 0; i < 100; i++) {
		FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
		if (!d) { j->valid = false; break; }
		FLAC__stream_decoder_delete(d);
	}
	j->valid = j->valid && td_flac_scope(NULL) == &j->heap && j->heap.error == TEDER_OK && !j->heap.bytes && !j->heap.live;
}
START_TEST(isolation)
{
	job jobs[2]; th_thread threads[2];
	for (unsigned i = 0; i < 2; i++) {
		ck_assert(th_tracker_init(&jobs[i].tracker)); ck_assert(th_gate_init(&jobs[i].gate));
		OK(td_flac_heap_init(&jobs[i].heap, th_allocator(&jobs[i].tracker), 1024 * 1024));
		ck_assert(th_thread_start(&threads[i], worker, &jobs[i]));
	}
	for (unsigned i = 0; i < 2; i++) th_gate_await(&jobs[i].gate);
	ck_assert_ptr_null(td_flac_malloc(8));
	for (unsigned i = 0; i < 2; i++) th_gate_release(&jobs[i].gate);
	for (unsigned i = 0; i < 2; i++) {
		th_thread_join(&threads[i]); ck_assert(jobs[i].valid && th_tracker_clean(&jobs[i].tracker));
		ck_assert(jobs[i].tracker.attempts == 500);
		th_gate_uninit(&jobs[i].gate); th_tracker_uninit(&jobs[i].tracker);
	}
}
END_TEST
int main(int argc, char **argv)
{
	if (argc != 2) return 2;
	const TTest *test = NULL;
	if (!strcmp(argv[1], "semantics")) test = semantics;
	else if (!strcmp(argv[1], "failures")) test = failures;
	else if (!strcmp(argv[1], "isolation")) test = isolation;
	else return 2;
	Suite *suite = suite_create("flac-heap"); TCase *tc = tcase_create(argv[1]); tcase_add_test(tc, test); suite_add_tcase(suite, tc);
	SRunner *runner = srunner_create(suite); srunner_set_fork_status(runner, CK_NOFORK); srunner_run_all(runner, CK_NORMAL);
	int failed = srunner_ntests_failed(runner); srunner_free(runner); return failed ? 1 : 0;
}
