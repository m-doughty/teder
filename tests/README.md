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
  converter constructor. libFLAC has no public allocation hook; its internal
  heap is not included in fail-at-N coverage. Check/framework/OS allocations
  are also outside the tracker. KissFFT caller storage can use the arena.
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

`ctest --test-dir build -L lifetime --output-on-failure` selects the five lifetime
cases. `-V -R lifetime.failures` prints injected engine/result allocation sites
and exact retained baselines. Each constructor has one `arena.chunk` allocation;
size, UTF-8 and ABI failures occur before allocation. Tests cover deep copies,
empty/wrong-kind/index reads, unchanged error outputs, exact budget boundaries,
one-time transfer, close rejection and two concurrent readers after engine
destruction. Inputs are borrowed and immutable only during private construction.
The installed consumer exercises the exported lifecycle using the libc allocator.

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
