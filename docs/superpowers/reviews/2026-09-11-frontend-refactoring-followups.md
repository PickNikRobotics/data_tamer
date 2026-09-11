# Frontend refactoring and improvement follow-ups

Recorded on 2026-09-11 after completion of `lockfree-frontend` at `5bf82e4`.

This document preserves the discussion of the five implementation rulings and
the simpler sink-lifecycle alternative. It is a backlog of possible improvements,
not an active implementation plan. The current safety rules and API behavior
remain in force until a replacement is justified and validated.

See the [completed plan](../plans/2026-09-11-lockfree-frontend-plan-4.md),
[implemented specification](../specs/2026-09-10-lockfree-frontend-design.md),
[measurements](../../benchmarks/2026-09-plan4.md), and
[independent design audit](2026-09-11-simpler-design-audit.md).

## Suggested order

| Priority | Follow-up | Trigger or reason |
|---|---|---|
| First | Profile the latency regression; compare the current spin policy with immediate PI blocking | The pinned two-sink median increased from 20,478 ns to 32,896 ns; the cause has not been isolated |
| Before modifying reclamation | Add a deterministic sink-unpublication overlap test | This is the explicitly accepted coverage gap |
| When touching concurrency tests | Keep observable completion checks; exercise native non-Linux builds | Linux-only observation and host fallback compilation have limited platform coverage |
| If profiling identifies a material cost | Investigate weaker ordering for specific atomic operations | Requires a replacement ordering proof, not just a faster benchmark |
| If callers need different behavior | Revisit control-call ergonomics and capacity semantics | These changes alter or extend the public contract |
| Separate bounded experiment | Prototype stop/join/drain/restart | Credible missed alternative; total lifecycle complexity has not been compared in a prototype |

## 1. Atomic ordering: optimize only with a replacement proof

### Current decision and reason

The epoch, sink publications, registered/enabled flags and dirty-mask handshake
use sequentially consistent atomics. Scalar payload values and diagnostic
counters retain relaxed atomics.

The original acquire/release sketch allowed a dangerous combination: a snapshot
could announce itself and still read an old sink pointer, while removal could
unpublish that pointer and still observe an old idle epoch. The controller could
then destroy a link still being used by the snapshot. Acquire/release alone does
not establish a shared ordering between these separate atomic variables.

The current SC handshake closes that store-buffering hole. Registration liveness
is also separate from requested enablement, so `setEnabled(true)` cannot revive
detached storage.

### Possible improvement

Profile the complete path first. If the handshake is a material contributor,
investigate weaker ordering for individual operations or a smaller equivalent
handshake. Do not weaken all atomics together or assume that the measured median
regression was caused by SC: that causal relationship has not been established.

Acceptance requires an explicit ordering argument covering old sink pointers,
cached masks, initialized replacement holders, exceptions and every early return.
Then run focused lifetime tests, ASAN/UBSAN and TSAN, and repeated controlled
before/after measurements. Passing stress tests or TSAN is supporting evidence,
not a proof that weak-memory executions are safe.

**Tradeoff if the current choice is too conservative:** extra ordering overhead.
**Risk of an incorrect optimization:** use-after-free or access to invalid value
storage. Keep the stronger ordering unless a measured benefit justifies the
additional proof and maintenance burden.

Relevant code: [channel.cpp](../../../data_tamer_cpp/src/channel.cpp) and
[shared_state.hpp](../../../data_tamer_cpp/include/data_tamer/details/shared_state.hpp).

## 2. Control-call restrictions: improve usability only for demonstrated needs

### Current decision and reason

Other threads may remove sinks and destroy `LoggedValue`s while logging. The
restrictions do not require the application to stop logging first.

Control operations must run outside writer/proxy guards and serializer callbacks.
On the snapshot thread itself, make control calls between snapshots. Calls through
the channel object also require ordinary external object-lifetime synchronization.

A deadlock is possible if a writer holds `scopedWrite()`, a snapshot enters and
waits for that mutex, and the writer destroys a `LoggedValue`: its destructor
waits for the active snapshot, which is waiting for the writer. A serializer
calling removal can similarly wait for its own snapshot. Bypassing the wait can
free storage that the snapshot still needs.

