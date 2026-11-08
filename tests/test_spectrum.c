#include "spectrum.h"
#include "support/harness.h"
#include <check.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef TD_FFT_AUDIT
#include "support/fft_alloc.h"
#include <kiss_fftr.h>
static unsigned fft_allocations, fft_releases;
void *th_fft_allocate(size_t bytes)
{
	(void)bytes;
	fft_allocations++;
	return NULL;
}
void th_fft_release(void *p)
{
	(void)p;
	fft_releases++;
}
#endif
#define OK(call) ck_assert_msg((call) == TEDER_OK, "%s failed", #call)
static const double pi = 3.1415926535897932384626433832795;

typedef struct {
	th_tracker tracker;
	td_arena *arena;
	td_spectrum *spectrum;
	float *pcm;
	size_t bytes, offset, pcm_offset;
	unsigned char *base;
} fixture;

static td_spectrum_config config(unsigned frames, unsigned bands)
{
	return (td_spectrum_config){frames, bands, 20, 20000, -90};
}

static void create(fixture *f, td_spectrum_config c)
{
	memset(f, 0, sizeof(*f));
	ck_assert(th_tracker_init(&f->tracker));
	f->bytes = 13; /* unrelated containing-owner bytes precede aligned FFT storage */
	OK(td_spectrum_plan(&f->bytes, &c, &f->offset));
	ck_assert(td_layout_add(
	    &f->bytes, (size_t)c.frames * 2, sizeof(float), _Alignof(float), &f->pcm_offset));
	f->arena = td_arena_create(th_allocator(&f->tracker), f->bytes + 16);
	ck_assert_ptr_nonnull(f->arena);
	f->base = td_arena_alloc(f->arena, f->bytes + 16, 1, _Alignof(max_align_t));
	memset(f->base, 0xa5, f->bytes + 16);
	f->pcm = (float *)(f->base + f->pcm_offset);
	OK(td_spectrum_init(f->base + f->offset, f->pcm_offset - f->offset, &c, &f->spectrum));
	f->tracker.fail_at = f->tracker.attempts + 1; /* all subsequent work is preallocated */
}

static void dispose(fixture *f)
{
	for (size_t i = 0; i < f->offset; i++)
		ck_assert(f->base[i] == 0xa5);
	for (size_t i = f->bytes; i < f->bytes + 16; i++)
		ck_assert(f->base[i] == 0xa5);
	ck_assert(f->tracker.live == 1 && f->tracker.attempts == 1);
	td_arena_destroy(f->arena);
	ck_assert(th_tracker_clean(&f->tracker));
	th_tracker_uninit(&f->tracker);
}

static void tone(fixture *f, unsigned n, double cycles, double left, double right)
{
	for (unsigned i = 0; i < n; i++) {
		double value = cos(2 * pi * cycles * i / n);
		f->pcm[2 * i] = (float)(left * value);
		f->pcm[2 * i + 1] = (float)(right * value);
	}
}

enum { REFERENCE_FRAMES = 2048 };

/* Independent O(N^2) DFT of the Hann-windowed stereo block, with no KissFFT or
 * production window. power[k] is the two-channel mean power of bin k, scaled so
 * a full-scale tone of one channel reads 1/2. The transform sums products of
 * pcm and cos/sin(2 pi k i / N), indexed through an N-entry table. */
static void reference_power(const float *pcm, unsigned frames, double *power)
{
	ck_assert(frames <= REFERENCE_FRAMES && !(frames & (frames - 1)));
	double window[REFERENCE_FRAMES], cosine[REFERENCE_FRAMES], sine[REFERENCE_FRAMES], energy = 0;
	for (unsigned i = 0; i < frames; i++) {
		window[i] = pow(sin(pi * i / frames), 2);
		cosine[i] = cos(2 * pi * i / frames);
		sine[i] = sin(2 * pi * i / frames);
		energy += window[i] * window[i];
	}
	for (unsigned k = 0; k <= frames / 2; k++) {
		power[k] = 0;
		for (unsigned ch = 0; ch < 2; ch++) {
			double re = 0, im = 0;
			for (unsigned i = 0; i < frames; i++) {
				double sample = pcm[2 * i + ch] * window[i];
				unsigned phase = (unsigned)(((uint64_t)k * i) % frames);
				re += sample * cosine[phase];
				im -= sample * sine[phase];
			}
			double factor = k == 0 || k == frames / 2 ? 1 : 2;
			power[k] += factor * (re * re + im * im) / (2 * frames * energy);
		}
	}
}

