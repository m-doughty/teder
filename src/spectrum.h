#ifndef TEDER_SPECTRUM_H
#define TEDER_SPECTRUM_H
#include "memory.h"
#include "teder.h"

enum { TD_FFT_MIN = 32, TD_FFT_MAX = 65536, TD_FFT_BANDS_MAX = 4096 };
typedef struct td_spectrum td_spectrum;
/* Resolved private settings. high_hz=0 follows Nyquist; explicit ranges may
 * extend beyond it (unavailable frequencies contribute zero power). */
typedef struct {
	uint32_t frames, bands;
	float low_hz, high_hz, floor_db;
} td_spectrum_config;
typedef struct {
	float peak[2], rms[2];
	const float *bands;
	uint32_t count;
} td_spectrum_values;

/* Append one aligned region to the containing configuration arena. Failure
 * leaves used/offset unchanged. No heap allocation or vendor ownership transfer. */
teder_code td_spectrum_plan(size_t *used, const td_spectrum_config *, size_t *offset);
teder_code
td_spectrum_init(void *storage, size_t bytes, const td_spectrum_config *, td_spectrum **);
/* Sole worker, distinct input and owned scratch. Input contains exactly frames*2
 * floats and is borrowed only until return. Failed calls leave prior bands and
 * out unchanged. Successful bands borrow this region until its next measurement
 * or destruction; copy them into the independent snapshot pool before reuse.
 * Quiesce the worker before the containing arena dies. No separate destructor. */
teder_code td_spectrum_measure(
    td_spectrum *, const float *stereo, uint32_t frames, uint32_t rate, td_spectrum_values *out);
#endif
