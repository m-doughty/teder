# teder C API contract

This is the implementation contract for the draft headers
`include/teder.h` and `include/teder_notcurses.h`.
[README.md](../README.md) lists the implemented symbols. ABI 1 is
provisional; incompatible changes before
release require updating this document and the headers.

## ABI and buffers

- C17, C calling convention (`__cdecl` on Windows), ordinary native alignment.
  No packed layouts, dependency structs, C enum parameters, variadics, or structs
  returned by value. Supported targets must provide IEEE binary32 `float`.
- IDs/counts/codes have the widths declared in the headers. Do not substitute
  `long`, `size_t`, a language boolean, or a platform enum in a binding.
- Sized structs require zero initialization, exact `struct_size`, and
  `abi_version=TEDER_ABI_VERSION`. Mismatch returns `ABI_MISMATCH` without
  accessing later fields. The caller must supply readable first eight bytes.
  Layouts are frozen within ABI 1; a future incompatible layout needs a new ABI.
  `text`, `blob`, `target`, `span`, and `rectangle` are frozen unsized records.
- Reserved fields and unknown input flag bits must be zero. Booleans are 0/1.
  NaN/infinite floats and invalid ranges return `INVALID`. Input validation
  precedes admission; state-dependent validation also runs before commitment.
- All non-optional pointers must be valid for the documented extent. Invalid
  addresses, stale handles and concurrent release are caller errors, not
  detectable failures. NULL handles are invalid except for release/destroy
  no-ops and the explicitly optional draw snapshot/default-device key.
- Paths are nonempty UTF-8 byte spans without embedded NUL, at most
  `max_input_bytes`; the terminator is neither required nor counted. Windows
  conversion is lossless; ill-formed text returns `ENCODING`. Relative paths
  are rejected: callers resolve absolute paths before submission.
- Input structs, paths and device keys are borrowed only during the call and
  copied before successful submission returns. No caller allocator callback,
  user-data pointer, or callback function is retained.
- Returned text/blob/array views are immutable and belong to their result or
  snapshot. Empty views have NULL data and zero length. Text uses explicit UTF-8
  byte lengths; tag values may contain NUL. Never call `strlen` or `free` on a
  view. Encoded artwork stays encoded. FLAC channel order follows its channel
  count; output v1 is interleaved stereo float internally.
- Except where specified, failed calls leave output storage unchanged. Create,
  acquire and take-result set valid handle out-parameters to NULL on failure;
  submissions set valid request out-parameters to zero. No partial handle escapes.
  `AGAIN` means a retryable empty/busy read; `BUSY` means admission contention;
  `LIMIT` means a configured capacity or representable size was exceeded.
- A request's bounded UTF-8 detail is NUL-terminated, at most 159 payload bytes;
  `detail_bytes` excludes NUL, and truncation ends on a code-point boundary.
  Errors require no new allocation to report. Stable codes, not detail text,
  determine program behavior. `outcome` is meaningful only for terminal requests.

## Calls, owners and arenas

All engine calls are serialized by the application, including close, reads,
cancel and destroy. They may migrate between host threads with synchronization.
Workers/callbacks run concurrently inside teder. Independent engines may be
used concurrently. No public function may be called from an audio callback.

Normal submissions/read operations do no file/device I/O, decoder work, joins,
or waits behind a worker. Temporary read contention returns `AGAIN`; submission
contention returns `BUSY`. Constructors and final releases can allocate/free and
should run away from a strict UI deadline. Ack/destroy may free untaken results;
destroy also joins the quiescent lifecycle worker before freeing engine storage.
These are bounded-work contracts, not a hard wall-clock deadline for an OS call.

Allocate together when objects die together. Engine bookkeeping and fixed
request/event/PCM storage occupy stable engine regions. Each source has a
separate arena; each detached result has one immutable arena. The independently
owned snapshot pool has its own arena. Each renderer has one owner region.
An arena may have checked, bounded chunks; it must never relocate live objects.
Reset/free a region only after all its borrowers have finished. Arena release
does not replace closing files, uninitializing devices or joining threads.

| Handle | Owner and release | Can outlive engine? |
|---|---|---|
| Engine | Caller; `request_close`, observe CLOSED, then `destroy` | No |
| Request ID | Engine slot; terminal `request_ack` or final destroy | No |
| Untaken result | Request slot; take transfers it; ack/destroy otherwise release | No |
| Taken result | Caller; `result_release` releases its arena | Yes |
| Snapshot reference | Caller; one `snapshot_release` per successful acquire | Yes |
| Renderer | Caller; `renderer_destroy`, with no active draw | Yes |
| Plane | Host; renderer borrows it for one draw | Host-defined |

