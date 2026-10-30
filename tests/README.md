# Native test support

Run `ctest --test-dir build --output-on-failure`. Use `-L harness` for harness
checks; `-V -R 'harness.(memory|hooks)'` prints allocation sites and baselines.
Check cases run without fork, each in its own CTest process with a 20-second
timeout. Four negative controls have a second process supervisor: three must
exit 42 with the intended diagnostic; the missing-worker control must reach
its checkpoint and time out. Logs are `build/tests/control-*.log`. A crash or
unrelated error cannot satisfy a control. Timeout kills the child process;
test code never frees storage while a stranded worker may still use it.

`support/harness.h` is private test infrastructure. Link `teder_test_support`
only into tests; it is neither installed nor linked into the public library.

- `th_tracker` uses 128 fixed records and a mutex. Allocation sites are stable
  strings. `fail_at` is a one-based attempt ordinal; zero disables injection.
  Configure it and inspect counters only after allocator users quiesce.
  Live/peak bytes include requested arena headers and storage, not malloc's
  internal overhead. A full record table rejects allocation. Realloc failure
  preserves the old block; successful replacement counts both blocks at peak.
  Free each block through its original tracker, then uninitialize the tracker.
- `th_allocator` supplies the actual private `src/memory.c` arena helper.
  Each arena has one bounded chunk, stable addresses and no growth. Headers,
  cleanup entries and padding are accounted for; deferred destructors run in
  reverse registration order before reset/free. Failed registration leaves
  resource ownership with the caller. Allocator context outlives the arena;
  reset/destroy require exclusive access and no borrowers. Engine and result
  creation now use this same helper and their own independent arenas.
- `th_ma_allocator` adapts the same tracker to miniaudio's callbacks. Tests
  inject every observed allocation in the pinned stereo 48000-to-44100 float
  converter constructor. `src/flac_heap.c` now routes the private libFLAC heap
  through the tracker too. Check/framework/OS and stdio internals remain
  outside the tracker. KissFFT caller storage can use the arena.
- `th_sink` borrows caller capture storage and a render callback. Step sizes,
  forced silence, short writes and latched device loss are explicit. It reports
  exact captured/underrun frame counts and rejects capacity overflow. The
  controller serializes its calls; only one step can be active. Stop prevents
  new callbacks but reports false while one is active. Join step workers before
  final uninitialization and releasing borrowed storage. Read capture/counters
  only when quiescent. The manually advanced nanosecond clock is independent
  of callback scheduling; no test relies on a sleep to order operations.
- `th_gate` provides one-shot arrival/release checkpoints for decoder, callback
  or control-worker tests. Initialize before starting users, await arrival,
  release, join, then uninitialize. Thread wrappers borrow their stable wrapper
  and context until join. Gate initialization unwinds partial event creation;
  thread-start failure leaves no joinable thread. Synchronization errors fail
  the process. These blocking helpers never belong on a production callback.

[Reference fixtures](fixtures/README.md) have independent integer expectations.
The fake sink tests sample capture and ownership observations, not real sound,
device negotiation, decoder scheduling or production gapless playback. Future
real-device tests must use a separate label and fail explicitly when their
required backend/device is unavailable. There is no automatic fake fallback.

`ctest --test-dir build -R '^lifetime\.' --output-on-failure` selects the five lifetime
cases. `-V -R lifetime.failures` prints injected engine/result allocation sites
and exact retained baselines. Each constructor has one `arena.chunk` allocation;
size, UTF-8 and ABI failures occur before allocation. Tests cover deep copies,
empty/wrong-kind/index reads, unchanged error outputs, exact budget boundaries,
one-time transfer, close rejection and two concurrent readers after engine
destruction. Inputs are borrowed and immutable only during private construction.
The installed consumer exercises the exported lifecycle using the libc allocator.

`-L requests` selects six request cases on the real native control worker. Private
executors and startup/commit gates exercise copied payloads, full queues and
retained completions, cancellation before/after commitment, result reservation
and cleanup, event loss, identity exhaustion, and close during startup. A separate
thread holds the bookkeeping gate to prove host calls return contention codes
while close still succeeds. No production entry point accepts these test hooks.
Initialization failure injection covers lock, wake, thread-start and post-start
publication stages. Logs count owned workers before/after join; 32 immediate-close
cycles exercise wakeup timing. Native OS stack/handle allocation is outside the
heap tracker.
State polls yield and wait for observed states; sleeps never establish ordering.

