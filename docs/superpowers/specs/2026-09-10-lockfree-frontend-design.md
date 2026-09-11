# Lock-free, allocation-free front end for `LogChannel`

**Date:** 2026-09-10 (revised through Plan 3 on 2026-09-11)
**Scope:** `data_tamer_cpp` — `LogChannel`, `LoggedValue`, `ValuePtr`, `DataSinkBase`, in-tree sinks
**Status:** approved design. Plan 1 implemented steps 0–3, Plan 2 steps 4–5,
and Plan 3 step 6. Plan 4 retains steps 7–9. The
Plan 3 channel-to-sink bridge still uses the channel structure locks and copies
the serialized legacy snapshot into pooled storage. Channel epoch publication,
direct pooled serialization, capacity/strict-mode APIs and the complete warm-up
contract remain Plan 4 work.

## 1. Goal

Make `LogChannel::takeSnapshot()` free of heap allocation after the first call
and free of unbounded blocking — the only lock it may take is the channel's
priority-inheritance write mutex, whose hold time is bounded by a writer's
critical section — while keeping the public `LogChannel` API, the `Snapshot`
struct, the wire format and the `storeSnapshot`-based sink interface unchanged.
Target: control loops up to 1 kHz.

### Requirements (agreed)

| # | Requirement |
|---|---|
| R1 | Exactly one thread per channel calls `takeSnapshot()` (the *snapshot thread*). |
| R2 | Registered values may be written from any number of other threads. A group of values written together in one transaction must appear together in a snapshot (all-or-nothing); no snapshot is ever dropped because of writer activity. Lone scalar writes are wait-free; the snapshot thread may wait only for a bounded, priority-inherited writer critical section. |
| R3 | When a sink cannot keep up, drop the newest snapshot, count it, return `false`. Memory is bounded. |
| R4 | `setEnabled()` is lock-free and callable from any thread while logging. |
| R5 | Registration (`registerValue*`, `createLoggedValue`) happens during setup only; it may use a mutex and still throws once logging has started. |
| R6 | Destroying a `LoggedValue` and `removeDataSink()` remain memory-safe while logging; they may block the *calling* thread, never the snapshot thread. |
| R7 | Sinks that override only `addChannel()` / `storeSnapshot()` compile unchanged. |
| R8 | Backend threads may lock and allocate; pre-allocation there is welcome but not required. |

### Non-goals

Registration after the first snapshot; a wait-free snapshot thread in the
presence of multi-value transactions (block-level triple buffering, recorded as
the alternative in §11); any change to `signal_logger`.

## 2. Architecture

### 2.1 Threads and roles

| Role | Who | May lock | May allocate |
|---|---|---|---|
| Snapshot thread | caller of `takeSnapshot()` | through Plan 3: the channel snapshot and sink-set mutexes plus the channel `WriteMutex`; after Plan 4: only the `WriteMutex` during serialization | Plan 3 proves zero allocation only for a warmed, fixed-size bridge; the complete guarantee remains Plan 4 |
| Writer threads | `LoggedValue::set()/getMutablePtr()/setEnabled()`, raw values under `scopedWrite()`, `LogChannel::setEnabled()` | the channel `WriteMutex` (shared with other writers and the snapshot thread); critical sections must be short and allocation-free | no |
| Control threads | `registerValue*`, `createLoggedValue`, `unregister`, `~LoggedValue`, `addDataSink`, `removeDataSink`, capacity setters | `control_mutex`, sink `store_mutex`; may wait one snapshot duration | yes |
| Sink threads | one per `DataSinkBase` | `store_mutex` | yes |

Roles are contracts, not OS threads: one thread may register values (control),
then enter a loop where it writes them (writer) and calls `takeSnapshot()`. The
only constraint is that a thread inside `takeSnapshot()` holds no writer guard
and no control mutex — true by construction since those are separate calls.
`setEnabled` is deliberately classified as a *writer* operation: in practice it
is called from the thread that produces the value (explicitly, or implicitly via
`set(v, auto_enable = true)`), so it must be as cheap as `set()` and must not
depend on the channel object at all (§2.2 `ChannelSharedState`, §5.2).

### 2.2 Components

| Unit | File | Purpose |
|---|---|---|
| `WriteMutex` | `include/data_tamer/details/write_mutex.hpp` | `Lockable` wrapper over `pthread_mutex_t` created with `PTHREAD_PRIO_INHERIT` (Linux); falls back to `std::mutex` elsewhere with a compile-time warning. `lock()`, `try_lock()`, `unlock()` |
| `ChannelSharedState` | `include/data_tamer/details/shared_state.hpp` | `WriteMutex write_mutex`; per-series `atomic<bool> enabled[]` (sized at freeze, append-only before); `atomic<bool> mask_dirty`. Owned by `shared_ptr` from the channel **and** from every `LoggedValue`, so writer-side operations never need the channel object |
| `SnapshotPool` | `include/data_tamer/details/snapshot_pool.hpp` | K pre-allocated `Snapshot` slots, each with an intrusive `atomic<uint32_t> refs`. `tryAcquire()` (snapshot thread only) scans for `refs == 0`. One pool per channel, owned by `shared_ptr` |
| `SnapshotRef` | same file | Move-only handle `{ shared_ptr<SnapshotPool> pool; PoolSlot* slot; }`; explicit `clone()` increments `refs`, destruction/reset decrements it. Keeps both the slot and pool alive |
| `SinkLink` | `src/channel.cpp` | Through Plan 3: `{ shared_ptr<DataSinkBase> sink; unique_ptr<ProducerToken> token; uint64_t dropped; }` — one per (channel, sink), stored under `sinks_mutex`; token declaration order makes it die before the sink |
| `SinkSlot` | `src/channel.cpp` | Planned for Plan 4: atomic publication of fixed sink links removes `sinks_mutex` from the snapshot path |
| `LogChannel::Pimpl` | `src/channel.cpp` | Through Plan 3: legacy snapshot buffer and mutex, 64-slot pool created after the first serialization, sink map and per-link counters. Plan 4 adds epoch publication and direct pooled serialization |
| `DataSinkBase::Pimpl` | `src/data_sink.cpp` | thread; pre-sized `BlockingConcurrentQueue<SnapshotRef>`; handoff and store mutexes; current callback ref; packed closed-bit/active-producer admission word; run and error counters |
| `LoggedValue<T>` | `include/data_tamer/logged_value.hpp`, `channel.hpp` | scalar `T` → `std::atomic<T>`; non-scalar → plain `T`; both hold `shared_ptr<ChannelSharedState>` and a `weak_ptr<LogChannel>` used only by the destructor |
| `ValuePtr` | `include/data_tamer/values.hpp` | function pointers instead of `std::function`; new constructor for `std::atomic<T>` |

Constants through Plan 3: `kLockSpinNs = 2000` (spin on `try_lock` before
sleeping), fixed `pool_capacity = 64` slots per channel (≈ 64 ms of stall
absorption at 1 kHz), and default sink
`queue_capacity = 1024` refs, sink thread wait timeout `50 ms`. The queue rounds
capacity to internal 32-entry blocks, so the argument is a minimum rather than
an exact limit. Plan 4 adds configurable pool/payload capacity and the fixed
`kMaxSinks = 8` publication array.

