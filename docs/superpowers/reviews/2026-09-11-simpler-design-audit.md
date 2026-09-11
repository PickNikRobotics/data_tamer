# Independent simpler-design audit — 2026-09-11

**Verdict:** Keep the atomic values, one PI write mutex, and pooled immutable delivery. There is a credible simpler sink-consumer design that the plan should have compared: stop/join, drain with one consumer, then restart. It is not a clear improvement worth reopening Plan 3 now. The smallest complete design still needs the remaining Plan 4 separation between control work and snapshots; stopping at the current locks does not satisfy the agreed requirements.

This is a read-only design challenge of the approved specification, Plans 1–3, the Plan 2 measurements, the Plan 3 task report, and the current implementation (production change `df16cef`). Only this report was added. Alternatives below are design analysis, not measured prototypes or fresh correctness certification. Source locations refer to the audited checkout; the specification's backend pseudocode was awaiting Plan 3 corrections at review time.

## What actually has to be solved

The hard requirements are the specification's R1–R8: one snapshot producer per channel; multiple writers and coherent transactions; wait-free lone scalar writes; bounded transport with drop-newest reporting; concurrent lock-free enable changes; setup registration; safe removal/destruction without making snapshots wait for control work; unchanged legacy sink callbacks; and unrestricted backend locking/allocation. The overall goal additionally prohibits snapshot allocations after initialization, with the explicit payload-growth exception. See `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md:7–30` and §4.6.

These do **not** independently demand zero-copy fanout, reference retention after callbacks, MPMC consumers, semaphore wakeups, eight sinks, or an epoch counter. Those are design choices. However, the accepted specification also promises queued delivery after channel destruction without waiting for the sink, live sink attachment/removal, retained snapshot handles, capacity configuration, and per-sink drop accounting (§§5.5–5.8, 6.1, 7). An alternative preserving only R1–R8 must not silently discard those additional commitments. Sink Pimpls were explicitly user-requested, not speculative scaffolding (`docs/superpowers/plans/2026-09-10-lockfree-frontend-plan-2.md:1872`).

Two qualifications matter when assessing any design:

- Bounded slot counts do not bound payload bytes when non-strict growth is allowed indefinitely. Zero-allocation and byte-budget claims require the configured payload ceiling/strict behavior or a workload that stays inside its reserved capacity (§4.6). This is already true of the accepted design, not an argument specific to alternatives.
- PI avoids priority inversion from a preempted owner; it does not turn arbitrary writer bodies, callback duration, or OS scheduling into a universal deadline. Plan 2's privileged PI case was unavailable, and its historical baseline was measured on another machine (`docs/benchmarks/2026-09-plan2.md:20–26`, “Correctness validation”). Do not use its numbers to prove either a hard deadline or a controlled speedup.

## Ranked decisions

| Rank | Alternative or decision | Same complete contract? | Recommendation |
|---|---|---|---|
| 1 | Keep atomic scalars + one PI write lock; finish direct pooled serialization and minimal control reclamation | Yes, once Plan 4 is implemented and validated | **Keep / complete Plan 4.** The present architecture is a reasonable minimum for the full contract. |
| 2 | Single active sink consumer: stop/join, drain, restart | Can preserve it with lifecycle/error handling | **Defer.** Best missed alternative, but not enough demonstrated net simplification to redo verified Plan 3. |
| 3 | `try_lock` once, then immediately PI-block, without the timed spin loop | Preserves R1–R8; changes an accepted tuning choice | **Defer to a targeted comparison.** Smaller code and earlier PI activation; contention latency may improve or regress. |
| 4 | Per-channel/sink SPSC queues, retaining the shared pool | Can preserve it, with registration/reclamation and consumer scheduling | **Keep existing queue.** Mostly replaces existing library machinery with new machinery. |
| 5 | Per-sink fixed payload buffers and producer copies | Preserves R1–R8; retained zero-copy handles require more ownership or a changed promise | **Do not switch now.** Attractive for a deliberately narrower, small-payload product, not a clear whole-library simplification. |
| 6 | One queue/dispatcher per channel, managing sinks entirely on backend | R1–R8 are achievable; short removal and non-waiting channel destruction need more machinery | **Consider only with changed latency/lifecycle expectations.** Stronger simplification of the snapshot side, with real backend tradeoffs. |

