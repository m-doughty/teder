#include "spectrum.h"
#include <kiss_fftr.h>
#include <math.h>
#include <string.h>

_Static_assert(_Generic((kiss_fft_scalar)0, float: 1, default: 0), "KissFFT must use float");
static const double tau = 6.283185307179586476925286766559;

struct td_spectrum {
	td_spectrum_config config;
	kiss_fftr_cfg fft;
	float *window, *input, *bands;
	kiss_fft_cpx *frequency;
	double *power, *edges, window_energy;
	uint32_t rate;
};

typedef struct {
	size_t bytes, plan_bytes, plan, window, input, frequency, power, edges, bands;
} layout;

static teder_code plan(const td_spectrum_config *c, layout *l)
{
	if (!c || !c->bands || c->frames < TD_FFT_MIN || (c->frames & (c->frames - 1)) ||
	    !isfinite(c->low_hz) || c->low_hz <= 0 || !isfinite(c->high_hz) ||
	    (c->high_hz != 0 && c->high_hz <= c->low_hz) || !isfinite(c->floor_db) || c->floor_db >= 0)
		return TEDER_INVALID;
	if (c->frames > TD_FFT_MAX || c->bands > TD_FFT_BANDS_MAX)
		return TEDER_LIMIT;
	*l = (layout){.bytes = sizeof(td_spectrum)};
	/* The bounded power-of-two size keeps KissFFT's internal size math in range.
	 * A non-NULL length requests caller storage; even the sizing call cannot malloc. */
	if (kiss_fftr_alloc((int)c->frames, 0, NULL, &l->plan_bytes) || !l->plan_bytes)
		return TEDER_INTERNAL;
	size_t bins = (size_t)c->frames / 2 + 1;
	if (!td_layout_add(&l->bytes, 1, l->plan_bytes, _Alignof(max_align_t), &l->plan) ||
	    !td_layout_add(&l->bytes, c->frames, sizeof(float), _Alignof(float), &l->window) ||
	    !td_layout_add(&l->bytes, c->frames, sizeof(float), _Alignof(float), &l->input) ||
	    !td_layout_add(
	        &l->bytes, bins, sizeof(kiss_fft_cpx), _Alignof(kiss_fft_cpx), &l->frequency) ||
	    !td_layout_add(&l->bytes, bins + 1, sizeof(double), _Alignof(double), &l->power) ||
	    !td_layout_add(
	        &l->bytes, (size_t)c->bands + 1, sizeof(double), _Alignof(double), &l->edges) ||
	    !td_layout_add(&l->bytes, c->bands, sizeof(float), _Alignof(float), &l->bands))
		return TEDER_LIMIT;
	return TEDER_OK;
}

teder_code td_spectrum_plan(size_t *used, const td_spectrum_config *c, size_t *offset)
{
	if (!used || !offset || used == offset)
		return TEDER_INVALID;
	layout l;
	teder_code code = plan(c, &l);
	if (code == TEDER_OK && !td_layout_add(used, 1, l.bytes, _Alignof(max_align_t), offset))
		code = TEDER_LIMIT;
	return code;
}

teder_code
td_spectrum_init(void *storage, size_t bytes, const td_spectrum_config *c, td_spectrum **out)
{
	if (!out)
		return TEDER_INVALID;
	*out = NULL;
	if (!storage || (uintptr_t)storage % _Alignof(max_align_t))
		return TEDER_INVALID;
	layout l;
	teder_code code = plan(c, &l);
	if (code != TEDER_OK)
		return code;
	if (bytes < l.bytes)
		return TEDER_LIMIT;
	unsigned char *base = storage;
	kiss_fftr_cfg fft = kiss_fftr_alloc((int)c->frames, 0, base + l.plan, &l.plan_bytes);
	if (!fft)
		return TEDER_INTERNAL;
	td_spectrum *s = storage;
	*s = (td_spectrum){.config = *c,
	                   .fft = fft,
	                   .window = (float *)(base + l.window),
	                   .input = (float *)(base + l.input),
	                   .frequency = (kiss_fft_cpx *)(base + l.frequency),
	                   .power = (double *)(base + l.power),
	                   .edges = (double *)(base + l.edges),
	                   .bands = (float *)(base + l.bands)};
	for (uint32_t i = 0; i < c->frames; i++) {
		s->window[i] = (float)(0.5 - 0.5 * cos(tau * i / c->frames));
		s->window_energy += (double)s->window[i] * s->window[i];
	}
	for (uint32_t i = 0; i < c->bands; i++)
		s->bands[i] = c->floor_db;
	*out = s;
	return TEDER_OK;
}

