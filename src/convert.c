#include "source.h"
#include <math.h>
ma_data_converter_config td_convert_config(uint32_t in, uint32_t out)
{
	ma_data_converter_config c = ma_data_converter_config_init(ma_format_f32, ma_format_f32, 2, 2, in, out);
	c.resampling.algorithm = ma_resample_algorithm_linear;
	c.resampling.linear.lpfOrder = 8;
	return c;
}
bool td_rate_frames(uint64_t n, uint32_t num, uint32_t den, bool ceil, uint64_t *out)
{
	if (!num || !den || n / den > UINT64_MAX / num) return false;
	uint64_t tail = (n % den) * num, result = (n / den) * num;
	tail = tail / den + (ceil && tail % den != 0);
	if (tail > UINT64_MAX - result) return false;
	*out = result + tail; return true;
}
void td_downmix(float *out, const int32_t *const *in, uint32_t frames, uint32_t channels, uint32_t bits)
{
	/* FLAC order: mono; LR; LRC; LRBLBR; LRCBLBR; LRCLFEBLBR;
	 * LRCLFEBCSLSR; LRCLFEBLBRSLSR. LFE is deliberately excluded.
	 * Normalize each side by its weight sum to preserve headroom. */
	float weights[8][2] = {{1,0},{0,1}}, scale = ldexpf(1.0f, 1 - (int)bits);
	const float surround = 0.7071067811865475f;
	if (channels == 1) weights[0][1] = 1;
	if (channels == 3 || channels >= 5) weights[2][0] = weights[2][1] = surround;
	if (channels == 4 || channels == 5 || channels == 6 || channels == 8) {
		unsigned back = channels == 4 ? 2 : channels == 5 ? 3 : 4;
		weights[back][0] = surround; weights[back+1][1] = surround;
	}
	if (channels == 7) weights[4][0] = weights[4][1] = 0.5f;
	if (channels >= 7) { weights[channels-2][0] = surround; weights[channels-1][1] = surround; }
	float sum[2] = {0};
	for (unsigned c = 0; c < channels; c++) for (unsigned side = 0; side < 2; side++) sum[side] += weights[c][side];
	for (unsigned i = 0; i < frames; i++) for (unsigned side = 0; side < 2; side++) {
		float value = 0;
		for (unsigned c = 0; c < channels; c++) value += ((float)in[c][i] * scale) * weights[c][side];
		out[i*2+side] = value / sum[side];
	}
}
