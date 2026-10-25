#ifndef TEDER_FIXTURE_REFERENCE_H
#define TEDER_FIXTURE_REFERENCE_H
#include <stdint.h>
/* Hand-written, interleaved signed integer expectations; never generated from
 * decoded audio or by generate.py. Divide by 2^(bits-1) for exact float PCM. */
static const int32_t track_a[] = {
	-8000,8000, -7000,7000, -6000,6000, -5000,5000,
	-4000,4000, -3000,3000, -2000,2000, -1000,1000,
	0,0, 1000,-1000, 2000,-2000, 3000,-3000,
	4000,-4000, 5000,-5000, 6000,-6000, 7000,-7000
};
static const int32_t track_b[] = {
	16000,-16000, 16500,-16500, 17000,-17000, 17500,-17500,
	18000,-18000, 18500,-18500, 19000,-19000, 19500,-19500,
	20000,-20000, 20500,-20500, 21000,-21000, 21500,-21500,
	22000,-22000, 22500,-22500, 23000,-23000, 23500,-23500
};
static const int32_t mono_24[] = {
	-8388608,8388607, -600000,-500000,-400000,-300000,-200000,-100000,
	0,100000,200000,300000,400000,500000,600000,700000
};
#endif