/* Cumulative power over bin cells: DC/Nyquist have half-width cells, all other
 * cells extend half a bin either side. Fractional edges conserve included energy
 * without pretending that narrow bands increase FFT frequency resolution. */
static double integral(const td_spectrum *s, double edge)
{
	uint32_t last = s->config.frames / 2;
	if (edge <= 0)
		return 0;
	if (edge >= last)
		return s->power[last + 1];
	uint32_t bin = (uint32_t)floor(edge + 0.5);
	double left = bin ? bin - 0.5 : 0;
	double width = bin == 0 || bin == last ? 0.5 : 1;
	return s->power[bin] + (s->power[bin + 1] - s->power[bin]) * (edge - left) / width;
}

teder_code td_spectrum_measure(
    td_spectrum *s, const float *pcm, uint32_t frames, uint32_t rate, td_spectrum_values *out)
{
	if (!s || !pcm || !out || !rate || frames != s->config.frames)
		return TEDER_INVALID;
	td_spectrum_config c = s->config;
	double high = c.high_hz ? c.high_hz : rate * 0.5;
	if (high <= c.low_hz)
		return TEDER_INVALID;
	td_spectrum_values result = {.bands = s->bands, .count = c.bands};
	double squares[2] = {0};
	/* Validate the whole borrow before overwriting a prior result. Scaling each
	 * channel before FFT keeps finite extreme inputs from overflowing float FFTs. */
	for (uint32_t i = 0; i < frames; i++)
		for (unsigned ch = 0; ch < 2; ch++) {
			float sample = pcm[2 * i + ch];
			if (!isfinite(sample))
				return TEDER_INVALID;
			result.peak[ch] = fmaxf(result.peak[ch], fabsf(sample));
			squares[ch] += (double)sample * sample;
		}
	if (s->rate != rate) {
		double low = log(c.low_hz), span = log(high) - low;
		for (uint32_t i = 0; i <= c.bands; i++) {
			double hz = i == 0 ? c.low_hz : i == c.bands ? high : exp(low + span * i / c.bands);
			s->edges[i] = hz * frames / rate;
		}
		s->rate = rate;
	}
	uint32_t bins = frames / 2 + 1;
	memset(s->power, 0, ((size_t)bins + 1) * sizeof(*s->power));
	for (unsigned ch = 0; ch < 2; ch++) {
		result.rms[ch] = (float)sqrt(squares[ch] / frames);
		double peak = result.peak[ch];
		if (!peak)
			continue;
		for (uint32_t i = 0; i < frames; i++)
			s->input[i] = (float)(pcm[2 * i + ch] / peak * s->window[i]);
		kiss_fftr(s->fft, s->input, s->frequency);
		double scale = peak * peak / (2 * frames * s->window_energy);
		for (uint32_t k = 0; k < bins; k++) {
			double re = s->frequency[k].r, im = s->frequency[k].i;
			double one_sided = k == 0 || k == bins - 1 ? 1 : 2;
			s->power[k + 1] += (re * re + im * im) * scale * one_sided;
		}
	}
	for (uint32_t k = 1; k <= bins; k++)
		s->power[k] += s->power[k - 1];
	for (uint32_t i = 0; i < c.bands; i++) {
		double power = integral(s, s->edges[i + 1]) - integral(s, s->edges[i]);
		/* 0 dB is mean-square 0.5: a unit-peak sine in both channels, with all
		 * its windowed energy inside one band. Clipped signals may exceed 0 dB. */
		s->bands[i] = power > 0 ? (float)fmax(c.floor_db, 10 * log10(2 * power)) : c.floor_db;
	}
	*out = result;
	return TEDER_OK;
}
