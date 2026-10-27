# ABI 1 and C consumers

Version 0.1.0 implements all 34 core exports and five optional renderer exports
declared in the public headers. This is still a development build. The release
version, ABI version and feature masks answer different questions.

Initialize every sized record with zeroes, its exact `sizeof`, and
`abi_version=TEDER_ABI_VERSION`. Query `teder_get_build_info` before creating
objects. ABI 1 rejects both shorter and longer records and every other version
with `TEDER_ABI_MISMATCH`. It reads only the first eight bytes before rejecting;
those bytes must be readable and suitably aligned. Zero size/version is not a
request for defaults. Zero configuration limits select defaults only after the
prefix succeeds. Queries copy values; there is no library-owned build-info view.

Layouts, member types, constant values and existing signatures are fixed within
ABI 1. Incompatible changes require a new ABI and library major/SONAME. Adding
functions or capability bits may be compatible; callers ignore unknown mask bits
and test each required known bit. Package patch/minor versions do not establish
ABI compatibility. The current CMake package deliberately uses exact package-version
matching; applications can instead omit the package version and check the ABI.

## Features and platforms

`capabilities` now reports FLAC, GAPLESS, ANALYSIS and ARTWORK (`0x0f`). These
describe implemented code, subject to documented formats, bounds and deadlines.
`backend_mask` uses bit `1 << TEDER_BACKEND_*`: WASAPI is `0x02` on Windows,
CoreAudio `0x04` on macOS, ALSA plus PulseAudio `0x18` on Linux. These are compiled
backends, not available-device counts or certification of a platform. Enumeration,
selection and request completion still report missing libraries, servers or devices.
No query opens an output device or changes routing.

The optional renderer query checks its loaded notcurses numeric version and returns
the core's build information, including these same masks. It does not certify fork
identity. Hosts and renderer must use the same pinned runtime and compatible plane
ABI. Core clients need no notcurses headers or runtime.

All supported targets are 64-bit little-endian with IEEE binary32 floats. Use the
fixed-width header types, native alignment and C calling convention (`__cdecl` on
Windows). Do not pack records or substitute `long`, `size_t`, booleans or enums.
Shared libraries and consumers must target the same OS/architecture ABI. Static
consumers also require compatible compiler/runtime libraries. Cross-CRT ownership
never transfers: release every teder allocation through its matching teder API.

| Record | Bytes | Alignment |
| --- | ---: | ---: |
| `teder_text`, `teder_blob`, `teder_target` | 16 | 8 |
| `teder_span` | 32 | 8 |
| `teder_config` | 56 | 8 |
| `teder_build_info` | 40 | 8 |
| `teder_status` | 208 | 8 |
| `teder_request_info` | 224 | 8 |
| `teder_event` | 88 | 8 |
| `teder_probe_info`, `teder_device_info` | 48 | 8 |
| `teder_artwork` | 80 | 8 |
| `teder_analysis_config` | 48 | 4 |
| `teder_snapshot_info` | 96 | 8 |
| `teder_renderer_config` | 32 | 4 |
| `teder_rectangle` | 16 | 4 |

The source inventory `tests/abi/contract.h` freezes every field offset, width and
type, constant, and function signature. Tests verify inventory coverage against
the headers, link every function address, and reject an unplanned exported symbol.
Shared libraries expose exactly their declared symbols. Static archives may contain
private dependency symbols; their presence does not make them public contracts.

## Complete usage examples

Build and run the [standalone examples](demos.md). `examples/play.c` demonstrates
device enumeration and playback. `examples/demo.c` and `visualizer.c` demonstrate
request completion, probe ownership, gapless preparation, transport, snapshots and
cleanup on errors. `examples/exercise.c` checks exact outcomes without a terminal.
The installed-consumer project compiles the same sources with installed headers
and libraries; neither private headers nor dependency layouts enter those clients.

An admitted request's `OK` is not its final outcome. The examples poll to TERMINAL,
take wanted results, acknowledge requests, and use fresh status targets for commands.
See the [ownership, completion and clock contract](api-contract.md) and
[gapless deadlines and conversion limits](sources.md) before adapting them.

Close, observe CLOSED, then destroy exclusively away from the frame loop. A timeout
does not permit early destruction. Release held results/snapshots separately after
their last readers and destroy each renderer after its last draw. The host owns its
planes/context. Keep core, renderer and native runtime loaded until all their
objects, borrowed views and calls are finished, including loans surviving an engine.

## Validation boundaries

Run `ctest --test-dir BUILD -L abi --output-on-failure` and the installed-consumer
commands in [build.md](build.md#installed-consumers). Tests require Python 3 and
GNU `nm`/`objdump` on Windows/Linux; macOS uses Apple `nm`. Guarded prefix checks
exercise native page protection, and GCC/Clang consumers also use `-fshort-enums`.
MSVC lacks that compiler switch; its normal consumer is still registered.
