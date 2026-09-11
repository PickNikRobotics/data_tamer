# Lock-free frontend, Plan 4: control lifetime and direct serialization

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete specification steps 7–9, validate the complete frontend, and push the completed `lockfree-frontend` branch.

**Architecture:** Keep atomic scalar values, the existing channel PI write mutex, pooled references, and blocking sink queues. Use one channel-local single-reader epoch to retire values and sink links on the control thread. Remove the temporary payload copy by serializing directly into a reserved pool slot.

**Tech Stack:** C++17, existing moodycamel queue, GoogleTest, CMake presets, ASAN+UBSAN, TSAN, existing benchmark/allocation harnesses.

**Spec:** `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md`

**Status:** Implementation, documentation, final review and verification
complete. Remote delivery is pending.

## Global Constraints

- Exactly one thread per channel calls `takeSnapshot()` (the *snapshot thread*).
- `setEnabled()` is lock-free and callable from any thread while logging.
- Destroying a `LoggedValue` and `removeDataSink()` remain memory-safe while logging; they may block the *calling* thread, never the snapshot thread.
- Backend threads may lock and allocate; pre-allocation there is welcome but not required.
- Preserve C++17, `Snapshot`, wire format, `addChannel`/`storeSnapshot` signatures, compatible same-name re-registration, and retained references surviving channel destruction.
- Keep the default pool at 64, maximum attached sinks at 8, default queue minimum at 1024, and the existing 2000 ns writer spin budget. Add no dependency or general reclamation framework.
- First-call freeze may lock and allocate. After freeze the only snapshot mutex is `WriteMutex`; pool exhaustion precedes serialization. Non-strict payload growth is the documented allocation exception.
- Control operations must run outside writer guards and outside serializer callbacks. Calls between snapshots are legal. Destruction of a channel still requires ordinary external synchronization of calls using that channel object.
- Setup registration is externally separated from writer access until freeze; after freeze the series layout and schema are immutable. `getActiveFlags()` is a snapshot-thread-only view of the last rebuilt mask, valid until the next snapshot or channel destruction.
- Each implementation task needs focused red/green evidence and a full Debug run; the controller runs ASAN+UBSAN and TSAN before marking it complete. Run test presets sequentially because existing MCAP fixtures share filenames.
- Root owns commits, full verification, benchmark measurements, and the authorized push. Workers do not spawn agents.

## Design corrections and execution scope

The old §5.1 acquire/release pseudocode permits store-buffering: removal can observe an old even epoch while a reader observes the old link. Use sequential consistency at the epoch, sink publication, and mask-dirty handshake boundaries. Document the ordering argument alongside the implementation. Sanitizers supplement that argument; they do not prove it.

Keep registered liveness distinct from requested enablement, preferably two bits in one atomic byte with sequentially consistent flag operations. An enable operation changes only the enabled bit; a dead field never becomes active. The cached mask is refreshed by a sequentially consistent dirty exchange on every snapshot that will serialize. Unregistration clears liveness, publishes dirty, waits out the observed reader, then detaches the holder. Re-registration initializes the holder before publishing liveness and dirty. Cached mask reuse is safe only with this handshake; do not rely on an acquire load happening to see a recent store. Registration IDs identify slots, not generations: after compatible reuse, an old ID still denotes that slot.

The alternative-design audit in `docs/superpowers/reviews/2026-09-11-simpler-design-audit.md` remains the whole-plan recommendation: finish this minimal protocol; defer stop/join/drain/restart and spin-policy experiments. No backend redesign is included.

### Task 1: Publish channel control changes with safe reclamation

**Files:**
- Modify: `data_tamer_cpp/src/channel.cpp`
- Modify: `data_tamer_cpp/include/data_tamer/channel.hpp`
- Modify: `data_tamer_cpp/include/data_tamer/details/shared_state.hpp`
- Modify if detaching requires it: `data_tamer_cpp/include/data_tamer/values.hpp`
- Create: `data_tamer_cpp/tests/channel_control_tests.cpp`
- Modify: `data_tamer_cpp/tests/CMakeLists.txt`, `data_tamer_cpp/tests/shared_state_tests.cpp`