### 2.3 Ownership

- The **pool** is owned by the channel and by every live `SnapshotRef`. A ref in
  a sink's queue keeps the pool alive after the channel is gone, so a channel may
  be destroyed while sinks still hold snapshots.
- A **slot** is free when `refs == 0`. Only the snapshot thread transitions
  `0 → 1`; sinks and the snapshot thread itself only decrement from `≥ 1`.
- A **`ProducerToken`** is bound to the sink's queue. Through Plan 3 each
  `SinkLink` declares its `shared_ptr<DataSinkBase>` before its token, so reverse
  member destruction destroys the token first. The sink and queue therefore
  outlive the token by construction. Plan 4 keeps the same ordering inside its
  fixed `SinkSlot` owners.
- Through Plan 3 the channel holds `sinks_mutex` while publishing and while a
  link is inserted or removed. Plan 4 replaces this with the raw
  `SinkSlot::link` publication and epoch reclamation described in §5.1.

### 2.4 Removed

After Plan 4: `Pimpl::mutex` and `sinks_mutex` as snapshot-side locks and
`Pimpl::snapshot`. Already removed through Plan 3:
`moodycamel::ConcurrentQueue<Snapshot>` (by value) in `DataSinkBase`;
`DataSinkBase::pushSnapshot()`; `LoggedValue::rw_mutex_`; `LoggedValue` move
constructor/assignment; the sink thread's 250 µs polling loop.

### 2.5 Unchanged

`Snapshot`; `DataSinkBase::addChannel/storeSnapshot` signatures; `ChannelsRegistry`;
schema, hash and wire format; all `registerValue` overloads and their
"throws after logging started" rule; existing `MCAPSink`/`ROS2PublisherSink`
call sites (the MCAP constructor only adds a final defaulted queue-capacity argument);
the vendored moodycamel headers (now used as intended: pre-sized,
explicit-producer, `try_enqueue`).

## 3. Hot path — `takeSnapshot(timestamp)`

The direct-to-pool algorithm below is the Plan 4 target. Plan 3 implements the
delivery half while retaining the legacy channel locks and buffer:

1. Under the channel snapshot mutex, rebuild the mask if dirty, serialize into
   `Pimpl::snapshot` under the channel `WriteMutex`, and fill its timestamp and
   schema hash.
2. After the first completed serialization, create a 64-slot `SnapshotPool`
   sized to that payload and mask. The pool owns a copy of the channel name and
   every slot's `Snapshot::channel_name` views that storage.
3. Acquire a slot and copy payload, mask, hash and timestamp from the legacy
   snapshot while the snapshot mutex is still held. If no slot is free,
   increment `pool_exhausted` and return `false`.
4. Hold a parent `SnapshotRef` while publishing under `sinks_mutex`. For each
   link, clone it and call `tryPush(token, std::move(delivery))`. A failed
   enqueue leaves `delivery` owning its reference; ordinary RAII releases it
   exactly once. Increment that link's drop counter and return `false` after all
   links have been attempted if any failed.

This bridge serializes once but performs one payload/mask copy into the pool.
It does not yet provide the complete frontend reservation or lock-free channel
structure contract.

### Step 0 — freeze (first call only; takes `control_mutex`)

Set `logging_started`. Compute
`payload_capacity = max(user_hint, 2 × current serialized size, 256)`.
Create `SnapshotPool(pool_capacity, payload_capacity, ceil(N/8))`. For every
`SinkSlot` with a sink: `sink->addChannel(name, schema)`, create
`SinkLink{sink, ProducerToken(sink->queue())}`, `slot.owner = link`,
`slot.link.store(link.get(), release)`. Cache `schema_hash`. After this the
series array is immutable and the snapshot thread never takes `control_mutex`
again.

### Steps 1–8 — every call

1. `epoch.fetch_add(1, acq_rel)` → odd = in progress.
2. Load each `SinkSlot::link` (acquire) into a local array; if all null, epoch
   exit, `return false`. `slot = pool.tryAcquire()`: scan slots round-robin from
   the last index for `refs.load(acquire) == 0`; on success `refs.store(1,
   relaxed)` (only this thread makes the `0 → 1` transition). If none is free:
   `pool_exhausted++`, epoch exit, `return false`. No serialization work is done.
3. `if (mask_dirty.exchange(false, acq_rel))` rebuild the private mask from
   `enabled[i].load(relaxed)`. From here on only the mask is consulted.
4. Locked serialization section. Acquire `state->write_mutex`: spin on
   `try_lock()` for up to `kLockSpinNs` (longer than any legal writer critical
   section, so the futex sleep is only reached when a writer was preempted mid
   transaction — which priority inheritance then bounds); if the spin fails,
   `write_lock_contended++`, record `t0`, `lock()`, and update
   `write_lock_wait_max_ns` from `now - t0` (clock reads only on this rare path).
   Under the lock: size pass over enabled series → ensure slot capacity (§4.6) →
   serialize enabled series into `slot->payload`. Unlock. Scalar atomics are read
   with `load(relaxed)`; they need no lock. A lone scalar `set()` during
   serialization may appear in this snapshot or the next; only writes inside
   a transaction are ordered together. The size pass is inside the section because a vector may change
   length between the two passes.
5. Fill the header: `timestamp`, cached `schema_hash`, `channel_name`
   (`string_view`), `memcpy` mask, `payload.resize(written)` (≤ capacity).
6. Publish to every sink. For each non-null link, keep the parent reference and
   use explicit clone ownership:

   ```cpp
   auto delivery = parent.clone();
   if (!link->sink->tryPush(link->token, std::move(delivery))) {
     ++link->dropped;
     all_pushed = false;
   }
   ```

   With an explicit producer token, `try_enqueue` neither allocates nor waits.
   On failure it leaves the move-only reference intact, so `delivery` releases
   it by RAII. There is no manual refcount decrement on this path.
7. Release the snapshot thread's own hold: `refs.fetch_sub(1, release)`. If no
   sink accepted, this returns the slot to the pool immediately.
8. `epoch.fetch_add(1, release)` → even. Return `true` iff every sink accepted.

The refcount protocol is the one subtle rule on this path: the snapshot thread
holds its own reference (step 2) until *every* `tryPush` is done (step 7), so a
fast sink can never return the slot to the pool while it is still being offered
to a later sink.

### Cost per call after Plan 4

Two atomic RMWs (epoch), one `exchange` (mask), ≤ `kMaxSinks` acquire loads,
≤ `pool_capacity` acquire loads in the worst-case free scan, one uncontended
`try_lock`/`unlock` pair (~40 ns), `N` mask bit tests, serialization, and per
sink: two refcount RMWs plus one moodycamel explicit-producer `try_enqueue`
(store-only fast path) and one semaphore increment. No memcpy of the payload for
any number of sinks. No allocation except §4.6.

Through Plan 3, add the legacy channel/sink mutexes and one payload/mask copy to
the costs above. The zero-allocation regression covers warmed fixed-size
publication, including queue-full failure and fanout. Variable payload growth,
control changes, and direct serialization are not covered by that claim.

