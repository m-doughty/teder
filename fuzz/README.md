# Coverage-guided fuzzing

These opt-in harnesses require Linux, Clang/libFuzzer and ASan/UBSan. libFLAC's
upstream configure checks also need a C++ compiler (g++ or clang++). No music
files, network input or host audio devices are used. Normal builds are unchanged.

| Target | Input | Reaches |
|---|---|---|
| `source` | 16-byte controls + FLAC bytes | Private FLAC open, seek, prime, decode and conversion |
| `metadata` | 16-byte controls + FLAC bytes | Private metadata/artwork reader and detached results |
| `sequence` | 8-byte config + operations | Public engine API through a fake audio backend |
| `renderer` | 55-byte header + operations | Notcurses renderer on real planes and snapshots |

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang-18 \
  -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_C_FLAGS_DEBUG=-O1 -g' \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DTEDER_BUILD_EXAMPLES=OFF \
  -DTEDER_SANITIZER=address -DTEDER_BUILD_FUZZERS=ON -DTEDER_WERROR=ON
cmake --build build-fuzz --parallel 3
python3 fuzz/run.py build-fuzz --smoke
```

Use separate directories for unit tests and fuzzing: coverage instrumentation adds
calls forbidden by callback import audits. The fuzz option instruments teder,
libFLAC, miniaudio and KissFFT before building them. System libc/kernel code and
the pinned dependencies' unreached paths are not covered.

The `renderer` target is built only when the same configuration also sets
`TEDER_WITH_NOTCURSES=ON` with the pinned notcurses described in
[the build guide](../docs/build.md). Build that notcurses shared, with the same
sanitizer flags plus `-fsanitize=fuzzer-no-link`, so libFuzzer also gets feedback
from inside it. `-fprofile-instr-generate -fcoverage-mapping`, with
`-fprofile-instr-generate` when linking, adds it to coverage reports.
`python3 tools/ci.py linux-fuzz` builds notcurses and every harness this way, then
runs `run.py --smoke`. CI runs it on x86_64 and aarch64 Linux.

`run.py` replays `corpus/<target>` and `regressions/<target>`, then optionally runs
a fixed smoke campaign with seed 2201: 10,000 mutations for `source` and `metadata`,
2,000 for `sequence` and 30,000 for `renderer`. `--target` limits the run. A build
without the renderer reports it as not built; naming it with `--target` there
fails. Fuzzers read standard input from `/dev/null`. Logs, profiles (one per
process and instrumented module), new corpus entries and failures stay under the
build directory. A failure stops the runner. CI runs the same command.

## libFLAC signed overflow

Corrupt residuals make libFLAC's predictor restore arithmetic overflow; upstream
annotates those functions so its own fuzzing tolerates the wrap, and the decoder's
later bit-depth and CRC checks reject the frame. teder reports CORRUPT. Every GCC
and Clang build compiles the private libFLAC copy with `-fwrapv`, so the wrap is
defined behaviour in production and here alike. UBSan therefore has no signed
overflow to report inside libFLAC; its other checks and ASan stay fatal, and teder's
own sources keep the signed-overflow check. Upstream's
`FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` is not used, because it also disables
CRC, MD5 and assertion behaviour that production builds keep.
`regressions/source/lpc-overflow` is the minimized input that first reached the
wrap; its replay must still end in a documented hostile-input outcome.

## Source and metadata contracts

Each file contains a 16-byte control prefix followed by at most 65,536 FLAC bytes.
Smaller prefixes and larger files are rejected by the harness. Controls select
native/conversion rates, staged seek modes, priming, small allocation budgets,
single-byte reads, artwork inclusion and cancellation. The remaining bytes go
unchanged into a uniquely owned temporary regular file. Generated valid seeds
and `flac.dict` help mutations reach deeper parser paths.

Each iteration owns its allocator context and file name on the stack. Allocations
retain their exact size and origin; aggregate requested storage must stay below
32 MiB and each adapter's configured reservation. The source harness owns one
PCM arena, clears it after the last view, retires the source and then frees it.
Metadata scratch must disappear before return; only one result arena may remain.
Its views are read after unlinking the input, then its final release must restore
zero allocations. Native file open/close counters must balance on every path.

The source oracle checks exact generation/source/track identity, contiguous output
positions, finite samples and converted frame totals at successful EOF. Every
failure must be a documented hostile-input code: CORRUPT, UNSUPPORTED, IO, LIMIT,
or CANCELLED after cancellation was requested. INVALID is allowed only for an
arbitrary seek beyond a known duration. NO_MEMORY, INTERNAL and BAD_STATE fail.
Metadata checks exact result kind/count bounds, indexed access, unrequested-artwork
omission and the same failure set plus ENCODING. INVALID there would mean the
result builder rejected the reader's own output.

Generated seeds that must succeed are checked exactly instead. `known-good.txt` lists
them, one line each: the target, the seed's name, `hash=` (a 64-bit FNV-1a of the whole
input, controls included) and its expected values. For a `source` seed these are the
stream's channels, rate, bit depth and duration, the number of output frames, and, where
neither a rate converter nor a downmix runs, the FNV-1a of every delivered sample's
binary32 bytes (`pcm=`). A `metadata` seed lists the stream values, the tag and picture
counts and a digest of every tag and picture (lengths included, so embedded NULs count).
The harness looks an input up by that hash; a hit makes any failure, a stream that stops
short of its final block, or a value that differs fatal. A miss, which includes every
mutated input, keeps the broad documented-outcome check above. The seeds' own controls
select the generous limits: native rate or a plain conversion, 4,096-byte reads, a 4 MiB
codec budget, a 1 MiB result and no early cancellation. `seeds.py` writes the values from
how it built each seed (the fixtures' original samples and the converter's exact ceiling
of the rate ratio), never by decoding it. `run.py` sets `TEDER_FUZZ_KNOWN` to the file,
fails if a listed hash is not a committed input of that name (an edited seed would
otherwise fall back to the broad check), and fails unless the replay log shows each
listed seed passing its exact checks. Without the variable the harness checks nothing
extra. Regenerate the manifest with the seeds, as below, and commit both.

Work is bounded to 16 prime and 128 pump calls, with cancellation after 4,096 native
reads (or an earlier selected read). Long valid streams may be retired before EOF.
Seeking occurs only before publication. Cancellation bounds parser work between
reads; a stalled individual call still triggers the fuzzer's ten-second timeout.
No timeout is converted into a successful input or used to free a live borrower.

## Sequence contract

The 8-byte header selects request/event capacity, PCM frames, snapshot slots,
loans, bands and spans, tight engine/result/input limits, fake-device failures
and an optional single injected allocation failure. Up to 96 operations follow.
Each chooses open, transport, seek, gain, prepare, clear, probe, enumerate, select,
analysis, get, poll, cancel, take, ack, event, snapshot acquire/release, result
release, render, device loss or a bounded wait. Arguments are fuzzed values;
handles, results and loans are always legal, so caller-induced UB is excluded.

`LLVMFuzzerInitialize` encodes four tracks with the pinned encoder: 3,000 and
30,000 stereo 16-bit frames at 44,100 Hz, 4,000 mono 24-bit frames at 22,050 Hz,
and an empty stream. A text file, the directory and a missing name complete the
paths. Each input owns one engine, a mutex-guarded tracking heap and fake backend.
Device 'A' (default) renders at 44,100 Hz and 'B' at 22,050 Hz, both float stereo.
The device renders only credited frames, at about one microsecond per frame.
Render and request waits are bounded by time and a two-second budget per input.

Oracles are schedule-independent:

- Valid arguments are never INVALID; each single invalid argument returns its exact
  documented code with a zero request ID. LIMIT requires a full request table.
- IDs increase; states only advance; terminal outcome and result kind never change.
  INTERNAL, BUSY and unexpected per-operation codes fail. A request may report
  NO_MEMORY only if injection was not yet visible before its submission and is
  visible when its terminal outcome is checked. At most one request per input
  may report it; repeated terminal reads do not consume the allowance again.
  Gain and clear-next never accept NO_MEMORY. Creation failure is checked separately.
- Take succeeds exactly once for a present result; ack invalidates the ID.
- Probe results match the generated STREAMINFO; device lists match the fake exactly.
- Status relations hold: no source means EMPTY; epochs, counters and sequences
  advance; the engine never exceeds its byte limit; loans never exceed their limit.
- Native callbacks run on one lifecycle thread, with at most one live device.
- Close makes every request terminal. Destroy succeeds; retained results and
  snapshots keep identical contents afterward. Workers, files, contexts, devices
  and every heap allocation balance to zero.

The NO_MEMORY rule is scoped to this fake backend: its tracked allocations occur
during context/device construction and enumeration, whose failed operations close
poisoned native heaps. Its start/stop callbacks allocate no tracked memory. Source
and analysis failures belong to one request or become asynchronous stream errors;
a later seek/reopen uses fresh source storage. An injected failure need not produce
a request NO_MEMORY: cancellation or another error can take precedence. Named
`oom-*` seeds cover creation, open, prepare, analysis and selection, with later
operations wherever engine creation succeeds.

When triaging a saved input, `TEDER_FUZZ_TRACE=1` logs each operation, admission,
cancellation, settle wait, terminal outcome and status snapshot (including clocks).
It also records the injected allocation's index and site, including failed creation.
`TEDER_FUZZ_TRACE=2` adds the library's worker checkpoints and fake device
start/stop. Lines go to a bounded in-memory log, written to stderr after the input
or when a check fails, so tracing perturbs worker scheduling as little as possible.
Level 2 still slows workers. If a timing-sensitive failure disappears, compare the
plain, level 1 and level 2 reproduction rates before trusting a passing trace.

## Renderer contract

`LLVMFuzzerInitialize` leaves any controlling terminal with `setsid()`, then starts
one headless notcurses context that writes to `/dev/null` using xterm-256color
terminfo. It checks that the loaded notcurses matches its headers and stops the
context at exit. Each input creates its own child plane (1–48 rows by 1–96
columns), renderer and snapshot pool, the last two on separate counting heaps; the
standard plane is never drawn. At the end every loan, the pool, the renderer and
the plane are released. The pile must then hold only the standard plane, and both
heaps must be empty.

The header chooses the plane's size, position, fill glyph and style, the renderer
configuration, an injected creation failure, the loan limit (1–4) and an optional
first snapshot. Up to 64 operations follow, each an opcode byte and a fixed operand
record: draw, put a glyph (at given coordinates, or at the cursor as text output
is, wrapping and scrolling), set the base cell, styles, channels, scrolling and
autogrow, resize, publish, acquire, release a loan, release the pool owner while
loans are held, configure, recreate and build information. Glyphs include ASCII,
two-column, combining and ZWJ sequences. Snapshots are published straight into the
private pool with 0–256 bands and fuzzed band, RMS and peak values; only valid
publications are generated. Handles are always of the right kind or NULL, so
caller-induced UB is excluded.

Sized records live in heap blocks of their exact size: the whole record when its
size and ABI version match, otherwise only the 8-byte prefix, so ASan rejects any
access beyond what the contract allows. Configurations cover every mode, colours
with alpha bits, the reserved field and floors of zero, -0, denormals, ±FLT_MAX,
NaN, infinities, positive values and raw bit patterns. Rectangle fields are near
the plane, raw 32-bit values or extremes such as INT32_MIN and UINT32_MAX.

The oracle follows [the renderer contract](../docs/renderer.md), not the
implementation:

- Before each draw it copies every plane cell (glyph bytes, style, channels and
  wide continuation) and the plane's drawing state.
- The expected code follows the documented order: an invalid renderer, plane or
  rectangle; success without changes for an empty area or intersection;
  UNSUPPORTED when a clipped row's boundary cuts a wide glyph; otherwise success.
- On success every cell outside the clip is unchanged, and every cell inside is a
  style-zero space with the configured foreground. Its background follows the
  documented mapping: band grouping, the LEVELS channel split and one-column
  maximum, heights against the original rectangle in exact arithmetic, and the
  peak marker. An exact half may be one cell lower.
- Any other code leaves every cell unchanged.
- Channels, styles, base cell, geometry, position and flags always survive; the
  cursor is not checked.
- Configure, draw and build information allocate nothing. Creation allocates
  once, or not at all after a validation failure. NO_MEMORY leaves nothing
  allocated and a NULL result. Build information equals the core's, and
  mismatched records keep their prefix.

LIMIT for plane dimensions above INT_MAX, IO for failing native cell access and
UNSUPPORTED from build information (a different notcurses runtime) cannot be
reached here. Allocation failures inside notcurses are not injected.
`TEDER_FUZZ_TRACE=1` logs each operation, each draw outcome and every height
accepted as an exact half rounded down.

`regressions/renderer` holds minimized inputs that overflowed rows in earlier
notcurses revisions. `scroll-wide-last-column` puts a two-column glyph at a
scrolling plane's last column. `resize-wide-obliterate` writes over a wide glyph
that a resize cut. `draw-dangling-wide` and `draw-dangling-wide-alias` reach such
a glyph through a renderer draw. Each must replay with the contract's result.

## Campaigns and failures

Pin the source, compiler, seed, corpus and limits before each campaign. Keep raw
logs, profiles and evolving corpora outside the distribution. For `sequence`, use
`-max_len=4096` and omit the dictionary. For `renderer`, use `-max_len=1591`, omit
the dictionary, redirect standard input from `/dev/null` and start it with
`setsid`, so notcurses cannot reach the terminal. Ctrl-C then cannot reach it
either: rely on `-max_total_time` or signal its process. Example with at least
1,800 seconds:

```sh
target=source
out="$PWD/fuzz-campaign/$target"
mkdir -p "$out/corpus" "$out/artifacts"
LLVM_PROFILE_FILE="$out/%p-%m.profraw" \
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
"build-fuzz/bin/teder_fuzz_$target" "$out/corpus" "fuzz/corpus/$target" \
  -dict=fuzz/flac.dict -seed=2201 -max_total_time=1800 -max_len=65552 \
  -timeout=10 -rss_limit_mb=1024 -malloc_limit_mb=64 -print_final_stats=1 \
  -artifact_prefix="$out/artifacts/"
