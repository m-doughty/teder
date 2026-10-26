# Builds and platform targets

Require CMake 3.25+, a C17 compiler, and a C++ compiler for libFLAC's upstream
configuration checks (teder itself is C). Ninja is recommended. All v1 targets
are 64-bit little-endian with IEEE binary32 float.

| Required v1 platform | Architecture | Toolchain target | Audio backend |
|---|---|---|---|
| Windows 10 22H2 / Windows 11 | x86_64 | MSYS2 UCRT64 GCC 13+ | WASAPI shared mode |
| Linux, glibc 2.35+ | x86_64, aarch64 | GCC 12+ or Clang 15+ | ALSA and PulseAudio |
| macOS 13+ | arm64, x86_64 | Apple Clang 15+ | CoreAudio |

These are release requirements. The CI workflow registers shared/static core and
installed-consumer jobs on all three OSes.
Windows MSVC 2022 core builds are allowed as an
additional toolchain, but the notcurses fork requires UCRT64 for Windows.
C17 lock-free unsigned atomics are checked during configuration. MSVC uses
`/experimental:c11atomics` as documented by the
[Microsoft C++ team](https://devblogs.microsoft.com/cppblog/c11-atomics-in-visual-studio-2022-version-17-5-preview-2/).
No native 32-bit, big-endian or other-OS support is promised. PipeWire is usable
through its PulseAudio compatibility service; no native PipeWire backend is selected.

## C formatting

Use clang-format 18 and Python 3. The shared `.clang-format` targets 100 columns,
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
if needed. The helper checks the formatter's major version and covers authored
`.c`/`.h` files in `src`, `include`, `tests`, and `cmake`. Downloaded dependencies,
build outputs and CMake templates stay outside the pass. CI runs the same check.
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

For native Linux/amd64 Docker, run `python tools/run-tsan-docker.py` from the
source tree. High-entropy ASLR can cause TSan to abort before `main` with
`unexpected memory mapping`; newer runtimes may instead fail when Docker blocks
their `personality` call. The helper uses a pinned default seccomp profile plus
one exact allowance for process-local ASLR control, then runs tests offline with
`setarch -R`. Host ASLR settings remain unchanged. See `tests/README.md` for its
requirements, retained logs and temporary-resource handling.

Check runs with `CK_NOFORK` on every OS. Each registered case runs as a separate
CTest process with a timeout. Current tests cover build metadata, rejected ABI
inputs, dependency linkage, engine lifecycles and detached result views. They do
not open an audio device or certify playback/DSP. The harness supplies arena
fault injection, reference FLAC/PCM, simulated output and controlled callback
quiescence. Tests include concurrent result readers and controlled request/close
interleavings on the actual control worker. See
`tests/README.md` in the source tree.

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

## Optional renderer foundation

`TEDER_WITH_NOTCURSES=OFF` is the default and performs no notcurses discovery.
Enabling it requires an explicitly supplied installation of
`m-doughty/notcurses` commit `7152070f1b6c7cf6acc4e7447e5b6854f9f1e904`, matching
Notcurses-Native's `binaries-notcurses-3.0.17-r17` source pin:

```sh
cmake -S . -B build-renderer -DTEDER_WITH_NOTCURSES=ON -DTEDER_NOTCURSES_PREFIX=/absolute/fork/install -DTEDER_NOTCURSES_REVISION=7152070f1b6c7cf6acc4e7447e5b6854f9f1e904
```

Headers and the shared core runtime/import library must exist under that prefix.
Private static copies of notcurses are rejected. Missing inputs
fail configuration. The revision is the builder's provenance assertion; a
numeric runtime version check cannot prove fork identity. Actual loaded-library identity must
be verified before any borrowed plane is accepted.
This target currently provides only the build/runtime-version query, not drawing.
An installed renderer consumer supplies `TEDER_NOTCURSES_LIBRARY` pointing to
that runtime's core import/shared library and links `teder::notcurses`.

## Source reproducibility

`cmake/Dependencies.cmake` contains immutable commit URLs and SHA-256 hashes.
Sources/builds stay under the build tree. CMake's standard
`FETCHCONTENT_SOURCE_DIR_TEDER_*_SOURCE` overrides support local/offline source
trees; these overrides bypass archive verification and must retain equivalent
provenance before release. Use a fresh build directory for a changed source pin.
