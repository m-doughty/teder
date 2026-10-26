# teder

A C17 playback and analysis library, with a separate notcurses
renderer. This is the native/test foundation, version 0.1.0, not a player yet.

Implemented exports:

- `teder_get_build_info`; optional `teder_renderer_get_build_info`.
- `teder_create`, `teder_get_config`, `teder_get_status`,
  `teder_request_close`, and `teder_destroy`.
- `teder_open` and `teder_seek`.
- `teder_request_poll`, `teder_request_get`, `teder_request_cancel`,
  `teder_request_take_result`, `teder_request_ack`, and `teder_event_poll`.
- `teder_result_kind`, `teder_result_probe`, `teder_result_tag`,
  `teder_result_artwork`, `teder_result_device_count`, `teder_result_device`,
  and `teder_result_release`.

Build queries still report zero capabilities/backend mask. FLAC open and seek
now prepare sources asynchronously and stream bounded PCM. Device output,
transport clocks, analysis and drawing remain future work. Other declarations
remain draft contracts and will not link.

Each engine owns one arena containing its handle, copied configuration and
fixed request/input/event/PCM storage. Each result owns a separate arena with
copied metadata or device data. Result views remain valid until result release,
including after engine destruction. See the [ownership contract](docs/api-contract.md).

PCM storage is split into blocks of up to 1024 stereo frames, with bounded
publication and return queues in the same arena. The default still reserves
512 KiB of samples, plus block/queue bookkeeping. Private tests exercise partial
reads, generation switches and source retirement. The private FLAC source now
streams decoded/converted PCM into this pool; device wiring follows.
No whole-track allocation is introduced.

The private libFLAC build routes codec allocations through a bounded worker-owned
budget. The source owns one arena for its wrapper, path, frame scratch and converter.
Tests cover decoding, staged seeking, conversion, cancellation and resource retirement.
See the [source contract](docs/sources.md) for public usage and memory reservations.

Creation starts a control/decode/lifecycle worker and a separate preparation
worker at LIVE/EMPTY with gain 1, no source
and no output device. Accepted requests retain their completion slots until
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

Contributor test instructions are in `tests/README.md` in the source tree.
