# Lock-free, allocation-free front end for `LogChannel`

**Date:** 2026-09-10 (implemented through Plan 4 on 2026-09-11)
**Scope:** `data_tamer_cpp` — `LogChannel`, `LoggedValue`, `ValuePtr`, `DataSinkBase`, in-tree sinks
**Status:** implemented. Plan 1 implemented steps 0–3, Plan 2 steps 4–5,
Plan 3 step 6, and Plan 4 steps 7–9. The temporary channel-to-sink copy bridge
and snapshot-side structure locks are gone; direct pooled serialization,
capacity controls and the sequentially consistent control-publication protocol
are the final runtime.

## 1. Goal

Make the library-owned `LogChannel::takeSnapshot()` path free of heap allocation
after successful first-call setup while payloads fit their slots. The only lock
it may take is the channel's priority-inheritance write mutex. Its latency still
depends on the writer critical section, serializers, allocator and OS scheduler,
so this is not a universal hard deadline or no-throw guarantee. Keep the
`Snapshot` struct, wire format and `storeSnapshot`-based sink interface
unchanged. Target: control loops up to 1 kHz.

### Requirements (agreed)

| # | Requirement |
|---|---|
| R1 | Exactly one thread per channel calls `takeSnapshot()` (the *snapshot thread*). |
| R2 | Registered values may be written from any number of other threads. A group of values written together in one transaction must appear together in a snapshot (all-or-nothing); no snapshot is dropped merely because a writer is active. Lone scalar writes are wait-free; the snapshot thread may wait for a priority-inherited writer critical section, whose duration the application controls. |
| R3 | When a sink cannot keep up, drop the newest snapshot, count it, return `false`. Slot count is bounded. Strict mode preserves existing per-slot capacities; non-strict byte usage is bounded only when payload sizes remain within established slot capacities. |
| R4 | `setEnabled()` is lock-free and callable from any thread while logging. |
| R5 | New registration (`registerValue*`, `createLoggedValue`) happens during setup only; it may use a mutex and throws after schema freeze. The implemented exception is compatible same-name re-registration into a dead existing slot. |
| R6 | Destroying a `LoggedValue` and `removeDataSink()` remain memory-safe while logging; they may block the *calling* thread, never the snapshot thread. |
| R7 | Sinks that override only `addChannel()` / `storeSnapshot()` compile unchanged. |
| R8 | Backend threads may lock and allocate; pre-allocation there is welcome but not required. |

### Non-goals

New registration after the first snapshot attempt (except compatible same-name
reuse of a dead existing slot); a wait-free snapshot thread in the presence of
multi-value transactions (block-level triple buffering, recorded as the
alternative in §11); any change to `signal_logger`.

The [simpler-design audit](../reviews/2026-09-11-simpler-design-audit.md)
identified stop/join the worker, drain, then restart as the leading alternative
that should have been compared earlier. It remains deferred because adopting it
now would reopen already validated sink and MCAP lifecycle behavior.

## 2. Architecture

### 2.1 Threads and roles

| Role | Who | May lock | May allocate |
|---|---|---|---|
| Snapshot thread | caller of `takeSnapshot()` | after setup, only the channel `WriteMutex` during serialization | no library-owned allocation while the acquired slot fits; non-strict growth and user serializers are exceptions |
| Writer threads | `LoggedValue::set()/getMutablePtr()/setEnabled()`, raw values under `scopedWrite()`, `LogChannel::setEnabled()` | the channel `WriteMutex` (shared with other writers and the snapshot thread); critical sections must be short and allocation-free | no |
| Control threads | `registerValue*`, `createLoggedValue`, `unregister`, `~LoggedValue`, `addDataSink`, `removeDataSink`, capacity setters | `control_mutex`, sink `store_mutex`; may wait one snapshot duration | yes |
| Sink threads | one per `DataSinkBase` | `store_mutex` | yes |

Roles are contracts, not OS threads: one thread may register values (control),
then enter a loop where it writes them (writer) and calls `takeSnapshot()`. The
caller must enter `takeSnapshot()` without a writer guard and must not invoke
control operations or `takeSnapshot()` recursively from serializer callbacks.
`setEnabled` is deliberately classified as a *writer* operation: in practice it
is called from the thread that produces the value (explicitly, or implicitly via
`set(v, auto_enable = true)`), so it must be as cheap as `set()` and must not
depend on the channel object at all (§2.2 `ChannelSharedState`, §5.2).

