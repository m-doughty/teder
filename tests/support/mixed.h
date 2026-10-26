#ifndef TEDER_TEST_MIXED_H
#define TEDER_TEST_MIXED_H
#include "flac_heap.h"
#include <FLAC/stream_encoder.h>
#include <check.h>
#include <math.h>

typedef struct {
	unsigned rate, channels, bits, frames;
} th_format;

static int32_t th_mixed_sample(unsigned frame, unsigned channel, unsigned bits)
{
	int64_t scale = INT64_C(1) << (bits - 4);
	return (int32_t)(((int64_t)((frame * 13 + channel * 7) % 15) - 7) * scale +
	                 (frame + channel) % scale);
}

static void th_make_mixed(const char *path, th_format format)
{
	td_flac_heap heap;
	ck_assert(td_flac_heap_init(&heap, td_libc_allocator(), 8 * 1024 * 1024) == TEDER_OK);
	td_flac_heap *previous = td_flac_scope(&heap);
	FLAC__StreamEncoder *encoder = FLAC__stream_encoder_new();
	ck_assert(encoder && FLAC__stream_encoder_set_streamable_subset(encoder, false));
	ck_assert(FLAC__stream_encoder_set_channels(encoder, format.channels));
	ck_assert(FLAC__stream_encoder_set_bits_per_sample(encoder, format.bits));
	ck_assert(FLAC__stream_encoder_set_sample_rate(encoder, format.rate));
	ck_assert(FLAC__stream_encoder_set_blocksize(encoder, 256));
	ck_assert(FLAC__stream_encoder_set_total_samples_estimate(encoder, format.frames));
	ck_assert(FLAC__stream_encoder_init_file(encoder, path, NULL, NULL) ==
	          FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	int32_t samples[256 * 8];
	for (unsigned frame = 0; frame < format.frames;) {
		unsigned count = format.frames - frame < 256 ? format.frames - frame : 256;
		for (unsigned i = 0; i < count; i++)
			for (unsigned c = 0; c < format.channels; c++)
				samples[i * format.channels + c] = th_mixed_sample(frame + i, c, format.bits);
		ck_assert(FLAC__stream_encoder_process_interleaved(encoder, samples, count));
		frame += count;
	}
	ck_assert(FLAC__stream_encoder_finish(encoder));
	FLAC__stream_encoder_delete(encoder);
	td_flac_scope(previous);
	ck_assert(heap.error == TEDER_OK && !heap.live && !heap.bytes);
}

/* Independent binary64 reference: FLAC speaker table, RBJ Butterworth sections
 * and rational-time interpolation. No teder/miniaudio conversion calls. */
static double th_mixed_input(th_format format, unsigned frame, unsigned side)
{
	if (frame >= format.frames)
		return 0;
	const char *layouts[] = {"M", "LR", "LRC", "LRlr", "LRClr", "LRCFlr", "LRCFBst", "LRCFlrst"};
	double value = 0, sum = 0;
	for (unsigned c = 0; c < format.channels; c++) {
		char speaker = layouts[format.channels - 1][c];
		double weight = 0;
		if (speaker == 'M' || speaker == (side ? 'R' : 'L'))
			weight = 1;
		if (speaker == 'C' || speaker == (side ? 'r' : 'l') || speaker == (side ? 't' : 's'))
			weight = sqrt(0.5);
		if (speaker == 'B')
			weight = 0.5;
		value +=
		    ldexp((double)th_mixed_sample(frame, c, format.bits), 1 - (int)format.bits) * weight;
		sum += weight;
	}
	return value / sum;
}

typedef struct {
	double b0, b1, b2, a1, a2, x1[2], x2[2], y1[2], y2[2];
} th_biquad;

static double th_mixed_filter(th_biquad filters[4], double x, unsigned side)
{
	for (unsigned i = 0; i < 4; i++) {
		th_biquad *f = &filters[i];
		double y = f->b0 * x + f->b1 * f->x1[side] + f->b2 * f->x2[side] - f->a1 * f->y1[side] -
		           f->a2 * f->y2[side];
		f->x2[side] = f->x1[side];
		f->x1[side] = x;
		f->y2[side] = f->y1[side];
		f->y1[side] = y;
		x = y;
	}
	return x;
}

static unsigned th_mixed_frames(th_format format, unsigned rate)
{
	return (unsigned)(((uint64_t)format.frames * rate + format.rate - 1) / format.rate);
}

static double th_check_mixed(const float *pcm, th_format format, unsigned rate)
{
	unsigned frames = th_mixed_frames(format, rate);
	unsigned skip = format.rate == rate ? 0 : (unsigned)(9ull * rate / format.rate);
	/* Float coefficients accumulate more error at narrow filter cutoffs. */
	double tolerance = format.rate > 24ull * rate || rate > 24ull * format.rate ? 0.0001 : 0.00001;
	double pi = acos(-1.0);
	double w = pi * (format.rate < rate ? (double)format.rate / rate : (double)rate / format.rate);
	th_biquad filters[4] = {0};
	for (unsigned i = 0; i < 4; i++) {
		double alpha = sin(w) * cos((1 + 2 * i) * pi / 16);
		filters[i].b0 = filters[i].b2 = (1 - cos(w)) / (2 * (1 + alpha));
		filters[i].b1 = 2 * filters[i].b0;
		filters[i].a1 = -2 * cos(w) / (1 + alpha);
		filters[i].a2 = (1 - alpha) / (1 + alpha);
	}
	double left[2] = {0}, right[2] = {0}, worst = 0;
	unsigned consumed = 0;
	for (unsigned k = 0; k < skip + frames; k++) {
		uint64_t time = (uint64_t)k * format.rate;
		unsigned position = (unsigned)(time / rate);
		double fraction = (double)(time % rate) / rate;
		while (consumed <= position && format.rate != rate) {
			for (unsigned side = 0; side < 2; side++) {
				left[side] = right[side];
				right[side] = th_mixed_input(format, consumed, side);
				if (format.rate > rate)
					right[side] = th_mixed_filter(filters, right[side], side);
			}
			consumed++;
		}
		for (unsigned side = 0; side < 2; side++) {
			double expected = format.rate == rate
			                      ? th_mixed_input(format, k, side)
			                      : left[side] + (right[side] - left[side]) * fraction;
			if (format.rate < rate)
				expected = th_mixed_filter(filters, expected, side);
			if (k < skip)
				continue;
			float actual = pcm[2 * (k - skip) + side];
			double error = fabs(actual - expected);
			ck_assert_msg(isfinite(actual) && error < tolerance,
			              "%u Hz/%u ch/%u bit -> %u Hz, frame %u side %u: %.9g != %.9g",
			              format.rate,
			              format.channels,
			              format.bits,
			              rate,
			              k - skip,
			              side,
			              actual,
			              expected);
			if (error > worst)
				worst = error;
		}
	}
	return worst;
}
#endif