### Possible improvement

Keep examples explicit about guard scope and destruction order. If real callers
need reentrant control operations, evaluate deferred control requests or deferred
destruction as a separately specified extension. Avoid adding a generic command
queue or reclamation framework without that need.

Any extension must define when removal becomes effective, who owns deferred
objects, how errors are reported, and how shutdown completes. It must preserve
snapshot-side allocation and locking guarantees. Validate the writer/snapshot
deadlock cycle, serializer reentrancy, exceptions and object lifetime.

**Tradeoff of the current rule:** some caller patterns need adaptation.
**Cost of extending it:** additional state, ownership and shutdown obligations.

## 3. Capacity API: decide whether applications need an exact ceiling

### Current decision and reason

| API or behavior | Current semantics |
|---|---|
| `setPoolCapacity(0)` | Throws `std::invalid_argument` |
| `setPayloadCapacity(0)` | Requests automatic reservation |
| Initial payload reservation | At least `max(hint, 2 * initial_serialized_size, 256)` per slot |
| Non-strict mode | The acquired slot grows when needed; growth is counted |
| Strict mode | An oversized payload drops against the acquired slot's actual capacity |
| Runtime strict-mode changes | Preserve existing capacities, including earlier growth |
| Pool and payload-hint setters | Rejected after the first snapshot attempt, including one without sinks |

The payload hint is a minimum, not a ceiling. A 4 KiB hint with an initial 6 KiB
payload reserves at least 12 KiB per slot. After non-strict growth, slots may have
different capacities; a later strict-mode payload can fit one slot and exceed
another. Warming alone does not bound future non-strict growth.

### Possible improvement

If callers need a predictable byte budget or uniform oversize behavior, first
specify whether they want an exact logical payload limit, uniform reservations,
or a way to finish warming before entering strict mode. These are different
requirements. An exact payload limit also does not by itself specify total
allocator or metadata memory usage.

Do not change the existing hint semantics silently. Check compatibility and
cover initial payloads above the hint, strict toggles after partial growth,
heterogeneous slot capacities, overflow, failed setup and retries. Runtime
strict toggles must not unexpectedly allocate or resize every slot.

**Tradeoff of current behavior:** simple implementation and retained capacity,
but a less uniform user-facing limit. **Cost of changing it:** API migration or
new configuration surface. Strict mode must still not be advertised as preventing
arbitrary user serializers from allocating or throwing.

## 4. Concurrency tests: preserve observable events and improve portability

### Current decision and reason

Fixed sleeps cannot establish that another thread entered a mutex operation or
finished consuming a queue. Two such assumptions failed during execution: a
mutex waiter could start after a 500 microsecond hold, and a sink could have
delivered only 2 of 10 records after a 1 millisecond sleep.

Portable tests now check exclusion, returned ownership and consistent accounting.
Linux-specific tests keep a mutex held and observe the waiter's sleeping state
through `/proc/self/task/<tid>/stat`. Their thread body makes the tested mutex
the only expected blocking operation after publishing the thread ID. The `S`
state alone is not a general-purpose identification of which mutex a thread is
waiting for. Registry tests explicitly drain queued work before checking counts.

### Possible improvement

Preserve this separation when extending tests. Use existing acknowledgements or
drain operations before adding sleeps. Keep procfs-specific checks conditional,
with portable ownership checks available elsewhere. If a platform-specific
replacement is needed, keep it confined to tests.

Run native macOS/Windows builds when those platforms are available. Current
non-Linux evidence is a host `-U__linux__` compile and a Generic-system CMake
target-selection check, not native execution. The privileged PI experiment also
remains unverified here because `CAP_SYS_NICE` was unavailable.

**Tradeoff:** procfs observation adds a small Linux-specific test dependency.
It may need adaptation if platform facilities change. Avoid converting observed
blocking behavior or PI behavior into a universal timing guarantee.

Relevant helper: [wait_for_sleeping_thread.hpp](../../../data_tamer_cpp/tests/wait_for_sleeping_thread.hpp).

## 5. Sink removal: strengthen the precise overlap test

### Accepted coverage limitation

