# teder

A C17 playback and analysis library, with a separate notcurses
renderer. This is the native/test foundation, version 0.1.0, not a player yet.

Implemented exports:

- `teder_get_build_info`; optional `teder_renderer_get_build_info`.
- `teder_create`, `teder_get_config`, `teder_get_status`,
  `teder_request_close`, and `teder_destroy`.
- `teder_result_kind`, `teder_result_probe`, `teder_result_tag`,
  `teder_result_artwork`, `teder_result_device_count`, `teder_result_device`,
  and `teder_result_release`.

Build queries still report zero capabilities/backend mask. Results have private
construction/transfer tests; public probe/device producers and request APIs are
not implemented yet. Other declarations remain draft contracts and will not
link. Playback, decoding integration, analysis and drawing are future work.

Each engine owns one arena containing its handle, copied configuration and
fixed request/input/event/PCM storage. Each result owns a separate arena with
copied metadata or device data. Result views remain valid until result release,
including after engine destruction. See the [ownership contract](docs/api-contract.md).

Creation starts at LIVE/EMPTY with gain 1, no source and no output device. Close
currently reaches CLOSED immediately because no workers or devices exist yet.
Destroy before CLOSED returns BUSY; successful destroy clears the caller's
pointer. Applications must observe CLOSED before destroying: worker integration
will make close asynchronous. The [installed consumer](tests/consumer/main.c)
exercises the current lifecycle through the public C interface.

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
