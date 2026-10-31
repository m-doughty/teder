# Spectrum calculation

`teder_configure_analysis` enables overlapping spectrum snapshots alongside
stereo peak/RMS. See [configuration and lifetime rules](api-contract.md#analysis-configuration).
The numerical core remains private; snapshots copy its output before scratch reuse.

## Storage and input

One region contains the descriptor, copied settings, KissFFT plan/internal
scratch, Hann coefficients, real input, complex output, cumulative power,
frequency edges and band values. The region appends to its owner's arena;
initialization and measurement allocate nothing. It needs no separate destructor.
The owner must stop all users before releasing the arena. One worker serializes
each instance; separate instances have independent scratch.

The FFT length N is a power of two from 32 through 65536. Band count B is
1 through 4096, additionally subject to engine limits and byte budgets.
Settings require finite positive low frequency, either zero high frequency
(Nyquist) or finite high frequency above low, and a finite negative dB floor.
The numerical core receives resolved settings; it supplies no other defaults.
The measurement input contains exactly N interleaved stereo frames and a nonzero
sample rate. Its buffer is borrowed only during that call.

Invalid settings, nonfinite samples or an auto-Nyquist range below the low edge
are rejected before replacing output. Returned band pointers borrow the region
until the next successful measurement or destruction. Publication must copy them
into the independent snapshot pool. A reference profile for measurement is
N=2048, B=64, 20–20000 Hz, floor −90 dB; the public default high edge instead follows Nyquist.

## Levels and normalization

Peak is the maximum absolute sample per channel. RMS is the square root of its
mean squared samples. Both use the unwindowed input and linear amplitude; silence
is exactly zero. Double accumulation keeps all finite binary32 inputs in range.

For spectrum, apply the periodic Hann window:

```
w[n] = (1 - cos(2*pi*n/N)) / 2, n = 0..N-1
Xc[k] = DFT(xc[n] * w[n])
q[k] = 1 at DC/Nyquist; 2 elsewhere
P[k] = q[k] * (|XL[k]|² + |XR[k]|²) / (2*N*sum(w[n]²))
```

KissFFT's forward transform is unnormalized. The implementation stores float
coefficients and normalizes by their actual double-precision squared sum.
Each channel is first scaled by its own peak to keep float FFT arithmetic finite;
power is rescaled in double afterward. The displayed spectrum averages channel
powers, never channel samples. Equal opposite-phase channels therefore retain
energy. A tone present in only one channel measures 3.0103 dB below the same
tone in both channels. No DC subtraction or extra high-pass filter is applied.

## Logarithmic bands

Edges are `low * (high/low)^(i/B)`, i=0..B. Zero high frequency resolves to
sample_rate/2. Explicit ranges can extend beyond Nyquist: unavailable frequencies
contribute zero; the requested edges are not silently clamped or repositioned.
The mapping is rebuilt when sample rate changes.

Bin k represents a frequency cell extending half a bin on either side, clipped
to [0, Nyquist]. DC and Nyquist cells therefore have half the usual width.
A band's power is the sum of cell powers weighted by the fraction of each cell
inside its edges. This conserves included power and handles edges between bins.
Narrow bands share a cell's energy; they do not gain additional frequency resolution.
The implementation integrates a cumulative array in O(N+B), after the two FFTs.

Band output is `max(floor_db, 10*log10(2*band_power))`; zero power gives the floor.
The 0 dB reference is mean-square power 0.5, the long-run power of a unit-peak
sine. A bin-aligned tone away from DC/Nyquist measures 0 dB when both channels
carry it and its Hann lobes fit inside the band. Halving amplitude lowers it
by 6.0206 dB. Finite-window power near DC/Nyquist can depend on phase; the
windowed-energy formula above remains the definition.
A full-range constant or Nyquist signal of magnitude one measures +3.0103 dB.
Values above 0 dB are allowed; there is no hidden clipping. Bands report integrated
energy, not energy per Hz, so wider bands contain more broadband noise power.

## Audio-time filters

The first valid window after enable/reconfigure, a gap, pause, epoch/rate change,
or invalid window primes all filters directly from raw measurements. A continuous
track boundary preserves history and filters, with identities covering the whole
overlapping window. Disabled/no-data intervals cause no filter updates.

For each subsequent window, dt = hop_frames / output_rate seconds:

- `smoothing_ms=0` preserves raw bands/RMS. Otherwise alpha =
  `1 - exp(-1000*dt/smoothing_ms)`. Band dB values use
  `(1-alpha)*previous + alpha*raw`. RMS uses the same recurrence in mean-square
  power, then takes its square root. This is not smoothing of linear RMS amplitude.
- `peak_decay_db_per_s=0` preserves raw peaks. Otherwise each channel uses
  `max(raw_peak, previous_peak * 10^(-dt*decay/20))`: instantaneous attack,
  exponential amplitude decay. Filtered peak and RMS need not retain their raw
  ordering when independently configured.

Both controls accept finite nonnegative values. There is no UI-clock dependence,
startup fade or per-paint decay. Pool pressure skips publication while filters
advance; feed discontinuities reset them. Paused views retain their last snapshot.

## Verification boundary

Tests compare against an independent direct DFT and explicit cell intersections.
Analytic checks cover silence, tones, opposite phase, single-channel input, DC,
Nyquist and impulses; finite extremes and invalid inputs exercise rollback.
A separate build of the pinned KissFFT sources denies heap allocation/release,
proves its hooks with controls, then initializes and measures every supported size.
This verifies the admitted out-of-place power-of-two path, not arbitrary KissFFT
usage. Worker tests cover overlapping identities, retained loans, sample-time
filters, different polling rates, pressure, invalid history, cancellation and
close. Public installed consumers enable spectrum without a terminal or device.
