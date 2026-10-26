#include "source.h"
#include <stdlib.h>
#include <string.h>

static teder_code layout(uint32_t bytes,
                         size_t codec,
                         size_t *capacity,
                         size_t *heap_offset,
                         size_t *path_offset,
                         size_t *native_offset,
                         size_t *total)
{
	if (!bytes || bytes > 32767)
		return TEDER_LIMIT;
	ma_data_converter_config c = td_convert_config(1, 2);
	size_t heap;
	/* The pinned linear converter's heap depends on channels/order, not rates.
	 * A nonmatching pair reserves the full resampler even for passthrough. */
	if (ma_data_converter_get_heap_size(&c, &heap) != MA_SUCCESS)
		return TEDER_INTERNAL;
	*capacity = sizeof(td_source);
	if (!td_layout_add(capacity, 1, heap, _Alignof(max_align_t), heap_offset) ||
	    !td_layout_add(capacity, (size_t)bytes + 1, 1, 1, path_offset) ||
	    !td_layout_add(
	        capacity, (size_t)bytes + 1, sizeof(wchar_t), _Alignof(wchar_t), native_offset) ||
	    !td_arena_size(*capacity, total) || !td_size_add(*total, codec, total))
		return TEDER_LIMIT;
	return TEDER_OK;
}

teder_code td_source_plan(uint32_t bytes, size_t codec, size_t *out)
{
	if (!out)
		return TEDER_INVALID;
	size_t capacity, heap, path, native, total;
	teder_code code = layout(bytes, codec, &capacity, &heap, &path, &native, &total);
	if (code == TEDER_OK)
		*out = total;
	return code;
}

static FLAC__StreamDecoderReadStatus
read_pcm(const FLAC__StreamDecoder *d, FLAC__byte *data, size_t *bytes, void *ctx)
{
	(void)d;
	td_source *s = ctx;
	if (s->error == TEDER_OK)
		s->error = td_file_read(s, data, bytes);
	if (s->error != TEDER_OK) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
	}
	return *bytes ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE
	              : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

static FLAC__StreamDecoderSeekStatus
seek_file(const FLAC__StreamDecoder *d, FLAC__uint64 offset, void *ctx)
{
	(void)d;
	td_source *s = ctx;
	if (s->error == TEDER_OK)
		s->error = td_file_seek(s, offset);
	return s->error == TEDER_OK ? FLAC__STREAM_DECODER_SEEK_STATUS_OK
	                            : FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
}