`-L pcm` selects sample/identity, ownership and callback-audit checks.
Cases cover non-power-of-two queue wrap, one-block pools, partial reads,
mixed callback sizes, old/future generations, every block ownership state at
quiescent close, a gated last reader, and 10,000 concurrently transferred blocks.
Allocation guards and deferred destructors verify no callback heap operation or
final release. The callback object is also checked with GNU/LLVM or Apple `nm`:
only compiler/sanitizer helpers, linker metadata and memory-copy primitives may be imported.
A separate unlinked object deliberately calling `free` must fail that audit.
Sanitizer runtime calls are diagnostic overhead, outside the production claim.
Test gates surround operations; they are not part of the callback implementation.
The test build requires `nm` (normally supplied by the toolchain).

`-R flac-heap` selects the codec budget tests and archive audit. The tests check
exact byte limits, realloc overlap/rollback, alignment/overflow, nested scopes,
two concurrent codec owners, and all 15 observed allocations in a reference
decode with comments and an inserted seek table. One-shot failure at allocation
15 is recovered by libFLAC's MD5 retry; the adapter retains NO_MEMORY. Persistent
failure at each ordinal must fail decoding and unwind to zero. Exact PCM,
metadata and MD5 still pass on the success path. The archive audit rejects
direct heap imports and verifies all five adapters; a deliberate free proves
rejection. New codec versions require source review as well as this symbol check.

These are private codec tests, not asynchronous source/player acceptance.
The existing fixture test uses stdio; its CRT-owned file buffers are outside
the codec budget. Production source callbacks will own/account I/O separately.

`-R '^source\.'` selects five private source cases. Tests generate original PCM
through the pinned encoder, then compare decoded output against independent integer
formulas. Coverage includes every depth 4–32, all layouts 1–8, rate endpoints,
maximum-size frames, UTF-8 filenames, five-byte reads, staged seeks/EOF, corruption,
unknown duration, full pools, gated open/seek cancellation and retained PCM views.
`fixtures/conversion_reference.py` independently computes the binary64 Butterworth
and interpolation vectors; converted samples allow 1e-6 absolute error. Pool sizes
64 and 3 must agree, including fractional 44100-to-48000 conversion and exact counts.
Failure sweeps cover both sequential decode and staged seek; codec/file/arena
baselines must recover. These tests do not exercise public scheduling or real devices.

`-R '^decode\.'` selects seven asynchronous engine cases: public open/seek and
exact PCM, commitment/cancellation, active progress during blocked preparation,
three-source retirement pressure, allocation/startup failures, close barriers,
and identity/budget/corruption/unknown-duration outcomes. Source cancellation
survives terminal request acknowledgement. Held PCM stays readable until the
consumer-quiescence handshake permits close. The external consumer is a private
native test endpoint, not evidence of real device output. Source conversion tests
also compare primed and direct conversion against the independent references.

## Callback rendering

`-R '^audio\.'` selects five rendering cases and two callback audits.
Exact checks cover partial/cross-block output, source-final boundaries, gain,
mute consumption, disabled preservation, stale/future generations, zero frames,
invalid capacities/configuration and untouched guard samples. A 4,000-block
producer/renderer race checks every stereo sample, silence suffix and frame count.
Heap guards verify no callback allocation/release; retired source arenas remain
live until the producer reaps their last returned blocks.

The audio object may import only `td_pcm_select`, `td_pcm_peek`, `td_pcm_consume`
and the existing compiler/sanitizer/memory primitives. The PCM object has its own
stricter audit, and deliberate `free` imports must fail both audit modes. These
tests supply coherent configuration by value; they do not certify future control
publication, native device stop, hardware deadlines, public underrun classification
or audible output. Missing frames are raw unavailable PCM, including after EOF.

## Backend context ownership

`-R '^backend\.'` selects context snapshots, key validation, allocation failures,
concurrent heap/realloc accounting and failed-context cleanup. Synthetic enumeration
covers all four ID formats, maximum key lengths, 128-output capacity, malformed
data and results surviving context destruction. The Windows unwind case compares
exact process handle counts after warming OS caches; upstream 0.11.25 leaked one
log handle per rejected context.