Worst case when a writer holds the mutex: remaining writer critical section
(µs, under your control) + PI boost (~1 µs) + one futex sleep/wake round-trip
(5–20 µs on PREEMPT_RT, 20–100 µs on a stock kernel) — a rare tail event whose
per-cycle probability is (transaction duration / cycle period) × transactions
per cycle. At 1 kHz with µs transactions this is well under 1 % of cycles and
well inside the budget. The harness (§10.1) reports the observed maximum.

## 4. Value synchronization

### 4.1 Scalar `LoggedValue<T>` — wait-free

`T` arithmetic, `bool`, `char`, or enum with underlying type ≤ 8 bytes;
`static_assert(std::atomic<T>::is_always_lock_free)`.

- Storage `std::atomic<T> value_`. Registered through a new
  `ValuePtr(const std::atomic<T>*)` whose serializer does
  `T tmp = p->load(relaxed); memcpy(dest, &tmp, sizeof(T))`. `BasicType`, schema
  and hash are unchanged.
- `set(v, auto_enable)`: `value_.store(v, relaxed)`; then only if
  `auto_enable && !enabled` take the enable path (§4.4).
- `get()`: `value_.load(relaxed)`.
- `getMutablePtr()`: `MutablePtr<T>` holds a copy loaded at construction; the
  destructor stores it back. `getConstPtr()`: copy at construction. Their `mutex()`
  returns `nullptr`. Two concurrent `MutablePtr`s on one scalar are
  last-writer-wins (documented).
- Relaxed ordering is sufficient: a snapshot is a sample, not a message.
- Selection is by trait, not by hand:
  `is_atomic_scalar_v<T> = IsNumericType<T>() && sizeof(T) <= 8 && std::atomic<T>::is_always_lock_free`;
  storage is `std::conditional_t<is_atomic_scalar_v<T>, std::atomic<T>, T>`.
  A numeric `T` that fails the trait (no lock-free 8-byte atomic on the target)
  falls through to the mutex path of §4.2 rather than failing to compile.
- Deprecations (scalar `T` only): `getMutablePtr()` and `getConstPtr()` are
  `[[deprecated]]` in favour of `set()`/`get()` because the proxy's write-back
  timing differs from today's in-place, exclusive access; `MutablePtr::mutex()` /
  `ConstPtr::mutex()` are `[[deprecated]]` for all `T` (there is no mutex a caller
  could usefully lock: scalars are atomics). Non-scalar `getMutablePtr()` stays
  undeprecated: in-place editing under the guard is the right tool for vectors
  and structs.

Before/after for the common case (`LoggedValue<double>`):

```cpp
// today (channel.hpp:375, :396)
void set(const T& val, bool auto_enable) {
  std::lock_guard lk(rw_mutex_);                 // per-value shared_mutex, NOT the channel mutex
  if (auto channel = channel_.lock()) {          // weak_ptr::lock on every call
    value_ = val;
    if (!enabled_ && auto_enable) { channel->setEnabled(id_, true); enabled_ = true; }
  } else { value_ = val; enabled_ |= auto_enable; }
}
T get() { rw_mutex_.lock_shared(); T t = value_; rw_mutex_.unlock_shared(); return t; }

// after
void set(const T& val, bool auto_enable) {
  value_.store(val, std::memory_order_relaxed);  // one plain mov
  if (auto_enable && !state_->isEnabled(id_)) state_->setEnabled(id_, true);   // atomics only
}
T get() const { return value_.load(std::memory_order_relaxed); }
```

Observable differences for existing callers of the scalar path:

| Change | Who notices | Kind |
|---|---|---|
| `set()` / `get()` | nobody — same signatures, same result | none |
| `getMutablePtr()` edit visible on guard destruction; overlapping guards are last-writer-wins | code using the proxy as a lock or expecting intermediate values in snapshots | semantic, silent → deprecated |
| `ConstPtr::operator*()` refers to an internal copy | code comparing its address or expecting later writes to show | semantic, very unlikely |
| `mutex()` returns `nullptr` | code locking it manually (none in-tree) | runtime → deprecated |
| move ctor / assignment deleted | code moving a `LoggedValue` (already a use-after-free today) | compile-time, intended |
| `get()` is `const` | nobody | additive |

### 4.2 Transactions: non-scalar `LoggedValue<T>`, raw `registerValue(const T*)`, multi-value updates — the channel `WriteMutex`

One priority-inheriting mutex per channel, living in `ChannelSharedState`. It is
the unit of consistency: everything written while it is held is seen by
`takeSnapshot()` all-or-nothing, because the snapshot thread serializes under
the same mutex (§3 step 4).

```cpp
// user code
{
  auto tx = channel->scopedWrite();     // nesting-aware guard of channel->writeMutex()
  pos->set({1, 2, 3});                  // non-scalar LoggedValue
  speed->set(3.2);                      // scalar LoggedValue: atomic store, ordered by the lock
  raw_flag = true;                      // raw registered pointer
}                                       // published atomically w.r.t. takeSnapshot()

imu->set(sample);                       // lone non-scalar set(): implicit one-value transaction
speed->set(3.3);                        // lone scalar set(): wait-free atomic store, no lock
```

**Consistency is opt-in, by design.** A lone `set()` promises only that the
value itself is never torn. If several values must be captured together the
user has two tools: put them in one struct and log a single
`LoggedValue<Struct>` (cheaper to serialize, too), or wrap the writes in
`scopedWrite()`. Lone scalar `set()` is deliberately kept wait-free rather than
taking the mutex "just in case": it is the hot path for sensor threads, and a
mutex there would buy ordering with respect to non-scalars but still not
atomicity across two consecutive scalar writes — only a transaction gives that.
The README states this rule next to the first `set()` example.

- Non-scalar `LoggedValue<T>::set(v)` = `std::lock_guard lk(state_->write_mutex);
  value_ = v;` unless the calling thread already holds the mutex through a
  `scopedWrite()` (detected through a thread-local chain of active transaction
  guards in `ChannelSharedState`, with no allocation or fixed channel limit).
  Guards are nonmovable and must remain on their creating thread; C++17
  guaranteed copy elision allows `scopedWrite()` to return them by value.
- `getMutablePtr()` on a non-scalar holds the mutex for the lifetime of the
  proxy and points at the live object — today's semantics. Holding it long
  blocks the snapshot thread for that long; documented as "keep it short".
- `getConstPtr()` on a non-scalar holds the mutex for the proxy lifetime, as
  `getMutablePtr()` does. `get()` takes the mutex, copies, and releases it.
  Both pointer proxies should be kept short and acquired outside a transaction.
- Raw pointers: `writeMutex()` keeps its signature and meaning (`Mutex&`, where
  `Mutex` is now the alias `DataTamer::WriteMutex` instead of
  `std::shared_mutex`). `std::lock_guard`, `std::unique_lock` and
  `std::scoped_lock` code compiles unchanged; only code calling `lock_shared()`
  breaks (none in-tree). `scopedWrite()` is added as the preferred spelling.
  A plain lock guard does not establish transaction nesting: use `scopedWrite()`
  when calling non-scalar `set()`/`get()` within a group of writes.