**Interfaces:** Consume the existing `SnapshotPool`, `SnapshotRef::clone()`, `DataSinkBase` producer-token factory and `tryPush`, `ValuePtr` type comparison, and shared `Transaction`. Produce one `control_mutex`, an immutable post-freeze series layout, a fixed array of eight atomic `SinkLink*` publications with control-owned links, atomic attachment drop counters, and exception-safe epoch entry/exit in `takeSnapshot`. Keep the temporary serialization/copy bridge in this task so direct serialization has its own gate. Supply `getActiveFlags()` using that bridge's mask; Task 2 moves the mask to its own storage.

- [x] **Step 1: Add deterministic failing lifetime/control tests.** Use a stopped test sink with manual drain and a custom serializer whose size pass can pause via a condition variable. These are test-only helpers. Check eight sinks, duplicate idempotence, ninth-sink rejection, null-sink rejection, schema registration, and removal. Hold an existing serializer at its size pass, start unregister on another thread, prove it has not returned before releasing the serializer, then free the value and re-enable its stale ID; subsequent snapshots must omit it. Preserve compatible re-registration and reject changed types.

```cpp
auto channel = LogChannel::create("control");
auto value = std::make_unique<uint64_t>(42);
auto id = channel->registerValue("value", value.get());
channel->addDataSink(sink);
ASSERT_TRUE(channel->takeSnapshot());
channel->unregister(id);
value.reset();
channel->setEnabled(id, true);
ASSERT_TRUE(channel->takeSnapshot());
sink->processQueuedSnapshots();
EXPECT_FALSE(GetBit(sink->last_mask, 0));
EXPECT_TRUE(sink->last_payload.empty());
```

Also run same-name `LoggedValue` create/destroy churn, concurrent enable toggles, and sink add/remove churn during snapshots; validate every delivered payload against its mask. A blocking `addChannel()` on a control thread after freeze must not stop snapshots to the existing sink. Cover removal during publication using a paused serialization and queued references after channel destruction. Keep the existing retained-ref tests.

- [x] **Step 2: Run the focused Debug tests and record actual failures.** From `data_tamer_cpp`: `cmake --preset debug`, `cmake --build --preset debug -j2`, then `ctest --preset debug -R 'ChannelControl|ChannelSharedState'`. Expected failures include stale-ID resurrection and the absent sink limit. Do not add timing-only assertions where a synchronization gate can establish the event order.

- [x] **Step 3: Implement one publication/reclamation protocol.** Use atomic registered/enabled bits, while preserving `isEnabled()` as effective enabled-and-registered state. Update only enablement in `setEnabled`. All epoch observations/updates, sink-pointer publication/load, and dirty publication/exchange use `std::memory_order_seq_cst`. Use an allocation-free local RAII epoch guard.

```cpp
void waitQuiescent() // control_mutex held; never called inside takeSnapshot
{
  const auto observed = epoch.load(std::memory_order_seq_cst);
  while ((observed & 1) && epoch.load(std::memory_order_seq_cst) == observed)
    std::this_thread::yield();
}
// unregister, under control_mutex:
// clear registered bit; publish mask_dirty=true; waitQuiescent(); detach holder.
// replace, under control_mutex:
// validate old type; initialize holder; publish registered+enabled; publish dirty.
```

Keep type identity after detachment so re-registration remains checked. If a small `ValuePtr` detach method is needed, clear only storage/serialization ownership while retaining type metadata. All destruction/allocation belongs to the controller. Preserve token-before-sink destruction. Drop counters are relaxed atomics. Add/remove/getters use only `control_mutex`; after freeze snapshots load their local link array without it. A failed `addChannel` or token allocation must not publish a partially constructed link. Freeze happens on the first snapshot call even when no sinks exist; prepare all links before reporting it complete, and keep retries safe after exceptions.

Move the registration lock outside each public registration template so `_type_registry`, recursive type discovery, and schema updates are protected too. Use a private mutex accessor and a lock-held `registerValueImpl`, not a second or recursive mutex. After freeze, type lookup may only reuse existing definitions; rejected registration must not alter the frozen schema. Provide tests for a rejected new custom type and concurrent compatible custom re-registration.

- [x] **Step 4: Run focused tests, full Debug, then controller sanitizer gates.** Include serializer exceptions followed by another snapshot/unregister to prove epoch exit. Ensure snapshots never acquire `control_mutex` after freeze and no control-side holder mutation is reachable by a cached active mask. Record the memory-order proof and exact test output in the task report.
- [x] **Step 5: Commit:** `feat: publish channel control changes with safe reclamation`.

