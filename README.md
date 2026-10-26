# teder

A C17 playback and analysis library, with a separate notcurses
renderer. Version 0.1.0 plays local FLAC, with play/pause and software gain/mute.
Stop/rewind, clocks, gapless transitions, analysis and rendering remain in progress.

Implemented exports:

- `teder_get_build_info`; optional `teder_renderer_get_build_info`.
- `teder_create`, `teder_get_config`, `teder_get_status`,
  `teder_request_close`, and `teder_destroy`.
- `teder_open`, `teder_seek`, `teder_enumerate_devices`, and `teder_select_device`.
- `teder_transport` (PLAY/PAUSE) and `teder_set_gain`.
- `teder_request_poll`, `teder_request_get`, `teder_request_cancel`,
  `teder_request_take_result`, `teder_request_ack`, and `teder_event_poll`.
- `teder_result_kind`, `teder_result_probe`, `teder_result_tag`,
  `teder_result_artwork`, `teder_result_device_count`, `teder_result_device`,
  and `teder_result_release`.

Build queries still report zero capabilities/backend mask. FLAC open and seek
prepare sources asynchronously and stream bounded PCM. AUTOPLAY starts the selected
output after bounded prefill. Other declarations
remain draft contracts and will not link.

Each engine owns one arena containing its handle, copied configuration and
fixed request/input/event/PCM storage. Each result owns a separate arena with
copied metadata or device data. Result views remain valid until result release,
including after engine destruction. See the [ownership contract](docs/api-contract.md).

PCM storage is split into blocks of up to 1024 stereo frames, with bounded
publication and return queues in the same arena. The default still reserves
512 KiB of samples, plus block/queue bookkeeping. Private tests exercise partial
reads, generation switches and source retirement. The private FLAC source now
streams decoded/converted PCM into this pool while the device consumes it.
No whole-track allocation is introduced.

The private callback core now renders stereo float PCM with gain, mute and
silence filling. It borrows the existing pool and allocates nothing. Tests cover
variable requests and concurrent production. A private device owner now connects
this core to miniaudio, with native start/stop and callback-safe teardown.
Public autoplay connects that consumer to independent refill. Deterministic tests check every sample separately.

Public device enumeration now runs asynchronously on the lifecycle worker. It
copies up to 128 outputs into detached results, with engine-scoped opaque keys.
Contexts reserve their arena plus an 8 MiB dependency heap cap before opening;
the cap reserves budget without allocating that whole amount. Cancellation and
failed construction unwind before request completion. See the
[enumeration contract](docs/api-contract.md#device-enumeration-implemented).
Each device owns a separate arena and borrows its context until native teardown
finishes. Configuration changes require a stopped device. Native tests submit
silence through WASAPI and headless ALSA. Public device selection now opens a
stopped output and rebuilds a retained track at its negotiated rate, preserving
identity and the saved source position. Open with `TEDER_OPEN_AUTOPLAY` starts
playback; it returns BAD_STATE without a selected output. Ordinary open stays READY.
`teder_transport` accepts PLAY or PAUSE with a copied current target. PLAY resumes
a paused source; pause preserves its next PCM frame while the device sends silence.
`teder_set_gain` applies finite linear gain 0..1 and mute together. Active changes
complete after a callback acknowledges them, without restarting the native stream.
STOP currently returns UNSUPPORTED. See the [transport contract](docs/api-contract.md#transport-controls-implemented-subset).

The private libFLAC build routes codec allocations through a bounded worker-owned
budget. The source owns one arena for its wrapper, path, frame scratch and converter.
Tests cover decoding, staged seeking, conversion, cancellation and resource retirement.
See the [source contract](docs/sources.md) for public usage and memory reservations.

Creation starts control/lifecycle, preparation and refill workers at LIVE/EMPTY
with gain 1, no source and no output device. Refill can continue during blocked
native enumeration. Source replacement waits for its producer borrow to end. Accepted requests retain their completion slots until
acknowledgement. Telemetry has a separate, lossy event ring. Close stops admission
without needing a free slot; the worker completes or cancels admitted work and
publishes CLOSED. Observe CLOSED before destroying. Destroy joins that worker,
releases storage and clears the caller's pointer; earlier destruction returns
BUSY. The [installed consumer](tests/consumer/main.c) exercises open, seek,
request acknowledgement and asynchronous close.

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
