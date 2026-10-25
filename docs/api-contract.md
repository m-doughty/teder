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
Create validates products/alignment and reserves fixed pools before publication.
The engine limit covers engine-owned arenas, active/staged/retiring sources,
untaken results, and its snapshot pool. Results additionally have a per-result
limit. A taken result leaves the engine's budget; its caller owns that memory
until release. Creating engines or retaining detached results repeatedly can
grow application memory: these APIs do not promise a process-global bound.
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
mixed formats use a fixed output stream and the defined conversion reference.
No native transition depends on event polling. Late preparation reports its
outcome; starvation emits UNDERRUN and initialized silence, never invented media.
EOF without a committed successor ends this sequence; late readiness alone
does not silently start another track. A new open is required after ENDED.

## Clocks, metadata, analysis and rendering

Positions count frames, not bytes or individual channel samples. Source frame
uses source rate; output frame is the next frame submitted to the device within
the epoch and includes underrun silence. Source position follows submitted PCM,
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

Analysis taps post-software-gain, pre-device-mixer stereo PCM. Enable/configure
is asynchronous and transactional; failure preserves the previous configuration.
Successful reconfiguration invalidates the current publication and increments
its revision. Bands/window/hop/ranges must fit engine limits and the supported
DSP configuration; no hidden clamping. The DSP specification defines finite defaults,
window, power aggregation, dB normalization and timing-dependent smoothing.
Peak/RMS are linear amplitude; bands are dB relative to that documented reference.
Only one analysis reconfiguration may be pending (otherwise BUSY); current and
staged configuration regions both count against the engine's memory budget.

Snapshot acquisition is allocation-free and bounded by `snapshot_loans`.
Snapshots belong to a separate stable pool; pinning every writable slot drops
new publication, never overwrites a reader. Disabled/no-current analysis returns
AGAIN. Each frame contains one epoch and a contiguous output interval, with
ordered, complete source spans (source/track zero denotes inserted silence).
If the bounded span budget cannot describe it accurately, drop that publication.
No FFT window crosses a seek/restart epoch. Dropped samples/publications have
counters; previously acquired snapshots remain immutable through reconfigure,
disable, close and destroy. Paused/no-new-data does not manufacture fresh frames;
the host decides when to display no-data instead of its retained snapshot.

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

There is no blocking wait/join API for the UI. An uninterruptible read leaves
close pending; a timeout never licenses detaching a worker or freeing storage.
Destroy before CLOSED returns BUSY. After CLOSED, exclusive `destroy(&engine)`
joins the lifecycle worker, discards unread outcomes/untaken results, drops the
engine's snapshot-pool pin, frees engine regions and sets that pointer to NULL.
The join completes before any storage that worker could use is released. Destroy
may wait for its final return and belongs off the UI thread. Other aliases become
invalid.
Detached result arenas and acquired snapshot references need no live engine.
