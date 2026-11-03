# Builds and platform targets

Require CMake 3.25+, a C17 compiler, and a C++ compiler for libFLAC's upstream
configuration checks (teder itself is C). Ninja is recommended. All v1 targets
are 64-bit little-endian with IEEE binary32 float.
Tests additionally require Python 3 and symbol-inspection tools; see [ABI checks](abi.md#validation-boundaries).

| Required v1 platform | Architecture | Toolchain target | Audio backend |
|---|---|---|---|
| Windows 10 22H2 / Windows 11 | x86_64 | MSYS2 UCRT64 GCC 13+ | WASAPI shared mode |
| Linux, glibc 2.35+ | x86_64, aarch64 | GCC 12+ or Clang 15+ | ALSA and PulseAudio |
| macOS 13+ | arm64, x86_64 | Apple Clang 17+ | CoreAudio |

CI builds and tests the shared and static libraries and their installed consumers
on Linux x86_64 and aarch64, macOS and Windows. Linux builds include the renderer,
and sanitizer builds run on Linux and macOS; see [Continuous integration](#continuous-integration).
Windows MSVC 2022 is an optional core-library toolchain outside this table; the
notcurses renderer requires UCRT64 on Windows.
C17 lock-free unsigned atomics are checked during configuration. MSVC uses
`/experimental:c11atomics` as documented by the
[Microsoft C++ team](https://devblogs.microsoft.com/cppblog/c11-atomics-in-visual-studio-2022-version-17-5-preview-2/).
No native 32-bit, big-endian or other-OS support is promised. PipeWire is usable
through its PulseAudio compatibility service; no native PipeWire backend is selected.

## C formatting

Use clang-format 18.1.3 and Python 3. The shared `.clang-format` targets 100 columns,
four-column tabs for indentation, and spaces for alignment. Keep one statement
per line. Function bodies, branches and loops expand onto separate lines.
Literal strings remain intact; a long diagnostic string can exceed the target.
Separate logical test stages with blank lines. `.editorconfig` matches tab width
and LF endings in editors.

```sh
python3 tools/format-c.py --write
python3 tools/format-c.py --check
```

Check mode is the default and never edits files. Pass `--clang-format /path/to/tool`
if needed. The helper covers authored `.c`/`.h` files in `src`, `include`, `tests`,
`cmake`, `examples` and `fuzz`. Downloaded dependencies, build outputs and CMake
templates stay outside the pass. CI's `format` job runs the same check.

The helper refuses any formatter other than 18.1.3. Patch releases of clang-format 18
disagree on layout: 18.1.8 joins brace-initializer macros onto one line, and 18.1.3
splits them, so each version rejects the other's output. Ubuntu 24.04's
`clang-format-18` package and the GitHub `ubuntu-24.04` runner provide 18.1.3.
Homebrew's `llvm@18` is 18.1.8 and is refused. On macOS, Windows or another
distribution, install the PyPI wheel, which matches Ubuntu's output, into its own
virtual environment:

```sh
python3 -m venv ~/.venvs/clang-format-18.1.3
~/.venvs/clang-format-18.1.3/bin/pip install clang-format==18.1.3
python3 tools/format-c.py --clang-format ~/.venvs/clang-format-18.1.3/bin/clang-format
```

On Windows the executable is `Scripts\clang-format.exe` inside the environment.
Formatting is separate from clang-tidy's static analysis; see the
[clang-format options](https://clang.llvm.org/docs/ClangFormatStyleOptions.html).

## Configurations

Use separate directories for shared/static and each sanitizer configuration:

```sh
cmake -S . -B build-shared -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=ON
cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF
cmake --build build-shared
ctest --test-dir build-shared --output-on-failure
```

`TEDER_SANITIZER=address` enables AddressSanitizer plus UndefinedBehaviorSanitizer;
`thread` enables ThreadSanitizer; default is `none`. The modes are exclusive.
Windows sanitizer configurations fail explicitly. Linux/macOS builds probe the
selected compiler/runtime before enabling instrumentation. Dependencies inherit
instrumentation; Check allocations are not teder's allocation-failure domain.
`TEDER_WERROR` applies to authored library code, not upstream warning policy.

On Linux, high-entropy ASLR can cause TSan to abort before `main` with
`unexpected memory mapping`. Run TSan tests under `setarch -R`, which disables
randomization for that process tree only; host ASLR settings remain unchanged.
[Continuous integration](#continuous-integration) runs each sanitizer
configuration and its local equivalent.

Check runs with `CK_NOFORK` on every OS. Each registered case runs as a separate
CTest process with a timeout. The harness supplies arena fault injection,
reference FLAC/PCM, simulated output and controlled callback quiescence. Tests
include concurrent result readers and controlled request/close interleavings on
the actual control worker. These tests do not open an audio device or require an
interactive terminal. See `tests/README.md` in the source tree.

## Continuous integration

`.github/workflows/build.yml` runs on every push and pull request:

| Job | Runners | Covers |
|---|---|---|
| `format` | `ubuntu-24.04` | `tools/format-c.py --check` with clang-format 18.1.3 |
| `core` | `macos-15`, `windows-latest` | Shared and static core libraries, the suite and installed consumers |
| `linux` | `ubuntu-24.04`, `ubuntu-24.04-arm` | One job per Linux configuration of `tools/ci.py` |
| `macos-asan` | `macos-15` | `tools/ci.py macos-asan` |

`tools/ci.py` holds the commands for these configurations, so a local run
executes exactly what CI does. Each builds Debug with `TEDER_WERROR=ON`:

| Configuration | Compiler | Build | Runs |
|---|---|---|---|
| `linux-shared` | GCC | Shared, renderer | Suite, then installed consumers |
| `linux-static` | GCC | Static, renderer | Suite, then installed consumers |
| `linux-asan` | Clang 18 | Shared, renderer, `TEDER_SANITIZER=address` | Suite |
| `linux-tsan` | GCC | Shared, renderer, `TEDER_SANITIZER=thread` | Suite under `setarch -R` |
| `linux-fuzz` | Clang 18 | Static libFuzzer harnesses, renderer, `TEDER_SANITIZER=address` | Corpus replay and smoke campaigns (`fuzz/run.py --smoke`) |
| `macos-asan` | Apple Clang | Shared, core only, `TEDER_SANITIZER=address` | Suite |

Linux configurations first fetch `m-doughty/notcurses` by the pinned commit hash
and verify the checkout. notcurses is then built shared with the configuration's
compiler and sanitizer flags, so one sanitizer runtime serves the whole process.
For `linux-fuzz` it also gets libFuzzer feedback and source coverage instrumentation.
A stamp in its install prefix records the revision, compiler, flags and options.
CI caches that prefix; any mismatch rebuilds it. The installed consumers compile
against the same notcurses and check a real plane through the installed renderer.

Every tree lives under `build-ci/`, or under `--build-root DIR`. `--phase build`
fetches sources and needs the network. `--phase test` runs CTest, the installation
and the consumers (or, for `linux-fuzz`, the fuzz replay and smoke runs) offline
from that build; the default runs both. The script prints the compiler, CMake,
Ninja, Python and, on Linux, glibc and sanitizer runtime versions first. A failing
step stops the run with that step's exit status.

On macOS, run the macOS configuration directly:

```sh
python3 tools/ci.py macos-asan
```

On a host with Docker for Linux/amd64 or Linux/arm64, run a Linux configuration
in a container of the host's architecture:

```sh
python3 tools/run-ci-docker.py linux-tsan
```

The container image comes from a pinned Ubuntu 24.04 index and adds the hosted
runners' toolchain packages. The build phase runs with network access; the test
phase runs offline under a hardened seccomp profile. See `tests/README.md` for
its requirements, retained logs and temporary-resource handling.

## Neovim and clangd

CMake already enables `CMAKE_EXPORT_COMPILE_COMMANDS`. With Ninja or Makefiles,
configuration writes `compile_commands.json` into the build directory. clangd
uses it for include paths, definitions and C17 flags, including private dependency
headers. See the [clangd project setup](https://clangd.llvm.org/installation.html#project-setup).

Generate the database on the machine running your editor; paths from another
machine or container won't resolve. From the source root:

```sh
cmake -S . -B build-macos -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-macos
ln -sfn build-macos/compile_commands.json compile_commands.json
```

This also downloads the pinned headers and creates generated headers. Restart
Neovim's clangd client afterward, or reopen Neovim. For another local build tree,
point the symlink at that tree's database. The root database and build trees are
already ignored by Git.

If missing includes remain, inspect `:LspLog` to see which database and compile
command clangd uses. A missing `miniaudio.h`, `FLAC/...` or `check.h` suggests a
missing/stale database or incomplete configuration. Missing system headers also
require a working toolchain; on macOS, check `xcrun --show-sdk-path`.

## Installed consumers

```sh
cmake --install build-shared --prefix /absolute/path/to/teder-install
cmake -S tests/consumer -B build-consumer -G Ninja -DCMAKE_PREFIX_PATH=/absolute/path/to/teder-install
cmake --build build-consumer
ctest --test-dir build-consumer --output-on-failure
```

Use `find_package(teder CONFIG REQUIRED)` and link `teder::teder`. The package
propagates `TEDER_STATIC` for static consumers. Keep the installation's `bin`
on PATH for Windows shared consumers, or its `lib` on the appropriate runtime
search path. The private FLAC archive and notices are installed with teder;
third-party headers and Check are not part of the installed API.

## Optional renderer

`TEDER_WITH_NOTCURSES=OFF` is the default and performs no notcurses discovery.
Enabling it requires an explicitly supplied installation of
`m-doughty/notcurses` commit `fd67f2bafec21365f72a418797799d2051376fbe`:

```sh
cmake -S . -B build-renderer -DTEDER_WITH_NOTCURSES=ON -DTEDER_NOTCURSES_PREFIX=/absolute/fork/install -DTEDER_NOTCURSES_REVISION=fd67f2bafec21365f72a418797799d2051376fbe
```

Headers and the shared core runtime/import library must exist under that prefix.
Private static copies of notcurses are rejected. Missing inputs
fail configuration. The revision is the builder's provenance assertion; a
numeric runtime version check cannot prove fork identity or inspect a foreign
plane safely. The host must supply the same loaded runtime that created the plane;
verify deployment/module identity before passing planes. Native tests record the
actual loaded library path.
This target implements the [renderer API](renderer.md).
An installed renderer host links `teder::notcurses` and the matching runtime's
core import/shared library. Static teder packages also require
`TEDER_NOTCURSES_LIBRARY` pointing to that library. Shared deployments must make
the runtime discoverable by the loader; ELF executable RUNPATH is not transitive.
For the installed test consumer, also supply `TEDER_NOTCURSES_PREFIX`; it compiles
against those headers and checks a real native plane through the installed library.
Core-only builds do not register renderer tests. Renderer-enabled CTest runs real
headless notcurses planes with xterm-256color terminfo; missing native requirements
fail explicitly. These checks inspect cells, not terminal-emulator presentation.

## Source reproducibility

`cmake/Dependencies.cmake` contains immutable commit URLs and SHA-256 hashes.
Sources/builds stay under the build tree. CMake's standard
`FETCHCONTENT_SOURCE_DIR_TEDER_*_SOURCE` overrides support local/offline source
trees; these overrides bypass archive verification and must retain equivalent
provenance before release. Use a fresh build directory for a changed source pin.