### 2.2 Components

| Unit | File | Purpose |
|---|---|---|
| `WriteMutex` | `include/data_tamer/details/write_mutex.hpp` | `Lockable` wrapper over `pthread_mutex_t` created with `PTHREAD_PRIO_INHERIT` (Linux); falls back to `std::mutex` elsewhere with a compile-time warning. `lock()`, `try_lock()`, `unlock()` |
| `ChannelSharedState` | `include/data_tamer/details/shared_state.hpp` | `WriteMutex write_mutex`; per-series lock-free `atomic<uint8_t>` flags separating registered liveness from requested enablement; `atomic<bool> mask_dirty`. Owned by `shared_ptr` from the channel **and** every `LoggedValue`, so writer-side operations never need the channel object |
| `SnapshotPool` | `include/data_tamer/details/snapshot_pool.hpp` | K pre-allocated `Snapshot` slots, each with an intrusive `atomic<uint32_t> refs`. `tryAcquire()` (snapshot thread only) scans for `refs == 0`. One pool per channel, owned by `shared_ptr` |
| `SnapshotRef` | same file | Move-only handle `{ shared_ptr<SnapshotPool> pool; PoolSlot* slot; }`; explicit `clone()` increments `refs`, destruction/reset decrements it. Keeps both the slot and pool alive |
| `SinkLink` | `src/channel.cpp` | `{ shared_ptr<DataSinkBase> sink; unique_ptr<ProducerToken> token; atomic<uint64_t> dropped; }`; one owned link per occupied fixed slot. Token declaration order makes it die before the sink |
| Sink publication arrays | `src/channel.cpp` | eight control-owned `unique_ptr<SinkLink>` slots plus eight sequentially consistent atomic raw-link publications remove the sink mutex from the snapshot path |
| `LogChannel::Pimpl` | `src/channel.cpp` | immutable post-freeze series/schema state, direct-serialization pool, cached mask, capacity/counters, fixed sink publications and a single-reader epoch |
| `DataSinkBase::Pimpl` | `src/data_sink.cpp` | thread; pre-sized `BlockingConcurrentQueue<SnapshotRef>`; handoff and store mutexes; current callback ref; packed closed-bit/active-producer admission word; run and error counters |
| `LoggedValue<T>` | `include/data_tamer/logged_value.hpp`, `channel.hpp` | scalar `T` → `std::atomic<T>`; non-scalar → plain `T`; both hold `shared_ptr<ChannelSharedState>` and a `weak_ptr<LogChannel>` used only by the destructor |
| `ValuePtr` | `include/data_tamer/values.hpp` | function pointers instead of `std::function`; new constructor for `std::atomic<T>` |

Constants: `kLockSpinNs = 2000` (spin on `try_lock` before sleeping), default
`pool_capacity = 64` slots per channel (≈ 64 ms of retained/in-flight capacity
at 1 kHz), maximum eight attached sinks, and default sink
`queue_capacity = 1024` refs, sink thread wait timeout `50 ms`. The queue rounds
capacity to internal 32-entry blocks, so the argument is a minimum rather than
an exact limit. Pool and initial payload capacity are configurable before the
first snapshot.

### 2.3 Ownership

- The **pool** is owned by the channel and by every live `SnapshotRef`. A ref in
  a sink's queue keeps the pool alive after the channel is gone, so a channel may
  be destroyed while sinks still hold snapshots.
- A **slot** is free when `refs == 0`. Only the snapshot thread transitions
  `0 → 1`; sinks and the snapshot thread itself only decrement from `≥ 1`.
- A **`ProducerToken`** is bound to the sink's queue. Each `SinkLink` declares
  its `shared_ptr<DataSinkBase>` before its token, so reverse
  member destruction destroys the token first. The sink and queue therefore
  outlive the token by construction.
- The control thread owns links in fixed slots. The snapshot thread reads their
  atomic publications inside the epoch described in §5.1; removal unpublishes,
  waits out an observed reader, then destroys the owner.

### 2.4 Removed