- Priority inheritance requires the writer to be a POSIX thread on Linux; it
  works whether the writer is `SCHED_FIFO` or `SCHED_OTHER` (a CFS writer is
  boosted to the waiter's real-time priority while holding the lock). On
  platforms without `PTHREAD_PRIO_INHERIT` the wrapper is a plain mutex and the
  bound is lost; the build emits a warning.

Why a single mutex rather than the wait-free alternatives: consistency across
values plus "never drop a snapshot" plus multiple writers means the snapshot
thread must either copy the whole value set per transaction (block-level triple
buffering, 3× memory, a block copy on every writer transaction, and a second
rule for lone scalar writes) or wait a bounded time for the writer. At ≤ 1 kHz
the bounded wait is the smaller, simpler price (§11, §12).

### 4.3 Raw pointers written by the snapshot thread

No synchronization needed; documented as the fastest path.

### 4.4 `LoggedValue` ↔ channel

`LoggedValue` holds `std::shared_ptr<ChannelSharedState> state_` and its
`RegistrationID`. `setEnabled(b)` and the auto-enable branch of `set()` operate
directly on `state_->enabled[id]` and `state_->mask_dirty` (§5.2) — pure atomics
on memory the `LoggedValue` co-owns, so they are valid on a writer thread, cost
the same as a `set()`, and work even if the channel has already been destroyed.
The `LoggedValue::enabled_` mirror is dropped; `isEnabled()` reads the shared
flag. `std::weak_ptr<LogChannel>` is kept **only** for the destructor
(`unregister` + epoch wait, a control operation). Move constructor and assignment
are deleted (fixes the dangling-pointer hazard; `createLoggedValue` already
returns a `shared_ptr`).

### 4.5 Race fixed

The pre-existing race between `LoggedValue::set()` (per-value mutex) and
`takeSnapshot()` (channel mutex) no longer exists: scalars are atomics,
non-scalars and the snapshot thread share the one `WriteMutex`. TSAN sees only
atomics and a real mutex — no suppressions needed.

### 4.6 Payload growth

If the serialized size exceeds the acquired slot's capacity:

- `strict_mode == false` (default): `slot->payload.reserve(size × 2)` on that slot
  only, `payload_reallocations++`. Other slots grow lazily when next acquired, so
  one growth event costs up to `pool_capacity` counted allocations spread over
  the following ticks. This is the single sanctioned allocation on the hot path.
- `strict_mode == true`: `refs.store(0, release)`, `dropped_oversize++`,
  `return false`.

`strict_mode` is an `std::atomic<bool>` and may be toggled at runtime.
`setPayloadCapacity(bytes)` before freeze avoids both paths.

## 5. Control path — Plan 4 target

All control operations serialize on `control_mutex` (`std::mutex`), never taken
by the snapshot thread after freeze.

### 5.1 Epoch and quiescence

```cpp
void waitQuiescent() {                       // caller holds control_mutex
  const uint64_t e = epoch.load(acquire);
  if ((e & 1) == 0) return;
  while (epoch.load(acquire) == e) std::this_thread::yield();
}
```

Contract: publish the change (atomic store read at the start of every snapshot)
*before* calling `waitQuiescent()`. Called from the snapshot thread itself it
returns immediately, so destroying a `LoggedValue` inside the control loop is
legal.

### 5.2 `setEnabled(id, bool)` — wait-free, any thread (writer-class operation)

For each field `state->enabled[i].exchange(enable, relaxed)`; if any changed,
`state->mask_dirty.store(true, release)`. No mutex, no `weak_ptr::lock`, no
dependency on the `LogChannel` object: `LogChannel::setEnabled` and
`LoggedValue::setEnabled` both call the same free function on
`ChannelSharedState`. Safe to call from a writer thread, from the snapshot thread
between snapshots, or from inside a `scopedWrite()` transaction.

Ordering with a value write from the same writer thread (the common
"write, then enable" case): the value store happens before the `mask_dirty`
release store in program order, and the snapshot thread acquires `mask_dirty`
before reading the mask, so a snapshot that sees the field enabled also sees at
least that value.

### 5.3 Registration — `control_mutex`, before freeze only

Unchanged logic; throws after freeze. Per-series `enabled` flags live in an
append-only container of atomics sized before freeze. Re-registering a previously
unregistered name after freeze stays allowed: set `holder`, `registered = true`,
then `enabled.store(true)` + `mask_dirty` (holder valid before its mask bit can
be set).

### 5.4 `unregister(id)` / `~LoggedValue`

Lock `control_mutex`; `enabled[i].store(false)`, `registered[i] = false`;
`mask_dirty.store(true, release)`; `waitQuiescent()`; detach `holder[i]`. The
destructor goes through `weak_ptr::lock()`; if the channel is gone it does
nothing.

### 5.5 `addDataSink(sink)`

Lock `control_mutex`; take the first free `SinkSlot` (throw if all `kMaxSinks`
used); `slot.sink = sink`. If already frozen: `sink->addChannel(name, schema)`,
create the `SinkLink` with a `ProducerToken` on `sink->queue()`, `slot.owner =
link`, `slot.link.store(link.get(), release)`.

### 5.6 `removeDataSink(sink)`

Lock `control_mutex`; `slot.link.store(nullptr, release)`; `waitQuiescent()`
(snapshot thread no longer uses the token); destroy `slot.owner` (moodycamel
allows destroying a `ProducerToken` with items still queued — they remain
dequeuable by the consumer); reset `slot.sink`. Refs already in the sink's queue
are delivered normally and release their slots when consumed. Removing the last
sink does not un-freeze the channel.

### 5.7 `~LogChannel`

Lock `control_mutex`; null every `SinkSlot::link`; `waitQuiescent()`; destroy
`SinkLink`s; drop the pool `shared_ptr`. Refs held by sinks keep the pool alive
until they drain; no wait is needed.

### 5.8 Capacity setters

`setPayloadCapacity(bytes)`, `setPoolCapacity(n)`: `control_mutex`, throw after
freeze. `setStrictMode(bool)`: atomic, any time. Sink queue capacity is a
`DataSinkBase` constructor argument.

### 5.9 `getSchema()`, counters

`getSchema()` takes `control_mutex`. Counters are `std::atomic<uint64_t>` with
relaxed RMW on the snapshot thread and relaxed loads elsewhere.

### 5.10 Blocking bounds for control threads

| Operation | Bound |
|---|---|
| `unregister` / `~LoggedValue` | `control_mutex` + one in-flight snapshot |
| `removeDataSink` | `control_mutex` + one in-flight snapshot |
| `addDataSink` after freeze | `control_mutex` + token creation (may allocate inside moodycamel) |
| `~LogChannel` | `control_mutex` + one in-flight snapshot |
| `setEnabled` | wait-free |

## 6. Backend

### 6.1 `SnapshotPool` and `SnapshotRef`

```cpp
class SnapshotPool {
 public:
  SnapshotPool(size_t capacity, size_t payload_capacity, size_t mask_bytes,
               std::string channel_name = {});
  PoolSlot* tryAcquire();            // snapshot thread only: scan for refs==0, set refs=1
  // slots are returned implicitly when refs reaches 0
 private:
  const std::string channel_name_;   // backs every slot's channel_name view
  std::unique_ptr<PoolSlot[]> slots_;
  size_t scan_from_ = 0;             // round-robin start, snapshot thread only
  std::atomic<uint64_t> exhausted_{0};
};

class SnapshotRef {                  // move-only; copy via explicit clone()
 public:
  SnapshotRef(std::shared_ptr<SnapshotPool>, PoolSlot*);   // takes one existing count
  SnapshotRef(SnapshotRef&&) noexcept; SnapshotRef& operator=(SnapshotRef&&) noexcept;
  ~SnapshotRef();                    // if slot: refs.fetch_sub(1, release)
  SnapshotRef clone() const;         // refs.fetch_add(1, relaxed)
  void reset();
  const Snapshot& operator*() const; const Snapshot* operator->() const;
  explicit operator bool() const;
 private:
  std::shared_ptr<SnapshotPool> pool_; PoolSlot* slot_ = nullptr;
};
```

The pool owns the channel-name string because `Snapshot::channel_name` remains a
`string_view`. A retained reference therefore remains valid after the original
name and channel are destroyed. The pool `shared_ptr` inside the ref likewise
keeps all slots alive.

The legacy callback still receives `const Snapshot&`. A derived sink that needs
to retain its current queued delivery calls protected `retainSnapshot()` from
inside `storeSnapshot()` on that callback thread. It explicitly clones the
current reference. Outside that context it asserts in Debug and returns an
empty handle in Release; a direct public call to a derived `storeSnapshot()`
cannot retain queue ownership.

Memory ordering: sinks release with `fetch_sub(1, release)`; the snapshot thread
acquires a free slot with `load(acquire) == 0`, which synchronizes with the last
release, so the sink's reads of the slot happen-before the snapshot thread's
next writes into it.

### 6.2 `DataSinkBase::Pimpl`

Members: `thread`, `atomic<bool> run`, a packed atomic admission word,
`moodycamel::BlockingConcurrentQueue<SnapshotRef> queue(queue_capacity)`,
`handoff_mutex`, `store_mutex`, `current_ref`, and
`atomic<uint64_t> store_errors`.

Thread loop:

```cpp
while (run.load()) {
  std::unique_lock handoff(handoff_mutex);
  std::unique_lock lock(store_mutex);
  handoff.unlock();
  if (!run.load()) break;
  if (queue.wait_dequeue_timed(current_ref, 50ms)) {
    deliver(self); // catch callback exception; increment store_errors; reset current_ref
  }
}
```

- `tryPush(token, SnapshotRef&&)` first admits the producer with a CAS increment
  of the active count unless the high closed bit is set. An RAII guard always
  decrements the count after `queue.try_enqueue(token, std::move(ref))`.
  Explicit-token enqueue neither allocates nor waits; failure leaves `ref`
  intact for its owner to release.
- `stopAcceptingSnapshots()` sets the closed bit and waits until the active
  producer count reaches zero. This is the admission barrier: every accepted
  enqueue is visible before a control thread drains. `startAcceptingSnapshots()`
  clears the bit. Control start/stop/drain calls require external serialization
  and must not run from a callback.
- The worker and manual drainer never test admission after dequeue. Closing
  admission prevents new work; all previously accepted references still run.
- `processQueuedSnapshots()` locks `handoff_mutex` and then `store_mutex`, and
  holds both while nonblocking dequeue and callback repeat. The worker takes the
  same lock order but releases handoff before its timed wait. This prevents the
  worker from repeatedly barging ahead of a waiting drainer; the prior ordering
  produced a measured 50.389-second focused-test stall.
- The shared delivery helper catches callback exceptions, increments
  `store_errors`, and resets `current_ref` before releasing `store_mutex`.
  Per-producer queue order is therefore also callback order when worker and
  drainer overlap. This FIFO guarantee applies to one continuous channel/sink
  attachment. Removal and re-attachment creates a replacement token; newer work
  may be delivered before older records still queued by the prior token, and no
  order is guaranteed across that boundary. Different channels use different
  producer tokens, so no global cross-channel timestamp order is promised.
  MCAP validation with stable attachments found zero per-channel inversions but
  497 global inversions in the two-channel writer example; consumers that
  require a timestamp merge must perform one.
- `stopThread()` stores false and joins without either mutex. The 50 ms timeout
  applies only to an idle semaphore wait. Callback duration, mutex scheduling
  and OS scheduling mean it is not a hard upper bound on `join()`.
- `pushSnapshot()` is removed. Private friend methods create the producer token
  and publish references; vendored queue types remain out of the public API.
- Destructor asserts `!thread.joinable()` in debug; in release joins and prints to
  stderr. Derived destructors must call `stopThread()` before destroying callback
  state. During constructor unwinding the base performs cleanup without masking
  the original exception. Destroying the queue releases any remaining refs.

Queue sizing: `queue_capacity` is a *minimum* total across all producer tokens
(moodycamel allocates blocks of 32 up front and hands them to explicit producers
on demand; explicit producers keep the blocks they have used). With many
channels on one sink, size it as `channels × per-channel depth` and allow for
block rounding. Use `sizeof(SnapshotRef)` when estimating storage; it is 24
bytes on the measured x86-64 GCC 15 build (`Snapshot` is 80 bytes and
`PoolSlot` is 192 bytes), rather than the earlier assumed 16 bytes.

### 6.3 In-tree sink ports

- `MCAPSink`: `thread_local merged_payload` → reserved member (safe: all
  `storeSnapshot` calls are under `store_mutex`); `finishQueueAndStop` =
  `stopAcceptingSnapshots(); processQueuedSnapshots(); stopRecording();` (the
  250 µs sleep and redundant second drain are removed). Explicit
  `restartRecording()` clears the forced-stop state and reopens admission after
  rebuilding channels. Automatic rollover runs through the internal restart
  path without either action, so it cannot undo a concurrent finish closure.
- `ROS2PublisherSink`, benchmark `NullSink`: unchanged apart from the
  constructor forwarding `queue_capacity` if they expose it.
- `DummySink`: constructor forwards `queue_capacity` the same way; in
  addition its public data members (`schemas`, `schema_names`,
  `snapshots_count`, `latest_snapshot`) are replaced by mutex-protected
  accessors — see §8 — so callers that read the members directly break.

### 6.4 Latency, idle, memory

Snapshot → `storeSnapshot` uses the blocking queue semaphore; the syscall and
idle behavior is measured rather than assumed in the Plan 3 benchmark report.
The 50 ms timeout permits about 20 idle waits/s, but process CPU tick resolution
and scheduler context switches are reported separately from work done.
Memory per channel = `pool_capacity × (payload_capacity + mask_bytes +
sizeof(Snapshot) + 64)`; defaults give ≈ 1 MB for a 1000-double channel,
independent of the number of sinks. Memory per sink is based on
`queue_capacity × sizeof(SnapshotRef)` plus moodycamel block overhead and
32-entry rounding; `sizeof(SnapshotRef) == 24` on the measured build.

## 7. Errors and counters

| Situation | Thread | Behaviour |
|---|---|---|
| Register after freeze, name with spaces, type change on re-register | control | `throw std::runtime_error` |
| Pool has no free slot | snapshot | no work, `pool_exhausted++`, return `false` (all sinks miss this snapshot) |
| Sink queue full | snapshot | that sink skipped, `link.dropped++`, return `false` |
| No sinks | snapshot | no work, return `false` |
| `WriteMutex` held by a writer at snapshot time | snapshot | spin ≤ 2 µs, then block (PI-bounded); `write_lock_contended++`, `write_lock_wait_max_ns` updated; snapshot proceeds normally |
| Payload > current bridge capacity (Plan 3) | snapshot | vector copy may grow the slot and allocate; fixed-size warm publication is the only tested zero-allocation case |
| Payload > configured capacity (Plan 4) | snapshot | planned non-strict growth counter or strict drop policy |
| `storeSnapshot` throws | sink | caught, `store_errors++`, ref destroyed (slot released) |
| Sink retains a callback `SnapshotRef` indefinitely | sink | pool starves → `pool_exhausted` grows; no UB |
| `DataSinkBase` destroyed with live thread | control | debug assert; release: join + stderr |

Through Plan 3, payload growth in the temporary copy bridge can still allocate
and throw. The final no-throw path depends on Plan 4's reservation/strict-mode
work. `takeSnapshot()` returns `true` iff every attached sink's queue accepted
the snapshot; a callback's `false` return is distinct from an enqueue failure.

Implemented accessors through Plan 3 are `LogChannel::poolExhausted()`,
`writeLockContended()`, `writeLockWaitMaxNs()`,
`droppedSnapshots(const std::shared_ptr<DataSinkBase>&)`, and `stats()` (whose
fields currently bundle the two write-lock counters and `pool_exhausted`).
`DataSinkBase::storeErrors()` counts thrown callbacks. Plan 4 adds the payload
growth/oversize counters and their `Stats` fields.

## 8. Public API delta

| Symbol | Change |
|---|---|
| `LogChannel::writeMutex()` | signature unchanged (`Mutex&`); `Mutex` is now `DataTamer::WriteMutex` (exclusive, priority-inheriting) instead of `std::shared_mutex` — `lock_shared()` callers break, `lock_guard`/`unique_lock`/`scoped_lock` callers do not |
| `LogChannel::scopedWrite()` | new; nonmovable, nesting-aware `ChannelSharedState::Transaction` guard of the channel's `WriteMutex` |
| `LogChannel::setPayloadCapacity/setPoolCapacity/setStrictMode` | planned for Plan 4 |
| `LogChannel::poolExhausted/writeLockContended/writeLockWaitMaxNs/droppedSnapshots/stats` | new through Plan 3; `Stats` currently contains the two lock counters and `pool_exhausted` |
| `LogChannel::payloadReallocations/droppedOversize` | planned for Plan 4 |
| `LoggedValue` move ctor / assignment | deleted |
| `LoggedValue<T>::getMutablePtr()/getConstPtr()` for atomic-scalar `T` | deprecated (proxy semantics); use `set()`/`get()` |
| `LoggedValue<T>::get()` | now `const` |
| `MutablePtr/ConstPtr::mutex()` | deprecated; returns `nullptr` for scalar `LoggedValue`s |
| `MutablePtr/ConstPtr::operator bool()` | now explicit (also applies to atomic scalar proxies) |
| `LoggedValue<T>::getLockedPtr()` | already deprecated; unchanged |
| `DataSinkBase::DataSinkBase(size_t queue_capacity = 1024)` | new constructor argument (default keeps old call sites compiling) |
| `DummySink(size_t queue_capacity = 1024)` / `MCAPSink(..., size_t queue_capacity = 1024)` | new final defaulted arguments; capacity is a block-rounded minimum shared by all producers |
| `DummySink` public members `schemas`, `schema_names`, `snapshots_count`, `latest_snapshot` | replaced by mutex-protected accessors `schema(hash)`, `schemaName(hash)`, `schemasCount()`, `firstSchemaHash()`, `snapshotsCount(hash)`, `latestSnapshot()` (source-breaking for tests that read the members) |
| `DataSinkBase::pushSnapshot` | removed |
| `DataSinkBase::storeErrors()` | new |
| `DataSinkBase::retainSnapshot()` | new protected callback-only ownership clone; keeps the legacy virtual callback signature unchanged |
| `DataTamer::SnapshotRef` | new move-only handle, defined in `details/snapshot_pool.hpp`; derived callbacks obtain one only through `retainSnapshot()` |
| `MCAPSink::finishQueueAndStop` | closes admission, waits for admitted enqueues, drains accepted work once, and no longer sleeps |
| `MCAPSink::restartRecording` | explicit restart clears forced-stop and reopens admission; automatic rollover preserves admission closure |
| Derived `DataSinkBase` lifecycle | destructor must call `stopThread()` before callback state is destroyed; normal Debug destruction diagnoses violations, and construction unwinding preserves the original exception |
| `MCAPSink` / `ROS2PublisherSink` | private state moved behind an out-of-line Pimpl; one-time ABI change requires downstream rebuilds, subsequent private fields do not change the sink object layout |
| `ROS2PublisherSink::schema_mutex_` | private `std::mutex` now lives in the Pimpl and protects the schema-change flag as well as schemas |

Implemented through Plan 3: atomic scalars, nested transactions, contention
counters, sink Pimpls, pooled queue delivery, removal of `pushSnapshot`, and
the no-sleep MCAP finish/restart lifecycle. Capacity/strict-mode APIs, payload
growth counters, channel epoch publication and direct pooled serialization
remain Plan 4. `write_lock_contended`
counts blocking acquisitions after the spin budget; `write_lock_wait_max_ns`
measures only the blocking acquisition, excluding serialization.

## 9. Testing

The items below distinguish completed coverage through Plan 3 from the Plan 4
checks that depend on direct serialization and capacity APIs.

- **Unit, single-threaded**: `SnapshotPool` acquire/release, exhaustion, round-robin
  scan, zero allocations after construction; `SnapshotRef` move semantics and
  refcount on destruction; `WriteMutex` is created with `PTHREAD_PRIO_INHERIT`
  (query the attribute) and satisfies `Lockable`; nested `set()` inside
  `scopedWrite()` on the same thread does not deadlock; mask/payload consistency
  when `setEnabled` fires between size and serialize passes (test hook).
- **Transaction consistency**: writer thread updates `pos` and `vel` inside
  `scopedWrite()` with a deliberately slow body; every consumed snapshot decodes
  either both old or both new values, never mixed. Repeat with a raw pointer in
  the transaction.
- **Priority inheritance** (runs only when the test has `CAP_SYS_NICE`): a
  `SCHED_OTHER` writer holds the lock while a CPU-bound `SCHED_OTHER` hog runs on
  the same core; a `SCHED_FIFO` snapshot thread's observed `writeLockWaitMaxNs`
  stays below the writer's critical section + a small constant, versus
  milliseconds with a plain `std::mutex` (control case).
- **Refcount protocol**: two sinks, one of which returns `true` from `tryPush` and
  immediately consumes; assert the slot is not reused before the second
  `tryPush` (the snapshot thread's own hold, §3 step 7).
- **Allocation, Plan 3 complete**: the `operator new/delete` hook reports zero
  allocations and zero deallocations after pool creation for fixed-size warmed
  successful fanout and persistent queue-full failures. Queue traits route its
  allocations through the same hook. Plan 4 adds variable payload growth,
  reservation and strict-mode checks before making the complete frontend claim.
- **Concurrency under TSAN** (new CI job): (a) writer thread hammering
  `LoggedValue<double>::set` and `LoggedValue<std::vector<double>>::set` during
  snapshots — no reports, every consumed snapshot decodes to a consistent vector;
  (b) random `setEnabled` toggles — decoded field set equals the mask;
  (c) Plan 4: create/destroy `LoggedValue`s and add/remove sinks in a loop during
  snapshots; (d) Plan 3: two channels sharing one sink preserve per-producer
  order; (e) Plan 3: sink worker and manual drainer serialize dequeue/callback.
- **Lifetime, Plan 3 complete**: a callback retains references until the 64-slot
  pool exhausts; payload, mask and pool-owned long channel name remain readable
  after queue, channel and sink destruction; releasing refs restores publication.
- **Pool sizing, Plan 4 measurement**: `MCAPSink` with `do_compression = true` on a channel of 1000
  doubles at 1 kHz for 60 s writing to a file on disk: `poolExhausted() == 0`
  with the default pool (the reason the default is 64, not 16).
- **Drop behaviour, Plan 3 complete**: a requested queue capacity of one rounds
  to one 32-entry block; the next publication increments only that attachment's
  `droppedSnapshots(sink)`, while another sink can accept it. Retaining all 64
  pool slots separately increments `poolExhausted()` and prevents publication to
  every sink.
- **Compatibility**: existing test suites pass; call sites using `pushSnapshot`
  or moving a `LoggedValue` are adjusted in the same change.
- **Benchmark**: see §10.1.

## 10. Delivery order

Each step is a self-contained, independently reviewable change that leaves the
tree green. Every step is developed test-first (write the failing test, make it
pass, refactor) and must pass the normal build plus dedicated **ASAN+UBSAN** and
**TSAN** builds of the test suite before it is considered done. Step 0
introduces that CI infrastructure so every later step inherits it.

| Step | Change | New tests | Depends on |
|---|---|---|---|
| 0 | CMake presets / CI jobs: `-fsanitize=address,undefined` and `-fsanitize=thread` builds running the existing gtest suite; allocation-counting hook (`operator new/delete`) as a test utility | existing suite under both sanitizers; hook self-test | — |
| 1 | `WriteMutex` (`details/write_mutex.hpp`) — standalone PI mutex wrapper | attribute check; `Lockable` conformance; spin-then-lock helper; PI bound test (privileged, skipped otherwise) | 0 |
| 2 | `SnapshotPool` + `SnapshotRef` (`details/snapshot_pool.hpp`) — standalone, not yet wired | acquire/exhaust/release; ref move semantics; producer + N consumer threads returning refs under TSAN; zero allocations after construction | 0 |
| 3 | `ValuePtr`: `std::function` → function pointers; new `std::atomic<T>` constructor; `LoggedValue` move ops deleted | serialization byte-for-byte identical to before (golden buffers); atomic scalar serializes as the same `BasicType` | 0 |
| 4 | Scalar `LoggedValue<T>` on `std::atomic<T>`; proxy `MutablePtr`/`ConstPtr` for scalars; `ChannelSharedState` introduced for the enable flags | `set` from a writer thread during snapshots is clean under TSAN (this is the first step that fixes the existing race); accessor semantics; `setEnabled` from a writer thread | 3 |
| 5 | `writeMutex()` re-typed to `WriteMutex` and moved into `ChannelSharedState`; non-scalar `LoggedValue<T>` and `scopedWrite()` use it; `takeSnapshot` serializes under it with the spin-then-lock helper (the old `Pimpl::mutex` still guards structure until step 7) | transaction-consistency test; vector writer thread during snapshots clean under TSAN; `writeLockContended` increments when a transaction overlaps a snapshot; nested `set()` in a transaction | 1, 4 |
| 6 | `DataSinkBase` on `BlockingConcurrentQueue<SnapshotRef>` with explicit producer tokens; `tryPush`; `pushSnapshot` removed; `store_mutex`; `MCAPSink` port; `storeErrors`. The channel still serializes into `Pimpl::snapshot` under the old mutex and copies it into a pool slot at the end of `takeSnapshot` (temporary bridge, removed in step 8) | two channels / one sink ordering; drop behaviour with a slow sink and a small queue; sink keeping refs; `finishQueueAndStop` no longer sleeps; sink destructor assert; existing sink tests | 2 |
| 7 | `LogChannel` control path: `control_mutex`, `ChannelSharedState` wired for all series, `mask_dirty`, epoch + `waitQuiescent`, `SinkSlot`/`SinkLink` array, `add/removeDataSink` publication, `~LogChannel` | `setEnabled` from a thread during snapshots → decoded fields equal the mask; create/destroy `LoggedValue` and add/remove sinks in a loop under ASAN+TSAN; channel destroyed while sink holds refs | 5, 6 |
| 8 | `takeSnapshot` final form: freeze step, acquire-before-serialize, mask-driven serialization into the pool slot, refcount publish protocol, epoch; old `Pimpl::mutex`, `sinks_mutex`, `Pimpl::snapshot` and the step-6 bridge removed | **zero-allocation test** over 10 000 snapshots with two sinks; refcount protocol test; growth policy both modes; benchmark before/after | 7 |
| 9 | Cleanup: docs, README section on RT guarantees, counters and sizing (`pool_capacity`, `queue_capacity`, compression stalls), CHANGELOG | — | 8 |

Steps 1–3 touch no behaviour a user can observe and can be reviewed in any
order; step 4 is the first user-visible improvement (race fix) and is still
small; steps 6 and 7 are the two larger reviews; step 8 is mostly deletion.

### 10.1 Benchmarking

Two kinds of measurement, with different purposes and different tooling:

- **Throughput micro-benchmarks** (Google Benchmark, `benchmarks/`): cheap, run
  locally, reported as ns/op. Good for "did this component get faster".
- **Latency-distribution harness** (`benchmarks/rt_latency.cpp`, plain
  executable): a 1 kHz loop for N seconds calling `takeSnapshot()` with
  configurable value counts, sink count, and optional writer threads hammering
  `LoggedValue::set` and `scopedWrite()` concurrently; records every call's
  duration with `steady_clock` and prints p50 / p99 / p99.9 / max plus the
  allocation count from the hook and the channel counters. This is the number a
  real-time user actually cares about. It also runs once under a
  `SCHED_FIFO` thread when the process has the privilege, so priority-inversion
  effects show up as max-latency spikes.

Benchmark results are **not** CI gates (too noisy on shared runners); each step's
PR description records before/after numbers from the same machine. A nightly
job may run the harness and archive the output.

| Step | Benchmark work | Why then |
|---|---|---|
| 0 | Extend `data_tamer_benchmark.cpp` with: `LoggedValue<double>::set` in a loop; `takeSnapshot` with 1, 2 and 4 sinks; `takeSnapshot` with a concurrent writer thread. Add `state.counters["allocs/op"]` from the allocation hook. Add the latency harness. **Run everything on the unmodified code and commit the numbers** to `docs/benchmarks/2026-09-baseline.md`. | Baseline must exist before step 3 changes `ValuePtr`; the harness is also needed by steps 4 and 8 to prove the tail latency claims. |
| 1 | Micro: uncontended `try_lock`/`unlock` of `WriteMutex` vs `std::mutex`; spin-then-lock with a writer holding for 0 / 1 / 5 µs. | Documents the per-snapshot cost of the lock (expected ~40 ns uncontended) and the contended tail with and without PI. |
| 2 | Micro: `tryAcquire` with 0 / half / all-but-one slots busy (scan cost); `SnapshotRef` copy/destroy; producer + 2 consumers round-trip. | Confirms the free scan and refcount are tens of ns; shows the worst case when the pool is nearly full. |
| 3 | Re-run the step-0 `takeSnapshot` cases. | `std::function` → function pointer is a perf change; the golden-buffer test guards correctness, the benchmark shows the (small) gain. |
| 4 | Re-run `LoggedValue<double>::set`; harness with writer threads. | Expected order-of-magnitude drop (weak_ptr::lock + shared_mutex → relaxed store); first tail-latency improvement. |
| 5 | Harness with a `std::vector` writer thread and a multi-value transaction writer; report `writeLockContended` and `writeLockWaitMaxNs`, with the snapshot thread `SCHED_FIFO` when permitted. | Turns the "rare 10–30 µs tail" claim into a measured number on your hardware and kernel; the value to compare against the 1 kHz budget. |
| 6 | Snapshot → `storeSnapshot` latency (timestamp in the slot vs `steady_clock` in a test sink); idle CPU of the sink thread (`/proc/self/stat` over 10 s); syscalls per tick on the snapshot thread (`strace -c` or `perf stat -e syscalls`). | Verifies the semaphore replaces the 250 µs poll without regressing idle wakeups, and quantifies the futex-wake cost on the RT thread. |
| 8 | Full harness: 1000 doubles, 1, 2 and 4 sinks, writer threads, `SCHED_FIFO`; `allocs/op == 0` after warm-up. Commit results next to the baseline. | The headline before/after comparison for the whole effort; the 4-sink case shows the zero-copy benefit of the pool. |
| 9 | Publish the baseline vs final table in the README RT section. | — |

## 11. Recorded alternative: block-level triple buffering

Kept here as the escalation path if the harness (§10.1, step 5) shows the
`WriteMutex` tail is not acceptable on the target hardware — e.g. a jitter budget
in the low tens of µs, loops above a few kHz, or a platform without
`PTHREAD_PRIO_INHERIT`.

**Mechanism.** Three copies of the entire cross-thread value set plus one
channel-wide atomic index byte. A writer transaction locks the writer mutex,
*copy-forwards* the stale back block from the last-published block, writes its
members, publishes with one `exchange`. The snapshot thread takes the freshest
block with one `exchange` (only if the FRESH bit is set) and serializes from it:
wait-free, never drops, all-or-nothing per transaction, TSAN-clean.

**Costs that rule it out at ≤ 1 kHz.**

- *Copy-forward amplification.* The back block a writer receives may be
  arbitrarily stale (it was the reader's old front). With per-member generation
  stamps the writer copies only members changed by anyone since that buffer was
  last current — but under multi-writer load that is routinely the other
  writers' whole data volume, paid on *this* writer's thread on every
  transaction. A scalar-writing thread ends up copying another thread's vectors.
- *Lone scalar writes lose the wait-free path.* Bypassing the block with atomic
  stores into all three buffers races with a concurrent copy-forward and can
  publish a one-snapshot-stale value; making it race-free needs value+generation
  in one atomic word, which does not fit. So lone scalar `set()` must take the
  writer mutex.
- *Storage moves into the blocks.* `LoggedValue::value_` becomes three
  placement-constructed objects addressed by index; `getMutablePtr()` must
  copy-forward its member before handing out a pointer.
- *Raw pointers are not covered* (the library does not own their storage), so
  the PI mutex would still exist alongside — two mechanisms, two contracts.
- *3× memory; ~250 lines; three interacting correctness rules* (buffer
  ownership, generation comparison, lone-write interaction) versus one.

**Migration path if needed.** The user-facing API is identical
(`scopedWrite()` transactions, `set()`, `get()`), so switching is an internal
change to `ChannelSharedState` and `takeSnapshot` step 4; the transaction
consistency tests of §9 apply unchanged.

## 12. Rationale summary

- **Publication vs reclamation**: atomics publish; the odd/even epoch answers
  "when may I free this" with two RMWs on the snapshot side and a spin on the
  control side (RCU pattern).
- **Atomics for scalars**: a relaxed 8-byte load/store is a plain move on x86 and
  ARM64 — wait-free and it removes the existing data race.
- **One priority-inheriting mutex per channel, not a wait-free scheme**: the
  hard requirements are (a) values written together appear together, (b) no
  snapshot is dropped because of writer activity, (c) any number of writer
  threads. A seqlock satisfies (a) and (c) but must drop or spin unboundedly
  when a writer is preempted mid-write, and its read section is a formal data
  race. Per-value triple buffering is wait-free and race-free but gives only
  per-value consistency, violating (a). Block-level triple buffering with
  copy-forward transactions satisfies all three with a wait-free reader, at the
  price of 3× memory, a full block copy on every writer transaction, and a
  separate rule for lone scalar writes. The PI mutex satisfies all three with 1×
  memory, no copies and ~30 lines; its price is a bounded wait on the snapshot
  thread — one writer critical section plus a futex round-trip, tens of µs,
  rarely — which is acceptable at the ≤ 1 kHz target. Lone scalar writes stay
  wait-free atomics because they carry no consistency promise.
- **Pool + refcounted handle instead of per-edge rings**: the snapshot is
  serialized once and shared by all sinks (zero copy), a sink that keeps a
  reference degrades to a counter instead of undefined behaviour, and the
  lifetime story ("free when the refcount is zero") is one every reviewer
  already knows. The cost is one more ownership hop (sink → pool) and one rule
  on the snapshot thread (hold your own reference until every push is done).
- **Intrusive refcount, not `std::shared_ptr<Snapshot>`**: constructing a
  `shared_ptr` allocates a control block, and a pre-created one would need
  `use_count()` (only approximate under concurrency) to detect freeness. The
  intrusive counter with release/acquire is allocation-free and TSAN-clean.
- **Reuse moodycamel with explicit producer tokens**: `try_enqueue(token, …)`
  never allocates and is per-producer FIFO (internally an SPSC sub-queue per
  token), `BlockingConcurrentQueue` already carries the semaphore wake-up. That
  is the per-(channel, sink) SPSC handoff from the earlier design, obtained from
  a tested library instead of new code.
- **Acquire before serialize**: the drop decision is made before any work; the
  snapshot is serialized directly into pool memory.