```

Add `fuzz/regressions/$target` as a further seed directory when it exists. Record
elapsed time, executions, final coverage/features and corpus entry/byte counts.
Inspect `llvm-profdata-18 merge` and `llvm-cov-18 report/show` output to establish
which functions actually ran; libFuzzer's aggregate edge count is not a percentage.
Pass each instrumented shared library to `llvm-cov-18` with `-object` so its
functions are reported too.

Replay a saved failure by passing its file path to the same binary with the same
limits. Use `-minimize_crash=1 -max_total_time=120` to minimize a reproducible crash;
`-merge=1 NEW_CORPUS OLD_CORPUS` minimizes a coverage corpus. Preserve original
inputs and logs first. Classify harness errors separately from library errors,
add focused regressions for confirmed defects, and rerun affected checks after
repair. Commit minimized findings under `regressions/<target>`. See
[LLVM's libFuzzer guide](https://llvm.org/docs/LibFuzzer.html).

The committed `corpus/` inputs are original CC0 synthetic fixtures. Regenerate
them into a separate directory with `python3 fuzz/seeds.py OUTPUT` and compare
bytes before updating them (`OUTPUT/known-good.txt` replaces `fuzz/known-good.txt`). They
cover stereo 16-bit, mono 24-bit, eight-channel 32-bit, seeks/rates, truncation/CRC, UTF-8/NUL, picture and count-limit cases, plus
playback, gapless, analysis, probe, seek, loss, cancellation, capacity, invalid
argument, tight-limit and allocation-failure sequences. Renderer seeds cover both
modes, NULL arguments, NULL and band-less snapshots, extreme rectangles, wide
glyphs inside and at either clip edge, complex graphemes, resize, scrolling and
autogrow (with text output at the cursor scrolling the plane), drawing after the pool owner is released, the loan limit, malformed
records, configure failure, failed creation, build information, extreme floors
and bands, an exact half and plane styles. They do not exhaust FLAC frame
encodings, thread interleavings, real device behaviour or terminal output.
