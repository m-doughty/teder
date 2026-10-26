#ifndef TEDER_SOURCE_H
#define TEDER_SOURCE_H
#include "pcm.h"
#include "flac_heap.h"
#include "miniaudio.h"
#include <FLAC/stream_decoder.h>

enum { TD_SOURCE_OPENED = 1, TD_SOURCE_READ, TD_SOURCE_CLOSED, TD_FLAC_FRAMES = 65535 };

typedef struct {
	uint64_t generation, source, track, output_start;
	uint32_t output_rate,
	    read_size; /* 0 rate selects native; 0 read_size selects 4096, its maximum */
	size_t codec_bytes, reservation;
	bool boundaries;              /* engine sources publish zero-frame EOF markers */
	const atomic_uint *cancel;    /* optional; stable until retirement */
	const atomic_uint *lifecycle; /* optional engine close latch; LIVE permits work */
	void *context;
	/* Optional first-publication ownership hook; never runs on the consumer. */
	teder_code (*publish)(void *, td_pcm_pool *, uint32_t, const td_pcm_meta *);
	void (*checkpoint)(void *, unsigned); /* private native-test seam, no public callback */
} td_source_config;

typedef struct td_source {
	td_pcm_generation generation;
	td_flac_heap heap;
	td_source_config config;
	teder_probe_info info;
	FLAC__StreamDecoder *decoder;
	ma_data_converter converter;
	td_pcm_pool *pool;
	intptr_t file;
	uint64_t file_size, file_cursor, frame_end, decoded, start, output, skip;
	uint32_t input_frames, input_offset, slot, filled;
	bool file_open, converted, eof, done, published, pending, close_failed, priming;
	float tail[2];
	teder_code error;
	float pcm[TD_FLAC_FRAMES * 2];
	void *converter_heap;
	size_t converter_bytes;
	char *path;
	void *native_path;
} td_source;

/* Pure sizing: caller reserves this total against the engine BEFORE open.
 * It includes one source arena and a retained codec cap; no file-sized buffers.
 * Calls are serialized and nonreentrant on the producer, never the callback.
 * source/allocator/config contexts survive until retirement clears the owner. */
teder_code td_source_plan(uint32_t path_bytes, size_t codec_bytes, size_t *reservation);
teder_code
td_source_open(const char *, uint32_t bytes, const td_source_config *, td_allocator, td_source **);
/* Staged seek only, before any publication. Failure poisons this staged source;
 * replace by staging a new source, never by seeking a live callback generation. */
teder_code td_source_seek(td_source *, uint64_t frame);
/* Repeated bounded steps until OK: one converted sample (or validated EOF)
 * retained privately. No pool binding/publication; seek must precede priming. */
teder_code td_source_prime(td_source *);
/* At most one FLAC frame or bounded conversion per call. OK publishes a block;
 * AGAIN needs another step or pool space; CLOSED means all final PCM published.
 * Pool is borrowed through retirement. Cancellation aborts unpublished PCM.
 * Call reap on this same producer before expecting returned slots to be free. */
teder_code td_source_pump(td_source *, td_pcm_pool *);
teder_code
td_source_retire(td_source **); /* BUSY until PCM returned/reaped; final IO still clears owner */
bool td_source_cancelled(td_source *);
void td_source_checkpoint(td_source *, unsigned);
teder_code td_path_validate(const char *, uint32_t);
teder_code td_file_open(td_source *, uint32_t path_bytes);
teder_code td_file_read(td_source *, void *, size_t *);
teder_code td_file_seek(td_source *, uint64_t);
void td_file_close(td_source *);
ma_data_converter_config td_convert_config(uint32_t in, uint32_t out);
bool td_rate_frames(uint64_t, uint32_t numerator, uint32_t denominator, bool ceil, uint64_t *);
void td_downmix(float *, const int32_t *const *, uint32_t frames, uint32_t channels, uint32_t bits);
#endif