There is no recommended runtime change to Plan 3 from this audit alone. The recommendations are not based merely on sunk cost: replacing working code is warranted for a clear reduction in total correctness obligations. None of the alternatives demonstrates that reduction while retaining every accepted behavior.

## 1. The existing writer design is already the simple option

Scalar atomics plus a shared channel mutex cover two different promises: a lone write cannot wait, while a transaction must appear as a group. Replacing both with one mutex violates scalar R2; independent atomics alone lose transaction atomicity. The implementation already centralizes the difficult part in `ChannelSharedState::Transaction` and serializes both size and payload under the same lock (`data_tamer_cpp/include/data_tamer/details/shared_state.hpp:32–75`, `data_tamer_cpp/src/channel.cpp:291–328`).

Requiring every coherent set to be one user-owned struct would simplify application usage, and is already supported. It would not replace the general API without giving up cross-value/raw-pointer transactions. The thread-local transaction chain is small and serves existing nested `set()` behavior; replacing it with a new transaction framework is unnecessary.

The spec already considered seqlocks and triple buffering (§§11–12). Retrying or abandoning a snapshot on writer contention violates R2's progress/drop rule. Per-value triple buffering does not provide a cross-value transaction. Block-level copy-forward buffering adds storage, copying, and raw-pointer exceptions while complicating lone wait-free scalar writes. These are not overlooked simpler solutions.

The timed spin is more debatable. A successful first `try_lock`, otherwise a measured `lock()`, would remove the clock-polled retry loop in `details/write_mutex.hpp:96–130`. PI starts only when blocking; on one CPU, spinning at higher priority can delay the owner. Conversely, on different CPUs, a short spin can avoid a sleep/wake. The existing hold-time benchmarks measure the chosen spin scheme, not the required comparison with immediate blocking. Keep the mutex; compare these two acquisition policies before claiming the 2 µs spin is necessary. This is a small independently reversible change, not a reason to redesign values.

## 2. The missed sink alternative: stop/join rather than competing consumers

Today the worker and manual drainer both dequeue. They must acquire delivery exclusion **before** dequeue to prevent a later item being delivered before an earlier worker-held item. Holding one mutex across the worker's timed wait then caused observed barging/starvation; Plan 3 added a handoff mutex (`data_tamer_cpp/src/data_sink.cpp:48–75,133–148`). The implementation evidence reviewed for this audit included an ordering test taking **50,389 ms** and a repeated three-second watchdog failure before the fix. The checked-in plan records the starvation finding and resulting handoff design (`docs/superpowers/plans/2026-09-11-lockfree-frontend-plan-3.md:52–66`). The second mutex addresses a real failure.

A different ownership rule avoids this class of failure:

1. Close admission and wait for in-flight enqueues when finishing a recording.
2. Set the worker stop flag and join it.
3. Drain accepted references synchronously; there is now exactly one consumer.
4. Recreate the worker when recording explicitly restarts.

For general `processQueuedSnapshots()` calls while logging, preserve the old behavior by remembering whether a worker was running, joining, draining, then restarting only if it was previously running. Admission can stay open for this general case; overload remains bounded and may produce counted drops. Calls after an explicit `stopThread()` must not unexpectedly restart a worker. The header already requires control calls to be externally serialized and forbids them inside callbacks (`data_sink.hpp:98–108`), so that part needs no new public restriction.

**What disappears:** simultaneous consumers, both base delivery mutexes, and the handoff fairness argument. Per-producer callback order follows single-consumer ownership. Keep the current queue initially: it is still many-producer, even with one consumer. The admission barrier also remains necessary; a lone `accept=false` cannot prevent a previously admitted enqueue from landing after the drain.

