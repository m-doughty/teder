# Coverage-guided fuzzing

These opt-in harnesses require Linux, Clang/libFuzzer and ASan/UBSan. No music
files, network input or host audio devices are used. Normal builds are unchanged.

| Target | Input | Reaches |
|---|---|---|
| `source` | 16-byte controls + FLAC bytes | Private FLAC open, seek, prime, decode and conversion |
| `metadata` | 16-byte controls + FLAC bytes | Private metadata/artwork reader and detached results |
| `sequence` | 8-byte config + operations | Public engine API through a fake audio backend |

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

`run.py` replays `corpus/<target>` and `regressions/<target>`, then optionally runs
a fixed smoke campaign with seed 2201: 10,000 mutations for `source` and `metadata`,
2,000 for `sequence`. `--target` limits the run. Logs, profiles, new corpus entries
and failures stay under the build directory. A failure stops the runner. The same
command is in CI.

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

## Campaigns and failures

Pin the source, compiler, seed, corpus and limits before each campaign. Keep raw
logs, profiles and evolving corpora outside the distribution. For `sequence`, use
`-max_len=4096` and omit the dictionary. Example with at least 1,800 seconds:

```sh
target=source
out="$PWD/fuzz-campaign/$target"
mkdir -p "$out/corpus" "$out/artifacts"
LLVM_PROFILE_FILE="$out/%p.profraw" \
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

Replay a saved failure by passing its file path to the same binary with the same
limits. Use `-minimize_crash=1 -max_total_time=120` to minimize a reproducible crash;
`-merge=1 NEW_CORPUS OLD_CORPUS` minimizes a coverage corpus. Preserve original
inputs and logs first. Classify harness errors separately from library errors,
add focused regressions for confirmed defects, and rerun affected checks after
repair. Commit minimized findings under `regressions/<target>`. See
[LLVM's libFuzzer guide](https://llvm.org/docs/LibFuzzer.html).

The committed `corpus/` inputs are original CC0 synthetic fixtures. Regenerate
them into a separate directory with `python3 fuzz/seeds.py OUTPUT` and compare
bytes before updating them. They cover stereo 16-bit, mono 24-bit, eight-channel
32-bit, seeks/rates, truncation/CRC, UTF-8/NUL, picture and count-limit cases, plus
playback, gapless, analysis, probe, seek, loss, cancellation, capacity, invalid
argument, tight-limit and allocation-failure sequences. They do not exhaust FLAC
frame encodings, thread interleavings or real device behaviour.