### Task 2: Serialize directly into reserved pool slots

**Files:**
- Modify: `data_tamer_cpp/src/channel.cpp`, `data_tamer_cpp/include/data_tamer/channel.hpp`
- Create: `data_tamer_cpp/tests/channel_capacity_tests.cpp`
- Modify: `data_tamer_cpp/tests/CMakeLists.txt`, `data_tamer_cpp/tests/sink_queue_tests.cpp`, `data_tamer_cpp/tests/snapshot_pool_tests.cpp`
- Modify: `data_tamer_cpp/benchmarks/rt_latency.cpp`, `data_tamer_cpp/benchmarks/rt_latency_cli_test.cmake`

**Interfaces:** Consume Task 1's control mutex, epoch guard, atomic sink links, shared liveness/dirty mask, and existing pool/ref ownership. Add `void setPayloadCapacity(size_t bytes)`, `void setPoolCapacity(size_t n)`, `void setStrictMode(bool strict)`, `uint64_t payloadReallocations() const`, `uint64_t droppedOversize() const`; extend `Stats` with `payload_reallocations` and `dropped_oversize`. Capacity setters throw after freeze; zero pool capacity throws `std::invalid_argument`; zero payload hint is valid (automatic minimum). Strict mode is atomic and may change at runtime.

- [x] **Step 1: Add failing capacity and allocation tests.** Cover freeze with no sink, invalid/after-freeze setters, configured two-slot exhaustion and recovery, strict drop/recovery, non-strict per-slot growth, a configured reservation avoiding growth, disabled oversized/dead fields, and exceptions releasing both slot and epoch. A serializer size counter must remain unchanged on a pool-exhausted call.

```cpp
channel->setPoolCapacity(2);
channel->setPayloadCapacity(256);
channel->setStrictMode(true);
ASSERT_TRUE(channel->takeSnapshot()); // freeze while vector is small
value->set(std::vector<double>(1024, 3.0));
EXPECT_FALSE(channel->takeSnapshot());
EXPECT_EQ(channel->droppedOversize(), 1u);
EXPECT_EQ(channel->payloadReallocations(), 0u);
channel->setStrictMode(false);
EXPECT_TRUE(channel->takeSnapshot());
EXPECT_EQ(channel->payloadReallocations(), 1u);
```

Use the allocation hook around only snapshot calls after initialization, with manually drained sinks: at least 10,000 calls with two sinks, both allocation and deallocation counts zero. Include dirty mask changes and concurrent control churn; control-thread allocations are outside the hook. Add a deterministic one-slot ref regression: release the first delivery before cloning the second, prove the parent prevents reacquisition, then prove the second delivery pins the slot after parent release. Retain channel two-sink fanout coverage to protect that ownership boundary in integration. Test all eight attachments and existing queue-full behavior. Keep synchronization deterministic; do not require every tight-loop publish to succeed when intentionally racing slow consumers.

- [x] **Step 2: Record expected compile/test failures for absent APIs, then implement.** At freeze, under the control and write mutexes, build the initial mask, compute size, and reserve each pool slot to `max(user_hint, 2 * size, 256)`. Check addition/doubling against vector limits before arithmetic; reject an impossible hint/count safely. Keep pool construction exception-safe. Cache schema hash and mask with independent snapshot-thread-owned storage; delete `Pimpl::snapshot` and the payload-copy bridge.

```cpp
auto* slot = pool->tryAcquire();
if (!slot) { ++pool_exhausted; return false; }
SnapshotRef parent(pool, slot);
// Under write_mutex: size only fields in the cached mask, then:
if (size > slot->snapshot.payload.capacity()) {
  if (strict_mode.load(std::memory_order_relaxed)) {
    ++dropped_oversize;
    return false; // parent and epoch guard release
  }
  slot->snapshot.payload.reserve(checkedDouble(size));
  ++payload_reallocations;
}
slot->snapshot.payload.resize(size);
// Serialize the same mask into this slot. Fill header/mask, publish parent clones.
```

Acquire the slot before mask rebuild, writer-lock acquisition, and size/serialize work. Keep parent ownership until every sink attempt ends, including failures. Count only successful growth allocations. Preserve exception propagation for user serializers/allocator failure; strict mode prevents capacity allocation, not arbitrary callback exceptions. Runtime strict toggles use existing per-slot capacity, including any previous non-strict growth.