/* Explicit per-cell integration of that power over each band's log-spaced
 * edges; no cached edges or cumulative integral helper. */
static void reference_bands(const double *power, td_spectrum_config c, unsigned rate, float *bands)
{
	double high = c.high_hz ? c.high_hz : rate / 2.0;
	double bin_hz = (double)rate / c.frames;
	for (unsigned b = 0; b < c.bands; b++) {
		double low_edge = c.low_hz * pow(high / c.low_hz, (double)b / c.bands);
		double high_edge = c.low_hz * pow(high / c.low_hz, (double)(b + 1) / c.bands);
		double sum = 0;
		for (unsigned k = 0; k <= c.frames / 2; k++) {
			double low_cell = fmax(0, (k - .5) * bin_hz);
			double high_cell = fmin(rate / 2.0, (k + .5) * bin_hz);
			double width = fmax(0, fmin(high_edge, high_cell) - fmax(low_edge, low_cell));
			sum += power[k] * width / (high_cell - low_cell);
		}
		bands[b] = sum > 0 ? (float)fmax(c.floor_db, 10 * log10(2 * sum)) : c.floor_db;
	}
}

START_TEST(bounds)
{
	td_spectrum_config c = config(64, 8);
	size_t used = 13, offset = 99;
	ck_assert(td_spectrum_plan(NULL, &c, &offset) == TEDER_INVALID);
	ck_assert(td_spectrum_plan(&used, &c, &used) == TEDER_INVALID && used == 13);
	for (unsigned mode = 0; mode < 10; mode++) {
		td_spectrum_config bad = c;
		teder_code expected = TEDER_INVALID;
		switch (mode) {
		case 0:
			bad.frames = 0;
			break;
		case 1:
			bad.frames = 30;
			break;
		case 2:
			bad.frames = 48;
			break;
		case 3:
			bad.frames = 131072;
			expected = TEDER_LIMIT;
			break;
		case 4:
			bad.bands = 0;
			break;
		case 5:
			bad.bands = TD_FFT_BANDS_MAX + 1;
			expected = TEDER_LIMIT;
			break;
		case 6:
			bad.low_hz = NAN;
			break;
		case 7:
			bad.high_hz = INFINITY;
			break;
		case 8:
			bad.high_hz = bad.low_hz;
			break;
		case 9:
			bad.floor_db = 0;
			break;
		}
		ck_assert(td_spectrum_plan(&used, &bad, &offset) == expected && used == 13 && offset == 99);
	}
	used = SIZE_MAX - 1;
	ck_assert(td_spectrum_plan(&used, &c, &offset) == TEDER_LIMIT && used == SIZE_MAX - 1 &&
	          offset == 99);
	used = 0;
	OK(td_spectrum_plan(&used, &c, &offset));
	th_tracker tracker;
	ck_assert(th_tracker_init(&tracker));
	tracker.fail_at = 1;
	ck_assert(!td_arena_create(th_allocator(&tracker), used) && th_tracker_clean(&tracker));
	tracker.fail_at = 0;
	td_arena *arena = td_arena_create(th_allocator(&tracker), used);
	ck_assert_ptr_nonnull(arena);
	unsigned char *base = td_arena_alloc(arena, used, 1, _Alignof(max_align_t));
	memset(base, 0xa5, used);
	td_spectrum *s = NULL;
	ck_assert(td_spectrum_init(base, used - 1, &c, &s) == TEDER_LIMIT && !s);
	ck_assert(td_spectrum_init(base + 1, used, &c, &s) == TEDER_INVALID && !s);
	for (size_t i = 0; i < used; i++)
		ck_assert(base[i] == 0xa5);
	OK(td_spectrum_init(base, used, &c, &s));
	ck_assert(s == (td_spectrum *)base && tracker.live == 1 && tracker.attempts == 2);
	td_arena_destroy(arena);
	ck_assert(th_tracker_clean(&tracker));
	th_tracker_uninit(&tracker);
}
END_TEST