Run `build/bin/teder_test_backend native` separately for actual context/enumeration
and fail-at-N context allocation checks. It fails if no native backend is available.
Linux needs the ALSA or PulseAudio runtime library; Ubuntu packages are
`libasound2t64` and `libpulse0`. A headless ALSA context may enumerate zero devices.
This is context/enumeration evidence, not playback or native callback-stop evidence.

## Native device ownership

`-R '^device\.'` selects lifecycle, controls, clocks, loss and two callback
import audits. The tests use real miniaudio lifecycle functions with synthetic
backend callbacks. They verify exact PCM/guards/counters, negotiated fields,
context BUSY loans, immutable running configuration, post-init allocation failure,
and start/stop failure after a native borrower exists. The closing gates sit in
synthetic native dispatch/teardown, outside the production audio callback.
Native uninit must join that borrower before releasing the device arena.
The Windows unwind check preserves exact handle counts over 16 rejected opens.

The adapter may import only the separately audited `td_audio_render` and allowed
compiler primitives. It cannot allocate, release, signal or control the device.
Its counters describe raw missing PCM, including EOF; they are not clock or
public underrun classification. `device.controls` checks exact gain/mute samples,
pause preservation, applied revisions, interrupted publication retaining old settings,
stale generation rejection and revision exhaustion. Configuration requires quiescence. Live report
reads use an atomic snapshot and return AGAIN on concurrent publication. Tests
check generation reset, final PCM and sticky version exhaustion without wrap.

`device.notifications` checks all six native notification kinds, duplicate loss,
intentional stop, silent output guards, preserved PCM and replacement-only reset.
`device.loss-state` stops the native endpoint without a notification and checks
the atomic-state fallback. `device.loss-startup` checks loss during init/start.
`device.loss-closing` holds independent data and notification borrows through
teardown; neither device storage nor its context loan may be released early.
`device.loss-race` overlaps 10,000 notifications with data callbacks, checking
silence and the exact last submitted source frame. These private tests do not
establish public engine recovery or physical unplug behavior.

Run `build/bin/teder_test_device native` separately. It opens the current default
on Windows, or the first enumerated output elsewhere, submits three silent PCM
frames, stops, closes and injects failure at every observed device-init allocation.
Normal stop must leave the private loss flag clear.
Missing backend/device support fails the test. Headless ALSA output and silent
WASAPI submission are native lifecycle evidence, not audible music acceptance.

## Engine device loss

`output.loss-states` checks EMPTY, READY, DRAINING, PAUSED and ENDED output loss,
exact clock/epoch resets, no new allocations, retained position and explicit recovery.
`output.loss-controls` removes output during unacknowledged gain/pause, STOP and
close. `output.loss-preparing` holds seek, successor, device-retarget and autoplay
preparation while loss retires output; request input remains pinned until STALE.
`output.loss-commit` exercises both sides of seek commitment.

`output.loss-closing` holds native dispatch through uninit: budgets and the old epoch
remain until that borrower returns. `output.loss-transition` retains the correct
source on either side of a gapless boundary, including its unreported final clock.
`output.loss-repeated` runs eight loss/failed-reopen/recovery cycles, checks warmed
Windows handles and verifies epoch exhaustion. All use public requests with a
synthetic native backend; physical unplug/default-change behavior is not inferred.

## Public device enumeration

`-R '^output\.'` selects the engine output integration cases. Enumeration covers
snapshot, cancellation, failures, identity and closing. They check exact names/keys/defaults, detached
results after destroy, cached context reservations, take/ack/destruction,
cancellation on both sides of commitment, queue saturation and same-thread
native initialization/uninitialization. Failed allocations cover the context
arena, native initialization, native enumeration and result arena. Budget/key
limits, missing backend, native errors, poisoned-context retry and token
exhaustion are explicit. A blocked enumeration keeps close pending until release.
The ordinary suite injects only native context operations; public request and
result paths are production code.

Run `build/bin/teder_test_output native` for real public enumeration and a sweep
of every observed allocation failure. Results are read after engine destruction.
It fails if no native backend can initialize; zero enumerated outputs is valid.
This is enumeration evidence, not playback. The installed consumer also checks
the public symbol and invalid-handle boundary without requiring an audio device.