- [x] **Step 3: Extend latency output with all channel counters and aggregate attachment drops.** Keep CLI validation; reject sink counts above eight with a normal diagnostic instead of an uncaught exception. Preserve allocation scope and existing benchmark semantics. After writers join and outside the timed loop, explicitly call `MCAPSink::finishQueueAndStop()` for MCAP sinks so the measured file contains every accepted record. Do not change backend lifecycle implementation.
- [x] **Step 4: Run focused tests and full Debug; controller runs ASAN+UBSAN, TSAN, Release and ROS shared gates.** Inspect the final function for control locks, allocations outside non-strict growth, extra payload copies, and exception leaks.
- [x] **Step 5: Commit:** `feat: serialize directly into configurable snapshot pools`.

### Task 3: Measure, document, review and push the full branch

**Files:**
- Create: `docs/benchmarks/2026-09-plan4.md`
- Modify: `README.md`, `data_tamer_cpp/CHANGELOG.rst`
- Modify: `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md`
- Modify: this plan's completion checkboxes and status.

**Interfaces:** Consume final public APIs/counters and the actual test/benchmark logs supplied by the controller. Documentation must describe the implemented contract, including SC ordering correction, first-call freeze, registration liveness, and exceptions.

- [x] **Step 1: Run final measurements with no competing builds/tests.** Save pre-change same-machine results from the Plan 3 executable. Run final Release `rt_latency --values 1000 --sinks 1/2/4 --writers 2 --transactions --seconds 10` separately; run vector writer and `--fifo` variants; run compressed MCAP `--values 1000 --sinks 1 --seconds 60 --mcap /tmp/data-tamer-plan4.mcap`. Record p50/p99/p99.9/max, allocations, all counters, requested versus actual scheduling. Default-pool MCAP exhaustion must be zero; investigate observed failures rather than deleting samples. Run the existing throughput cases and record comparisons with historical baseline as different-machine evidence, not a speedup proof.

```sh
build/release/benchmarks/rt_latency --values 1000 --sinks 2 --writers 2 --transactions --seconds 10
build/release/benchmarks/rt_latency --values 1000 --sinks 1 --seconds 60 --mcap /tmp/data-tamer-plan4.mcap
/home/davide/Apps/mcap-linux-amd64 info /tmp/data-tamer-plan4.mcap
/home/davide/Apps/mcap-linux-amd64 doctor /tmp/data-tamer-plan4.mcap
```

- [x] **Step 2: Write measured results and user documentation.** Explain first-call initialization and reservation, variable growth/strict drop semantics, bounded slot count versus variable byte count, per-attachment drops versus global pool exhaustion, 64-slot compression-stall sizing, eight sinks, queue block rounding, retained-ref starvation, same-name re-registration, control/write-guard restrictions, and scalar versus transaction consistency. Publish a baseline/final table with hardware caveats and actual maxima. Update every stale planned/bridge claim in the spec and API table, preserving historical Plan 1–3 records. Do not claim universal hard deadlines or no-throw custom serialization.
- [x] **Step 3: Review task documentation against actual APIs and supplied evidence, then commit:** `docs: publish final frontend guarantees and measurements`.
- [x] **Step 4: Whole-branch review.** Review from feature merge-base `e761f4f153b7a88138d99c843e59392c57b346da`; include all R1–R8, remaining steps 7–9 and the simpler-design audit. Use one final fix wave and one scoped re-review if necessary, with covering verification for any changed code.
- [ ] **Step 5: Finish.** Confirm all requirements implemented, required tests passing, docs accurate, working tree clean. Commit completion status, push `lockfree-frontend` to its existing `origin` tracking branch, and verify local and remote HEAD match. Mark the active goal complete only after the push is confirmed. Preserve all rulings in the final report and delete only this plan's SDD scratch directory.

## Plan self-review

