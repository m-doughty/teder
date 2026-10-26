# FLAC source contract

`teder_open` and `teder_seek` prepare native FLAC asynchronously. Submission OK
means admitted; inspect the request until TERMINAL, check its outcome, then
acknowledge it. Open copies the absolute UTF-8 path and caller track label.
Seek copies the current `{epoch, source}` target and exact source-frame position.
A failed or cancelled preparation preserves the current source. A successful open
creates a new source ID; seeking preserves it. Both advance the epoch.
The [installed C consumer](../tests/consumer/main.c) exercises this sequence.

Status exposes source rate, channels, duration and the pending `media_request`.
With no device, READY means prepared audio; decoding does not advance position.
Device selection determines conversion rate; without a selected output, PCM keeps
the source rate. Selection rebuilds a retained source at its saved source-frame
position and leaves it PAUSED. Device-open failure leaves it paused with output
LOST; source rebuilding failure leaves output READY and transport FAILED. A new
select, seek or open can recover. `TEDER_OPEN_AUTOPLAY` requires a selected READY
output, otherwise BAD_STATE. After preparation, control stops the old consumer,
commits the new source, prefills the bounded PCM pool and starts output. PLAYING
is published only after native startup succeeds. Startup failure leaves output
LOST and the new source PAUSED. A decode failure during prefill leaves FAILED.
Ordinary open leaves READY; a seek during playback resumes the replacement.
PLAY/PAUSE/STOP and gain/mute use the [transport contract](api-contract.md#transport-controls-implemented-subset).
STOP reserves the media slot through cancellation cleanup and staged rewind. It
retains source identity, advances epoch and returns READY at frame zero. A failed
rewind leaves FAILED. The [playback clock](api-contract.md#playback-clock-implemented)
tracks consumed PCM, not decoder read-ahead, and preserves the final submitted
source position across device replacement.

Three fixed engine job records bound active, ready, staged and retiring sources together.
Each source reserves its arena plus an 8 MiB codec cap before construction.
The cap is a reservation, not an eager allocation. Exhausting the engine/codec
budget returns LIMIT. A pending media mutation or no free job returns BUSY.
Clear-next needs no free source job; it still needs a request slot.
A retired source still holds its reservation until every PCM reference returns.

The private worker API in `src/source.h` implements this handoff; its header is
not installed. Source tests in `tests/test_source.c` cover conversion separately.

Inputs support 4–32-bit integer PCM, 1–8 channels in FLAC order, sample rates
1–1048575 Hz, and FLAC frames up to 65535 samples per channel. Output is stereo
float at a private caller-selected rate in the same range; zero selects the native rate. Capacity exhaustion returns
LIMIT; supported format values do not override the caller's memory reservation.
Integer samples divide by `2^(bits-1)`; binary32 rounding applies to wide samples.

Mono duplicates to both sides; stereo passes through. Multichannel mapping gives
front L/R weight 1, center weight sqrt(1/2) to both, back/side channels sqrt(1/2)
to their respective side, and back-center 1/2 to both. LFE is excluded. Each side
divides by its weight sum to preserve headroom. No hidden clipping is applied.

Matching rates bypass resampling. Other rates use the pinned miniaudio linear
resampler with an eighth-order Butterworth low-pass filter. Its reported latency
`floor(9 * output_rate / source_rate)` is trimmed; zero input drains the converter
at EOF, retaining exactly `ceil(remaining_source_frames * output_rate / source_rate)`
frames. This is the defined conversion reference, not a claim of exact frequency-
independent filter delay. Source ranges use rational frame mapping and are estimates
after conversion. Independent numerical vectors and chunk-size comparisons test it.

Paths are absolute UTF-8 without NUL, at most 32767 bytes. Windows accepts drive
and ordinary UNC paths, converted losslessly to UTF-16; device/extended namespace
prefixes are rejected. OS path-length/access restrictions can still return IO.
Only regular disk files are accepted. Ogg, ID3-prefixed and other containers are
unsupported. Truncated frames, CRC/MD5 failures, inconsistent sample positions or
declared durations, and trailing bytes fail with CORRUPT. Unknown duration remains
unknown until validated EOF. Seeking disables whole-stream MD5 checking in libFLAC;
frame integrity checks remain. A seek exactly to known EOF produces no PCM and reports ENDED. Zero-offset
preparation primes the fresh decoder directly, including empty input, preserving
whole-stream MD5 validation. Positive seeks retain frame integrity checks.

Call `td_source_plan` and reserve its total against engine memory before open.
The total includes one arena (wrapper, immutable identities, copied/native path,
65535 stereo frames of scratch, and preallocated converter) plus the codec cap.
Retain the entire reservation until final retirement. No allocation scales with
track length. Native handles and OS-internal storage are outside the heap budget.

Preparation owns open, staged seek and priming. Priming retains a converted
sample (or validated EOF) inside the source, without publishing to the active pool.
An engine-gate handoff transfers ownership to the sole PCM producer. Pump performs
a bounded decode/conversion step,
retains partial blocks, and uses one output sample of lookahead to mark final blocks.
AGAIN means more work or returned pool space is needed; CLOSED means final PCM was
published, not submitted to hardware. Seek is allowed only before priming or the first pump.
Cancellation aborts unpublished work; already published blocks retain their arena.
Retire closes the file/decoder/converter and returns BUSY until those blocks return
and are reaped. Final success clears the owner; a POSIX close diagnostic returns IO
with the owner still cleared. Consumers must quiesce before a pool clear. Never
cancel native threads or detach a blocked file operation and reclaim its storage.

Preparation keeps replacement I/O separate from active decoding. A refill worker
can own the producer while control performs blocking native device operations.
Control waits for an explicit idle acknowledgement before touching that producer
or replacing its source. All I/O stays outside the engine bookkeeping gate.
Refill checks for returned PCM at a requested 1 ms interval while delegated and
unable to progress; this is not a device deadline or latency guarantee. Consumers
never signal it. Stopped public output needs no polling. Without an active consumer
or source/retired blocks needing work, refill waits on an event.
Cancellation belongs to the source job, surviving request acknowledgement/reuse.
Close waits for blocked operations, joins preparation and refill, quiesces consumers,
clears PCM and retires all sources before CLOSED. The callback publishes a bounded
atomic report; the producer observes final PCM submission and changes transport
to ENDED with SUBMIT_EOF. Output continues with silence until close or replacement,
so the final native buffer is not cut off. Empty validated input ends without
starting output. Public underruns exclude pause/EOF silence. Source positions
advance only through consumed PCM; conversion reports estimated positions. Hardware
latency remains unknown, so no estimated audible-end flag is published.

## Prepared successors

`teder_prepare_next` copies the current target, expected successor ID, absolute
UTF-8 path and track label. Use zero as the expected ID when no successor exists.
To replace one, pass `status.next_source`. Both identities are checked again at
commitment. A changed target/successor returns STALE. Admission returns BUSY while
another media mutation is pending; retry from a fresh status snapshot later.

```c
teder_status status = {
	.struct_size = sizeof(status), .abi_version = TEDER_ABI_VERSION
};
uint64_t request = 0;
teder_code code = teder_get_status(engine, &status);
if (code == TEDER_OK)
	code = teder_prepare_next(engine, &status.current, status.next_source,
	                          absolute_path, path_bytes, track_label, &request);
```

The snippet assumes a live engine and an absolute UTF-8 path with its byte count.
OK admits the request; poll it to TERMINAL, check its outcome, then acknowledge it.
AGAIN/BUSY requires a later retry, scheduled outside an audio callback. Successful
preparation sets `next_state=TEDER_NEXT_READY`, `next_source` and `next_track`.
Its request outcome identifies the successor; transport commands still target
`status.current`. During preparation, existing next fields remain unchanged and
`media_request` identifies the pending operation.

Each successful preparation creates a distinct source ID, even for the same file
and track label. It preserves the active epoch, playback position and transport.
The new source owns its decoder, file, converter and primed sample in the existing
bounded source lifetime. Failed/cancelled preparation preserves the prior ready
successor unless an independent stream change invalidates it. Slow preparation
runs separately from active decoding. Repeated replacement shares the same three
job slots and existing PCM pool; it never allocates an entire track.

`teder_clear_next(engine, &status.current, status.next_source, &request)` removes
a matching ready successor. Zero when none exists is a successful no-op. Both
clear and prepare support request cancellation before their state change; after
that change, their actual outcome stands. Cancelling an already completed prepare
request does not clear its successor: submit clear-next instead.

Successful load/seek, stop and device replacement/loss invalidate successor intent.
Playback failure or EOF discards an uncommitted ready successor. EMPTY/ENDED/FAILED
reject preparation with BAD_STATE. EOF during preparation completes it TOO_LATE.
Close joins preparation and playback before freeing their source arenas.

Readiness alone currently does not start the next track. Automatic
splicing and transition reporting are not implemented yet. Their ownership hook is already implemented:
the first successor PCM publication changes READY to COMMITTED. Replacement or
clear that loses to publication returns TOO_LATE, and published PCM pins its source
until returned and reaped. A validated empty successor may become READY but has no
PCM to publish; its transition scheduling is also not implemented yet.