START_TEST(dft)
{
	const unsigned rates[] = {44100, 48000, 96000, 1000, 48000};
	for (unsigned n = 32; n <= REFERENCE_FRAMES; n *= 2) {
		td_spectrum_config c = config(n, 37); /* bands narrower than some FFT cells */
		fixture f;
		create(&f, c);
		for (unsigned i = 0; i < n; i++) {
			f.pcm[2 * i] = (float)(.13 + .7 * sin(2 * pi * 3.2 * i / n));
			f.pcm[2 * i + 1] = (float)(.27 * cos(2 * pi * 5.7 * i / n));
		}
		double power[REFERENCE_FRAMES / 2 + 1];
		reference_power(f.pcm, n, power);
		for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
			float expected[37];
			reference_bands(power, c, rates[r], expected);
			td_spectrum_values got;
			OK(td_spectrum_measure(f.spectrum, f.pcm, n, rates[r], &got));
			ck_assert(got.count == c.bands);
			for (unsigned b = 0; b < c.bands; b++)
				ck_assert_msg(fabs(got.bands[b] - expected[b]) < .03,
				              "N=%u rate=%u band=%u got=%g expected=%g",
				              n,
				              rates[r],
				              b,
				              (double)got.bands[b],
				              (double)expected[b]);
		}
		dispose(&f);
	}
}
END_TEST

/* Fill one block. Mode 0 is DC and 1 the Nyquist alternation. Modes 2 to 4 are
 * single impulses where the Hann window is 0, 1/2 and 1, and mode 5 mixes DC,
 * Nyquist, a bin-centred tone and an off-bin tone with unequal channels. */
static void edge_signal(fixture *f, unsigned n, unsigned mode)
{
	memset(f->pcm, 0, (size_t)n * 2 * sizeof(float));
	if (mode >= 2 && mode <= 4) {
		unsigned at = mode == 2 ? 0 : mode == 3 ? n / 4 : n / 2;
		f->pcm[2 * at] = 1;
		f->pcm[2 * at + 1] = -.5f;
		return;
	}
	for (unsigned i = 0; i < n; i++) {
		double alternate = i & 1 ? -1 : 1;
		if (mode == 0) {
			f->pcm[2 * i] = .5f;
			f->pcm[2 * i + 1] = -.25f;
		} else if (mode == 1) {
			f->pcm[2 * i] = (float)alternate;
			f->pcm[2 * i + 1] = (float)(.5 * alternate);
		} else {
			f->pcm[2 * i] = (float)(.1 + .2 * alternate + .3 * cos(2 * pi * (n / 8) * i / n) +
			                        .2 * sin(2 * pi * 5.5 * i / n));
			f->pcm[2 * i + 1] = (float)(-.2 + .1 * alternate + .4 * sin(2 * pi * (n / 4) * i / n));
		}
	}
}

START_TEST(dft_edges)
{
	const unsigned rates[] = {48000, 44100, 1000};
	static float signals[6][2 * REFERENCE_FRAMES];
	static double powers[6][REFERENCE_FRAMES / 2 + 1];
	for (unsigned n = 256; n <= REFERENCE_FRAMES; n *= 2) {
		/* The direct DFT depends only on the block, so each signal's power is computed
		 * once per size and reused for every layout and rate. */
		for (unsigned mode = 0; mode < 6; mode++) {
			fixture scratch = {.pcm = signals[mode]};
			edge_signal(&scratch, n, mode);
			reference_power(signals[mode], n, powers[mode]);
		}
		for (unsigned layout = 0; layout < 4; layout++) {
			for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
				/* Band edges on the DC cell's top, on cell boundaries either side of the
				 * Nyquist cell, inside four cells, and the default range as 64 bands. */
				double bin_hz = (double)rates[r] / n;
				td_spectrum_config c = config(n, layout == 3 ? 64 : layout + 1);
				if (layout == 0) {
					c.low_hz = (float)(1.5 * bin_hz);
					c.high_hz = (float)((n / 2 - .5) * bin_hz);
				} else if (layout == 1) {
					c.low_hz = (float)(.5 * bin_hz);
					c.high_hz = 0;
				} else if (layout == 2) {
					c.low_hz = (float)(n / 4 * bin_hz);
					c.high_hz = (float)((n / 4 + 3) * bin_hz);
					c.floor_db = -120;
				}
				fixture f;
				create(&f, c);
				for (unsigned mode = 0; mode < 6; mode++) {
					edge_signal(&f, n, mode);
					ck_assert(!memcmp(f.pcm, signals[mode], (size_t)n * 2 * sizeof(float)));
					float expected[64];
					reference_bands(powers[mode], c, rates[r], expected);
					td_spectrum_values got;
					OK(td_spectrum_measure(f.spectrum, f.pcm, n, rates[r], &got));
					ck_assert(got.count == c.bands);
					for (unsigned b = 0; b < c.bands; b++)
						ck_assert_msg(fabs(got.bands[b] - expected[b]) < .03,
						              "N=%u layout=%u rate=%u mode=%u band=%u got=%g expected=%g",
						              n,
						              layout,
						              rates[r],
						              mode,
						              b,
						              (double)got.bands[b],
						              (double)expected[b]);
				}
				dispose(&f);
			}
		}
	}
}
END_TEST