Selection adds `output.selection`, `.rates`, `.select-failures` and `.select-cancel`.
Checks cover copied/stale/malformed keys, media exclusion, default/explicit IDs,
negotiated fields, old-output preservation before commitment, native failure after
commitment, budget rejection before allocation, device/converter/source allocation
failures, epoch exhaustion and poisoned-context loss. Rates compare 44100-to-22050
PCM with the independent conversion reference, then exact native-rate PCM after a
seek and output replacement. Control is gated while tests borrow stopped PCM.
Cancellation/close gates cover both sides of commitment, native uninit and source
preparation. A dispatched generation remains fixed across native loss before
preparation, including the last representable epoch.

`output.reopen-queue` plays three frames, then holds the retained source's reopen
after selection and after STOP. It parks control where it waits (idle for
selection, the preparation wait for stop), admits PLAY for the new target, and
requires PLAY to stay queued with no device start through a full control pass. It
then requires the selection/stop to be terminal before PLAY commits, with a zero
clock at the new epoch. Selection reopens at saved frame 3 and plays frames 3–15;
stop rewinds and plays 0–15. A reopen reset to zero or a skipped rewind fails.
Before selection queued later requests, it ran PLAY early.

Four cases share that held reopen and queued PLAY. Each runs both schedules, and
checks at every stage that the epoch never decreases and that, within an epoch,
the output clock and underrun count never run backwards. Output never starts until
a reopen commits.

- `output.reopen-cancel` cancels the committed reopen. Nothing rolls back: both
  requests succeed and play as above.
- `output.reopen-loss` loses the device. The reopen and PLAY finish STALE, and the
  source stays PAUSED with DEVICE_LOST at frame 3 (selection) or 0 (stop).
  Selection observes the loss while PLAY is still queued; stop observes it when its
  preparation finishes. Reselection then reopens and plays from that frame.
- `output.reopen-failure` fails the reopen's FLAC decoder allocation. The reopen
  finishes NO_MEMORY with output READY, the source FAILED and the epoch unchanged.
  PLAY gets BAD_STATE, and reselection retries at the saved frame.
- `output.reopen-close` closes the engine. Both requests finish CANCELLED and every
  source, device and allocation is released.

`build/bin/teder_test_output native-select` opens a real output, verifies status,
closes and fails each observed allocation in turn. It selects the default on
Windows and the first enumerated output elsewhere. It never starts playback.

## Public playback

`output.playback` checks 32,769 exact stereo frames through a 2,048-frame pool,
variable native callback sizes and silence guards while enumeration is blocked.
`output.play-changes` holds a native borrower across seek and close, checks exact
post-seek samples, and verifies ordinary open stops playback. `output.play-failures`
covers native start/stop failure and every observed autoplay allocation failure.
All use the real callback and public request path with synthetic native dispatch.
`support/track.h` generates the shared long-stream fixture with a separately checked
sample formula; callback allocation/import audits remain mandatory.

For audible acceptance, build and run `teder_play` with an absolute FLAC path.
Record actual output/format, human observation and process exit separately from
these synthetic tests. The example flushes prompts even when stdout is piped.

`output.transport-pcm` drives callbacks explicitly to verify partial-block pause,
exact next-frame resume, atomic gain/mute, stable identity and one native start.
It arms the next allocation failure and proves no allocation is attempted.
`output.transport-states` covers empty/ready/playing/paused/draining/ended/failed
validation, controls during preparation, stale targets and invalid gain values.
`output.transport-boundaries` gates cancellation before commitment, FIFO changes
after publication, close before acknowledgement and native startup failure.
The installed consumer links both new public exports without requiring hardware.

## Refill scheduling

`refill.native-progress` blocks native enumeration while draining 32,769 exact
stereo PCM frames through a 2,048-frame pool. Its variable-size consumer performs
no wake calls. The independent sample formula checks every frame/channel and
keeps decode EOF separate from public playback clocks. This checks refill
continuity under blocked control, not a hardware callback deadline.
`refill.handoff` gates a producer borrow while replacing a source or closing;
old arenas remain owned until acknowledgement. `refill.initialization` rejects
each new event/thread stage and checks matched worker starts/stops, zero tracked
resources and unchanged warmed Windows handle counts. `refill.idle` keeps a full,
stopped output parked through native enumeration. `refill.close-resume` holds
refill after a pause acknowledgement and control after a new resume, then closes;
exit must follow acknowledgement of that outstanding resume.
Shared generated fixtures use a CTest resource lock. Callback audits are unchanged.

## Linux ThreadSanitizer in Docker