**What is added:** worker start/restart code, remembering whether stopping was intentional, thread-creation failure handling, and careful sequencing of acceptance reopening. Restart allocation is legal on control/backend threads (R8), but failure must leave the sink stopped/closed rather than silently accepting undeliverable work. Joining can wait through the current callback and up to the 50 ms dequeue timeout; this is not a 50 ms bound on total completion. General drains introduce thread churn. A permanently running single-consumer worker with flush requests avoids thread churn, but introduces request/acknowledgment state and wakeup logic; it is not automatically simpler.

Only MCAP finalization uses manual drain in normal production sink code; the latency probe already stops then drains (`src/sinks/mcap_sink.cpp:180–190`, `benchmarks/sink_latency.cpp:43–44`). This makes the alternative plausible. But the protected drain method and explicit recording restart must continue working, and MCAP must not join while holding the mutex needed by its callback (`mcap_sink.cpp:118–120,197–221`). Reworking this now touches base lifecycle, MCAP restart, and the validated concurrency tests. **Verdict: a valid alternative that should have been compared earlier; retain the compact tested handoff today unless drain/lifecycle problems recur.**

## 3. SPSC rings and copies: fewer primitives, more surrounding work

**Per-edge SPSC queues of pooled handles.** R1 makes each channel/sink edge single-producer, provided the sink has one active consumer. This can eliminate explicit moodycamel tokens and give exact edge capacity and simpler enqueue operations. A single SPSC queue per sink is insufficient: different channels publish concurrently to the same sink, as exercised by `tests/sink_queue_tests.cpp:211–249`.

The complete replacement also needs sink-side enumeration of edge queues, fairness among busy/quiet channels, wakeup aggregation, add/remove synchronization, and queue lifetime after detachment while records remain. These are precisely the surrounding services obtained from the existing vendored queue. Its blocking wrapper documents producer-owned allocation blocks and supplies nonallocating explicit-token enqueue plus semaphore signaling (`3rdparty/ConcurrentQueue/blockingconcurrentqueue.h:48–59,232–254`). A custom ring may improve a measured contention/capacity problem; it does not remove most system ownership work. No new dependency or ring implementation is warranted now.

**Fixed buffers per edge, no shared payload pool.** Serialize once into channel scratch storage and copy those bytes into each accepting edge's preallocated slot. This keeps consistent values across sinks, bounds producer work, and eliminates cross-sink slot reference counts. Serializing each sink separately is not equivalent: unlocked scalar writers could produce different samples for the same snapshot. Variable payloads require an explicit slot ceiling/oversize policy; ordinary queueing of `Snapshot` by value does not solve this because its vectors copy/allocate. Plan 2 measured two allocations per sink per snapshot (`docs/benchmarks/2026-09-plan2.md:40–42`).

This is a legitimate R1–R8 design if callback data is borrowed until return. It trades roughly `K × payload_capacity` storage per channel for `S × D × payload_capacity` across S sinks with depth D, plus one scratch payload and S copies per snapshot. Example, excluding masks/metadata: 64 slots of 8,000 bytes are 512 KB; four equal-depth sink rings are 2.048 MB. A slow sink no longer consumes the fast sinks' payload slots, which is a real benefit: the current shared pool can exhaust and drop a snapshot for **all** sinks (§7).

Zero-copy retention beyond callbacks changes the conclusion. A ring slot cannot be reused while retained; either retention pins/references slots again, or the backend makes an owned copy. Backend copying is allowed by R8, but changes the accepted pooled-handle retention behavior and must own the channel-name string as well as its vectors. Retention is not inherently required by R7's `storeSnapshot(const Snapshot&)`, but it was expressly accepted in §6.1 and Plan 3. Removing the public retention helper alone saves little: concurrent fanout still needs internal reference lifetimes with the current pool. **Verdict: a simpler narrower contract exists; a complete replacement is not an obvious reduction now.**

