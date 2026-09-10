# Lock-free, allocation-free front end for `LogChannel`

**Date:** 2026-09-10 (revised: snapshot pool instead of per-edge rings; PI write mutex instead of seqlock)
**Scope:** `data_tamer_cpp` — `LogChannel`, `LoggedValue`, `ValuePtr`, `DataSinkBase`, in-tree sinks
**Status:** approved design. Implementation plans: `docs/superpowers/plans/2026-09-10-lockfree-frontend-plan-1.md` (steps 0–3); plans 2–4 (steps 4–5, 6–7, 8–9) are written as each previous plan lands.

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
| Snapshot thread | caller of `takeSnapshot()` | the channel `WriteMutex` (priority-inheriting), for the serialization only; waits at most one writer critical section | never after the first snapshot (one counted exception, §4.6) |
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
| `SnapshotRef` | same file | Move-only handle `{ shared_ptr<SnapshotPool> pool; Snapshot* slot; }`; copy = `refs++`, destruction = `refs--` (release). Keeps both the slot and the pool alive |
| `SinkLink` | `src/channel.cpp` | `{ DataSinkBase* sink; moodycamel::ProducerToken token; atomic<uint64_t> dropped; }` — one per (channel, sink); the token belongs to the sink's queue |
| `SinkSlot` | `src/channel.cpp` | `{ atomic<SinkLink*> link; shared_ptr<DataSinkBase> sink; unique_ptr<SinkLink> owner; }`, `kMaxSinks` of them |
| `LogChannel::Pimpl` | `src/channel.cpp` | frozen series array, `shared_ptr<ChannelSharedState>`, `shared_ptr<SnapshotPool>`, private `ActiveMask`, `epoch`, `control_mutex`, `SinkSlot[kMaxSinks]`, counters |
| `DataSinkBase::Pimpl` | `src/data_sink.cpp` | thread; `moodycamel::BlockingConcurrentQueue<SnapshotRef> queue` (pre-sized); `std::mutex store_mutex` serializing `storeSnapshot` calls; `atomic<bool> run, accept` |
| `LoggedValue<T>` | `include/data_tamer/logged_value.hpp`, `channel.hpp` | scalar `T` → `std::atomic<T>`; non-scalar → plain `T`; both hold `shared_ptr<ChannelSharedState>` and a `weak_ptr<LogChannel>` used only by the destructor |
| `ValuePtr` | `include/data_tamer/values.hpp` | function pointers instead of `std::function`; new constructor for `std::atomic<T>` |

Constants: `kMaxSinks = 8`, `kLockSpinNs = 2000` (spin on `try_lock` before
sleeping), default `pool_capacity = 64` slots per channel (≈ 64 ms of stall
absorption at 1 kHz — enough for a zstd chunk flush in `MCAPSink`), default sink
`queue_capacity = 1024` refs, sink thread wake timeout `50 ms`.

### 2.3 Ownership

- The **pool** is owned by the channel and by every live `SnapshotRef`. A ref in
  a sink's queue keeps the pool alive after the channel is gone, so a channel may
  be destroyed while sinks still hold snapshots.
- A **slot** is free when `refs == 0`. Only the snapshot thread transitions
  `0 → 1`; sinks and the snapshot thread itself only decrement from `≥ 1`.
- A **`ProducerToken`** is bound to the sink's queue; the channel's `SinkSlot`
  holds a `shared_ptr<DataSinkBase>`, so the sink (and its queue) outlives the
  token by construction.
- The snapshot thread sees only the raw `SinkSlot::link` pointer, cleared before
  the channel destroys the `SinkLink`, and that destruction happens only after an
  epoch wait (§5.1).

### 2.4 Removed

`Pimpl::mutex` and `sinks_mutex` as snapshot-side locks; `Pimpl::snapshot`;
`moodycamel::ConcurrentQueue<Snapshot>` (by value) in `DataSinkBase`;
`DataSinkBase::pushSnapshot()`; `LoggedValue::rw_mutex_`; `LoggedValue` move
constructor/assignment; the sink thread's 250 µs polling loop.

### 2.5 Unchanged

`Snapshot`; `DataSinkBase::addChannel/storeSnapshot` signatures; `ChannelsRegistry`;
schema, hash and wire format; all `registerValue` overloads and their
"throws after logging started" rule; `MCAPSink`/`ROS2PublisherSink` public API;
the vendored moodycamel headers (now used as intended: pre-sized,
explicit-producer, `try_enqueue`).