static FLAC__StreamDecoderTellStatus
tell_file(const FLAC__StreamDecoder *d, FLAC__uint64 *offset, void *ctx)
{
	(void)d;
	*offset = ((td_source *)ctx)->file_cursor;
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus
length_file(const FLAC__StreamDecoder *d, FLAC__uint64 *bytes, void *ctx)
{
	(void)d;
	*bytes = ((td_source *)ctx)->file_size;
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool eof_file(const FLAC__StreamDecoder *d, void *ctx)
{
	(void)d;
	td_source *s = ctx;
	return s->file_cursor >= s->file_size;
}

static void metadata(const FLAC__StreamDecoder *d, const FLAC__StreamMetadata *m, void *ctx)
{
	(void)d;
	td_source *s = ctx;
	if (m->type != FLAC__METADATA_TYPE_STREAMINFO)
		return;
	const FLAC__StreamMetadata_StreamInfo *i = &m->data.stream_info;
	if (s->info.sample_rate) {
		s->error = TEDER_CORRUPT;
		return;
	}
	if (!i->sample_rate || i->sample_rate > 1048575 || !i->channels || i->channels > 8 ||
	    i->bits_per_sample < 4 || i->bits_per_sample > 32 || i->max_blocksize > TD_FLAC_FRAMES) {
		s->error = TEDER_UNSUPPORTED;
		return;
	}
	s->info = (teder_probe_info){.struct_size = sizeof(s->info),
	                             .abi_version = TEDER_ABI_VERSION,
	                             .codec = TEDER_CODEC_FLAC,
	                             .sample_rate = i->sample_rate,
	                             .channels = i->channels,
	                             .bits_per_sample = i->bits_per_sample,
	                             .duration_frames = i->total_samples,
	                             .duration_known = i->total_samples != 0,
	                             .seekable = 1};
}

static void
decoder_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus error, void *ctx)
{
	(void)d;
	(void)error;
	td_source *s = ctx;
	if (s->error == TEDER_OK)
		s->error = TEDER_CORRUPT;
}

static FLAC__StreamDecoderWriteStatus write_pcm(const FLAC__StreamDecoder *d,
                                                const FLAC__Frame *f,
                                                const FLAC__int32 *const pcm[],
                                                void *ctx)
{
	(void)d;
	td_source *s = ctx;
	if (td_source_cancelled(s))
		s->error = TEDER_CANCELLED;
	if (s->error != TEDER_OK)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	if (f->header.channels != s->info.channels ||
	    f->header.bits_per_sample != s->info.bits_per_sample ||
	    f->header.sample_rate != s->info.sample_rate || !f->header.blocksize ||
	    f->header.blocksize > TD_FLAC_FRAMES ||
	    f->header.number_type != FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER ||
	    f->header.number.sample_number != s->decoded || s->input_offset != s->input_frames ||
	    s->decoded > ((UINT64_C(1) << 36) - 1) - f->header.blocksize ||
	    (s->info.duration_known && (s->decoded > s->info.duration_frames ||
	                                f->header.blocksize > s->info.duration_frames - s->decoded))) {
		s->error = TEDER_CORRUPT;
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	}
	td_downmix(s->pcm, pcm, f->header.blocksize, s->info.channels, s->info.bits_per_sample);
	s->input_frames = f->header.blocksize;
	s->input_offset = 0;
	s->decoded += f->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static teder_code checked(td_source *s, bool success)
{
	if (td_source_cancelled(s))
		s->error = TEDER_CANCELLED;
	else if (s->heap.error != TEDER_OK)
		s->error = s->heap.error;
	else if (!success && s->error == TEDER_OK)
		s->error = TEDER_CORRUPT;
	return s->error;
}

teder_code td_source_open(const char *path,
                          uint32_t bytes,
                          const td_source_config *config,
                          td_allocator allocator,
                          td_source **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!config || !allocator.allocate || !allocator.release || !config->generation ||
	    !config->source || config->output_rate > 1048575 || config->read_size > 4096)
		return TEDER_INVALID;
	teder_code code = td_path_validate(path, bytes);
	if (code != TEDER_OK)
		return code;
	size_t capacity, heap, name, native, total;
	code = layout(bytes, config->codec_bytes, &capacity, &heap, &name, &native, &total);
	if (code != TEDER_OK)
		return code;
	if (total > config->reservation)
		return TEDER_LIMIT;
	if (config->cancel && atomic_load(config->cancel))
		return TEDER_CANCELLED;
	td_arena *arena = td_arena_create(allocator, capacity);
	if (!arena)
		return TEDER_NO_MEMORY;
	unsigned char *base = td_arena_alloc(arena, capacity, 1, _Alignof(max_align_t));
	memset(base, 0, capacity);
	td_source *s = (td_source *)base;
	td_pcm_generation_init(
	    &s->generation, arena, config->generation, config->source, config->track);
	td_flac_heap_init(&s->heap, allocator, config->codec_bytes);
	s->config = *config;
	if (!s->config.read_size)
		s->config.read_size = 4096;
	s->slot = TD_PCM_NONE;
	s->path = (char *)base + name;
	s->native_path = base + native;
	memcpy(s->path, path, bytes);
	s->converter_heap = base + heap;
	s->converter_bytes = name - heap;
	s->error = td_file_open(s, bytes);
	unsigned char magic[4];
	size_t cursor = 0;
	while (s->error == TEDER_OK && cursor < sizeof(magic)) {
		size_t count = sizeof(magic) - cursor;
		s->error = td_file_read(s, magic + cursor, &count);
		if (!count && s->error == TEDER_OK)
			s->error = TEDER_CORRUPT;
		cursor += count;
	}
	if (s->error == TEDER_OK && memcmp(magic, "fLaC", 4))
		s->error = TEDER_UNSUPPORTED;
	if (s->error == TEDER_OK)
		s->error = td_file_seek(s, 0);
	td_flac_heap *previous = td_flac_scope(&s->heap);
	if (s->error == TEDER_OK) {
		s->decoder = FLAC__stream_decoder_new();
		bool success =
		    s->decoder && FLAC__stream_decoder_set_md5_checking(s->decoder, true) &&
		    FLAC__stream_decoder_set_metadata_ignore_all(s->decoder) &&
		    FLAC__stream_decoder_set_metadata_respond(s->decoder, FLAC__METADATA_TYPE_STREAMINFO) &&
		    FLAC__stream_decoder_init_stream(s->decoder,
		                                     read_pcm,
		                                     seek_file,
		                                     tell_file,
		                                     length_file,
		                                     eof_file,
		                                     write_pcm,
		                                     metadata,
		                                     decoder_error,
		                                     s) == FLAC__STREAM_DECODER_INIT_STATUS_OK &&
		    FLAC__stream_decoder_process_until_end_of_metadata(s->decoder);
		checked(s,
		        success && s->info.sample_rate &&
		            FLAC__stream_decoder_get_decode_position(s->decoder, &s->frame_end));
	}
	td_flac_scope(previous);
	if (s->error == TEDER_OK) {
		if (!s->config.output_rate)
			s->config.output_rate = s->info.sample_rate;
		/* Very narrow float-filter cutoffs lose coefficient precision. Reject
		 * these conversions before priming or replacing any active source. */
		if ((uint64_t)s->info.sample_rate > 64ull * s->config.output_rate ||
		    (uint64_t)s->config.output_rate > 64ull * s->info.sample_rate)
			s->error = TEDER_UNSUPPORTED;
	}
	if (s->error == TEDER_OK) {
		ma_data_converter_config c = td_convert_config(s->info.sample_rate, s->config.output_rate);
		size_t needed;
		if (ma_data_converter_get_heap_size(&c, &needed) != MA_SUCCESS ||
		    needed > s->converter_bytes)
			s->error = TEDER_LIMIT;
		else if (ma_data_converter_init_preallocated(&c, s->converter_heap, &s->converter) !=
		         MA_SUCCESS)
			s->error = TEDER_UNSUPPORTED;
		else {
			s->converted = true;
			s->skip = ma_data_converter_get_output_latency(&s->converter);
		}
	}
	code = s->error;
	if (code != TEDER_OK) {
		td_source_retire(&s);
		return code;
	}
	*out = s;
	return TEDER_OK;
}

teder_code td_source_seek(td_source *s, uint64_t frame)
{
	if (!s)
		return TEDER_INVALID;
	if (s->published || s->pool || s->priming || s->generation.retiring)
		return TEDER_BAD_STATE;
	if (s->error != TEDER_OK)
		return s->error;
	if (frame >= (UINT64_C(1) << 36) || (s->info.duration_known && frame > s->info.duration_frames))
		return TEDER_INVALID;
	s->start = s->decoded = frame;
	s->input_frames = s->input_offset = 0;
	s->eof = s->info.duration_known && frame == s->info.duration_frames;
	if (td_source_cancelled(s))
		return s->error = TEDER_CANCELLED;
	if (ma_data_converter_reset(&s->converter) != MA_SUCCESS)
		return s->error = TEDER_INTERNAL;
	s->skip = ma_data_converter_get_output_latency(&s->converter);
	if (s->eof)
		return TEDER_OK;
	td_flac_heap *previous = td_flac_scope(&s->heap);
	checked(s, FLAC__stream_decoder_seek_absolute(s->decoder, frame));
	if (s->error == TEDER_OK &&
	    !FLAC__stream_decoder_get_decode_position(s->decoder, &s->frame_end))
		s->error = TEDER_CORRUPT;
	td_flac_scope(previous);
	return s->error;
}

static void advance(td_source *s)
{
	td_flac_heap *previous = td_flac_scope(&s->heap);
	uint64_t before = s->decoded;
	checked(s, FLAC__stream_decoder_process_single(s->decoder));
	if (s->error == TEDER_OK && before != s->decoded &&
	    !FLAC__stream_decoder_get_decode_position(s->decoder, &s->frame_end))
		s->error = TEDER_CORRUPT;
	if (s->error == TEDER_OK &&
	    FLAC__stream_decoder_get_state(s->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM) {
		checked(s, FLAC__stream_decoder_finish(s->decoder));
		if (s->error == TEDER_OK &&
		    (s->frame_end != s->file_size ||
		     (s->info.duration_known && s->decoded != s->info.duration_frames)))
			s->error = TEDER_CORRUPT;
		s->eof = true;
		if (s->error == TEDER_OK) {
			s->info.duration_frames = s->decoded;
			s->info.duration_known = 1;
		}
	}
	td_flac_scope(previous);
}

teder_code td_source_prime(td_source *s)
{
	if (!s || s->pool || s->published || s->generation.retiring)
		return TEDER_BAD_STATE;
	s->priming = true;
	if (td_source_cancelled(s))
		return s->error = TEDER_CANCELLED;
	if (s->error != TEDER_OK)
		return s->error;
	if (s->pending)
		return TEDER_OK;
	uint64_t target = 0;
	if (s->eof) {
		if (!td_rate_frames(
		        s->decoded - s->start, s->config.output_rate, s->info.sample_rate, true, &target))
			return s->error = TEDER_LIMIT;
		if (!target) {
			s->done = true;
			return TEDER_OK;
		}
	}
	ma_uint64 required;
	if (ma_data_converter_get_required_input_frame_count(&s->converter, 1, &required) != MA_SUCCESS)
		return s->error = TEDER_INTERNAL;
	if (!s->eof && s->input_offset == s->input_frames && required) {
		advance(s);
		return s->error == TEDER_OK ? TEDER_AGAIN : s->error;
	}
	float discard[TD_PCM_BLOCK_FRAMES * 2], zero[128] = {0};
	bool padding = s->eof && s->input_offset == s->input_frames;
	ma_uint64 in = padding ? 64 : s->input_frames - s->input_offset;
	ma_uint64 out = s->skip ? (s->skip < TD_PCM_BLOCK_FRAMES ? s->skip : TD_PCM_BLOCK_FRAMES) : 1;
	if (ma_data_converter_process_pcm_frames(&s->converter,
	                                         padding ? zero : s->pcm + s->input_offset * 2,
	                                         &in,
	                                         s->skip ? discard : s->tail,
	                                         &out) != MA_SUCCESS ||
	    (!in && !out))
		return s->error = TEDER_INTERNAL;
	if (!padding)
		s->input_offset += (uint32_t)in;
	if (s->skip)
		s->skip -= out;
	else
		s->pending = out != 0;
	return s->pending ? TEDER_OK : TEDER_AGAIN;
}

static teder_code fail(td_source *s, teder_code code)
{
	s->error = code;
	if (s->slot != TD_PCM_NONE) {
		td_pcm_abort(s->pool, s->slot);
		s->slot = TD_PCM_NONE;
	}
	return code;
}

static teder_code publish(td_source *s, bool final)
{
	uint64_t begin, end;
	if (!td_rate_frames(s->output, s->info.sample_rate, s->config.output_rate, false, &begin) ||
	    !td_rate_frames(
	        s->output + s->filled, s->info.sample_rate, s->config.output_rate, false, &end) ||
	    s->config.output_start > UINT64_MAX - s->output ||
	    s->filled > UINT64_MAX - s->config.output_start - s->output)
		return fail(s, TEDER_LIMIT);
	if (final)
		end = s->decoded - s->start;
	if (end > s->decoded - s->start)
		end = s->decoded - s->start;
	td_pcm_meta m = {s->start + begin,
	                 end - begin,
	                 s->config.output_start + s->output,
	                 s->filled,
	                 (s->published ? 0 : TD_PCM_BOUNDARY | (s->start ? TD_PCM_DISCONTINUITY : 0)) |
	                     (final ? TD_PCM_FINAL : 0) | (!s->filled ? TD_PCM_EMPTY : 0)};
	teder_code code = !s->published && s->config.publish
	                      ? s->config.publish(s->config.context, s->pool, s->slot, &m)
	                      : td_pcm_publish(s->pool, s->slot, &m);
	if (code != TEDER_OK)
		return fail(s, code);
	s->output += s->filled;
	s->filled = 0;
	s->slot = TD_PCM_NONE;
	s->published = true;
	s->done = final;
	return TEDER_OK;
}

teder_code td_source_pump(td_source *s, td_pcm_pool *pool)
{
	if (!s || !pool)
		return TEDER_INVALID;
	if (s->generation.retiring || pool->closed || (s->pool && s->pool != pool))
		return TEDER_BAD_STATE;
	s->pool = pool;
	td_pcm_reap(pool);
	if (td_source_cancelled(s))
		return fail(s, TEDER_CANCELLED);
	if (s->error != TEDER_OK)
		return fail(s, s->error);
	if (s->done) {
		if (!s->config.boundaries || s->published)
			return TEDER_CLOSED;
		teder_code code = td_pcm_acquire(pool, &s->generation, &s->slot);
		return code == TEDER_OK ? publish(s, true) : code;
	}
	if (s->slot == TD_PCM_NONE) {
		teder_code code = td_pcm_acquire(pool, &s->generation, &s->slot);
		if (code != TEDER_OK)
			return code;
		if (s->pending) {
			memcpy(pool->blocks[s->slot].samples, s->tail, sizeof(s->tail));
			s->filled = 1;
			s->pending = false;
		}
	}
	uint64_t target = 0;
	if (s->eof &&
	    !td_rate_frames(
	        s->decoded - s->start, s->config.output_rate, s->info.sample_rate, true, &target))
		return fail(s, TEDER_LIMIT);
	if (s->eof && s->output + s->filled > target)
		return fail(s, TEDER_CORRUPT);
	if (s->eof && s->output + s->filled == target) {
		if (s->filled)
			return publish(s, true);
		td_pcm_abort(pool, s->slot);
		s->slot = TD_PCM_NONE;
		s->done = true;
		return TEDER_CLOSED;
	}
	ma_uint64 required;
	if (ma_data_converter_get_required_input_frame_count(&s->converter, 1, &required) != MA_SUCCESS)
		return fail(s, TEDER_INTERNAL);
	if (!s->eof && s->input_offset == s->input_frames && required) {
		advance(s);
		return s->error == TEDER_OK ? TEDER_AGAIN : fail(s, s->error);
	}
	td_pcm_block *b = &pool->blocks[s->slot];
	float discard[TD_PCM_BLOCK_FRAMES * 2], zero[128] = {0};
	bool lookahead = s->filled == b->capacity,
	     padding = s->eof && s->input_offset == s->input_frames;
	ma_uint64 in = padding ? 64 : s->input_frames - s->input_offset;
	ma_uint64 out = s->skip     ? (s->skip < TD_PCM_BLOCK_FRAMES ? s->skip : TD_PCM_BLOCK_FRAMES)
	                : lookahead ? 1
	                            : b->capacity - s->filled;
	if (s->eof && !s->skip && out > target - s->output - s->filled)
		out = target - s->output - s->filled;
	float *output = s->skip ? discard : lookahead ? s->tail : b->samples + s->filled * 2;
	if (ma_data_converter_process_pcm_frames(
	        &s->converter, padding ? zero : s->pcm + s->input_offset * 2, &in, output, &out) !=
	        MA_SUCCESS ||
	    (!in && !out))
		return fail(s, TEDER_INTERNAL);
	if (!padding)
		s->input_offset += (uint32_t)in;
	if (s->skip)
		s->skip -= out;
	else if (lookahead && out) {
		s->pending = true;
		return publish(s, false);
	} else
		s->filled += (uint32_t)out;
	return TEDER_AGAIN;
}

teder_code td_source_retire(td_source **owner)
{
	if (!owner)
		return TEDER_INVALID;
	if (!*owner)
		return TEDER_OK;
	td_source *s = *owner;
	if (s->slot != TD_PCM_NONE) {
		if (!s->pool->closed)
			td_pcm_abort(s->pool, s->slot);
		s->slot = TD_PCM_NONE;
	}
	td_flac_heap *previous = td_flac_scope(&s->heap);
	if (s->decoder) {
		FLAC__stream_decoder_delete(s->decoder);
		s->decoder = NULL;
	}
	td_flac_scope(previous);
	if (s->heap.live || s->heap.bytes)
		abort();
	if (s->converted) {
		ma_data_converter_uninit(&s->converter, NULL);
		s->converted = false;
	}
	td_file_close(s);
	if (s->pool)
		td_pcm_reap(s->pool);
	bool close_failed = s->close_failed;
	td_pcm_generation *g = &s->generation;
	teder_code code = td_pcm_retire(&g);
	if (code == TEDER_OK) {
		*owner = NULL;
		if (close_failed)
			return TEDER_IO;
	}
	return code;
}