Immutable result/snapshot reads may run concurrently while the caller keeps
the relevant ownership reference alive. Release happens after all reads using
that reference, including draws, finish. A result has one owning reference;
copying its pointer does not retain it. Snapshot acquires are counted references,
possibly with equal addresses. Distinct references may be released concurrently.
Release functions accept NULL; a second release of the same ownership reference
is invalid. Keep both shared libraries loaded until their final objects die.

Zero config limits choose finite build defaults, reported by `get_config`.
The current defaults are:

| Field | Default |
|---|---:|
| `request_capacity` | 32 |
| `event_capacity` | 64 |
| `max_input_bytes` | 4096 per request |
| `pcm_frames` | 65536 stereo frames |
| `snapshot_slots` / `snapshot_loans` | 4 / 16 |
| `max_bands` / `max_spans` | 256 / 16 |
| `engine_bytes_limit` | 67108864 bytes (64 MiB) |
| `result_bytes_limit` | 16777216 bytes (16 MiB) |

Request, input, event, PCM and analysis-feed storage shares one engine arena.
PCM capacity is split into blocks of at most 1024 stereo float frames; a final shorter block
preserves the exact configured frame count. Block records and both handoff queues
also count against the engine byte limit. Native output consumes this handoff.
Private FLAC sources now stream into this handoff; their format, conversion and
reservation rules and implemented asynchronous open/seek behavior are specified
in [sources.md](sources.md). Snapshot limits size an independent pool at creation,
even while analysis is disabled. Both arenas, including their overhead, must fit
the engine limit before either allocation. Enabled meter configuration adds one
worker-owned arena containing its accumulator and bounded source spans.
Public enumeration and probing produce detached device lists and metadata.
`result_tag` requires distinct key
and value output pointers. Identical pointers return INVALID without changes.

Create validates products/alignment and reserves fixed pools before publication.
The engine limit covers engine-owned arenas, active/staged/retiring sources,
untaken results, and its snapshot pool. Results additionally have a per-result
limit. A taken result leaves the engine's budget; its caller owns that memory
until release. Creating engines or retaining detached results repeatedly can
grow application memory: these APIs do not promise a process-global bound.
Byte limits cover requested heap storage, including arena overhead, rather than
allocator/thread-runtime bookkeeping or OS-managed thread stacks and handles. Worker result
construction reserves budget before allocation and releases unused reservation
afterward; untaken results remain accounted until transfer or actual release.
Snapshot loans never expand a pool. Retirement counts against capacity until
quiescence, even after cancellation; admission can remain BUSY during retirement.

## Admission, completion and observation

Submission `OK` allocates a monotonically increasing, nonzero request ID and
reserves its terminal-outcome slot. Rejection has no effect and owns no input.
There is no wrap/reuse within an engine lifetime: exhaustion returns `LIMIT`.
An admitted request moves QUEUED -> RUNNING -> TERMINAL, or QUEUED -> TERMINAL.
It has exactly one immutable terminal outcome, including on close or failure.
Nonapplicable output fields are zero. IDs from another engine must never be
passed as local IDs: their numbers may coincide, and origin is not detectable.

`request_poll` returns the oldest terminal request in completion order, leaving
it retained. Until acknowledged it may be returned again. `request_get` reads
any retained ID. `take_result` transfers a successful PROBE/DEVICES result once;
before terminal it returns `BUSY`, and absent/already-taken results return
`NOT_FOUND`. The request retains its result-kind description after transfer.
Ack requires TERMINAL (`BUSY` otherwise), discards any untaken result and
invalidates the ID (`NOT_FOUND` thereafter). Not acknowledging eventually makes
all submissions return `LIMIT`. Terminal transitions themselves cannot overflow.

Cancellation consumes no request slot. For a pending ID, `request_cancel` sets
intent and returns OK; a terminal ID returns `TOO_LATE`. If cancellation wins
before commitment, outcome is CANCELLED and temporary resources retire safely.
If commitment wins, its actual outcome stands. Poll until terminal to learn
which occurred. Cancellation does not roll back committed work or transfer
ownership to the caller. Close uses a separate latch, independent of all pools.
On brief engine-gate contention, status/request/event reads return AGAIN with
unchanged output. Admission, cancel, take and ack return BUSY; take still clears
its output handle. Retry from a later host iteration. Close does not acquire
this gate and remains available under contention and queue saturation.

Events are lossy copies. Their sequence starts at one, advances for every
generated event, and does not wrap; the oldest queued event is dropped on
overflow. `events_dropped` and sequence gaps expose loss. Status is a consistent
current-state copy, not a reconstructed event history. Request outcomes never
share event retention. Counters saturate at UINT64_MAX; ID/sequence exhaustion
initiates close with LIMIT rather than silently reusing identity.

