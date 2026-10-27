#include "audio_callback.h"
#include <string.h>

_Static_assert(SIZE_MAX / sizeof(float) / 2 >= UINT32_MAX,
               "Callback frame counts require a 64-bit address space");

teder_code td_audio_render(td_pcm_pool *pool,
                           td_audio_config config,
                           float *output,
                           size_t sample_capacity,
                           uint32_t frames,
                           uint64_t output_frame,
                           td_audio_result *result,
                           td_analysis_feed *analysis,
                           uint32_t output_rate)
{
	if (!result)
		return TEDER_INVALID;
	*result = (td_audio_result){0};
	size_t samples = (size_t)frames * 2;
	if ((frames && !output) || sample_capacity < samples)
		return TEDER_INVALID;
	if (frames)
		memset(output, 0, samples * sizeof(*output));
	if (!pool || !(config.gain >= 0 && config.gain <= 1) || config.enabled > 1 || config.mute > 1 ||
	    (config.enabled && !config.generation))
		return TEDER_INVALID;
	td_analysis_write analysis_write = td_analysis_begin(
	    analysis, config.generation, output_rate, output_frame, config.enabled && frames);
	if (!config.generation)
		return TEDER_OK;
	teder_code code = td_pcm_select(pool, config.generation);
	if (code != TEDER_OK || !frames)
		return code;
	td_pcm_view view;
	if (!config.enabled) {
		code = td_pcm_peek(pool, &view);
		return code == TEDER_AGAIN ? TEDER_OK : code;
	}
	uint32_t empty = 0;
	while (result->pcm_frames < frames && empty < pool->count) {
		code = td_pcm_peek(pool, &view);
		if (code == TEDER_AGAIN)
			break;
		if (code != TEDER_OK)
			goto failed;
		if (!view.frames)
			empty++;
		uint32_t count = frames - result->pcm_frames;
		if (count > view.frames)
			count = view.frames;
		if (!config.mute && config.gain != 0) {
			float *target = output + (size_t)result->pcm_frames * 2;
			for (size_t i = 0; i < (size_t)count * 2; i++)
				target[i] = view.samples[i] * config.gain;
		}
		bool final = count == view.frames && (view.meta->flags & TD_PCM_FINAL);
		uint64_t source = view.generation->source, track = view.generation->track;
		uint64_t span = view.meta->source_frames, length = view.meta->frames;
		uint64_t offset = (uint64_t)view.offset + count;
		/* Quotient/remainder avoids overflow even for the largest valid span.
		 * Matching rates are exact; converted block ranges are estimates. */
		uint64_t position =
		    view.meta->source_start +
		    (length ? (span / length) * offset + (span % length) * offset / length : 0);
		if (output_frame > UINT64_MAX - result->pcm_frames) {
			code = TEDER_LIMIT;
			goto failed;
		}
		final = td_pcm_mark(pool, output_frame + result->pcm_frames, final);
		/* No view access after consume: the producer may reclaim immediately. */
		code = td_pcm_consume(pool, count);
		if (code != TEDER_OK)
			goto failed;
		td_analysis_append(
		    &analysis_write, output + (size_t)result->pcm_frames * 2, count, source, track);
		result->pcm_frames += count;
		result->source = source;
		result->track = track;
		result->source_frame = position;
		result->final = final;
		if (final)
			break;
	}
	result->missing_frames = frames - result->pcm_frames;
	if (!result->final)
		td_analysis_append(
		    &analysis_write, output + (size_t)result->pcm_frames * 2, result->missing_frames, 0, 0);
	td_analysis_finish(&analysis_write);
	return TEDER_OK;
failed:
	memset(output, 0, samples * sizeof(*output));
	return code;
}
