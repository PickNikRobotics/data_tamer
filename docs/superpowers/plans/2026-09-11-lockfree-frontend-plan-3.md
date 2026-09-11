# Pooled sink delivery — Plan 3 (spec step 6)

> **For agentic workers:** Use superpowers:subagent-driven-development to execute the tasks below without stopping between tasks.

**Goal:** Deliver one pooled snapshot to every sink through preallocated queues, wake consumers on enqueue, and reliably drain accepted work without polling sleeps.

**Architecture:** Keep the existing channel serialization and structure locks for this stage. Copy the finished snapshot into one `SnapshotPool` slot, then enqueue move-only `SnapshotRef` clones through one explicit producer token per channel/sink pair. `DataSinkBase` owns the blocking queue and serializes dequeue plus callback with one mutex. Plan 4 removes the temporary copy and channel structure locks.

**Tech Stack:** C++17, existing vendored moodycamel queue/semaphore, CMake presets, GoogleTest, existing per-thread allocation counter. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md`, §§2, 6–10, especially delivery step 6 and measurement step 6.

## Global Constraints

- Preserve `Snapshot`, wire bytes, schema hashes, `addChannel(const std::string&, const Schema&)`, and `storeSnapshot(const Snapshot&)`.
- Defaults: 64 pool slots, 1024 minimum queue entries, 50 ms idle wait. Queue capacity is block-rounded and shared across producers, not an exact per-channel limit.
- Remove `pushSnapshot`. Producer publication uses explicit tokens and `try_enqueue`, with no allocation or waiting for the consumer.
- Keep vendored queue headers unchanged and private. A private friend factory constructs `std::unique_ptr<moodycamel::ProducerToken>`; the public header only forward-declares the token. A sink owner must outlive its token.
- Backend queue traits use nothrow global `operator new` and matching `operator delete`, allowing the existing allocation hook to observe queue allocations without vendor modifications.
- Pool storage owns the channel-name string, so a retained reference remains valid after its channel is destroyed. Do not change `Snapshot::channel_name` from `string_view`.
- Correct the spec pseudocode: failed enqueue leaves the reference intact; RAII releases it exactly once. Accepted snapshots are processed even after acceptance closes. Serialize dequeue and callback together to preserve producer order with a manual drainer.
- Closing acceptance must wait for previously admitted enqueues before draining. Use one atomic admission word (closed bit plus active producer count), with CAS admission and an RAII count decrement. Control methods may wait; producers must not take a mutex. Start/stop/drain calls must be externally serialized and must not be invoked from `storeSnapshot` callbacks.
- Preserve the legacy callback signature. Add protected `SnapshotRef retainSnapshot() const`, valid only during a queue-dispatched callback on the callback thread; it explicitly clones the currently delivered reference. Direct calls to public derived `storeSnapshot` methods cannot retain queue ownership.
- `storeErrors()` counts exceptions from callbacks; errors do not kill the worker or strand references. Keep false callback returns distinct from exceptions, as in the spec.
- Derived destructors stop their worker before destroying state. Debug assertion catches missing calls; release fallback diagnoses and joins. Constructor unwinding must still propagate the original exception safely (allow base cleanup while an exception is active).
- No new channel epoch, fixed sink array, strict mode, reservation API, or direct pooled serialization in this plan. Existing channel locks are deliberately retained until Plan 4.
- Use focused tests during implementation. Before completion run debug, ASAN+UBSAN, TSAN, Release, and the available ROS checks; no sanitizer suppressions. Privileged PI skip remains documented.
- Continue the existing `lockfree-frontend` checkout: this is the user's ongoing branch, with the previously established read-only-git worktree fallback. Root coordinates git commits and broad validation.

## Task 1: Measure the old and new sink delivery with the same probe

**Files:** Create `data_tamer_cpp/benchmarks/sink_latency.cpp`; modify `data_tamer_cpp/benchmarks/CMakeLists.txt`.

**Interfaces:** Consumes existing channel and sink APIs only; produces `build/release/benchmarks/sink_latency [--idle]`. It must compile before and after Task 2.

- [ ] Implement a small sink whose callback records `steady_clock::now() - snapshot.timestamp` into a pre-reserved vector. Send exactly 10,000 snapshots at 1 kHz, passing a steady-clock timestamp explicitly to `takeSnapshot`. Record successful/failed submissions and p50/p99/p99.9/max delivery delay. Do not mix wall-clock and steady-clock epochs.
- [ ] Stop the worker and then drain before reading results, so this probe is safe against the old backend too. All callback storage is pre-reserved; reject unknown arguments with a nonzero exit code.
- [ ] `--idle` creates one sink with no traffic, measures process CPU ticks from `/proc/self/stat` before/after a 10-second steady-clock interval, and prints ticks, clock ticks/second, elapsed time and CPU percentage. Parse fields after the closing parenthesis of the process name. Non-Linux builds report that this measurement is unavailable.
- [ ] Add `CompileBenchmark(sink_latency Threads::Threads)`. Build Release, run both modes on CPUs 0–5, capture complete pre-change output under `/tmp/data-tamer-plan3-before-*`, and check unknown arguments fail. No artificial performance threshold.
- [ ] Review the implementation and commit `bench: measure sink delivery latency and idle CPU`.

## Task 2: Pooled transport, channel bridge, and sink lifecycle

**Files:** Modify `include/data_tamer/data_sink.hpp`, `src/data_sink.cpp`, `include/data_tamer/details/snapshot_pool.hpp`, `include/data_tamer/channel.hpp`, `src/channel.cpp`, `include/data_tamer/sinks/{dummy_sink,mcap_sink}.hpp`, `src/sinks/mcap_sink.cpp`, `tests/{snapshot_pool_tests.cpp,CMakeLists.txt}` (all under `data_tamer_cpp/`). Create `tests/sink_queue_tests.cpp`. Modify other sink constructors only if they expose queue capacity.

**Interfaces consumed:** Existing `SnapshotPool`, `SnapshotRef`, channel locks and shared write transactions. **Produced:** `DataSinkBase(size_t queue_capacity = 1024)`, private friend `makeProducerToken()` and `tryPush(token, SnapshotRef&&)`, public `storeErrors()`, protected `retainSnapshot()`, channel `poolExhausted()`/`droppedSnapshots(sink)` plus `Stats::pool_exhausted`, optional final `queue_capacity` argument on MCAP and Dummy constructors.

- [ ] Add failing tests before implementation for retained payload/mask/name after channel destruction, bounded queue overflow versus pool exhaustion, successful/failed fanout reference release, two channels sharing a sink with concurrent manual draining, exception recovery, closing acceptance during publication, and debug destructor enforcement. Use stopped workers or explicit condition-variable handshakes for deterministic overflow and in-flight callback tests; never timing guesses for correctness.
- [ ] Give `SnapshotPool` an optional owned channel name, set every slot's view to it, and keep that view when copying into slots. Existing three-argument construction remains valid. Add a lifetime regression that destroys the source name/channel before checking the retained view.
- [ ] Implement `BlockingConcurrentQueue<SnapshotRef, QueueTraits>`, sized in the base constructor. Construct members before launching the worker; launch only after the base `_p` is assigned. Catch exceptions in one shared delivery helper used by both worker and drainer, increment `store_errors`, and reset the current reference before releasing the store mutex.

  The worker loop has this ordering:

  ```cpp
  while (run.load()) {
    std::unique_lock lock(store_mutex);
    if (!run.load()) break;
    if (queue.wait_dequeue_timed(current_ref, std::chrono::milliseconds(50))) {
      deliver(self); // catches callback exceptions, then current_ref.reset()
    }
  }
  ```

  `processQueuedSnapshots()` holds the same mutex through the entire nonblocking drain. `stopThread()` stores false and joins without taking that mutex. The timeout bounds only the semaphore wait, not callback duration, mutex fairness, or OS scheduling.

- [ ] Gate producer admission with one atomic closed/count word. A successful CAS increment admits the producer; an RAII decrement follows `queue.try_enqueue(token, std::move(ref))`. Failure does not manually decrement slot references. `stopAcceptingSnapshots()` sets the closed bit and waits for the active count to become zero; `startAcceptingSnapshots()` clears the closed bit. Neither worker nor drainer tests acceptance after dequeue.
- [ ] Replace the channel sink set with ownership of a sink and its token (sink destroyed after token), keeping `sinks_mutex` around insertion, removal and publication. Duplicate attachment does not create another producer. On first finished serialization create the 64-slot pool sized to payload and mask. Acquire a slot, copy payload/mask/hash/timestamp while the channel snapshot mutex is held, and keep a parent `SnapshotRef` until every enqueue attempt has finished:

  ```cpp
  auto delivery = parent.clone();
  if (!sink->tryPush(*token, std::move(delivery))) {
    ++dropped;
    all_pushed = false;
  }
  ```

  No sinks or pool exhaustion returns false. Pool exhaustion increments an atomic channel counter; failed publication increments that link's counter. Expose counters with their existing lock discipline, and document one snapshot producer per channel.
- [ ] Add the callback-only retention helper without changing the virtual callback. Assert the worker has been stopped in normal debug destruction, and preserve constructor exception propagation. The base destructor's release fallback is diagnostic, not a replacement for derived cleanup.
- [ ] Move MCAP `merged_payload` from thread-local storage into Pimpl (retain capacity between writes). Remove both the 250 µs sleep and the redundant second drain from `finishQueueAndStop`. Preserve the writer mutex for public direct calls. Cover file finalization with a regression that checks every accepted message is written, including a second recording after explicit restart. Explicit restart clears `forced_stop_recording` and reopens acceptance; automatic rollover inside a callback must not reopen acceptance that a finishing control thread has closed. Forward queue capacity from MCAP/Dummy constructors; other default constructors remain source-compatible.
- [ ] Prove zero producer allocations/deallocations after pool creation at a fixed payload size, including filled-queue failure and successful fanout. The queue traits make this check cover queue allocations too. Keep the whole frontend zero-allocation claim reserved for Plan 4's complete warmup/reservation contract.
- [ ] Run focused tests, then the full Debug suite. Review the complete transport diff and commit `feat: deliver pooled snapshots through blocking sink queues`.

## Task 3: Validate and publish measured results

**Files:** Create `docs/benchmarks/2026-09-plan3.md`; modify the design spec, this plan's checkboxes/status, and `data_tamer_cpp/CHANGELOG.rst`. No runtime changes unless validation exposes a defect.

**Interfaces:** Consumes Task 1 probe and Task 2 transport; produces reproducible evidence and exact API/lifecycle documentation.

- [ ] Run all four CMake preset gates and the available ROS constructor/publisher and ABI checks. Exercise compressed MCAP and the writer example; inspect files with `/home/davide/Apps/mcap-linux-amd64 info` and `doctor`.
- [ ] With builds/tests idle, rerun the same pinned Task 1 measurements. Run `rt_latency` with 1000 values, one/two/four sinks and two transaction writers for 10 seconds each; record allocation counts, failures, wait counters and distributions. A valid before/after delivery comparison uses Task 1's same-machine output, not the older machine's historical baseline.
- [ ] Measure syscalls on the snapshot thread using `strace -c` without `-f` (main thread publishes); report its periodic scheduling syscalls separately from futex wakeups. Trace before/after binaries if available. If strace/perf access is restricted, record the exact limitation and available evidence, without claiming unmeasured counts. Capture idle context switches externally if useful to distinguish a sub-tick CPU result from zero wakeups.
- [ ] Correct spec §§3/6 pseudocode for RAII failure ownership, acceptance barrier, dequeue/callback ordering, retained channel-name storage, callback retention API, and timeout qualifications. Replace the hard-coded 16-byte reference assumption with `sizeof(SnapshotRef)` (24 bytes on this build if confirmed). Update API delta and changelog with `pushSnapshot` removal, queue sizing, drop/error counters and derived-sink lifecycle requirements.
- [ ] Run the final whole-branch review, resolve required findings, record measured results and remaining Plan 4 scope. Commit `docs: record pooled delivery validation and measurements`.

## Self-review

Task 1 deliberately uses only preexisting APIs, making its baseline comparable. Task 2 changes transport and its channel caller together so removing `pushSnapshot` leaves no intermediate broken build. Pool-owned names, admission quiescence, and serialized dequeue/callback close lifetime and ordering gaps in the original pseudocode. Task 3 checks both functional behavior and the actual semaphore cost; no performance target substitutes for measurement. Channel epoch publication and direct pooled serialization remain Plan 4 work.
