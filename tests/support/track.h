#ifndef TEDER_TEST_TRACK_H
#define TEDER_TEST_TRACK_H
#include "flac_heap.h"
#include <FLAC/stream_encoder.h>
#include <check.h>
static void th_make_track_samples(const char *path, unsigned frames, bool silent_edges)
{
	td_flac_heap heap;
	ck_assert(td_flac_heap_init(&heap, td_libc_allocator(), 8 * 1024 * 1024) == TEDER_OK);
	td_flac_heap *previous = td_flac_scope(&heap);
	FLAC__StreamEncoder *encoder = FLAC__stream_encoder_new();
	ck_assert(encoder);
	ck_assert(FLAC__stream_encoder_set_channels(encoder, 2));
	ck_assert(FLAC__stream_encoder_set_bits_per_sample(encoder, 16));
	ck_assert(FLAC__stream_encoder_set_sample_rate(encoder, 44100));
	ck_assert(FLAC__stream_encoder_set_blocksize(encoder, 256));
	ck_assert(FLAC__stream_encoder_set_total_samples_estimate(encoder, frames));
	ck_assert(FLAC__stream_encoder_init_file(encoder, path, NULL, NULL) ==
	          FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	int32_t samples[512];
	for (unsigned frame = 0; frame < frames;) {
		unsigned count = frames - frame < 256 ? frames - frame : 256;
		for (unsigned i = 0; i < count; i++) {
			samples[2 * i] = (int32_t)((frame + i) % 2048) - 1024;
			samples[2 * i + 1] = 1024 - (int32_t)((frame + i) % 2048);
			if (silent_edges && (frame + i == 0 || frame + i + 1 == frames))
				samples[2 * i] = samples[2 * i + 1] = 0;
		}
		ck_assert(FLAC__stream_encoder_process_interleaved(encoder, samples, count));
		frame += count;
	}
	ck_assert(FLAC__stream_encoder_finish(encoder));
	FLAC__stream_encoder_delete(encoder);
	td_flac_scope(previous);
	ck_assert(heap.error == TEDER_OK && heap.live == 0 && heap.bytes == 0);
}

static void th_make_track(const char *path, unsigned frames)
{
	th_make_track_samples(path, frames, false);
}
#endif