From the package root, run `python tools/run-tsan-docker.py` with Python 3 and
native Linux/amd64 Docker. It builds a cached Ubuntu builder image, compiles in
a temporary Linux volume, runs the full suite, saves logs in
`build-tsan-docker/`, and removes the volume. Failures propagate to the caller.

High-entropy ASLR can put executables/libraries outside TSan's address ranges,
causing `unexpected memory mapping` before `main`. Newer runtimes can also fail
when Docker blocks their attempt to disable ASLR. This runner uses `setarch -R`
only for the test process family. It adds one exact `personality` argument to
a checksum-verified Moby default seccomp profile. Tests run offline, with all
capabilities dropped and no-new-privileges; host ASLR settings remain unchanged.
Builds use the default policy. The helper does not disable TSan checks or retry
failing tests. Ordinary native sanitizer builds keep their existing CMake flags.

## Mixed-format gapless references

`output.gapless-rates` checks every ordered pair of 8000, 22050, 44100,
48000, 88200, 96000 and 192000 Hz at fixed 44100 and 48000 Hz outputs (98 pairs).
`output.gapless-formats` covers all 64 ordered FLAC channel-layout pairs and
all 29 bit depths (4–32) in both source positions. `output.gapless-edges` checks
empty, one-frame and multiblock sources with 1/7/257-frame callbacks, both ratio
limits and the largest FLAC rate. Boundary events, source clocks, exact output
lengths, zero underruns and a single native start are checked for every pair.

`support/mixed.h` independently computes stereo mapping, four binary64 RBJ
Butterworth sections and rational-time interpolation. It calls no production
converter and compares every captured sample, including drained tails. Absolute
full-scale tolerances are 1e-5 through 24:1 and 1e-4 through 64:1 in either direction;
narrow-cutoff binary32 coefficient/accumulator error motivates the latter bound.
The reference trims floor(9*out/in) startup frames and retains ceil(N*out/in).
Equal rates bypass filtering and retain N frames. These are numerical conversion
checks, not bit-perfect or analog frequency-response claims.

`output.gapless-limits` rejects ratios outside 1:64–64:1 during live playback,
retaining the previous ready successor, source position, device and reservation.
`decode.next-failures` injects every allocation failure into a mixed-format
replacement. `output.gapless-failures` exercises corrupt successor input before
and after adoption with both matching and mixed formats. All cases check cleanup.

## Private metadata reader

`metadata.reference` checks repeated tags, embedded NUL values, exact artwork
bytes/fields, Unicode paths, omitted artwork and detached result ownership.
`metadata.validation` uses independent wire fixtures, seven malformed/encoding
mutations and every truncated prefix. STREAMINFO duration is reported without
decoding frames; zero means unknown. This does not validate audio integrity or images.
`metadata.limits` checks 1024 tags, 16 pictures, 256 blocks and work/result budgets.
Counts are checked after parsing; the codec heap is bounded throughout, including
unrequested pictures and other metadata. Padding is skipped through checked seeks.
`metadata.failures` rejects each allocation in turn and checks complete unwind.
`metadata.cancellation` gates open, partial parsing and close, and runs two readers
with independent allocators/cancellation. All temporary owners die before return;
only one result arena survives success.

`probe.results` checks public transfer, untaken-result acknowledgement, source
replacement and retained results after engine destruction. `probe.admission`
holds worker startup to prove copied input, bounded FIFO slots and queued cancellation.
`probe.cancellation` gates reads and both sides of commitment against cancel/close;
pending slots remain owned until terminal. `probe.limits` checks work/headroom
rejection, exact result capacity and one byte below it. `probe.failures` injects all
32 allocation failures; `probe.initialization` rejects each new event/thread stage
and later publication, with joined workers and unchanged warmed Windows handle counts.

`refill.probe-progress` holds a probe read while consuming 32,769 exact PCM frames
and preparing a successor. Its constrained budget leaves just one source preparation
plus 32 KiB of result space beyond existing owners and probe scratch/codec capacity.
This verifies worker independence and memory headroom, not OS scheduling deadlines.

## Composed playback shutdown checkpoint

`output.shutdown-states` closes EMPTY without/with output, READY, PLAYING,
PAUSED, DRAINING, ENDED, FAILED and PAUSED after device loss. Three unacknowledged
terminal requests saturate capacity while event capacity is one. Exact request
IDs, outcomes, result kinds and remaining engine bytes survive repeated close.
Each case retains both an independent device list and metadata result across
destruction, verifies their fields, then releases the last two tracked arenas.

