# teder

A C17 playback and analysis library, with a separate notcurses renderer.
Version 0.1.0 plays local FLAC, with play/pause/stop,
seeking, gapless playback across source-format changes, submitted clocks and
software gain/mute, peak/RMS meters and overlapping spectrum snapshots.
[Spectrum calculation](docs/spectrum.md) defines logarithmic bands, normalization
and audio-time smoothing/decay. The optional [notcurses renderer](docs/renderer.md) draws spectrum and stereo levels.

Implemented exports:

- `teder_get_build_info`.
- Optional renderer: `teder_renderer_get_build_info`, `teder_renderer_create`,
  `teder_renderer_configure`, `teder_renderer_draw`, and `teder_renderer_destroy`.
- `teder_create`, `teder_get_config`, `teder_get_status`,
  `teder_request_close`, and `teder_destroy`.
- `teder_open`, `teder_seek`, `teder_probe`, `teder_enumerate_devices`, and `teder_select_device`.
- `teder_transport` (PLAY/PAUSE/STOP) and `teder_set_gain`.
- `teder_prepare_next` and `teder_clear_next`; readiness is in status.
- `teder_request_poll`, `teder_request_get`, `teder_request_cancel`,
  `teder_request_take_result`, `teder_request_ack`, and `teder_event_poll`.
- `teder_result_kind`, `teder_result_probe`, `teder_result_tag`,
  `teder_result_artwork`, `teder_result_device_count`, `teder_result_device`,
  and `teder_result_release`.
- `teder_configure_analysis`, `teder_snapshot_acquire`, `teder_snapshot_get_info`,
  `teder_snapshot_bands`, `teder_snapshot_spans`, and `teder_snapshot_release`.
  Snapshots contain linear peak/RMS, optional dB bands and complete source spans.

The engine budgets a disabled post-gain feed in its arena and an independent
snapshot pool. Native output borrows the stable feed until device retirement.
The pool pin lasts until destroy; held loans retain only that pool afterward.
A dedicated worker handles transactional configuration and bounded publication.
See [analysis configuration](docs/api-contract.md#analysis-configuration).

Build queries report implemented features and compiled backends, as defined by the
[ABI 1 contract](docs/abi.md); they do not certify device availability. FLAC open and seek
prepare sources asynchronously and stream bounded PCM. AUTOPLAY starts the selected
output after bounded prefill. Prepared successors follow the current
track within the same device stream. See [successor usage](docs/sources.md#prepared-successors)
and [gapless behavior](docs/sources.md#gapless-playback).
Every public declaration is implemented.

`teder_probe` reads FLAC tags and optional artwork on a dedicated worker without
decoding audio or blocking source preparation. It copies accepted data into one
detached result arena. Work and result budgets leave room for one source preparation.
See [metadata probing](docs/api-contract.md#metadata-probing).

Each engine owns one arena containing its handle, copied configuration and
fixed request/input/event/PCM storage. Each result owns a separate arena with
copied metadata or device data. Result views remain valid until result release,
including after engine destruction. See the [ownership contract](docs/api-contract.md).

PCM storage is split into blocks of up to 1024 stereo frames, with bounded
publication and return queues in the same arena. The default reserves 512 KiB of
samples, plus block/queue bookkeeping. FLAC sources stream decoded and converted
PCM into this pool while the device consumes it; no allocation scales with track
length. libFLAC's allocations go through a bounded worker-owned budget, and each
source owns one arena for its wrapper, path, frame scratch and converter. See the
[source contract](docs/sources.md) for usage and memory reservations.

Status reports consumed source frames and submitted playback frames. Underrun
silence advances only the output clock; pause and EOF freeze both. See the
[clock contract](docs/api-contract.md#playback-clock).

The audio callback renders stereo float PCM with gain, mute and silence filling.
It borrows the PCM pool and allocates nothing. Output runs through miniaudio
with native start/stop and callback-safe teardown, and an independent refill
worker refills the pool during playback. `TEDER_OPEN_AUTOPLAY` returns BAD_STATE
without a selected output; ordinary open stays READY.

The callback latches native loss notifications and unexpected stops without
allocating, then sends silence until the device is replaced. After native
teardown the engine reports the output LOST and pauses the retained source. There
is no automatic restart or default-device following: recovery requires an
explicit device selection followed by PLAY. See
[device loss](docs/api-contract.md#device-loss).

Device enumeration runs asynchronously on the lifecycle worker. It copies up to
128 outputs into detached results, with engine-scoped opaque keys. Contexts
reserve their arena plus an 8 MiB dependency heap cap before opening; the cap
reserves budget without allocating that whole amount. Cancellation and failed
construction unwind before request completion. Each device owns a separate arena
and borrows its context until native teardown finishes. See the
[enumeration contract](docs/api-contract.md#device-enumeration). Device selection
opens a stopped output and rebuilds a retained track at its negotiated rate,
preserving identity and the saved source position.

`teder_transport` accepts PLAY, PAUSE or STOP with a copied current target. PLAY
resumes a paused source; pause preserves its next PCM frame while the device
sends silence. `teder_set_gain` applies finite linear gain 0..1 and mute
together. Active changes complete after a callback acknowledges them, without
restarting the native stream. STOP cancels pending preparation, flushes old PCM
and rewinds the retained source to READY at frame zero; rewind failure leaves
FAILED at the advanced epoch. See the
[transport contract](docs/api-contract.md#transport-controls).

Creation starts control/lifecycle, preparation, refill, probe and analysis
workers at LIVE/EMPTY with gain 1, no source and no output device. Refill
continues during blocked native enumeration. Source replacement waits for its
producer borrow to end. Accepted requests retain their completion slots until
acknowledgement. Telemetry has a separate, lossy event ring. Close stops
admission without needing a free slot; the worker completes or cancels admitted
work and publishes CLOSED. Observe CLOSED before destroying. Destroy joins that
worker, releases storage and clears the caller's pointer; earlier destruction
returns BUSY. The [installed consumer](tests/consumer/main.c) exercises open,
seek, request acknowledgement and asynchronous close.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTEDER_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /absolute/path/to/teder-install
```

First configuration downloads checksum-verified source archives. Check is
downloaded only with `BUILD_TESTING=ON`. Nothing is installed globally by
configuration or building. See [builds and platforms](docs/build.md),
[dependency choices](docs/dependencies.md), and the [API contract](docs/api-contract.md).

The [standalone player](examples/play.c) uses only the public API:

```sh
build/bin/teder_play /absolute/path/to/track.flac
```

On Windows: `build-shared\bin\teder_play.exe "C:\Music\track.flac"`.
An optional final device index selects an enumerated output. Otherwise it uses
its reported default. The player prints the actual output and negotiated format.
After final PCM submission, press Enter once the sound finishes. ENDED describes
submission, not an audible clock; the example leaves output running until then.
Build examples default on for a top-level build; disable with `-DTEDER_BUILD_EXAMPLES=OFF`.
Contributor test instructions are in `tests/README.md` in the source tree.

The optional `teder_visualizer` adds live spectrum/levels, pause, seek, mute and
one prepared successor. See [standalone demonstrations](docs/demos.md) for keys,
three-platform launch instructions, bounded checks and cleanup ownership.

## Licence

teder is distributed under the BSD 3-Clause licence; see [LICENSE](LICENSE).
Third-party notices for the bundled dependencies (miniaudio, libFLAC and
KissFFT) are installed under `share/licenses/teder`. See
[dependency choices](docs/dependencies.md) for their versions and licences.