## Media state and commitment

Lifecycle, transport, output-device state and pending media work are separate.
Loading is reported by `media_request` with operation OPEN, not by erasing the
current playable source. No source means `current.source=0`, track/rate/position
zero and transport EMPTY. Epoch starts at one. Source IDs identify occurrences;
caller track labels may repeat, including zero. IDs are scoped to one engine.
Creation starts with gain 1, mute off, analysis disabled, no output device and
no successor. AUTOPLAY requires output READY at commitment or fails BAD_STATE
without replacing the prior source.

Every targeted command copies `{epoch, source}` from status and validates it
again at commitment. A mismatch produces STALE. New open, successful seek,
stop, and device-stream replacement/loss increment epoch and invalidate old analysis
and pending successor intent. A natural gapless boundary changes source ID,
preserves epoch, and keeps the output frame clock continuous. Old borrowed
snapshot data remains valid with its old identity.

At most one media mutation (open, seek, prepare/replace, clear, select-device)
is admitted until it terminates. Another returns BUSY. Probes/enumeration have
bounded independent jobs and cannot starve playback. Control actions and close
remain admissible under their own slot rules. Queued control actions execute in
admission order; slow preparation commits only after rechecking current state.
Stop cancels outstanding media mutation and successor intent when stop commits.
Cancel an in-flight preparation and wait for terminal before submitting a new
one; a ready successor can be replaced directly.

| Operation | Commit and resulting state | Failure/cancellation |
|---|---|---|
| Open | Stage decoder/initial PCM; replace source atomically; READY or PLAYING with AUTOPLAY | Preserve previous source/state; initially remain EMPTY |
| Play | READY/PAUSED -> PLAYING; PLAYING is idempotent | EMPTY/ENDED/FAILED -> BAD_STATE; seek/stop before replay |
| Pause | PLAYING/DRAINING -> PAUSED at a callback boundary; PAUSED/READY no-op | EMPTY/ENDED/FAILED -> BAD_STATE |
| Stop | Flush safely, rewind retained source to frame zero, clear successor, READY | EMPTY is a successful no-op; rewind failure enters FAILED |
| Seek | Stage exact source-frame position, clear successor, increment epoch; retain play/pause intent | Unsupported/out-of-range -> error, previous stream preserved |
| Gain | Finite linear 0..1 and mute 0/1 applied together at a callback boundary | Rejection preserves gain; may succeed without a source |
| Prepare | New occurrence and initial PCM become ready successor; replace expected old successor atomically | Preserve old successor; committed old successor -> TOO_LATE |
| Clear-next | Remove matching uncommitted successor; zero when none is a no-op | Changed successor -> STALE; committed successor -> TOO_LATE |
| Select-device | Quiesce old stream, advance epoch, clear successor, initialize requested output; retain source paused | No hidden fallback; output LOST after opening fails; explicit retry |

Seek accepts positions through known duration inclusive; reaching EOF yields
ENDED once pending output is submitted. Seeking ENDED/FAILED to a valid earlier
position yields READY; playing seeks resume playing, paused seeks remain paused.
READY pause is a no-op. DRAINING play continues draining; an EOF-seen paused
source resumes draining. Play without an output device reports BAD_STATE.
Stop advances epoch at quiescence even if rewind then fails. Select-device
validation can reject before quiescence without changing the existing device.
Initial create opens no device; select one explicitly before autoplay/play.
Unexpected active decode failure enters FAILED and emits ERROR. Device loss
sets output LOST, pauses the source, invalidates prepared work and analysis;
recovery is explicit select-device followed by play. No automatic default switch.

Successor commitment is the release-publication of the first successor PCM to
the callback-consumable stream. It can precede audible transition. READY ->
COMMITTED transfers ownership to playback; replacement/clear can no longer
claim it. First successor PCM submission changes current source and emits a
transition with both source/track identities and the first output-frame index.
Before that boundary, stop/seek/open quiesce the whole affected stream if needed.
If replacement loses to commitment during preparation, it returns TOO_LATE even
if the current source has already advanced; other target mismatches return STALE.
Preparing for EMPTY/ENDED/FAILED is BAD_STATE; if EOF wins during preparation,
the request ends TOO_LATE and its staged source retires.

