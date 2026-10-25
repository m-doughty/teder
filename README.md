# teder

A C17 playback and analysis library, with a separate notcurses
renderer. This is the build foundation, version 0.1.0, not a player yet.

Implemented exports: `teder_get_build_info`, and, when enabled,
`teder_renderer_get_build_info`. Both report zero capabilities/backend mask.
Other public declarations are the draft contract;
calling them will not link. Playback, arenas, decoding integration, analysis
and drawing are not implemented by compiling their dependencies.

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
