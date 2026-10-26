#include "device.h"

_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
               "Callback reports require lock-free 64-bit atomics");

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
	td_audio_result result;
	teder_code code =
	    td_audio_render(feed->pool, config, output, (size_t)frames * 2, frames, &result);
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
		atomic_store(&feed->final, feed->report.final);
		atomic_store(&feed->control, feed->report.control);
		atomic_store(&feed->error, feed->report.error);
		atomic_store(&feed->sequence, sequence + 2);
	} else
		atomic_store(&feed->sequence, UINT64_MAX); /* sticky exhaustion, never ABA */
	atomic_fetch_add_explicit(&feed->calls, 1, memory_order_release);
}
