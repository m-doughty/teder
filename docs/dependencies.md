# Pinned dependencies

Exact archive hashes live in
`cmake/Dependencies.cmake`; builds verify them before extraction.

| Library | Version / commit | Role | License used |
|---|---|---|---|
| miniaudio | 0.11.25 / `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` | Device output and conversion | MIT-0 alternative |
| libFLAC | 1.5.0 / `1507800de4b70e21be71f38caa0d9079d0bc6e45` | Native FLAC decoding and metadata | BSD-3-Clause, COPYING.Xiph |
| KissFFT | 131.2.0 / `7bce4153c6bc8aba2db0e889e576f9d00505cbe1` | Float real FFT | BSD-3-Clause |
| Check | 0.15.2 / `11970a7e112dfe243a2e68773f014687df2900e8` | Tests only | LGPL-2.1-or-later |

The optional renderer uses the separately supplied notcurses fork documented
in [build.md](build.md). Runtime dependency notices are installed under
`share/licenses/teder`. Check is not linked into or installed with teder.
The project has not selected its own distribution license yet.

## Selected paths and limits

miniaudio's decoder, encoder, resource manager, engine and node graph are
disabled. Only WASAPI, CoreAudio, or ALSA/PulseAudio are compiled for the target
OS; null-device fallback is disabled. OS APIs/libraries retain their own heap
ownership. No miniaudio structs cross the public ABI.

`cmake/Miniaudio.cmake` applies reviewed context-failure cleanup fixes to a
generated copy of the pinned header. It verifies the original header hash and
patch anchors; downloaded sources stay unchanged. The fixes release the internal
log and backend APIs on failed initialization, unwind context mutex stages, and
release WASAPI/ALSA/PulseAudio libraries on their early failure paths.
`backend.unwind` checks the reproduced Windows handle leak; allocator and native
enumeration tests cover the context owner. `cmake/MiniaudioDevice.cmake` also
repairs device initialization cleanup: release the stop event on backend failure,
and run the resource destructor when initialization fails before a worker starts.
CoreAudio now checks and tracks its separate stop event and frees it after unit
disposal. Windows handle counts and post-init destructor assertions reproduce
the device defects.

libFLAC serves both playback decoding and metadata probing. This avoids a second
FLAC decoder and keeps frame/error/metadata handling in the maintained codec.
Ogg, tools, examples, C++ wrappers and encoder multithreading are disabled.
The upstream static library contains encoder objects, but teder exposes no
encoding API. Planned v1 input coverage is native FLAC, 4–32-bit integer PCM,
1–8 channels in FLAC channel order, rates 1–1048575 Hz. The adapter must validate
that range and frame-size arithmetic before conversion; performance and exact
format fixtures are not established by these dependency limits.
Output remains stereo float at one negotiated device rate across track changes.

libFLAC's stream callbacks support seek/tell/length and read-abort. Metadata
callbacks expose STREAMINFO, repeated Vorbis comments and pictures; copy requested
data into a bounded result arena during the callback. Never retain callback
buffers. A playback decoder ignores unnecessary metadata; a probe requests only
the metadata it needs. Cancellation is cooperative between read/decode steps;
a blocked OS read is not forcibly interruptible by the codec.

## Allocation audit

- miniaudio has allocator callbacks with caller data. Its data converter also
  has size-query/preallocated initialization. Device/context addresses and
  allocator context must remain stable through their final uninitialization.
  The private backend owner now groups context and bounded enumeration scratch
  in one arena. Its concurrent allocator charges headers and realloc overlap
  against a reserved cap. Native OS/library allocations remain dependency-owned;
  this cap covers requests through miniaudio's allocator callbacks.
- libFLAC has no public allocator callbacks. `stream_decoder_new` performs
  checked allocations and unwinds partial construction; `stream_decoder_delete`
  calls finish and releases decoder-owned storage. Use those APIs, never free
  its interior pointers or pretend that its reallocations are arena no-ops.
  The private build now redirects malloc/calloc/realloc/free/strdup through
  `src/flac_heap.c`, including temporary metadata and alignment padding.
  Headers and realloc overlap count against a worker-owned byte budget;
  new allocations require a scope. This is a private build adaptation, not
  an upstream allocator API or process-wide allocator replacement.
  Source integration must reserve that budget against the engine limit first.
  Keep the heap descriptor in its source arena through final decoder deletion
  and zero live codec allocations. Dependency buffers are individually freed
  and reused; only objects with common lifetimes share the source arena.
  Do not hand foreign malloc buffers to codec APIs that adopt ownership.
  Production sources must use stream callbacks with separately owned I/O;
  libFLAC's stdio helpers can allocate CRT storage outside this adapter.
- Corrupt predictors overflow libFLAC's signed restore arithmetic by design
  (upstream marks `FLAC__fixed_restore_signal*`, `FLAC__lpc_restore_signal*` and
  `undo_channel_coding` for its own fuzzing); later bit-depth/CRC checks reject
  the frame. GCC and Clang builds compile only the private libFLAC copy with
  `-fwrapv`, making that wrap defined. Decoded output is identical and decode
  cost is unchanged within noise. Clang's
  restore loops compile to identical code. MSVC has no equivalent option, so
  MSVC builds use libFLAC as upstream ships it.
- `kiss_fftr_alloc` supports querying the complete plan size and initializing
  caller-owned aligned storage. That plan and its scratch fit one analysis
  region. Restrict v1 FFT sizes to bounded powers of two and use distinct input
  and output arrays: generic-radix and in-place paths can allocate temporaries.
  Release caller storage through its arena, not `kiss_fft_free` on a borrowed view.
- Check owns its suite/case/runner allocations; `srunner_free` releases the
  complete runner graph. Tests must not mix those allocations into engine budgets.
  Its 0.15.2 CMake build mistakes missing `librt` for missing `clock_gettime` on
  UCRT64. Our integration removes only that compatibility source when a native
  compile/link probe succeeds; timer shims remain. A Check-only compatibility
  header enables MinGW's `_POSIX`/`__USE_MINGW_ALARM` gates and includes `io.h`
  after Check's platform configuration. Upstream sources are unchanged.

Arena use remains required for teder-owned objects with a common destruction
boundary. External objects still need their proper destructor before their
containing source/engine region is released. The private device callback, audio
renderer and PCM consumer have separate import audits with deliberate-free
negative controls.

Source evidence: miniaudio's `ma_allocation_callbacks` and
`ma_data_converter_init_preallocated`; libFLAC's `stream_decoder.c` constructor,
destructor, metadata readers and public `stream_decoder.h`; KissFFT's
`kiss_fftr_alloc`, `kf_bfly_generic` and in-place branch in `kiss_fft_stride`;
Check's `src/check_run.c`. These pins are compiled and smoke-tested, not a
substitute for the adversarial input/lifetime tests.