Gapless guarantees require successor PCM ready before the native commitment
deadline. A matching-format transition equals concatenated reference PCM;
mixed formats use a fixed output stream and the
[defined conversion reference and limits](sources.md#gapless-playback).
No native transition depends on event polling. Late preparation reports its
outcome; starvation emits UNDERRUN and initialized silence, never invented media.
EOF without a committed successor ends this sequence; late readiness alone
does not silently start another track. A new open is required after ENDED.

## Clocks, metadata, analysis and rendering

Positions count frames, not bytes or individual channel samples. Source frame
uses source rate; output frame is the next frame submitted to the device within
the epoch and includes underrun silence, excluding intentional pause/EOF silence.
Source position follows submitted PCM,
never decoder read-ahead; it does not advance through underrun silence.
Conversion may make it an estimate, reported by `position_quality`.
Latency uses output frames and a measured/estimated/unknown qualifier.
`output_period_frames` reports the nominal callback period, or zero if unknown.
Never infer an exact audible source position by subtracting it across a boundary.

DRAINING means decoder EOF with native PCM still pending. ENDED means final PCM
submitted. `DECODE_EOF`, `SUBMIT_EOF`, and `AUDIBLE_END_ESTIMATED` distinguish
those observations; the last is only set with a supported latency estimate.
Pause/stop completion does not promise already-submitted hardware audio is
inaudible. Unknown duration has a clear flag and a zero value, not a fabricated
length. Clock flags apply to the current occurrence, not the next decoder.

Probe results contain all requested metadata within `result_bytes_limit` or
fail with LIMIT; no silently truncated artwork/tags. Invalid tag UTF-8 fails
the probe with ENCODING without breaking independent playback. Without ARTWORK,
the artwork count is zero. Device keys are opaque byte strings scoped to the
originating engine; they contain no borrowed pointer/OS handle. They remain
usable without their result, but hardware may disappear. Invalid keys fail
validation, keys from another engine return STALE, and missing devices fail
explicitly. Enumeration never promises a device will open later.
Keys fit `max_input_bytes` and carry self-contained backend identity plus an
engine-instance token; no growing engine-side ID registry is retained. All key
lengths/fields are validated before backend access, including copied/fabricated
input. This token is provenance validation, not a security credential.

### Metadata probing (implemented)

Submit `teder_probe(engine, path, path_bytes, flags, &request)` with an absolute
UTF-8 path of at most `min(max_input_bytes, 32767)` bytes. Flags are zero or
`TEDER_PROBE_ARTWORK`. OK means the input was copied into a retained request slot;
BUSY means brief contention, and LIMIT means admission capacity was exhausted.
Poll the request until terminal. On success, take its result, acknowledge the
request, read immutable views and finally call `teder_result_release`. Taken
results survive source replacement and engine destruction. The
[installed consumer](../tests/consumer/main.c) demonstrates this sequence.

One dedicated worker processes probes in request order. Queued input is bounded
by the existing request pool; only one probe owns temporary parsing storage at a
time. Playback, preparation and device control use independent workers. Before
parsing, teder reserves scratch, an 8 MiB codec cap and the available result budget
under the engine limit. It leaves room for one maximum-path source preparation,
including that source's codec cap. Insufficient capacity produces terminal LIMIT.
Other retained results and native operations still share the finite engine budget.

The reader accepts up to 1024 tags, 16 pictures and 256 metadata blocks. These
count checks follow parsing; the codec byte cap applies throughout, including
unrequested artwork. The result cap includes its arena overhead. There are no
partial results on excess. Repeated tags retain order/case and explicit UTF-8
lengths; values may contain NUL. Picture bytes remain encoded and are never fetched
from a network. Invalid UTF-8 gives ENCODING, malformed metadata CORRUPT, other
container magic UNSUPPORTED, and native file errors IO. Duration is STREAMINFO's
sample count; zero remains unknown. Success does not validate audio or image integrity.

Commitment occurs after parsing, copying and temporary cleanup, immediately before
attaching the result to its request. Cancellation/close before commitment discards
the candidate; commitment first preserves its successful result. Cancellation is
checked between native reads of at most 4096 bytes. A blocked OS call keeps close
pending until it returns. CLOSED requires the probe worker to have joined; no
timeout or cancellation flag permits freeing borrowed input or engine storage.

### Device enumeration (implemented)

Submit `teder_enumerate_devices(engine, &request)` from the serialized host
thread. OK means queued; BUSY means retry admission later, and LIMIT means no
request slot is available. This call performs no native enumeration itself and
does not occupy `media_request` or change source/transport/output status.

Initialize `teder_request_info` with its size and ABI version. Poll/get until
TERMINAL, then inspect `outcome` and `result_kind`. An OK completion has
`TEDER_RESULT_DEVICES`. Take its result with `teder_request_take_result`, then
acknowledge the request. Read `teder_result_device_count` and indexed
`teder_result_device` views while retaining the result. Initialize each output
struct's size/version. Release the result after all readers finish.
Take/ack can return BUSY during brief bookkeeping; retry them later.
Acknowledging without taking frees the list. Destroy also frees untaken lists.

A successful list may be empty. It contains at most 128 outputs and fits
`result_bytes_limit`; every key also fits `max_input_bytes`. Capacity overflow
returns LIMIT without truncating the list. Missing native backend support returns
UNSUPPORTED or DEVICE_LOST as the request outcome, never a synthetic device.
Enumeration results remain valid after engine destruction; keys identify their
originating engine, so another engine cannot adopt them.

The first enumeration reserves a context arena and an 8 MiB dependency heap cap
against `engine_bytes_limit`. The heap grows only within that reservation.
Construction also reserves the lesser of remaining engine budget and the result
limit before building the list. Taking transfers the result's bytes out of engine
accounting. A committed context stays cached until close. A failed/cancelled first
request releases its provisional context; later result failures retain a healthy
context. A native allocator failure discards that context before retry.

Cancellation before commitment discards the result; a racing commitment can win.
A blocked native operation keeps cancellation/close pending until it returns;
the UI thread does not join it. No device is selected or started by enumeration.
### Device selection (implemented)

Submit `teder_select_device(engine, key, bytes, &request)`. NULL/zero selects the
current default; otherwise pass the complete key from this engine's enumeration.
The call copies the key, so its result or caller buffer may be released afterward.
Use the same terminal/outcome/ack sequence as enumeration; selection has no result.
It claims the single media-mutation slot until source preparation finishes.

Malformed/foreign keys, device-arena budget rejection and cancellation before
commitment preserve the previous output and epoch. Key validation precedes native
access. Device-arena reservation temporarily overlaps the previous device's
reservation. Native teardown ends old callback borrows before advancing the epoch,
returning obsolete PCM, clearing successor intent and opening the requested output.
The outcome includes the committed target even when native opening fails.
Such failure leaves output LOST with no hidden fallback. Cancellation after
commitment does not roll back the switch; close still waits for native/worker exit.

Success reports backend, negotiated stereo-float rate/channels and nominal period.
Latency remains unknown. The stream is initialized but stopped. With a retained
source, preparation reopens it at the saved source-frame position and new rate;
source ID and track label are preserved, transport becomes PAUSED. Preparation
still needs a source reservation alongside the retained source. Its failure leaves
the output READY and source FAILED; retry select, seek or open explicitly.

Open with AUTOPLAY prefills the bounded PCM pool, then starts the selected output.
PLAYING requires successful native startup; missing output returns BAD_STATE.
Failed startup leaves output LOST and the committed source PAUSED. Preparation
failure before commitment preserves the old source. Ordinary open stops the old
consumer and leaves READY. Seeking while playing resumes at the requested frame.
Build capability bits remain unchanged while the broader contracts are incomplete.

The producer reads coherent, generation-tagged callback reports. Final PCM
submission sets SUBMIT_EOF and ENDED; output continues with silence until explicit
replacement or close. No audible-end estimate is published. The public playback clock excludes EOF padding from underruns and advances source
position only for consumed PCM. Latency quality remains UNKNOWN; see below.
A selected context's native allocator failure during enumeration closes its
device/context, advances epoch and pauses the source. Explicit selection recovers.

### Device loss (implemented)

Unexpected native stop, reroute or interruption invalidates the selected output.
Detection continues while READY, PAUSED or ENDED. Worker scheduling and in-flight
native/file operations can delay publication; there is no detection deadline.
The callback sends silence after observing loss and never initiates recovery.

The lifecycle worker retires native users, preserves their final submitted source
position and advances the epoch. Output becomes LOST; transport becomes PAUSED,
or EMPTY without a source. Output/underrun counters and output format reset to zero;
position and latency quality become UNKNOWN. Prepared successors are cleared.
A DEVICE event reports DEVICE_LOST with the new epoch and retained source/track.
The event queue remains lossy; inspect status even when no event is available.

Old preparation is cancelled cooperatively and finishes STALE after returning its
borrowed input and temporary resources. Explicit cancellation before commitment
still takes precedence. Pending callback controls fail DEVICE_LOST if loss is
observed before successful acknowledgement. Loss during STOP preserves the final
position rather than reporting a completed rewind. Epoch exhaustion closes the
engine with LIMIT instead of wrapping.

Recover by selecting a device explicitly, waiting for successful completion,
reading the new target and submitting PLAY. Selection reopens the retained source
at its saved frame and leaves it PAUSED. There is no automatic restart or default
device following. A failed selection leaves LOST and permits explicit retry.

### Transport controls (implemented subset)

`teder_transport(engine, &target, TEDER_PLAY, &request)` starts READY output or
resumes PAUSED playback. Copy target from a successful status read. PLAY is a
no-op while PLAYING/DRAINING; it fails BAD_STATE without an output or from
EMPTY/ENDED/FAILED. PAUSE succeeds unchanged in READY/PAUSED and fails from
EMPTY/ENDED/FAILED. STOP rewinds the retained source to READY at frame zero.
Targets are checked at admission and again before commitment; mismatch is STALE.

For example, with `status` from a successful `teder_get_status` call:

```c
teder_target target = status.current;
uint64_t request;
teder_code code = teder_transport(engine, &target, TEDER_PAUSE, &request);
```

OK admits the request. Observe its terminal outcome, then acknowledge it using
the existing request API. BUSY means retry admission later; the call performs no
native wait. These controls do not occupy media_request and remain admissible
during preparation. They execute in admission order; a source change can make a
queued target stale. Neither pause/resume nor gain/mute changes the epoch.

`teder_set_gain(engine, linear_gain, mute, &request)` follows the same outcome
protocol. Gain must be finite and within 0..1; mute must be 0 or 1. Both apply
as one change. Gain works without a source or output; those settings govern the
next start. Repeating the current pair succeeds without another callback change.

On running output, pause/resume and gain/mute complete after a callback applies
them. Pause sends silence without consuming PCM; resume continues at the next
unconsumed frame. The device stays open and running. Decode EOF with unsubmitted
PCM reports DRAINING; EOF-seen paused playback resumes draining. If final
submission wins a pause race, transport stays ENDED. Already-submitted hardware
audio may remain audible after pause completion.

Cancellation before commitment preserves the old settings. After publication,
ordinary cancellation does not revoke the command. Close ends native borrows
before deciding a pending command's outcome: an acknowledged change keeps its
actual result; a change never applied returns CANCELLED. No timeout frees a
callback borrower. Submitted positions are reported by the playback clock below.

STOP commits cancellation of the outstanding media request, then quiesces native
output and advances epoch. Source ID, track label, output selection and gain/mute
are retained. Old PCM is discarded only after the callback returns. Preparation
reopens the retained path at frame zero; its source arena uses the same reservation
and lifetime rules as seek. Failure leaves FAILED and reports the committed target.
EMPTY stop succeeds without changing epoch, but still cancels pending preparation.
Repeated stop with a retained source performs another rewind and advances epoch.

During stop, `media_request` names the stop request until rewind completes. New
media mutations return BUSY. Later controls remain queued in admission order;
close is always available. Cancelled preparation keeps its slot and native resources
until its worker returns. Stop never detaches a blocked file read. Cancellation
before stop commitment preserves playback; cancellation afterward cannot undo it.
Close during rewind waits for cleanup and returns CANCELLED if rewind did not finish.

Seek to known duration, including a validated empty stream, yields ENDED with
DECODE_EOF and SUBMIT_EOF: no remaining PCM needs submission. An earlier seek
preserves paused/playing intent; from ENDED or FAILED it yields READY. Invalid
ranges and decoder failures before commitment preserve the existing stream.

### Playback clock (implemented)

`teder_get_status` copies a coherent worker-published snapshot. Its epoch, source,
track, position, format and state belong together. It returns AGAIN with the
output unchanged on gate contention. Callers own the copied value; it borrows no
engine storage. A snapshot can lag the latest callback by worker scheduling time.

`source_frame` is the next source-frame position represented by consumed PCM.
`output_frame` counts submitted playback frames in the current epoch, including
starvation silence. `underrun_frames` counts only that starvation silence and resets
with the epoch. Gain zero and mute consume PCM normally. Pause freezes these
counters; resume continues them. Final-buffer padding and later EOF silence do
not count. ENDED means final PCM submitted, not measured audible completion.

Source positions are MEASURED for matching rates and exact staged open/seek
positions. After rate conversion they are ESTIMATED: within each PCM block, map
the consumed fraction to that block's source range using integer floor division.
The final endpoint equals the source duration. Missing PCM advances output time
without advancing media time. Decoder read-ahead never advances `source_frame`.

The clock uses monotonic integer frame counts within an epoch, not wall time.
Elapsed playback seconds are `output_frame / output_rate`; media seconds are
`source_frame / source_rate`. There is no wall-clock extrapolation, including
while paused, stopped, ended or interrupted. A new epoch resets output and underrun
counts. Seek sets the new source position; stop sets zero. Device replacement
joins the old callback and saves its final submitted source position before
reopening. Loss keeps that saved position with UNKNOWN quality and no output clock.

For example, after declaring a live `teder_engine *engine`:

```c
teder_status status = {0};
status.struct_size = sizeof(status);
status.abi_version = TEDER_ABI_VERSION;
if (teder_get_status(engine, &status) == TEDER_OK && status.source_rate) {
    double media_seconds = (double)status.source_frame / status.source_rate;
    /* Display media_seconds alongside status.position_quality. */
    (void)media_seconds;
}
```

UNDERRUN events coalesce missing frames since the preceding worker observation;
`event.output_frame` is the observation endpoint, not the first missing frame.
Their epoch/source/track identifies the affected playback. Events can be dropped
by the bounded event queue; the status counter remains authoritative. Counter
exhaustion reports LIMIT without wrapping; status retains its last valid clock.

Latency remains `latency_quality=UNKNOWN`, `latency_frames=0` on all four backends.
Zero with UNKNOWN does not mean zero latency. The pinned miniaudio device API has
no portable live latency/cursor query, and its callback supplies no hardware
presentation timestamp. `output_period_frames` is a scheduling period, not an
end-to-end latency estimate. AUDIBLE_END_ESTIMATED remains clear.

| Backend | Integration limit |
|---|---|
| WASAPI | Padding is queried internally for buffer submission; no presentation cursor is exported to teder. |
| CoreAudio | Native timestamps stay inside the backend callback; teder receives frame counts only. |
| ALSA | Buffer availability supports writes; teder has no delay query. |
| PulseAudio | Internal buffering does not provide a latency value through the device API. |

### Snapshot storage (implemented foundation)

Snapshot slots, copied bands and complete source spans share one independent
bounded arena. Each successful acquire owes one release, even when two loans
have the same address. Immutable views last through their owning loan; copying
a pointer does not create another loan. Distinct loans may be released on
different threads. Release can wait briefly for the pool gate and never accesses
an engine. The original allocator and library code must outlive all loans.

The private publisher validates intervals, contiguous complete spans, finite
values and configured capacities before changing publication. Pinning all slots
drops publication and increments a saturating counter. Sequence exhaustion
returns LIMIT without replacing the current snapshot. Invalidation removes
current data while retaining prior loans and counters. The last loan after owner
removal destroys the gate and arena exactly once. No pool operation is audio-safe.

The engine creates and budgets this pool before starting workers. Any startup
failure releases it. Close invalidates current data after native users retire;
destroy drops the engine pin. The pool remains charged until destroy even with
held loans. Outstanding loans then retain only its arena, with no engine pointer.

Public acquire and all four access/release functions link. Acquire takes no
allocation and never waits for a gate: contention returns AGAIN. Publication and
invalidation share the engine gate, so obsolete work cannot restore current data.

### Audio analysis feed (implemented foundation)

The private tap copies post-software-gain stereo PCM into a bounded ring in its
owner's arena. Records contain at most 256 frames, one source/track identity,
output-frame start, epoch, rate and analysis revision. Adjacent records preserve
mixed-track boundaries; inserted starvation silence uses source/track zero.
Muted music retains its source identity. Pause and final-buffer padding add no data.

One callback stages copies, then publishes the complete interval only on render
success. Insufficient ring space drops the whole interval and saturates a dropped
frame counter; it never delays playback or publishes an inaccurate partial span.
An aborted render publishes nothing. The next successful record marks discontinuity
after drops, pause, revision/rate/epoch changes or noncontiguous output time.
The consumer borrows each record until it returns that slot. All payload and
identity data are copied; no source/decoder pointer or PCM block is retained.

Disabling uses revision zero; an in-flight callback may finish its old revision.
Re-enable/reconfigure uses a new revision. The consumer must filter old revisions;
disable alone does not discard the queue or prove quiescence. Native attachment
changes require stopped output. Both feed roles and all borrowed views must finish
before their owner arena is released. The engine reserves 64 usable records plus
one empty ring cell, holding at most 16384 stereo frames. Track boundaries can
use extra records. Device selection joins the old producer before attaching the
same stable feed to the new stopped device. Close joins native users; destroy
releases the containing arena. Queued records are never reset during replacement.
The analysis worker consumes enabled configurations; it sleeps while disabled
and polls at 5 ms intervals when enabled without queued data. The callback never
wakes it. No spectrum or level calculation runs on the callback.

### Analysis configuration (implemented meters)

`teder_configure_analysis` copies a prefixed configuration into a retained request.
Only one may be pending (otherwise BUSY). Enable, reconfigure and disable commit
transactionally on the analysis worker; cancellation or failure preserves the old
configuration, partial window and publication. Old and candidate arenas both count
against the engine budget until cleanup. Each success advances a nonwrapping
revision and clears current data. Revision exhaustion returns LIMIT unchanged.

Set `enabled=1`, `bands=0` for meters; `enabled=0` disables. `window_frames=0`
selects 2048; explicit sizes 1–65536 are supported. `hop_frames=0` uses that window
size. Only equal window/hop sizes are supported now. All frequency, floor,
smoothing and decay fields must be zero. Nonfinite floats, invalid enable/reserved
values return INVALID. Oversized window/band limits return LIMIT; spectrum,
overlap and nonzero DSP options return UNSUPPORTED, including on disable.

Complete, nonoverlapping rectangular windows produce independent stereo peak
(maximum absolute sample) and RMS (square root of mean squared samples), in
linear amplitude. Accumulation uses double precision. Bands have count zero and
NULL data. No partial final window is published. Each snapshot covers one epoch,
revision, sample rate and contiguous output interval, with complete ordered source
spans. Adjacent equal identities merge; source/track zero denotes inserted silence.
Mute preserves source identity. Pause/final padding creates no new analysis data.

Acquire is bounded by `snapshot_loans`. Disabled/no-current data returns AGAIN;
closing returns CLOSED; exhausted loan capacity returns LIMIT. A held snapshot
stays immutable through publication, pause, seek, disable, close and destroy.
Epoch/revision mismatches are discarded. Discontinuities reset partial windows;
a window never combines samples across a gap, pause, rate change or new epoch.

Status exposes cumulative sequence, dropped-frame and dropped-publication counters.
Callback overflow counts the complete rejected interval immediately. Incomplete
windows abandoned at a same-epoch/revision discontinuity count their frames.
Nonfinite samples or excessive spans discard the entire window, counting its
frames and one publication. All-pinned slots count one dropped publication only.
Intentional epoch/configuration invalidation is not counted as pressure loss.
Counters saturate; successful publication sequence exhaustion closes with LIMIT.
Snapshots copy counters at publication; failed publication leaves current data
and existing loans unchanged. The host chooses when retained data becomes stale.

### Spectrum and rendering (planned)

The private [spectrum core](spectrum.md) specifies and tests Hann normalization,
stereo power and logarithmic bands. Public spectrum configuration, overlapping
history, smoothing and decay await worker integration. Full analysis capability
stays clear; the meters-only contract above is unchanged.

The renderer owns no analysis history; decay belongs to analysis time, not UI
paint frequency. Draw borrows the snapshot and plane only until return, clips
with overflow-checked arithmetic, and fully redraws the intersection. Colors
are 0x00RRGGBB; higher bits are invalid. An empty intersection is successful.
NULL snapshot paints no-data using the configured background. Host serializes
plane/renderer operations, owns presentation and keeps their runtime loaded.
Renderer scratch has a finite bound independent of rectangle area; teder does
not allocate per-cell/per-draw buffers. A drawing error may leave partial cell
updates; retry uses the same full-redraw contract.

## Shutdown

LIVE -> CLOSING closes admission immediately. Already terminal outcomes stay;
uncommitted requests become CANCELLED after cleanup, committed ones retain their
actual outcome. Reads, poll/get/take/ack, repeated close and cancellation remain
available. New submissions and snapshot acquires return CLOSED. Existing loans
and detached results remain usable. CLOSING -> CLOSED occurs only after device
callbacks, workers and publication have quiesced and their external resources
have been cleaned up. The lifecycle worker joins all other workers, publishes
CLOSED as its final engine-memory access, and returns. Status/completions remain
stored in the live engine; CLOSED means quiescence, not that this final worker's
OS thread handle has already been reaped.
The wake event and bookkeeping gate stay alive until exclusive final destroy,
so permitted reads and repeated close never race their teardown. The control/decode
worker owns lifecycle and joins preparation and refill before consumer/pool/source
reclamation. Producer handoff requires the refill worker's idle acknowledgement;
a request to pause is insufficient. Probe and analysis workers also join before
current snapshot data is cleared and CLOSED is published.

There is no blocking wait/join API for the UI. An uninterruptible read leaves
close pending; a timeout never licenses detaching a worker or freeing storage.
Destroy before CLOSED returns BUSY. After CLOSED, exclusive `destroy(&engine)`
joins the lifecycle worker, discards unread outcomes/untaken results, drops the
engine's snapshot-pool pin, frees engine regions and sets that pointer to NULL.
The join completes before any storage that worker could use is released. Destroy
may wait for its final return and belongs off the UI thread. Other aliases become
invalid.
Detached result arenas and acquired snapshot references need no live engine.
