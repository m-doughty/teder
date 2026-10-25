#include "fixtures/reference.h"
#include <FLAC/stream_decoder.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "flac_heap.h"

typedef struct {
	const char *name;
	const int32_t *samples;
	unsigned channels, bits, rate, frames, comments, info, errors;
	bool matches;
} reference;
static FLAC__StreamDecoderWriteStatus write_pcm(const FLAC__StreamDecoder *d,
	const FLAC__Frame *f, const FLAC__int32 *const pcm[], void *context)
{
	reference *r = context; (void)d;
	if (f->header.channels != r->channels || f->header.bits_per_sample != r->bits ||
		f->header.sample_rate != r->rate || f->header.blocksize > 16 - r->frames) {
		r->matches = false; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	for (unsigned i = 0; i < f->header.blocksize; i++) for (unsigned c = 0; c < r->channels; c++)
		if (pcm[c][i] != r->samples[(r->frames + i) * r->channels + c]) r->matches = false;
	r->frames += f->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void metadata(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *context)
{
	reference *r = context; (void)d;
	if (m->type == FLAC__METADATA_TYPE_STREAMINFO) {
		r->info++;
		if (m->data.stream_info.total_samples != 16 || m->data.stream_info.channels != r->channels ||
			m->data.stream_info.bits_per_sample != r->bits || m->data.stream_info.sample_rate != r->rate) r->matches = false;
	} else if (m->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
		char expected[64]; int length = snprintf(expected, sizeof(expected), "TITLE=%s", r->name);
		r->comments++;
		if (m->data.vorbis_comment.num_comments != 1) { r->matches = false; return; }
		FLAC__StreamMetadata_VorbisComment_Entry entry = m->data.vorbis_comment.comments[0];
		if (entry.length != (unsigned)length || memcmp(entry.entry, expected, (size_t)length)) r->matches = false;
	}
}
static void error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus status, void *context)
{
	reference *r = context; (void)d;
	r->errors++; fprintf(stderr, "%s decoder error=%d\n", r->name, status);
}
static bool validate(reference *r)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.pcm", FIXTURE_DIR, r->name);
	FILE *file = fopen(path, "rb");
	if (!file) return false;
	for (unsigned i = 0; i < 16 * r->channels; i++) {
		uint32_t value = 0;
		for (unsigned b = 0; b < r->bits / 8; b++) {
			int byte = fgetc(file);
			if (byte == EOF) { fclose(file); return false; }
			value |= (uint32_t)byte << (8 * b);
		}
		int64_t sample = value;
		if (value & (UINT32_C(1) << (r->bits - 1))) sample -= INT64_C(1) << r->bits;
		if (sample != r->samples[i]) r->matches = false;
	}
	if (fgetc(file) != EOF || ferror(file)) r->matches = false;
	if (fclose(file)) r->matches = false;
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	if (!d) return false;
	bool configured = FLAC__stream_decoder_set_md5_checking(d, true) &&
		FLAC__stream_decoder_set_metadata_respond(d, FLAC__METADATA_TYPE_VORBIS_COMMENT);
	snprintf(path, sizeof(path), "%s/%s.flac", FIXTURE_DIR, r->name);
	bool opened = configured && FLAC__stream_decoder_init_file(d, path, write_pcm, metadata, error, r) == FLAC__STREAM_DECODER_INIT_STATUS_OK;
	bool processed = opened && FLAC__stream_decoder_process_until_end_of_stream(d);
	bool checksum = opened && FLAC__stream_decoder_finish(d);
	FLAC__stream_decoder_delete(d);
	return processed && checksum && r->matches && r->frames == 16 && r->info == 1 && r->comments == 1 && !r->errors;
}
int main(void)
{
	td_flac_heap heap;
	if (td_flac_heap_init(&heap, td_libc_allocator(), 1024 * 1024) != TEDER_OK) return 1;
	td_flac_heap *previous = td_flac_scope(&heap);
	reference fixtures[] = {
		{"track-a", track_a, 2, 16, 44100, 0, 0, 0, 0, true},
		{"track-b", track_b, 2, 16, 44100, 0, 0, 0, 0, true},
		{"mono-24", mono_24, 1, 24, 48000, 0, 0, 0, 0, true}
	};
	for (size_t i = 0; i < 3; i++) if (!validate(&fixtures[i])) {
		fprintf(stderr, "fixture mismatch: %s\n", fixtures[i].name); return 1;
	}
	td_flac_scope(previous);
	return heap.live || heap.bytes || heap.error != TEDER_OK ? 1 : 0;
}