Removed: `Pimpl::mutex`, `sinks_mutex` and `Pimpl::snapshot` from the channel;
`moodycamel::ConcurrentQueue<Snapshot>` (by value) in `DataSinkBase`;
`DataSinkBase::pushSnapshot()`; `LoggedValue::rw_mutex_`; `LoggedValue` move
constructor/assignment; the sink thread's 250 µs polling loop.

### 2.5 Unchanged

`Snapshot`; `DataSinkBase::addChannel/storeSnapshot` signatures; `ChannelsRegistry`;
schema, hash and wire format; all `registerValue` overloads and their
"throws after schema freeze" rule for new names, with compatible same-name
dead-slot reuse; existing `MCAPSink`/`ROS2PublisherSink`
call sites (the MCAP constructor only adds a final defaulted queue-capacity argument);
the vendored moodycamel headers (now used as intended: pre-sized,
explicit-producer, `try_enqueue`).

## 3. Hot path — `takeSnapshot(timestamp)`

The implemented path serializes directly into one acquired pool slot and fans
out references. It contains no channel staging snapshot or snapshot-side
structure lock.

### Step 0 — freeze/setup (first attempt; retries until successful; takes `control_mutex`)

Set `schema_frozen` and cache `schema_hash`; this happens on the first attempt,
even if later setup throws. Create the pool only after its slots reserve
successfully, using `max(user_hint, 2 × current serialized size, 256)`. A sink
link and its `ProducerToken` are created by `addDataSink()`; the first successful
setup calls `addChannel(name, schema)` for each link that has not registered its
schema, records each successful registration, then SC-publishes the links and
sets `logging_started`. A failed setup is retryable: completed schema
registrations are not repeated and nothing is published early. The first attempt
may lock, allocate or throw; after successful setup the series layout is
immutable and the snapshot thread never takes `control_mutex` again.

### Steps 1–8 — every call

1. `epoch.fetch_add(1, seq_cst)` → odd = in progress.
2. Load each published sink link (`seq_cst`) into a local array; if all null, epoch
   exit, `return false`. `slot = pool.tryAcquire()`: scan slots round-robin from
   the last index for `refs.load(acquire) == 0`; on success `refs.store(1,
   relaxed)` (only this thread makes the `0 → 1` transition). If none is free:
   `pool_exhausted++`, epoch exit, `return false`. No serialization work is done.
3. `if (mask_dirty.exchange(false, seq_cst))` rebuild the private mask from
   the sequentially consistent registered/enabled flags. A field is active only
   when both bits are set. From here on only the mask is consulted.
4. Locked serialization section. Acquire `state->write_mutex`: try-lock for the
   nominal `kLockSpinNs` budget (2 µs), then block if it is still held. If the
   spin fails,
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
8. `epoch.fetch_add(1, seq_cst)` → even. Return `true` iff every sink accepted.

The refcount protocol is the one subtle rule on this path: the snapshot thread
holds its own reference (step 2) until *every* `tryPush` is done (step 7), so a
fast sink can never return the slot to the pool while it is still being offered
to a later sink.

### Cost per call after setup

Two sequentially consistent atomic RMWs (epoch), one sequentially consistent
`exchange` (mask), ≤ `kMaxSinks` sequentially consistent link loads,
≤ `pool_capacity` acquire loads in the worst-case free scan, one uncontended
`try_lock`/`unlock` pair (~40 ns), `N` mask bit tests, serialization, and per
sink: two refcount RMWs plus one moodycamel explicit-producer `try_enqueue`
(store-only fast path) and one semaphore increment. No memcpy of the payload for
any number of sinks. No allocation except §4.6.

The zero-allocation regression covers warmed fixed-size publication, queue-full
failure, fanout, variable-value reservation, strict oversize drops and pool
exhaustion. Non-strict per-slot growth and user serializer behavior remain the
documented exceptions.

When a writer holds the mutex, latency includes the remaining writer critical
section, scheduling and a possible futex sleep/wake. Priority inheritance
mitigates inversion on supported Linux systems but establishes no universal
upper bound. The measured shared-desktop runs include millisecond maxima; see
[Plan 4 measurements](../../benchmarks/2026-09-plan4.md).

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
- On Linux the wrapper requests `PTHREAD_PRIO_INHERIT`; elsewhere it uses the
  platform fallback. This may mitigate priority inversion, but applications must
  not infer a latency bound from the mutex type or scheduling policy.

