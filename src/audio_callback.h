#ifndef TEDER_AUDIO_CALLBACK_H
#define TEDER_AUDIO_CALLBACK_H
#include "pcm.h"

/* Caller supplies a coherent snapshot, never a racing copy of mutable control
 * state. This private core owns no configuration publication mechanism. */
typedef struct {
	uint64_t generation;
	float gain;             /* finite, 0..1 */
	uint32_t enabled, mute; /* each 0 or 1 */
} td_audio_config;

typedef struct {
	uint64_t source, track, source_frame; /* last successfully consumed PCM endpoint */
	uint32_t final;                       /* sequence EOF cutoff won in this call */
	uint32_t pcm_frames;                  /* consumed, including muted frames */
	uint32_t missing_frames; /* enabled but unavailable, not an EOF/underrun classification */
} td_audio_result;

/* Sole PCM consumer; stereo interleaved float output, capacity in samples.
 * Pool, result and output must not overlap and must outlive this call. The
 * existing pool/generation protocol owns every sample borrow and retirement.
 * Nothing is retained after return. No allocation, locks, I/O or worker wakes.
 *
 * Invalid output/capacity/result leaves output untouched and consumes nothing. Given
 * a valid output span, even invalid configuration leaves it fully silent.
 * Result is required and reset on entry; zero frames permits NULL output.
 * Generation zero is valid only when disabled and leaves the pool untouched.
 * Disabled calls with a generation return stale blocks but preserve current
 * PCM. FINAL marks a source boundary; its atomic cutoff decides sequence EOF.
 * output_frame is the submitted index at the start of this call. Empty markers
 * advance identity without advancing the sample count.
 * An internal pool error may follow partial consumption; pcm_frames records
 * completed consume operations and the entire output is silenced. Missing counts are set only
 * on success. Native device control/public status integration is separate. */
teder_code td_audio_render(td_pcm_pool *,
                           td_audio_config,
                           float *output,
                           size_t sample_capacity,
                           uint32_t frames,
                           uint64_t output_frame,
                           td_audio_result *);
#endif