- Spec coverage: Task 1 implements step 7 and lifecycle/publication requirements; Task 2 implements step 8, capacity APIs and complete allocation coverage; Task 3 implements step 9, remaining measurements and authorized delivery. Earlier steps remain covered by their completed plans and regression suite.
- Interfaces: Task 1 keeps the bridge temporarily; Task 2 replaces it while retaining its cached-mask accessor contract. Both use the same single-reader epoch. Task 3 reports actual final behavior rather than the superseded acquire/release pseudocode.
- Scope: no queue replacement, dispatcher, new dependency, generic RCU facility, or spin-policy experiment. Deferred alternatives remain explicitly documented.

## Completion audit

| Requirements | Implementation and regression coverage |
|---|---|
| R1 | One snapshot producer owns pool acquisition, cached mask and reader epoch. |
| R2 | Atomic scalars and nesting-aware transactions; grouped scalar/raw writes, vector races and observed sleeping-waiter tests. |
| R3 | Acquire before sizing, parent-owned fanout and bounded drop-newest queues; separate attachment, pool and oversize counters; overflow and retention tests. Strict mode fixes per-slot capacities; non-strict byte growth remains allowed. |
| R4 | Lock-free registered/enabled bits and SC mask publication; enable races, allocation checks and dead-slot tests. |
| R5 | First-attempt schema freeze and serialized type discovery; compatible dead-slot reuse and frozen custom-schema rejection tests. |
| R6 | SC single-reader publication/reclamation; paused-reader, churn, exception and retained-lifetime tests, with external object lifetime synchronization. |
| R7 | Existing sink callback signatures and wire format retained; in-tree ports, ROS shared builds and constructor/ABI checks. Public API/ABI changes are documented. |
| R8 | Backend queues, callback serialization, retained references and MCAP final drain; callback/order/close/restart tests. |

Steps 0–3 were delivered by Plan 1; its deferred mutex/pool measurements were
completed in Plan 2 (`a14c1ff`). Plan 2 delivered steps 4–5, and Plan 3 delivered
step 6. Plan 4 runtime steps 7–8 are `20fdbe0` and `16e812a`; `9d59fe2` repairs
test scheduling assumptions without changing the runtime. Step 9 evidence is
recorded in [the final benchmark report](../../benchmarks/2026-09-plan4.md).

Rulings recorded during execution, in order:

1. Strengthen epoch, sink, flag and dirty-mask handshakes to sequential
   consistency: the original acquire/release protocol permits store-buffering.
   Cost if wrong: ordering overhead and rework; weakening requires a new proof.
2. Require control calls between snapshots and outside writer guards/serializer
   callbacks, plus ordinary external channel lifetime synchronization. A
   reentrant quiescence bypass can free live data; a held guard can deadlock.
   Cost if wrong: callers relying on unsupported reentrancy need adaptation.
3. Reject zero pool capacity, treat zero payload hint as automatic, and apply
   strict mode to existing per-slot capacities. Cost if wrong: these boundary
   semantics may need revision before release.
4. Replace sleep-duration assertions with observed sleeping-waiter checks on
   Linux and portable ownership checks: a pre-call signal cannot prove mutex
   entry. Cost if wrong: procfs/platform changes may require test adaptation;
   runtime is unchanged.
5. Preserve the sink-removal test as queued/retained-lifetime coverage without
   adding a production publication hook. Deterministic unregister coverage,
   the shared SC epoch proof and control stress complement it; its pre-call
   signal does not prove unpublication. Cost if wrong: a sink-specific overlap
   regression may require a targeted internal test seam later.

Final whole-feature review covered `e761f4f..ea2d918`. Its portability finding,
PI comment corrections, allocation/churn progress check and documentation
corrections were resolved in `4dfe9a9`; a final Debug run exposed an old
sink-registry sleep assumption, repaired with explicit test-only draining in
`340632c`. One combined scoped re-review approved `ea2d918..340632c`, including
Task 3 spec compliance and quality. The removal-test coverage limitation above
is the only accepted residual review item; no open blocking finding remains.

Final verification at `340632c`: Debug 125 passed/one privileged skip out of
126 discovered; ASAN+UBSAN with leak detection, TSAN, Release and ROS each
124 passed/one privileged skip out of 125. Builds completed without compiler
or sanitizer diagnostics. The final exact-count MCAP confirmation recorded
zero allocations and false returns across 60,000 timed calls, zero pool/growth/
oversize/attachment counters, and 60,010 messages passing official MCAP checks.
Native macOS/Windows builds and privileged FIFO execution were unavailable;
the recorded fallback checks and scheduling limitations remain explicit.
