# Builds and platform targets

Require CMake 3.25+, a C17 compiler, and a C++ compiler for libFLAC's upstream
configuration checks (teder itself is C). Ninja is recommended. All v1 targets
are 64-bit little-endian with IEEE binary32 float.
Tests additionally require Python 3 and symbol-inspection tools; see [ABI checks](abi.md#validation-boundaries).

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
templates stay outside the pass. CI runs the same check.

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

## Neovim and clangd

CMake already enables `CMAKE_EXPORT_COMPILE_COMMANDS`. With Ninja or Makefiles,
configuration writes `compile_commands.json` into the build directory. clangd
uses it for include paths, definitions and C17 flags, including private dependency
headers. See the [clangd project setup](https://clangd.llvm.org/installation.html#project-setup).

Generate the database on the machine running Neovim. Windows/Docker build paths
will not resolve on macOS. From `teder/`:

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
require a working macOS SDK/toolchain; check `xcrun --show-sdk-path`.

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
`m-doughty/notcurses` commit `7152070f1b6c7cf6acc4e7447e5b6854f9f1e904`, matching
Notcurses-Native's `binaries-notcurses-3.0.17-r17` source pin:

```sh
cmake -S . -B build-renderer -DTEDER_WITH_NOTCURSES=ON -DTEDER_NOTCURSES_PREFIX=/absolute/fork/install -DTEDER_NOTCURSES_REVISION=7152070f1b6c7cf6acc4e7447e5b6854f9f1e904
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