Why a single mutex rather than the wait-free alternatives: consistency across
values plus "never drop a snapshot" plus multiple writers means the snapshot
thread must either copy the whole value set per transaction (block-level triple
buffering, 3× memory, a block copy on every writer transaction, and a second
rule for lone scalar writes) or wait for the writer. The mutex is the smaller,
simpler choice here, with the measured tail behavior documented separately
(§11, §12).

### 4.3 Raw pointers written by the snapshot thread

No synchronization needed; documented as the fastest path.

### 4.4 `LoggedValue` ↔ channel

`LoggedValue` holds `std::shared_ptr<ChannelSharedState> state_` and its
`RegistrationID`. `setEnabled(b)` and the auto-enable branch of `set()` operate
through `state_->setEnabled(id, b)` (§5.2), which changes only the enabled bit
and dirties the cached mask. These are pure atomics on memory the `LoggedValue`
co-owns, so they are valid on a writer thread, cost the same as a `set()`, and
work even if the channel has already been destroyed.
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

- `strict_mode == false` (default): `slot->payload.reserve(checkedDouble(size))` on that slot
  only, `payload_reallocations++`. Other slots grow lazily when next acquired, so
  one growth event costs up to `pool_capacity` counted allocations spread over
  the following ticks. This is the single sanctioned allocation on the hot path.
- `strict_mode == true`: release the parent reference, `dropped_oversize++`,
  `return false`. The acquired slot's actual `std::vector` capacity is the limit,
  not the original hint; growth retained from earlier non-strict calls remains
  usable.

`strict_mode` is an `std::atomic<bool>` and may be toggled at runtime.
`setPayloadCapacity(bytes)` before freeze contributes a minimum to the initial
`max(bytes, 2 * initial_payload_size, 256)` reservation. It is not an exact
ceiling. Strict mode fixes the existing per-slot capacities: every later payload
that exceeds its acquired slot drops instead of growing it. Without strict mode,
later larger input may grow memory again.
Size queries, custom serializers and vector allocation can throw, so neither
mode is a universal no-throw contract.

## 5. Control path

All control operations serialize on `control_mutex` (`std::mutex`), never taken
by the snapshot thread after freeze.

### 5.1 Epoch and quiescence

```cpp
void waitQuiescent() {                       // caller holds control_mutex
  const uint64_t e = epoch.load(seq_cst);
  if ((e & 1) == 0) return;
  while (epoch.load(seq_cst) == e) std::this_thread::yield();
}
```

Contract: publish the change sequentially consistently before calling
`waitQuiescent()`. The snapshot's SC epoch entry precedes its SC sink loads and
dirty exchange. If a controller observes the reader's odd epoch, it waits for
that reader to exit. If it observes an old even epoch, the SC total order places
the controller's publication before the later reader entry, so that reader
cannot also consume the old publication. This closes the store-buffering hole
in the earlier acquire/release sketch; sanitizer runs support but do not prove
the ordering argument.

For mask reuse, a reader that exchanges `mask_dirty` before an unregistering
dirty store may use the old mask, but its odd epoch forces unregistration to
wait before detaching the holder. A reader that exchanges afterward rebuilds
from the new SC registered/enabled flag. Re-registration initializes the holder
before its SC registered publication, so a rebuilt active bit observes a valid
holder. Control calls must be made between snapshots and outside writer/proxy
guards and serializer callbacks.

### 5.2 `setEnabled(id, bool)` — wait-free, any thread (writer-class operation)

For each field, an SC `fetch_or` or `fetch_and` changes only the requested
enabled bit; if it changed, `mask_dirty.store(true, seq_cst)`. It never sets the
registered bit, so enabling a dead field cannot revive it. No mutex, no `weak_ptr::lock`, no
dependency on the `LogChannel` object: `LogChannel::setEnabled` and
`LoggedValue::setEnabled` both dispatch to `ChannelSharedState::setEnabled`.
Safe to call from a writer thread, from the snapshot thread
between snapshots, or from inside a `scopedWrite()` transaction.