## 4. One dispatcher per channel is another real architectural option

A channel could publish to one bounded queue. Its backend thread would own the sink traversal and call each sink under backend serialization. Sink attachment/removal would then never touch the snapshot path, eliminating that path's sink-pointer reclamation, per-edge tokens, and fanout reference operations. The existing scalar/transaction and value-removal machinery would remain.

This can preserve literal R1–R8: the producer never waits for sinks; a stalled backend fills its bounded queue and causes counted drop-newest; callbacks retain their signatures. But it changes delivery isolation: a slow sink delays every later sink, not merely reuse of pool slots after the backlog fills. Threads scale with channels instead of sinks; a sink shared by channels needs callback exclusion and scheduling. The simple version joins/drains its dispatcher during channel destruction and can make sink removal wait for callback I/O, rather than one snapshot duration. Preserving §5.6/5.7 instead requires an independently owned dispatcher with retired work/lifetime management, bringing complexity back.

**Verdict:** worth considering for an application with few channels and no latency-sensitive secondary sink, if the additional lifecycle expectations change. Not a silent substitute for the accepted library behavior. Keeping both the dispatcher and the existing sink workers adds a queue/thread hop and is not the minimum version.

## 5. What can actually be simplified in Plan 4

Retain the existing value atomics and PI lock. Remove the two structure locks from the snapshot path, acquire a free slot before serialization, and write into it directly. That deletes the staging payload and copy bridge (`src/channel.cpp:249–365`) and avoids doing serialization when the pool is exhausted. This is both the planned optimization and a genuine simplification of state ownership.

Some publication/reclamation protocol is necessary for concurrent `LoggedValue` destruction and sink removal. Merely retaining `Pimpl::mutex`/`sinks_mutex`, changing them to PI mutexes, or collapsing them into the write mutex leaves snapshots waiting for control operations; sink attachment currently invokes arbitrary `addChannel()` while holding the sink lock (`channel.cpp:166–180`). PI does not bound arbitrary callback or allocator work. An atomic `shared_ptr` topology is not automatically lock-free in this C++17 target, and releasing its last owner on the snapshot thread can run destruction there. Never reclaiming removed objects avoids a race by accumulating unbounded retired state. Those shortcuts do not meet the complete contract.

The minimum direction is the planned immutable post-freeze series layout, stable sink slots, atomic publication, and **one channel-local reclamation protocol**. Do not add a general RCU library, command framework, or separate reclamation subsystem per object category. Keep control allocation and destruction on the calling control thread. An epoch protocol still needs an actual publication/reclamation ordering proof; short pseudocode or a clean stress run is not that proof. Registered liveness and user-enabled state must also be distinguished so a concurrent enable cannot resurrect a destroyed value.

If concurrent removal and destruction were disallowed until logging stops, most of this control machinery could disappear. That is the largest available simplification, but explicitly relaxes R6. Likewise, setup-only **new** registration in R5 does not by itself authorize removing the accepted same-name re-registration behavior in §5.3. Latest-only or overwriting rings change R3: they discard previously accepted records instead of the newest attempted one. Those are requirement changes, not implementation shortcuts.

## Recommended minimum from here

Complete Plan 3's validation/documentation and retain its runtime. Complete Plan 4 with the existing pool/queue, direct serialization, and the smallest proven single-reader control-reclamation scheme. Preserve the pool's channel-name ownership and producer-held reference until all fanout attempts finish (`details/snapshot_pool.hpp:35–43,112–171`, `channel.cpp:342–365`). These are lifetime requirements, not optional abstraction layers.

Record stop/join/drain/restart as the leading deferred alternative; compare immediate PI blocking against the timed spin when latency is next measured. Neither requires a wholesale transport rewrite. A much smaller library is possible if live reclamation, retained handles, or independent sink progress is no longer wanted, but that decision must be explicit. With the full accepted contract retained, no substantially simpler whole-system design was established by this audit.
