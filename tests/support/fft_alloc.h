#ifndef TEDER_TEST_FFT_ALLOC_H
#define TEDER_TEST_FFT_ALLOC_H
#include <stddef.h>
void *th_fft_allocate(size_t);
void th_fft_release(void *);
#define KISS_FFT_MALLOC th_fft_allocate
#define KISS_FFT_FREE th_fft_release
#endif