## 3. Hot path — `takeSnapshot(timestamp)`

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
   with `load(relaxed)`; they need no lock, but being read under it makes a lone
   scalar `set()` that happens during serialization simply land in the next
   snapshot. The size pass is inside the section because a vector may change
   length between the two passes.
5. Fill the header: `timestamp`, cached `schema_hash`, `channel_name`
   (`string_view`), `memcpy` mask, `payload.resize(written)` (≤ capacity).
6. Publish to every sink. For each non-null link:
   `refs.fetch_add(1, relaxed)`; `ok = link->sink->tryPush(link->token,
   SnapshotRef(pool, slot))` (= `queue.try_enqueue(token, ref)`, never allocates,
   signals the sink's semaphore); if `!ok`: the moved-from ref is inert, so
   `refs.fetch_sub(1, relaxed)`, `link->dropped++`.
7. Release the snapshot thread's own hold: `refs.fetch_sub(1, release)`. If no
   sink accepted, this returns the slot to the pool immediately.
8. `epoch.fetch_add(1, release)` → even. Return `true` iff every sink accepted.

The refcount protocol is the one subtle rule on this path: the snapshot thread
holds its own reference (step 2) until *every* `tryPush` is done (step 7), so a
fast sink can never return the slot to the pool while it is still being offered
to a later sink.

### Cost per call

Two atomic RMWs (epoch), one `exchange` (mask), ≤ `kMaxSinks` acquire loads,
≤ `pool_capacity` acquire loads in the worst-case free scan, one uncontended
`try_lock`/`unlock` pair (~40 ns), `N` mask bit tests, serialization, and per
sink: two refcount RMWs plus one moodycamel explicit-producer `try_enqueue`
(store-only fast path) and one semaphore increment. No memcpy of the payload for
any number of sinks. No allocation except §4.6.

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
  auto tx = channel->scopedWrite();     // std::lock_guard<WriteMutex>; same as lock_guard(channel->writeMutex())
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
  `scopedWrite()` (detected by a thread-local "transaction depth" counter in
  `ChannelSharedState`, so nested use does not deadlock and does not need a
  recursive mutex).
- `getMutablePtr()` on a non-scalar holds the mutex for the lifetime of the
  proxy and points at the live object — today's semantics. Holding it long
  blocks the snapshot thread for that long; documented as "keep it short".
- `getConstPtr()` and `get()` on a non-scalar take the mutex, copy, release.
  Bounded; no spinning.
- Raw pointers: `writeMutex()` keeps its signature and meaning (`Mutex&`, where
  `Mutex` is now the alias `DataTamer::WriteMutex` instead of
  `std::shared_mutex`). `std::lock_guard`, `std::unique_lock` and
  `std::scoped_lock` code compiles unchanged; only code calling `lock_shared()`
  breaks (none in-tree). `scopedWrite()` is added as the preferred spelling.
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

## 5. Control path

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
  SnapshotPool(size_t capacity, size_t payload_capacity, size_t mask_bytes);
  Snapshot* tryAcquire();            // snapshot thread only: scan for refs==0, set refs=1
  // slots are returned implicitly when refs reaches 0
  std::atomic<uint64_t> exhausted{0};
 private:
  struct Slot { Snapshot snapshot; alignas(64) std::atomic<uint32_t> refs{0}; };
  std::vector<Slot> slots_;          // payload.reserve(payload_capacity); active_mask.resize(mask_bytes)
  size_t scan_from_ = 0;             // round-robin start, snapshot thread only
};

