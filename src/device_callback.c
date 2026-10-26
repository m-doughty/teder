#include "device.h"

static uint64_t add(uint64_t value, uint32_t frames)
{
	return value > UINT64_MAX - frames ? UINT64_MAX : value + frames;
}

void td_device_callback(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
	(void)input;
	td_device_feed *feed = device->pUserData;
	td_audio_result result;
	teder_code code =
	    td_audio_render(feed->pool, feed->config, output, (size_t)frames * 2, frames, &result);
	feed->report.pcm_frames = add(feed->report.pcm_frames, result.pcm_frames);
	feed->report.missing_frames = add(feed->report.missing_frames, result.missing_frames);
	if (feed->report.error == TEDER_OK)
		feed->report.error = code;
	atomic_fetch_add_explicit(&feed->calls, 1, memory_order_release);
}