The SC flag/dirty handshake makes mask changes visible to a later rebuilding
snapshot. Scalar payload values remain relaxed atomics and retain their separate
consistency contract in §4.1.

### 5.3 Registration — `control_mutex`, before freeze only

New names still throw after freeze. Per-series flag bytes live in an append-only
container of atomics. Re-registering a previously unregistered name with the
same compatible type remains allowed after freeze: initialize `holder`, then SC
publish `registered | enabled` and dirty the mask. The returned registration ID
identifies the reused schema slot, not a generation; an older same-name ID also
denotes the replacement slot.

### 5.4 `unregister(id)` / `~LoggedValue`

Lock `control_mutex`; clear the registered bit without changing requested
enablement; `mask_dirty.store(true, seq_cst)`; `waitQuiescent()`; detach
`holder[i]`. The
destructor goes through `weak_ptr::lock()`; if the channel is gone it does
nothing.

### 5.5 `addDataSink(sink)`

Lock `control_mutex`; take the first free slot (throw if all `kMaxSinks` are
used); create its `SinkLink` and `ProducerToken`. If the schema is already
frozen, call `sink->addChannel(name, schema)` and record that registration before
installing the owner. If logging has started, publish the completed raw link with
an SC store; otherwise first-call setup registers and publishes it.

### 5.6 `removeDataSink(sink)`

Lock `control_mutex`; publish null with an SC store; `waitQuiescent()`
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

`getSchema()` and `droppedSnapshots(sink)` take `control_mutex`; the latter is
defined only for the current attachment and returns zero when detached. The
ordinary counters backing `stats()` are `std::atomic<uint64_t>` with relaxed
RMW on the snapshot thread and relaxed loads elsewhere. `Stats` itself is a
plain point-in-time value and does not include attachment drops or sink callback
errors.

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
| `WriteMutex` held by a writer at snapshot time | snapshot | try-lock for the nominal 2 µs spin budget, then block; `write_lock_contended++`, `write_lock_wait_max_ns` updated; snapshot proceeds normally. Linux PI may mitigate inversion but gives no portable or universal bound. |
| Payload > acquired slot's actual capacity, non-strict | snapshot | reserve twice the required size on that slot; on success `payload_reallocations++`; allocation/serializer exceptions propagate |
| Payload > acquired slot's actual capacity, strict | snapshot | release the slot, `dropped_oversize++`, return `false`; previously retained growth remains usable |
| `storeSnapshot` throws | sink | caught, `store_errors++`, ref destroyed (slot released) |
| Sink retains a callback `SnapshotRef` indefinitely | sink | pool starves → `pool_exhausted` grows; no UB |
| `DataSinkBase` destroyed with live thread | control | debug assert; release: join + stderr |

`takeSnapshot()` returns `true` iff every attached sink's queue accepted the
snapshot; a callback's `false` return is distinct from an enqueue failure.
Library-owned allocation is absent only after successful setup while the
acquired slot fits. Custom size/serialization code and non-strict growth can
allocate or throw, and strict mode does not catch those exceptions.

`LogChannel::stats()` bundles `write_lock_contended`,
`write_lock_wait_max_ns`, `pool_exhausted`, `payload_reallocations` and
`dropped_oversize`. The same counters have individual accessors.
`droppedSnapshots(sink)` is the current attachment's queue-failure count and
takes `control_mutex`; `DataSinkBase::storeErrors()` separately counts thrown
callbacks on the backend.

## 8. Public API delta

| Symbol | Change |
|---|---|
| `LogChannel::writeMutex()` | signature unchanged (`Mutex&`); `Mutex` is now `DataTamer::WriteMutex` (exclusive, priority-inheriting) instead of `std::shared_mutex` — `lock_shared()` callers break, `lock_guard`/`unique_lock`/`scoped_lock` callers do not |
| `LogChannel::scopedWrite()` | new; nonmovable, nesting-aware `ChannelSharedState::Transaction` guard of the channel's `WriteMutex` |
| `LogChannel::setPayloadCapacity/setPoolCapacity/setStrictMode` | new; payload/pool setters freeze on the first snapshot attempt, strict mode is runtime atomic |
| `LogChannel::poolExhausted/writeLockContended/writeLockWaitMaxNs/droppedSnapshots/stats` | new; `Stats` contains the two lock counters, pool exhaustion, payload reallocations and oversize drops; attachment drops remain a mutex-protected lookup |
| `LogChannel::payloadReallocations/droppedOversize` | new |
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

