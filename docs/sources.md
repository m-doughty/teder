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
PLAY/PAUSE/STOP and gain/mute use the [transport contract](api-contract.md#transport-controls).
STOP reserves the media slot through cancellation cleanup and staged rewind. It
retains source identity, advances epoch and returns READY at frame zero. A failed
rewind leaves FAILED. The [playback clock](api-contract.md#playback-clock)
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
float at a private caller-selected rate in the same range; zero selects the native rate.
Resampling supports ratios from 1:64 through 64:1, inclusive. More extreme ratios
return UNSUPPORTED before priming; the float filter loses precision at those cutoffs.
Matching rates remain supported across the full source range. Capacity exhaustion returns
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
Gapless transitions concatenate these per-source converted outputs, including
source-format changes. Each source starts with its own filter history, trims its
reported startup latency and drains to its exact output-frame count. Filter tail
inside that duration is retained; no additional decay frames extend the track.
Frame rounding is per source, with less than one output frame of rounding per track.

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
so the final native buffer is not cut off. Empty validated input without a committed
successor ends without starting output. Public underruns exclude pause/EOF silence. Source positions
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

## Gapless playback

Adjacent sources may differ in sample rate, channel count and bit depth. Each
source must satisfy the input and conversion-ratio limits above. Both convert to
the selected device's fixed stereo float format before entering the PCM queue.
The device stays open across the boundary, with no rate switch or callback-side
conversion. An unsupported conversion returns UNSUPPORTED during preparation,
preserving current playback and the previous ready successor.

Select an output, open the first track without AUTOPLAY, and prepare its successor.
Wait for successful preparation before submitting PLAY with a fresh current target.
For an already playing track, prepare the successor early. The producer appends
its PCM once the current source is fully decoded. Neither event polling nor a
host EOF callback is needed for the transition. READY remains replaceable;
COMMITTED means successor PCM has been published and replacement/clear is TOO_LATE.

The callback consumes the end of one source and the beginning of the next inside
one output buffer when needed. It adds no overlap, fades or silence, and does not
restart the device. Matching-rate output equals the concatenated source samples,
including recorded leading/trailing silence. Per-source stereo conversion
applies when the device rate differs. Conversion positions remain estimates.

The first submitted successor frame changes `status.current.source` and `track`,
clears the next fields, and emits TRANSITION with both source/track identities and
the exact submitted `output_frame`. The epoch and output clock stay continuous;
source position and duration switch to the successor. Repeated labels/files keep
distinct source IDs. Events may be dropped by the bounded telemetry ring; playback
and current status continue independently. The public API retains one successor.

Empty sources use a zero-frame boundary marker. An empty successor advances
identity at the preceding source's endpoint without adding a sample. An empty
first source can lead into a prepared successor through PLAY. An isolated empty
source still ends without starting output. Callback marker processing is bounded
by the PCM pool size.

Prepare early enough to queue sufficient PCM before the native boundary. At least
two PCM blocks are needed to queue both sides; the default pool has 64. Smaller
pools or delayed I/O can miss the deadline even while status says READY. If the
callback wins the EOF cutoff before successor publication, the sequence ends;
late preparation reports TOO_LATE, and a losing publication reports an ERROR
with TOO_LATE instead of starting another track. A linked but starved stream
reports underruns and initialized silence. It cannot report that silence as media.

A decode error before adoption discards the successor and preserves current audio.
After adoption, a decode error fails the stream and quiesces output. Published PCM
continues to pin its source until returned and reaped. Seek/stop/load/device changes
flush through the quiescence protocol. Final sequence submission sets
ENDED/SUBMIT_EOF; hardware latency remains unknown, so this is not an audible-end
measurement. Empty-marker publication preserves an already completed EOF seek.