START_TEST(signals)
{
	td_spectrum_config c = config(256, 1);
	c.low_hz = FLT_TRUE_MIN; /* effectively the full one-sided frequency range */
	c.high_hz = 0;
	fixture f;
	create(&f, c);
	for (unsigned mode = 0; mode < 8; mode++) {
		tone(&f, c.frames, 16, 1, mode == 1 ? -1 : mode == 2 ? 0 : 1);
		double expected = mode == 2 ? 10 * log10(.5) : 0;
		if (mode == 3) {
			tone(&f, c.frames, 16, .5, .5);
			expected = 10 * log10(.25);
		} else if (mode == 4 || mode == 5) {
			tone(&f, c.frames, mode == 4 ? 0 : c.frames / 2, 1, 1);
			expected = 10 * log10(2);
		} else if (mode == 6 || mode == 7) {
			memset(f.pcm, 0, c.frames * 2 * sizeof(float));
			if (mode == 6) {
				f.pcm[c.frames] = f.pcm[c.frames + 1] = 1;
				expected = 10 * log10(16.0 / (3 * c.frames));
			} else
				expected = c.floor_db;
		}
		td_spectrum_values got;
		OK(td_spectrum_measure(f.spectrum, f.pcm, c.frames, 48000, &got));
		ck_assert_msg(fabs(got.bands[0] - expected) < .0001,
		              "mode=%u got=%g expected=%g",
		              mode,
		              (double)got.bands[0],
		              expected);
		double peak = mode == 7 ? 0 : mode == 3 ? .5 : 1;
		double rms = mode >= 4 && mode <= 5 ? 1
		             : mode == 6            ? 1.0 / 16
		             : mode == 7            ? 0
		                                    : peak / sqrt(2);
		ck_assert(fabs(got.peak[0] - peak) < .000001 && fabs(got.rms[0] - rms) < .000001);
		ck_assert(fabs(got.rms[1] - (mode == 2 ? 0 : rms)) < .000001);
	}
	dispose(&f);
}
END_TEST

START_TEST(validation)
{
	td_spectrum_config c = config(32, 8);
	fixture f;
	create(&f, c);
	tone(&f, c.frames, 3, FLT_MAX, -FLT_MAX);
	td_spectrum_values got;
	OK(td_spectrum_measure(f.spectrum, f.pcm, c.frames, 48000, &got));
	float saved[8];
	memcpy(saved, got.bands, sizeof(saved));
	ck_assert(got.peak[0] == FLT_MAX && isfinite(got.rms[0]));
	for (unsigned b = 0; b < 8; b++)
		ck_assert(isfinite(saved[b]));
	for (unsigned mode = 0; mode < 5; mode++) {
		td_spectrum_values unchanged;
		memset(&unchanged, 0xa5, sizeof(unchanged));
		unsigned char bytes[sizeof(unchanged)];
		memcpy(bytes, &unchanged, sizeof(bytes));
		float sample = f.pcm[63];
		if (mode < 2)
			f.pcm[63] = mode ? INFINITY : NAN;
		ck_assert(td_spectrum_measure(f.spectrum,
		                              mode == 2 ? NULL : f.pcm,
		                              mode == 3 ? 31 : 32,
		                              mode == 4 ? 0 : 48000,
		                              &unchanged) == TEDER_INVALID);
		ck_assert(!memcmp(bytes, &unchanged, sizeof(bytes)) &&
		          !memcmp(saved, got.bands, sizeof(saved)));
		f.pcm[63] = sample;
	}
	tone(&f, c.frames, 3, FLT_TRUE_MIN, FLT_TRUE_MIN);
	OK(td_spectrum_measure(f.spectrum, f.pcm, c.frames, 48000, &got));
	for (unsigned b = 0; b < 8; b++)
		ck_assert(got.bands[b] == c.floor_db);
	dispose(&f);
}
END_TEST