The final implementation includes atomic scalars, nested transactions,
contention counters, sink Pimpls, pooled queue delivery, removal of
`pushSnapshot`, the no-sleep MCAP finish/restart lifecycle, capacity/strict-mode
APIs, payload counters, SC channel epoch publication and direct pooled
serialization. `write_lock_contended`
counts blocking acquisitions after the spin budget; `write_lock_wait_max_ns`
measures only the blocking acquisition, excluding serialization.

## 9. Testing

The items below describe completed coverage through Plan 4.

- **Unit, single-threaded**: `SnapshotPool` acquire/release, exhaustion,
  round-robin scan, zero allocations after construction; `SnapshotRef` move
  semantics and refcount on destruction; `WriteMutex` reports its compile-time
  Linux PI configuration and satisfies `Lockable`; nested `set()` inside
  `scopedWrite()` on the same thread does not deadlock. Separate shared-state
  tests cover enablement changes and ensure a dead field cannot be revived.
- **Transaction consistency**: writer thread updates `pos` and `vel` inside
  `scopedWrite()` with a deliberately slow body; every consumed snapshot decodes
  either both old or both new values, never mixed. Repeat with a raw pointer in
  the transaction.
- **Priority inheritance** (runs only when the test has `CAP_SYS_NICE`): a
  `SCHED_OTHER` holder and CPU hogs share one core while a `SCHED_FIFO` waiter
  directly acquires `WriteMutex`; the test asserts an observed wait below 1 ms.
  It was skipped in the recorded gates because `CAP_SYS_NICE` was unavailable,
  and it does not provide a plain-mutex control comparison or a general bound.
- **Refcount protocol**: two sinks, one of which returns `true` from `tryPush` and
  immediately consumes; assert the slot is not reused before the second
  `tryPush` (the snapshot thread's own hold, §3 step 7).
- **Allocation**: the `operator new/delete` hook reports zero
  allocations and zero deallocations after pool creation for fixed-size warmed
  successful fanout and persistent queue-full failures. Queue traits route their
  allocations through the same hook. Variable payload reservation, non-strict
  growth, strict drops and pool exhaustion have focused coverage.
- **Concurrency under TSAN** (new CI job): (a) writer thread hammering
  `LoggedValue<double>::set` and `LoggedValue<std::vector<double>>::set` during
  snapshots — no reports, every consumed snapshot decodes to a consistent vector;
  (b) random `setEnabled` toggles — decoded field set equals the mask;
  (c) create/destroy `LoggedValue`s and add/remove sinks in a loop during
  snapshots; (d) two channels sharing one sink preserve per-producer
  order; (e) sink worker and manual drainer serialize dequeue/callback.
- **Lifetime**: a callback retains references until the 64-slot
  pool exhausts; payload, mask and pool-owned long channel name remain readable
  after queue, channel and sink destruction; releasing refs restores publication.
- **Pool sizing measurement**: compressed `MCAPSink` on the 1000-value harness
  at 1 kHz for 60 s wrote 60,010 records (including warm-up), passed official
  MCAP `info`/`doctor` checks, and reported `poolExhausted() == 0` with the
  default 64-slot pool. The separate file-order checker found zero inversions
  and parse errors across those container records; it verifies MCAP container
  parsing/order, not payload decoding.
- **Drop behaviour**: a requested queue capacity of one rounds
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
| 1 | `WriteMutex` (`details/write_mutex.hpp`) — standalone PI mutex wrapper | compile-time Linux PI configuration check; `Lockable` conformance; spin-then-lock helper; privileged direct-mutex behavioral experiment (skipped without `CAP_SYS_NICE`) | 0 |
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
  separate rule for lone scalar writes. The mutex satisfies the consistency and
  no-writer-drop requirements with 1× memory and no copies; its price is that
  the snapshot thread waits for a writer. Linux PI may mitigate inversion, but
  observed shared-desktop tails reach milliseconds and no general timing bound is
  claimed. Lone scalar writes stay wait-free atomics because they carry no
  consistency promise.
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