class SnapshotRef {                  // move-only; copy via explicit clone()
 public:
  SnapshotRef(std::shared_ptr<SnapshotPool>, Snapshot*);   // takes one existing count
  SnapshotRef(SnapshotRef&&) noexcept; SnapshotRef& operator=(SnapshotRef&&) noexcept;
  ~SnapshotRef();                    // if slot: refs.fetch_sub(1, release)
  const Snapshot& operator*() const; const Snapshot* operator->() const;
  explicit operator bool() const;
 private:
  std::shared_ptr<SnapshotPool> pool_; Snapshot* slot_ = nullptr;
};
```

`SnapshotRef` is what a sink is allowed to keep: holding one past
`storeSnapshot()` is safe and merely starves the pool (visible as
`pool_exhausted`). The pool `shared_ptr` inside the ref is what lets a channel be
destroyed while sinks still hold its snapshots.

Memory ordering: sinks release with `fetch_sub(1, release)`; the snapshot thread
acquires a free slot with `load(acquire) == 0`, which synchronizes with the last
release, so the sink's reads of the slot happen-before the snapshot thread's
next writes into it.

### 6.2 `DataSinkBase::Pimpl`

Members: `thread`, `atomic<bool> run, accept`,
`moodycamel::BlockingConcurrentQueue<SnapshotRef> queue(queue_capacity)`,
`std::mutex store_mutex`, `atomic<uint64_t> store_errors`.

Thread loop:

```
while (run) {
  SnapshotRef ref;
  if (!queue.wait_dequeue_timed(ref, 50 ms)) continue;      // wakes on enqueue or timeout
  std::lock_guard lk(store_mutex);
  if (accept) { try { storeSnapshot(*ref); } catch (...) { store_errors++; } }
}                                                             // ref destroyed here → slot released
```

- `tryPush(token, SnapshotRef&&)` (called by channels): `return
  queue.try_enqueue(token, std::move(ref));` — with an explicit producer token
  this never allocates and never blocks; on failure the ref is left intact for
  the caller to discard.
- `stopThread()`: `run = false; join()`; the timed wait bounds the join to 50 ms.
- `stopAcceptingSnapshots()`, `startAcceptingSnapshots()` unchanged.
  `processQueuedSnapshots()`: on the calling thread, `while (queue.try_dequeue(ref))
  { lock store_mutex; storeSnapshot(*ref); }` — concurrent `try_dequeue` with the
  sink thread is safe (MPMC), and `store_mutex` serializes `storeSnapshot`.
- `pushSnapshot()` removed. `queue()` exposed to `LogChannel` (friend or
  `details` accessor) for token creation.
- Destructor asserts `!thread.joinable()` in debug; in release joins and prints to
  stderr. Derived sinks must still call `stopThread()` in their destructors.
  Destroying the queue destroys any remaining refs, releasing their slots.

Queue sizing: `queue_capacity` is a *minimum* total across all producer tokens
(moodycamel allocates blocks of 32 up front and hands them to explicit producers
on demand; explicit producers keep the blocks they have used). With many
channels on one sink, size it as `channels × per-channel depth`. At 16 bytes per
ref, 1024 entries is 16 KB, so err on the large side.

### 6.3 In-tree sink ports

- `MCAPSink`: `thread_local merged_payload` → reserved member (safe: all
  `storeSnapshot` calls are under `store_mutex`); `finishQueueAndStop` =
  `stopAcceptingSnapshots(); processQueuedSnapshots(); stopRecording();` (the
  250 µs sleep is removed).
- `ROS2PublisherSink`, benchmark `NullSink`: unchanged apart from the
  constructor forwarding `queue_capacity` if they expose it.
- `DummySink`: constructor forwards `queue_capacity` the same way; in
  addition its public data members (`schemas`, `schema_names`,
  `snapshots_count`, `latest_snapshot`) are replaced by mutex-protected
  accessors — see §8 — so callers that read the members directly break.

### 6.4 Latency, idle, memory

Snapshot → `storeSnapshot`: one futex wake when the sink thread is asleep
(issued inside `try_enqueue` only if a waiter exists), otherwise the next
dequeue. Idle wakeups: 20/s (timeout only).
Memory per channel = `pool_capacity × (payload_capacity + mask_bytes +
sizeof(Snapshot) + 64)`; defaults give ≈ 1 MB for a 1000-double channel,
independent of the number of sinks. Memory per sink = `queue_capacity × 16 B`
plus moodycamel block overhead.

## 7. Errors and counters

| Situation | Thread | Behaviour |
|---|---|---|
| Register after freeze, name with spaces, type change on re-register, > `kMaxSinks`, capacity setter after freeze | control | `throw std::runtime_error` |
| Pool has no free slot | snapshot | no work, `pool_exhausted++`, return `false` (all sinks miss this snapshot) |
| Sink queue full | snapshot | that sink skipped, `link.dropped++`, return `false` |
| No sinks | snapshot | no work, return `false` |
| `WriteMutex` held by a writer at snapshot time | snapshot | spin ≤ 2 µs, then block (PI-bounded); `write_lock_contended++`, `write_lock_wait_max_ns` updated; snapshot proceeds normally |
| Payload > capacity | snapshot | non-strict: reserve ×2, `payload_reallocations++`; strict: slot returned, `dropped_oversize++`, return `false` |
| `storeSnapshot` throws | sink | caught, `store_errors++`, ref destroyed (slot released) |
| Sink keeps a `SnapshotRef` indefinitely | sink | pool starves → `pool_exhausted` grows; no UB |
| `DataSinkBase` destroyed with live thread | control | debug assert; release: join + stderr |

Nothing on the snapshot path throws. `takeSnapshot()` returns `true` iff every
sink accepted the snapshot.

Accessors on `LogChannel`: `poolExhausted()`, `writeLockContended()`,
`writeLockWaitMaxNs()`, `payloadReallocations()`, `droppedOversize()`,
`droppedSnapshots(const std::shared_ptr<DataSinkBase>&)`, and `Stats stats()
const` bundling them. On `DataSinkBase`: `storeErrors()`.

## 8. Public API delta

| Symbol | Change |
|---|---|
| `LogChannel::writeMutex()` | signature unchanged (`Mutex&`); `Mutex` is now `DataTamer::WriteMutex` (exclusive, priority-inheriting) instead of `std::shared_mutex` — `lock_shared()` callers break, `lock_guard`/`unique_lock`/`scoped_lock` callers do not |
| `LogChannel::scopedWrite()` | new; `std::lock_guard<WriteMutex>` sugar, the documented way to write a transaction |
| `LogChannel::setPayloadCapacity/setPoolCapacity/setStrictMode` | new |
| `LogChannel::poolExhausted/writeLockContended/writeLockWaitMaxNs/payloadReallocations/droppedOversize/droppedSnapshots/stats` | new |
| `LoggedValue` move ctor / assignment | deleted |
| `LoggedValue<T>::getMutablePtr()/getConstPtr()` for atomic-scalar `T` | deprecated (proxy semantics); use `set()`/`get()` |
| `LoggedValue<T>::get()` | now `const` |
| `MutablePtr/ConstPtr::mutex()` | deprecated; returns `nullptr` for scalar `LoggedValue`s |
| `LoggedValue<T>::getLockedPtr()` | already deprecated; unchanged |
| `DataSinkBase::DataSinkBase(size_t queue_capacity = 1024)` | new constructor argument (default keeps old call sites compiling) |
| `DummySink` public members `schemas`, `schema_names`, `snapshots_count`, `latest_snapshot` | replaced by mutex-protected accessors `schema(hash)`, `schemaName(hash)`, `schemasCount()`, `firstSchemaHash()`, `snapshotsCount(hash)`, `latestSnapshot()` (source-breaking for tests that read the members) |
| `DataSinkBase::pushSnapshot` | removed |
| `DataSinkBase::storeErrors()` | new |
| `DataTamer::SnapshotRef` | new public type (sinks may keep one) |
| `MCAPSink::finishQueueAndStop` | no longer sleeps |

## 9. Testing

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
- **Allocation**: `operator new/delete` counting hook in the test binary. After two
  warm-up snapshots, 10 000 `takeSnapshot()` calls with scalars, vectors, custom
  types and two sinks report zero allocations. Grow a vector: at most
  `pool_capacity` allocations and `payloadReallocations()` equal to that count.
  Same in strict mode: zero allocations and `droppedOversize() ≥ 1`.
- **Concurrency under TSAN** (new CI job): (a) writer thread hammering
  `LoggedValue<double>::set` and `LoggedValue<std::vector<double>>::set` during
  snapshots — no reports, every consumed snapshot decodes to a consistent vector;
  (b) random `setEnabled` toggles — decoded field set equals the mask;
  (c) create/destroy `LoggedValue`s and add/remove sinks in a loop during
  snapshots — clean under TSAN and ASAN; (d) two channels sharing one sink — all
  snapshots arrive, per-channel order preserved; (e) sink thread and
  `processQueuedSnapshots()` dequeuing concurrently.
- **Lifetime**: a sink that keeps every `SnapshotRef` — `poolExhausted` grows, no
  UB under ASAN; destroy the channel while the sink still holds refs — refs
  remain readable, pool freed when the last ref dies.
- **Pool sizing**: `MCAPSink` with `do_compression = true` on a channel of 1000
  doubles at 1 kHz for 60 s writing to a file on disk: `poolExhausted() == 0`
  with the default pool (the reason the default is 64, not 16).
- **Drop behaviour**: sink whose `storeSnapshot` sleeps; `setPoolCapacity(4)`,
  100 snapshots → `poolExhausted() == 96` and 96 `false` returns; separately a
  sink constructed with `queue_capacity` small enough to fill → `droppedSnapshots(sink)`
  increments while the pool does not exhaust.
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