The removal test pauses a serializer and starts a removal thread. Its promise is
fulfilled before `removeDataSink()` is entered. The scheduler can pause that
thread after the promise, allowing serialization to finish before unpublication.

The test proves queued and retained references survive the removal request and
subsequent channel/sink destruction. It does not deterministically prove that an
unpublished sink link remains alive while removal waits for the paused reader.
Its name and comment now state that limit.

Complementary evidence includes the SC ordering review, concurrent control stress,
sanitizer runs, and an unregister test that observes changed liveness while
serialization is paused. These do not replace the missing sink-specific event
observation.

### Possible improvement

Before changing sink reclamation, consider the smallest internal test observation
point that acknowledges actual unpublication. A stronger test should pause the
reader, observe the link being unpublished, establish that reclamation cannot
complete yet, release the reader, then verify completion and retained-reference
lifetime. Another pre-call flag or a longer sleep does not establish this order.

Prefer a narrowly scoped internal/test-build hook over a new public API. Keep it
out of the production snapshot path and avoid a general instrumentation framework.

**Tradeoff of deferral:** a sink-specific overlap regression has less direct test
coverage. **Cost of strengthening it:** test instrumentation and maintenance of
an implementation-coupled test. This remains the explicit residual coverage gap.

Relevant tests: [channel_control_tests.cpp](../../../data_tamer_cpp/tests/channel_control_tests.cpp).

## 6. Backend alternative: stop, join, drain, restart

### Proposed simplification

The current worker and manual drainer can both dequeue. Two `DataSinkBase`
delivery mutexes preserve callback order and prevent the worker from repeatedly
taking precedence over a waiting drainer.

An alternative gives the queue one active consumer at a time:

1. When finishing, close admission and wait for already admitted enqueues.
2. Stop and join the worker.
3. Drain accepted snapshots synchronously.
4. Restart the worker only when recording or the prior running state requires it.

This could remove both base delivery mutexes and their coordination argument.
It would retain the queue, pool, reference ownership and admission barrier.
Sink-specific locks may still be needed.

### Costs that a prototype must include

Worker restart requires remembering intentional stop state, handling thread
creation failure and reopening admission safely. General drains can introduce
thread churn. Joining can wait for the current callback plus the queue's 50 ms
timeout; 50 ms is not a bound on total shutdown duration. MCAP must not join while
holding a lock that its callback needs.

A general `processQueuedSnapshots()` call also needs defined behavior while
producers continue publishing. It must not restart an explicitly stopped sink or
silently accept work that no worker can deliver after a restart failure.

### Comparison and decision criteria

Build a small alternative on a separate branch and compare total lifecycle code
and ownership obligations, not just mutex count. Validate continuous-attachment
FIFO, manual drain with active producers, close during enqueue, retained refs,
callback exceptions, MCAP finish/restart/rollover and restart failure. Measure
drain/shutdown latency, thread churn, idle CPU and delivery latency.

**Assessment:** a credible missed alternative that deserved comparison earlier.
No prototype has established a net simplification yet. The existing backend
remains the default until that comparison supports a change.

Relevant code: [data_sink.cpp](../../../data_tamer_cpp/src/data_sink.cpp) and
[mcap_sink.cpp](../../../data_tamer_cpp/src/sinks/mcap_sink.cpp).

## Related small experiment: immediate PI blocking

Compare the current nominal 2,000 ns spin against one `try_lock()` followed by
immediate blocking acquisition. Immediate blocking removes the clock-polled loop
and activates PI sooner; a short spin may avoid a sleep/wakeup when another CPU
is about to release the mutex. Neither policy is universally faster.

Use repeated paired runs on the same machine and affinity, with no competing
builds/tests. Cover no writer, scalar writers, transactions and vector updates;
record median/tail/max latency, blocking counts, wait duration, exact allocations
and all drop counters. Exercise shared-core and separate-core arrangements, and
record requested versus actual scheduling. Preserve negative results and do not
combine this experiment with an atomic-ordering or queue redesign.

The existing pinned comparison showed p50 increasing from 20,478 ns to 32,896 ns,
while the observed maximum decreased from 214,852 ns to 181,903 ns. Those results
justify investigation; they do not identify the cause or prove either proposed
optimization will help.
