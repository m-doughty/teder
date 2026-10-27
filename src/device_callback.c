#include "device.h"

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 &&
                   ATOMIC_LLONG_LOCK_FREE == 2,
               "Callback reports require lock-free 64-bit atomics");

void td_device_notification(const ma_device_notification *notification)
{
	td_device_feed *feed = notification->pDevice->pUserData;
	if ((notification->type == ma_device_notification_type_stopped &&
	     atomic_load(&feed->expected_running)) ||
	    notification->type == ma_device_notification_type_rerouted ||
	    notification->type == ma_device_notification_type_interruption_began)
		atomic_store(&feed->lost, 1);
}

static uint64_t add(uint64_t value, uint32_t frames)
{
	return value > UINT64_MAX - frames ? UINT64_MAX : value + frames;
}

void td_device_callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
	(void)input;
	td_device_feed *feed = device->pUserData;
	uint64_t wanted = atomic_load(&feed->wanted);
	if (!(wanted & 1)) {
		uint64_t settings = atomic_load(&feed->settings);
		if (wanted == atomic_load(&feed->wanted)) {
			feed->applied = settings;
			feed->report.control = wanted;
		}
	}
	td_audio_config config = feed->config;
	uint32_t bits = (uint32_t)feed->applied;
	memcpy(&config.gain, &bits, sizeof(bits));
	config.mute = (uint32_t)(feed->applied >> 32) & 1;
	config.enabled = (uint32_t)(feed->applied >> 33) & 1;
	if (feed->report.final || atomic_load(&feed->lost))
		config.enabled = 0;
	td_audio_result result;
	teder_code code = td_audio_render(feed->pool,
	                                  config,
	                                  output,
	                                  (size_t)frames * 2,
	                                  frames,
	                                  feed->report.output_frames,
	                                  &result,
	                                  feed->report.error == TEDER_OK ? feed->analysis : NULL,
	                                  device->sampleRate);
	if (code == TEDER_OK && feed->report.error == TEDER_OK && config.enabled &&
	    !feed->report.final) {
		/* Intentional pause/EOF silence is outside the playback clock. A short
		 * final buffer ends at its last PCM frame, not at the buffer's padding. */
		uint32_t submitted = result.final ? result.pcm_frames : frames;
		uint32_t missing = result.final ? 0 : result.missing_frames;
		if (feed->report.output_frames > UINT64_MAX - submitted ||
		    feed->report.underrun_frames > UINT64_MAX - missing)
			code = TEDER_LIMIT;
		feed->report.output_frames = add(feed->report.output_frames, submitted);
		feed->report.underrun_frames = add(feed->report.underrun_frames, missing);
		if (result.source) {
			feed->report.source = result.source;
			feed->report.track = result.track;
			feed->report.source_frame = result.source_frame;
		}
	}
	feed->report.pcm_frames = add(feed->report.pcm_frames, result.pcm_frames);
	feed->report.missing_frames = add(feed->report.missing_frames, result.missing_frames);
	feed->report.final |= result.final;
	if (feed->report.error == TEDER_OK)
		feed->report.error = code;
	/* All payload fields are atomic: a racing failed read is still defined C.
	 * Sequential consistency brackets one coherent snapshot without retries. */
	uint64_t sequence = atomic_load(&feed->sequence);
	if (sequence < UINT64_MAX - 1) {
		atomic_store(&feed->sequence, sequence + 1);
		atomic_store(&feed->pcm_frames, feed->report.pcm_frames);
		atomic_store(&feed->missing_frames, feed->report.missing_frames);
		atomic_store(&feed->source, feed->report.source);
		atomic_store(&feed->track, feed->report.track);
		atomic_store(&feed->source_frame, feed->report.source_frame);
		atomic_store(&feed->output_frames, feed->report.output_frames);
		atomic_store(&feed->underrun_frames, feed->report.underrun_frames);
		atomic_store(&feed->final, feed->report.final);
		atomic_store(&feed->control, feed->report.control);
		atomic_store(&feed->error, feed->report.error);
		atomic_store(&feed->sequence, sequence + 2);
	} else
		atomic_store(&feed->sequence, UINT64_MAX); /* sticky exhaustion, never ABA */
	atomic_fetch_add_explicit(&feed->calls, 1, memory_order_release);
}