`output.shutdown-borrowers` combines an adopted successor, a seek held after
file open, a probe held before read, held native dispatch, two untaken results
and a queued gain command. All five slots are occupied. Two release orders prove
that native/source retirement can finish while probe close remains pending, or
that probe retirement can finish while source preparation remains blocked.
Destroy returns BUSY at each held boundary; no timeout reclaims borrowed storage.
Each schedule joins all five engine workers, closes exactly three source files
and three probe files, and releases the native device/context exactly once.

`output.shutdown-startup` rejects the engine allocation and all fourteen startup
stages, including post-thread publication. It checks exact worker entry/exit
masks, zero tracked allocations and unchanged warmed Windows handle counts.
`output.rollback-composed` fails every allocation observed during probe,
successor replacement, output replacement, seek, STOP rewind and spectrum
configuration replacement. Each operation starts with a paused 128-frame source,
ready successor, active FFT arena, held spectrum loan, detached results and an
untaken probe result. Probe/next/seek/configuration failures preserve the old
state and reservation. The loan's complete info, bands and source span remain
unchanged through failures, successful replacements and engine destruction.
After destroy, exactly two result arenas and one snapshot pool remain; releasing
results leaves precisely the pool, and final loan release restores zero allocations.

`output.shutdown-analysis` holds spectrum publication and a probe read together.
Native output and source retirement finish first, but destroy remains BUSY.
Both worker release orders preserve the loan and prevent a late publication.
All five worker exits and exact native/file counts are checked before destroy;
independent result/snapshot owners then follow the same staged release checks.

These extend the existing cancellation, preparation, refill, native-init/loss,
PCM-reference and dependency-allocation tests. Native dispatch is synthetic;
regular FLAC files, codec allocation, public requests and engine teardown are real.
Run `ctest --test-dir <build> --output-on-failure -R '^output\.(shutdown-|rollback-composed)'`
for the new cases. CTest terminates an entire hung test process; it never frees
resources out from under a blocked thread. OS/driver allocations and every
possible native scheduling interleaving are outside fault-injection coverage.

## Independent snapshot pool

`snapshot.bounds` verifies checked layout, exact-budget admission, allocator and
native-gate-stage rollback, and one arena allocation. `snapshot.retention` checks
copied values/spans, repeated loans to one address, loan/slot exhaustion, current
invalidation and independent ownership after the original pin is removed.
`snapshot.validation` rejects malformed intervals/spans, nonfinite values and
capacity/ABI errors without changing current data; counters never wrap.
`snapshot.concurrent` holds the pool gate to force AGAIN, then retains exact
values while a publisher replaces current data 4,095 times. `snapshot.final-release`
races three reader releases against owner removal for 32 rounds, requiring one
matching arena free per round. These exercise the private pool and public views.

## Bounded analysis feed

The `audio.feed-*` cases share the existing PCM/callback allocator fixture, with
feed storage in that same arena. They verify exact post-gain/mute PCM, starvation
identity, omitted pause/final padding, mixed-track records after source retirement,
whole-interval overflow, unpublished render-error prefixes, 256-frame chunking,
clock/drop-counter exhaustion, revision handoff and independent discontinuities.
The concurrent case forces 98 initial dropped intervals, then checks every accepted
sample/identity while wrapping the ring; accepted plus dropped frames equals 14,000.
Producer and consumer views finish before the fixture arena is released.

`device.analysis` runs the real native callback through synthetic miniaudio dispatch,
checks stopped-only attachment and exact 48000-Hz post-gain data, then reads copies
after device/source retirement. The feed callback object has its own import audit
and deliberate forbidden-free control. The renderer may call only its three audited
producer helpers. No timing deadline, public analysis worker or real DSP is inferred.

## Native renderer

Renderer-enabled builds register `renderer.bounds/cells/clipping/levels/wide`.
These use the pinned real core runtime and inspect exact glyphs, styles and RGB
channels on native planes, including wide/complex existing cells, resize, extremes,
no-data and retained worker snapshots after engine destruction. A private allocator
seam proves the single arena, failed creation and allocation-free configure/draw.
Tests print the actual loaded runtime path. Headless initialization uses explicit
xterm-256color terminfo and fails if unavailable; it is not emulator presentation.
Installed renderer consumers also create and inspect a real plane through the
installed public library. Core-only builds require no notcurses installation.