START_TEST(sizes)
{
#ifdef TD_FFT_AUDIT
	ck_assert(!kiss_fftr_alloc(32, 0, NULL, NULL) && fft_allocations == 1);
	kiss_fft_free(NULL);
	ck_assert(fft_releases == 1);
	fft_allocations = fft_releases = 0; /* controls prove both interceptors are active */
#endif
	for (unsigned n = TD_FFT_MIN; n <= TD_FFT_MAX; n *= 2) {
		td_spectrum_config c = config(n, n == TD_FFT_MIN ? TD_FFT_BANDS_MAX : 64);
		fixture f;
		create(&f, c);
		tone(&f, n, 3, 1, -1);
		for (unsigned repeat = 0; repeat < 3; repeat++) {
			td_spectrum_values got;
			OK(td_spectrum_measure(f.spectrum, f.pcm, n, 48000, &got));
			ck_assert(got.count == c.bands && got.peak[0] == 1 &&
			          fabs(got.rms[0] - sqrt(.5)) < .000001);
			for (unsigned b = 0; b < c.bands; b++)
				ck_assert(isfinite(got.bands[b]) && got.bands[b] >= c.floor_db);
		}
		dispose(&f);
	}
#ifdef TD_FFT_AUDIT
	ck_assert(!fft_allocations && !fft_releases);
#endif
}
END_TEST

static double cpu_seconds(void)
{
#ifdef _WIN32
	FILETIME created, exited, kernel, user;
	ck_assert(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
	uint64_t ticks = ((uint64_t)kernel.dwHighDateTime << 32) + kernel.dwLowDateTime +
	                 ((uint64_t)user.dwHighDateTime << 32) + user.dwLowDateTime;
	return ticks * 1e-7;
#else
	clock_t value = clock();
	ck_assert(value != (clock_t)-1);
	return (double)value / CLOCKS_PER_SEC;
#endif
}

START_TEST(bench)
{
	td_spectrum_config c = config(2048, 64);
	fixture f;
	create(&f, c);
	tone(&f, c.frames, 31.5, .8, -.6);
	td_spectrum_values got;
	OK(td_spectrum_measure(f.spectrum, f.pcm, c.frames, 48000, &got));
	double start = cpu_seconds();
	teder_code code = TEDER_OK;
	for (unsigned i = 0; i < 2000; i++) {
		code = td_spectrum_measure(f.spectrum, f.pcm, c.frames, 48000, &got);
		if (code != TEDER_OK)
			break;
	}
	double seconds = cpu_seconds() - start;
	OK(code);
	printf("FFT N=2048 bands=64 region=%u bytes; 2000 windows %.3f s CPU, %.1f us/window\n",
	       (unsigned)(f.pcm_offset - f.offset),
	       seconds,
	       seconds * 1e6 / 2000);
	dispose(&f);
}
END_TEST

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const struct {
		const char *name;
		const TTest *test;
	} cases[] = {{"bounds", bounds},
	             {"dft", dft},
	             {"dft-edges", dft_edges},
	             {"signals", signals},
	             {"validation", validation},
	             {"sizes", sizes},
	             {"bench", bench}};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (strcmp(argv[1], cases[i].name))
			continue;
		Suite *suite = suite_create("spectrum");
		TCase *tc = tcase_create(argv[1]);
		tcase_add_test(tc, cases[i].test);
		suite_add_tcase(suite, tc);
		SRunner *runner = srunner_create(suite);
		srunner_set_fork_status(runner, CK_NOFORK);
		srunner_run_all(runner, CK_NORMAL);
		int failed = srunner_ntests_failed(runner);
		srunner_free(runner);
		return failed ? 1 : 0;
	}
	return 2;
}
