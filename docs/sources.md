# FLAC source contract

The private worker API in `src/source.h` streams native FLAC into bounded PCM
blocks. Public loading and engine scheduling remain future work. The executable
C examples are the source tests in `tests/test_source.c`; this header is not installed.

Inputs support 4–32-bit integer PCM, 1–8 channels in FLAC order, sample rates
1–1048575 Hz, and FLAC frames up to 65535 samples per channel. Output is stereo
float at a caller-selected rate in the same range. Capacity exhaustion returns
LIMIT; supported format values do not override the caller's memory reservation.
Integer samples divide by `2^(bits-1)`; binary32 rounding applies to wide samples.

Mono duplicates to both sides; stereo passes through. Multichannel mapping gives
front L/R weight 1, center weight sqrt(1/2) to both, back/side channels sqrt(1/2)
to their respective side, and back-center 1/2 to both. LFE is excluded. Each side
divides by its weight sum to preserve headroom. No hidden clipping is applied.

Matching rates bypass resampling. Other rates use the pinned miniaudio linear
resampler with an eighth-order Butterworth low-pass filter. Its reported latency
`floor(9 * output_rate / source_rate)` is trimmed; zero input drains the converter
at EOF, retaining exactly `ceil(remaining_source_frames * output_rate / source_rate)`
frames. This is the defined conversion reference, not a claim of exact frequency-
independent filter delay. Source ranges use rational frame mapping and are estimates
after conversion. Independent numerical vectors and chunk-size comparisons test it.

Paths are absolute UTF-8 without NUL, at most 32767 bytes. Windows accepts drive
and ordinary UNC paths, converted losslessly to UTF-16; device/extended namespace
prefixes are rejected. OS path-length/access restrictions can still return IO.
Only regular disk files are accepted. Ogg, ID3-prefixed and other containers are
unsupported. Truncated frames, CRC/MD5 failures, inconsistent sample positions or
declared durations, and trailing bytes fail with CORRUPT. Unknown duration remains
unknown until validated EOF. Seeking disables whole-stream MD5 checking in libFLAC;
frame integrity checks remain. A seek exactly to known EOF produces no PCM.

Call `td_source_plan` and reserve its total against engine memory before open.
The total includes one arena (wrapper, immutable identities, copied/native path,
65535 stereo frames of scratch, and preallocated converter) plus the codec cap.
Retain the entire reservation until final retirement. No allocation scales with
track length. Native handles and OS-internal storage are outside the heap budget.

One producer owns all source calls. Pump performs a bounded decode/conversion step,
retains partial blocks, and uses one output sample of lookahead to mark final blocks.
AGAIN means more work or returned pool space is needed; CLOSED means final PCM was
published, not submitted to hardware. Seek is allowed only before the first pump.
Cancellation aborts unpublished work; already published blocks retain their arena.
Retire closes the file/decoder/converter and returns BUSY until those blocks return
and are reaped. Final success clears the owner; a POSIX close diagnostic returns IO
with the owner still cleared. Consumers must quiesce before a pool clear. Never
cancel native threads or detach a blocked file operation and reclaim its storage.
