# Lock-free Front End — Plan 2 (spec steps 4–5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `LoggedValue<T>::set()` wait-free for scalars and give every cross-thread write (non-scalar values, raw pointers, multi-value transactions) one priority-inheriting mutex that `takeSnapshot()` also serializes under — the first user-visible behaviour change of the redesign, and the one that fixes the existing `set()`/`takeSnapshot()` data race.

**Architecture:** `ChannelSharedState` (new) holds the per-series enable flags, the `mask_dirty` flag and the `WriteMutex`; it is co-owned by the channel and by every `LoggedValue`, so writer-side operations never touch the channel object. Scalar `LoggedValue`s store an `std::atomic<T>` registered through the `ValuePtr(const std::atomic<T>*)` constructor from Plan 1. `LogChannel::writeMutex()` keeps its signature but returns the shared `WriteMutex`; `takeSnapshot()` takes it (spin-then-lock) only around the size+serialize passes, while the old `Pimpl::mutex` keeps guarding structure until Plan 4 removes it. The two sinks move their private state behind PIMPL for ABI stability.

**Tech Stack:** C++17, CMake presets `debug`/`asan`/`tsan`/`release` (Plan 1), GoogleTest, Google Benchmark, `WriteMutex` and `ValuePtr` from Plan 1.

**Spec:** `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md` — §2.1–2.2 (roles, `ChannelSharedState`), §3 step 4 (locked serialization), §4.1 (atomic scalars), §4.2 (transactions), §4.4 (`LoggedValue` ↔ channel), §5.2 (`setEnabled`), §7 (counters), §8 (API delta), §9 (tests), §10 steps 4–5, §10.1 steps 4–5.

## Execution status (2026-09-11)

Tasks 1–4 were already committed at `40c1812`, `57df98f`, `ff477be`, and
`18a187c` when execution resumed. Tasks 5–6 are committed as `f1cd33e` and
`3812c2f`. Final review fixes and
the carried-over standalone benchmarks are committed as `a14c1ff`. Task 7 is
complete: API documentation, changelog, and all measurements are in
`docs/benchmarks/2026-09-plan2.md`. Final review found no remaining blocking
issues. Debug/ASAN+UBSAN/TSAN each pass 87 tests with one privileged PI skip;
Release passes 88 with the same skip, including the CLI regression.

The following corrections supersede the original implementation sketches:

- Transaction nesting uses an allocation-free chain of active guards, with no
  eight-channel limit. Guards are nonmovable; C++17 copy elision returns them.
- Contention counts only acquisition attempts that block after the spin budget.
  Maximum wait excludes serialization. Snapshot unlocking uses RAII, including
  when a user serializer throws; size and serialization use the same active mask.
- ROS Pimpl is fully defined out of line. A normalized-interface delegating
  constructor preserves the public node-like template inputs. Its destructor
  stops the worker before freeing private state, and schema flag access is locked.
- The latency harness includes `--vector-writer` for spec §10.1, in addition
  to `--transactions`.
- Validation uses this machine's GCC 15 and ROS Lyrical. A missing `<cstdint>`
  include in vendored MCAP was fixed in `5ae22de`. ROS validation supplies the
  installed MCAP library path explicitly; the six ROS publisher tests and both
  sink layout assertions were compiled and run despite the legacy gtest-vendor
  discovery falling back to system GTest.
- Measurements use the requested CPU affinity and repetitions, but this machine
  differs from the historical baseline; the results document makes no controlled
  speedup claim. LeakSanitizer runs outside the traced sandbox with leak detection
  enabled; no suppressions are added.

## Global Constraints

- Every task ends with `debug`, `asan`, `tsan` and `release` presets green: `cmake --preset <p> && cmake --build --preset <p> && ctest --preset <p>` (run from `data_tamer_cpp/`). Zero sanitizer diagnostics, zero compiler warnings (library flags: `-Wall -Wconversion -Wextra -Wsign-conversion -Werror -Wpedantic -Wno-sign-conversion`).
- Wire format, schema hash and `Snapshot` are unchanged. Golden tests from Plan 1 (`ValuePtrGolden.*`) and every existing `payload.size()` assertion must keep passing.
- Spec constants: `kLockSpinNs = 2000` (already in `WriteMutex`); `LoggedValue` scalar trait `is_atomic_scalar_v<T> = IsNumericType<T>() && sizeof(T) <= 8 && std::atomic<T>::is_always_lock_free`.
- Consistency is opt-in (spec §4.2): a lone scalar `set()` is a relaxed atomic store with no cross-value ordering promise; `scopedWrite()` or one `LoggedValue<Struct>` gives all-or-nothing.
- `writeMutex()` keeps returning `Mutex&`; the alias `DataTamer::Mutex` becomes `DataTamer::WriteMutex`. `lock_guard`/`unique_lock`/`scoped_lock` callers compile unchanged; `lock_shared()` callers break (none in-tree).
- Test source files are added to `DATATAMER_TEST_SOURCES` in `tests/CMakeLists.txt` (one list, Plan 1 cleanup).
- Tests run under the `tsan` preset with per-test discovery (`CROSSCOMPILING_EMULATOR`); TSAN stress tests must be real (writer thread + snapshot thread on shared state) and must be clean without suppressions.
- Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Baseline for before/after numbers: `docs/benchmarks/2026-09-baseline.md` (library commit `a851887`); the harness is `build/release/benchmarks/rt_latency`, the micro-benchmarks `build/release/benchmarks/data_tamer_benchmark`. Record numbers pinned with `taskset -c 0-5` on an idle machine.

---

## File map

| File | Responsibility | Task |
|---|---|---|
| `include/data_tamer/details/shared_state.hpp` | `ChannelSharedState`: `WriteMutex write_mutex`, per-series `enabled` atomics, `mask_dirty`, `setEnabled()` free function | 1 |
| `tests/shared_state_tests.cpp` | | 1 |
| `include/data_tamer/details/locked_reference.hpp` | `Mutex` alias → `WriteMutex`; `MutablePtr`/`ConstPtr` exclusive-lock only; `AtomicProxy` for scalars | 2 |
| `include/data_tamer/logged_value.hpp`, `include/data_tamer/channel.hpp` (LoggedValue section) | scalar storage `std::atomic<T>`, `shared_ptr<ChannelSharedState>`, new `set/get/setEnabled/getMutablePtr/getConstPtr` | 3, 4 |
| `include/data_tamer/channel.hpp` (LogChannel section), `src/channel.cpp` | `registerValue(const std::atomic<T>*)`, `sharedState()`, `scopedWrite()`, `writeMutex()` on `WriteMutex`, enable flags in shared state, locked serialization, counters | 3, 4, 5 |
| `tests/logged_value_tests.cpp` | scalar atomics, proxies, race test, deprecation checks | 3 |
| `tests/transaction_tests.cpp` | all-or-nothing transactions, contention counter, nested `set()` | 5 |
| `benchmarks/data_tamer_benchmark.cpp`, `benchmarks/rt_latency.cpp` | transaction writer case; print the new counters | 5 |
| `include/data_tamer/sinks/mcap_sink.hpp`, `src/sinks/mcap_sink.cpp` | PIMPL | 6 |
| `include/data_tamer/sinks/ros2_publisher_sink.hpp`, `src/sinks/ros2_publisher_sink.cpp` | PIMPL (ROS build only) | 6 |
| `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md` §8, `CHANGELOG.rst` | API delta rows | 7 |
| `docs/benchmarks/2026-09-plan2.md` | measured numbers after steps 4–5 | 7 |

---

### Task 1: `ChannelSharedState`

**Files:**
- Create: `include/data_tamer/details/shared_state.hpp`
- Create: `tests/shared_state_tests.cpp`
- Modify: `tests/CMakeLists.txt` (append to `DATATAMER_TEST_SOURCES`), `CMakeLists.txt` (add the header to `add_library(data_tamer ...)`)

**Interfaces:**
- Consumes: `DataTamer::WriteMutex` (`details/write_mutex.hpp`), `RegistrationID` (`types.hpp`).
- Produces:
  ```cpp
  namespace DataTamer {
  class ChannelSharedState {
   public:
    WriteMutex write_mutex;
    std::atomic<bool> mask_dirty{true};
    void addSeries();                                   // control thread, before freeze; appends one flag (enabled = true)
    size_t seriesCount() const;
    bool isEnabled(size_t index) const;                 // relaxed load
    void setEnabled(const RegistrationID& id, bool);    // wait-free: exchange per field + mask_dirty release store
    void setEnabled(size_t index, bool);                // single field
  };
  }
  ```
  Tasks 3–5 use these names.

Design note: the flags are `std::atomic<bool>` in a `std::deque` (element addresses are stable under `push_back`, and `std::atomic` is not movable, so `std::vector` cannot hold it). Registration is setup-only (spec R5), so appends never race with the snapshot thread.

- [x] **Step 1: Write the failing tests**

`tests/shared_state_tests.cpp`:

```cpp
#include "data_tamer/details/shared_state.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

using namespace DataTamer;
using DataTamerTest::AllocCounter;

TEST(ChannelSharedState, SeriesStartEnabledAndMaskStartsDirty)
{
  ChannelSharedState state;
  ASSERT_TRUE(state.mask_dirty.load());
  state.addSeries();
  state.addSeries();
  ASSERT_EQ(state.seriesCount(), 2u);
  ASSERT_TRUE(state.isEnabled(0));
  ASSERT_TRUE(state.isEnabled(1));
}

TEST(ChannelSharedState, SetEnabledFlipsFlagsAndDirtiesMaskOnlyOnChange)
{
  ChannelSharedState state;
  for(int i = 0; i < 4; i++)
  {
    state.addSeries();
  }
  state.mask_dirty.store(false);

  state.setEnabled(RegistrationID{ 1, 2 }, false);
  ASSERT_TRUE(state.isEnabled(0));
  ASSERT_FALSE(state.isEnabled(1));
  ASSERT_FALSE(state.isEnabled(2));
  ASSERT_TRUE(state.isEnabled(3));
  ASSERT_TRUE(state.mask_dirty.exchange(false));

  // same value again: no change, mask stays clean
  state.setEnabled(RegistrationID{ 1, 2 }, false);
  ASSERT_FALSE(state.mask_dirty.load());

  state.setEnabled(2, true);
  ASSERT_TRUE(state.isEnabled(2));
  ASSERT_TRUE(state.mask_dirty.load());
}

TEST(ChannelSharedState, SetEnabledDoesNotAllocateOrLock)
{
  ChannelSharedState state;
  for(int i = 0; i < 8; i++)
  {
    state.addSeries();
  }
  AllocCounter::Scope scope;
  for(int i = 0; i < 1000; i++)
  {
    state.setEnabled(RegistrationID{ 0, 8 }, (i & 1) != 0);
  }
  ASSERT_EQ(scope.allocations(), 0u);
}

TEST(ChannelSharedState, WriteMutexIsUsableWithLockGuard)
{
  ChannelSharedState state;
  {
    std::lock_guard<WriteMutex> lk(state.write_mutex);
  }
  ASSERT_TRUE(state.write_mutex.try_lock());
  state.write_mutex.unlock();
}

// A toggling thread and a reading thread on the same flags; the reader must
// only ever observe true/false (never torn) and the run must be TSAN-clean.
TEST(ChannelSharedState, ConcurrentToggleAndReadIsRaceFree)
{
  ChannelSharedState state;
  for(int i = 0; i < 16; i++)
  {
    state.addSeries();
  }
  std::atomic_bool stop{ false };
  std::thread toggler([&] {
    bool v = false;
    while(!stop)
    {
      state.setEnabled(RegistrationID{ 0, 16 }, v);
      v = !v;
    }
  });
  for(int n = 0; n < 100000; n++)
  {
    if(state.mask_dirty.exchange(false, std::memory_order_acq_rel))
    {
      for(size_t i = 0; i < 16; i++)
      {
        const bool e = state.isEnabled(i);
        ASSERT_TRUE(e == true || e == false);
      }
    }
  }
  stop = true;
  toggler.join();
}
```

- [x] **Step 2: Register and run to see the failure**

Append `shared_state_tests.cpp` to `DATATAMER_TEST_SOURCES` in `tests/CMakeLists.txt`; add `include/data_tamer/details/shared_state.hpp` to the `add_library(data_tamer ...)` list in `CMakeLists.txt` next to `write_mutex.hpp`.

Run: `cmake --preset debug && cmake --build --preset debug`
Expected: FAIL — `data_tamer/details/shared_state.hpp: No such file or directory`.

- [x] **Step 3: Write the header**

`include/data_tamer/details/shared_state.hpp`:

```cpp
#pragma once

#include "data_tamer/details/write_mutex.hpp"
#include "data_tamer/types.hpp"

#include <atomic>
#include <cstddef>
#include <deque>

namespace DataTamer
{

/**
 * @brief State shared between a LogChannel and every LoggedValue registered
 * in it. Owned through shared_ptr by both, so writer-side operations
 * (set, setEnabled, transactions) never need the channel object and keep
 * working if the channel is destroyed first.
 *
 * Series flags are appended during registration (setup only, single
 * control thread) and read by the snapshot thread; std::deque keeps the
 * atomics at stable addresses while appending.
 */
class ChannelSharedState
{
public:
  /// The transaction lock: writers hold it for a scopedWrite(); the snapshot
  /// thread holds it for serialization. Priority-inheriting where available.
  WriteMutex write_mutex;

  /// Set (release) by any enable/disable; consumed (acq_rel exchange) by the
  /// snapshot thread when it rebuilds its private active mask.
  std::atomic<bool> mask_dirty{ true };

  void addSeries() { enabled_.emplace_back(true); }

  size_t seriesCount() const { return enabled_.size(); }

  bool isEnabled(size_t index) const
  {
    return enabled_[index].load(std::memory_order_relaxed);
  }

  /// Wait-free; callable from any thread, including inside a transaction.
  void setEnabled(size_t index, bool enable)
  {
    if(enabled_[index].exchange(enable, std::memory_order_relaxed) != enable)
    {
      mask_dirty.store(true, std::memory_order_release);
    }
  }

  void setEnabled(const RegistrationID& id, bool enable)
  {
    bool changed = false;
    for(size_t i = 0; i < id.fields_count; i++)
    {
      changed |= enabled_[id.first_index + i].exchange(enable, std::memory_order_relaxed) != enable;
    }
    if(changed)
    {
      mask_dirty.store(true, std::memory_order_release);
    }
  }

private:
  std::deque<std::atomic<bool>> enabled_;
};

}  // namespace DataTamer
```

- [x] **Step 4: Run under all presets**

Run: `cmake --build --preset debug && ctest --preset debug -R ChannelSharedState`, then the full `asan`, `tsan`, `release` presets.
Expected: 5 new tests pass; all presets green; `ConcurrentToggleAndReadIsRaceFree` clean under TSAN.

- [x] **Step 5: Commit**

```bash
git add include/data_tamer/details/shared_state.hpp tests/shared_state_tests.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add ChannelSharedState (write mutex + wait-free enable flags)

Shared between a channel and its LoggedValues so writer-side operations
never touch the channel object (spec §2.2, §5.2). Not wired yet.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `Mutex` alias → `WriteMutex`; scalar proxies in `locked_reference.hpp`

**Files:**
- Modify: `include/data_tamer/details/locked_reference.hpp` (whole file)
- Modify: `include/data_tamer/sinks/ros2_publisher_sink.hpp:82` (`Mutex schema_mutex_;` → `std::mutex schema_mutex_;`)
- Create: `tests/locked_reference_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `using Mutex = WriteMutex;` (global namespace, as today, plus `DataTamer::Mutex` re-export); `MutablePtr<T>`/`ConstPtr<T>` unchanged in shape but their `mutex()` is `[[deprecated]]`; new `template <typename T> class AtomicProxy` (holds a copy; writes back on destruction) and `template <typename T> class AtomicConstProxy` (holds a copy) with `operator*`, `operator->`, `explicit operator bool`, `mutex()` returning `nullptr`. Task 3 returns these from scalar `LoggedValue`s.

Why the ROS sink line changes: `ROS2PublisherSink::schema_mutex_` is declared as `Mutex`, which becomes the PI `WriteMutex`; it only ever needs a plain mutex, and giving a backend object a PI mutex would be misleading. This is the only in-tree `Mutex` user outside the channel/`LoggedValue` pair (verified by `grep -rn "\bMutex\b" include src`).

- [x] **Step 1: Write the failing tests**

`tests/locked_reference_tests.cpp`:

```cpp
#include "data_tamer/details/locked_reference.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <type_traits>

using namespace DataTamer;

TEST(LockedReference, MutexAliasIsTheWriteMutex)
{
  static_assert(std::is_same_v<Mutex, WriteMutex>);
  Mutex m;
  std::lock_guard<Mutex> lk(m);  // BasicLockable still satisfied
}

TEST(LockedReference, MutablePtrLocksExclusivelyForItsLifetime)
{
  WriteMutex m;
  int value = 1;
  {
    MutablePtr<int> p(&value, &m);
    ASSERT_TRUE(p);
    ASSERT_FALSE(m.try_lock());  // held by p
    *p = 2;
  }
  ASSERT_TRUE(m.try_lock());
  m.unlock();
  ASSERT_EQ(value, 2);
}

TEST(LockedReference, ConstPtrAlsoLocksExclusively)
{
  WriteMutex m;
  const int value = 7;
  {
    ConstPtr<int> p(&value, &m);
    ASSERT_EQ(*p, 7);
    ASSERT_FALSE(m.try_lock());
  }
  ASSERT_TRUE(m.try_lock());
  m.unlock();
}

TEST(LockedReference, AtomicProxyWritesBackOnDestruction)
{
  std::atomic<double> target{ 1.5 };
  {
    AtomicProxy<double> p(&target);
    ASSERT_TRUE(p);
    ASSERT_EQ(*p, 1.5);
    *p += 1.0;
    ASSERT_EQ(target.load(), 1.5);  // not yet visible
    ASSERT_EQ(p.mutex(), nullptr);
  }
  ASSERT_EQ(target.load(), 2.5);
}

TEST(LockedReference, AtomicConstProxyHoldsACopy)
{
  std::atomic<int> target{ 3 };
  AtomicConstProxy<int> p(&target);
  target.store(4);
  ASSERT_EQ(*p, 3);
  ASSERT_TRUE(p);
}

TEST(LockedReference, NullProxiesAreFalse)
{
  AtomicProxy<int> a(nullptr);
  AtomicConstProxy<int> b(nullptr);
  MutablePtr<int> c(nullptr, nullptr);
  ASSERT_FALSE(a);
  ASSERT_FALSE(b);
  ASSERT_FALSE(c);
}
```

- [x] **Step 2: Register and run to see the failures**

Append `locked_reference_tests.cpp` to `DATATAMER_TEST_SOURCES`.

Run: `cmake --build --preset debug 2>&1 | grep -E "error" | head -5`
Expected: FAIL — `AtomicProxy` not declared; `static_assert(std::is_same_v<Mutex, WriteMutex>)` fails.

- [x] **Step 3: Rewrite `locked_reference.hpp`**

```cpp
#pragma once

#include "data_tamer/details/write_mutex.hpp"

#include <atomic>
#include <utility>

/// The channel's transaction lock. Exclusive and priority-inheriting: a
/// writer holding it is boosted to the priority of a waiting snapshot thread.
/// (Was std::shared_mutex; lock_shared() is no longer available.)
using Mutex = DataTamer::WriteMutex;

namespace DataTamer
{
using ::Mutex;

/**
 * @brief Const pointer that holds the mutex for its lifetime.
 */
template <typename T>
class ConstPtr
{
public:
  ConstPtr(const T* obj, Mutex* mutex);
  ConstPtr(const ConstPtr&) = delete;
  ConstPtr& operator=(const ConstPtr&) = delete;
  ConstPtr(ConstPtr&&) noexcept;
  ConstPtr& operator=(ConstPtr&&) noexcept;
  ~ConstPtr();

  explicit operator bool() const { return obj_ != nullptr; }
  [[deprecated("the lock is held for the lifetime of this object; there is nothing to lock manually")]]
  Mutex* mutex() { return mutex_; }
  const T& operator*() const { return *obj_; }
  const T* operator->() const { return obj_; }

private:
  const T* obj_ = nullptr;
  Mutex* mutex_ = nullptr;
};

/**
 * @brief Mutable pointer that holds the mutex for its lifetime.
 */
template <typename T>
class MutablePtr
{
public:
  MutablePtr(T* obj, Mutex* mutex);
  MutablePtr(const MutablePtr&) = delete;
  MutablePtr& operator=(const MutablePtr&) = delete;
  MutablePtr(MutablePtr&&) noexcept;
  MutablePtr& operator=(MutablePtr&&) noexcept;
  ~MutablePtr();

  explicit operator bool() const { return obj_ != nullptr; }
  [[deprecated("the lock is held for the lifetime of this object; there is nothing to lock manually")]]
  Mutex* mutex() { return mutex_; }
  T& operator*() { return *obj_; }
  T* operator->() { return obj_; }

private:
  T* obj_ = nullptr;
  Mutex* mutex_ = nullptr;
};

/**
 * @brief Write-back proxy for an atomic scalar: holds a local copy, and the
 * destructor stores it back with one atomic store. No lock is involved, so
 * two overlapping proxies on the same value are last-writer-wins.
 */
template <typename T>
class AtomicProxy
{
public:
  explicit AtomicProxy(std::atomic<T>* target)
    : target_(target), copy_(target ? target->load(std::memory_order_relaxed) : T{})
  {}
  AtomicProxy(const AtomicProxy&) = delete;
  AtomicProxy& operator=(const AtomicProxy&) = delete;
  AtomicProxy(AtomicProxy&& other) noexcept : target_(other.target_), copy_(other.copy_)
  {
    other.target_ = nullptr;
  }
  AtomicProxy& operator=(AtomicProxy&& other) noexcept
  {
    if(this != &other)
    {
      commit();
      target_ = other.target_;
      copy_ = other.copy_;
      other.target_ = nullptr;
    }
    return *this;
  }
  ~AtomicProxy() { commit(); }

  explicit operator bool() const { return target_ != nullptr; }
  Mutex* mutex() { return nullptr; }
  T& operator*() { return copy_; }
  T* operator->() { return &copy_; }

private:
  void commit()
  {
    if(target_)
    {
      target_->store(copy_, std::memory_order_relaxed);
    }
  }
  std::atomic<T>* target_;
  T copy_;
};

/// Read-only counterpart of AtomicProxy: a copy taken at construction.
template <typename T>
class AtomicConstProxy
{
public:
  explicit AtomicConstProxy(const std::atomic<T>* target)
    : valid_(target != nullptr), copy_(target ? target->load(std::memory_order_relaxed) : T{})
  {}
  explicit operator bool() const { return valid_; }
  Mutex* mutex() { return nullptr; }
  const T& operator*() const { return copy_; }
  const T* operator->() const { return &copy_; }

private:
  bool valid_;
  T copy_;
};

//----------------------------------------------------

template <typename T>
inline ConstPtr<T>::ConstPtr(const T* obj, Mutex* mutex) : obj_(obj), mutex_(mutex)
{
  if(mutex_)
  {
    mutex_->lock();
  }
}

template <typename T>
inline ConstPtr<T>::ConstPtr(ConstPtr&& other) noexcept : obj_(other.obj_), mutex_(other.mutex_)
{
  other.obj_ = nullptr;
  other.mutex_ = nullptr;
}

template <typename T>
inline ConstPtr<T>& ConstPtr<T>::operator=(ConstPtr&& other) noexcept
{
  if(this != &other)
  {
    if(mutex_)
    {
      mutex_->unlock();
    }
    obj_ = other.obj_;
    mutex_ = other.mutex_;
    other.obj_ = nullptr;
    other.mutex_ = nullptr;
  }
  return *this;
}

template <typename T>
inline ConstPtr<T>::~ConstPtr()
{
  if(mutex_)
  {
    mutex_->unlock();
  }
}

template <typename T>
inline MutablePtr<T>::MutablePtr(T* obj, Mutex* mutex) : obj_(obj), mutex_(mutex)
{
  if(mutex_)
  {
    mutex_->lock();
  }
}

template <typename T>
inline MutablePtr<T>::MutablePtr(MutablePtr&& other) noexcept : obj_(other.obj_), mutex_(other.mutex_)
{
  other.obj_ = nullptr;
  other.mutex_ = nullptr;
}

template <typename T>
inline MutablePtr<T>& MutablePtr<T>::operator=(MutablePtr&& other) noexcept
{
  if(this != &other)
  {
    if(mutex_)
    {
      mutex_->unlock();
    }
    obj_ = other.obj_;
    mutex_ = other.mutex_;
    other.obj_ = nullptr;
    other.mutex_ = nullptr;
  }
  return *this;
}

template <typename T>
inline MutablePtr<T>::~MutablePtr()
{
  if(mutex_)
  {
    mutex_->unlock();
  }
}

}  // namespace DataTamer
```

Behaviour notes: `ConstPtr` used to take a *shared* lock; with an exclusive `WriteMutex` it now excludes other readers too — required, since the snapshot thread must not serialize while a `ConstPtr` guarantees a stable value. The old move constructor of `ConstPtr` did not null the source's `mutex_`, so a moved-from `ConstPtr` unlocked the mutex a second time in its destructor; fixed here. `operator bool` is now `explicit` (the old implicit one allowed `int x = ptr;`); the in-tree uses are all `if(auto p = ...)`, which still compile.

- [x] **Step 4: Fix the one external `Mutex` user**

In `include/data_tamer/sinks/ros2_publisher_sink.hpp`, change `Mutex schema_mutex_;` to `std::mutex schema_mutex_;` (the file already includes `<unordered_map>`; add `#include <mutex>` next to it). This file is compiled only in the ROS build; the change is by inspection, and the `ros2.yml` CI covers it.

- [x] **Step 5: Run under all presets**

Run: `cmake --build --preset debug && ctest --preset debug -R LockedReference`, then full `asan`, `tsan`, `release`.
Expected: 6 new tests pass; existing `DataTamerBasic.LockedPtr` still passes (it uses `getMutablePtr()` on a `LoggedValue<float>`, unchanged until Task 3); no `-Wdeprecated-declarations` warnings in the library (the deprecated `mutex()` is not called in-tree).

- [x] **Step 6: Commit**

```bash
git add include/data_tamer/details/locked_reference.hpp include/data_tamer/sinks/ros2_publisher_sink.hpp tests/locked_reference_tests.cpp tests/CMakeLists.txt
git commit -m "refactor: Mutex alias is now the priority-inheriting WriteMutex; add atomic proxies

ConstPtr/MutablePtr hold the exclusive transaction lock for their lifetime
(spec §4.2). AtomicProxy/AtomicConstProxy are the write-back proxies scalar
LoggedValues will return. The ROS2 sink's schema mutex becomes a plain
std::mutex. Fixes a double-unlock in ConstPtr's move constructor.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Scalar `LoggedValue<T>` on `std::atomic<T>` with `ChannelSharedState`

**Files:**
- Modify: `include/data_tamer/logged_value.hpp` (whole file)
- Modify: `include/data_tamer/channel.hpp` — add `registerValue(const std::atomic<T>*)`, `sharedState()`; rewrite the `LoggedValue` member definitions at the bottom of the file
- Modify: `src/channel.cpp` — `Pimpl` gains `std::shared_ptr<ChannelSharedState> shared`; `registerValueImpl` calls `shared->addSeries()` for new series; `setEnabled` forwards to the shared state; `takeSnapshot` reads flags from the shared state (mask rebuild) — the old `mutex` still guards everything else
- Modify: `tests/logged_value_tests.cpp` (extend), `tests/dt_tests.cpp` (`LockedPtr` test expectations)
- Modify: `examples/T01_basic_example.cpp` comments (accessor semantics)

**Interfaces:**
- Consumes: `ChannelSharedState` (Task 1), `AtomicProxy`/`AtomicConstProxy`/`MutablePtr`/`ConstPtr` (Task 2), `ValuePtr(const std::atomic<T>*)` (Plan 1).
- Produces:
  ```cpp
  template <typename T> inline constexpr bool is_atomic_scalar_v = IsNumericType<T>() && sizeof(T) <= 8 && std::atomic<T>::is_always_lock_free;
  template <typename T, std::enable_if_t<IsNumericType<T>(), bool> = true>
  RegistrationID LogChannel::registerValue(const std::string& name, const std::atomic<T>* value);
  std::shared_ptr<ChannelSharedState> LogChannel::sharedState() const;
  // LoggedValue<T>: set(), get() const, setEnabled(), isEnabled() const,
  //   getMutablePtr() -> AtomicProxy<T> (scalar) | MutablePtr<T> (other)
  //   getConstPtr()   -> AtomicConstProxy<T> (scalar) | ConstPtr<T> (other)
  ```
  Task 5 relies on `sharedState()` and on non-scalar `LoggedValue`s using `state_->write_mutex`.

- [x] **Step 1: Write the failing tests**

Replace `tests/logged_value_tests.cpp` with:

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

using namespace DataTamer;
using DataTamerTest::AllocCounter;

TEST(LoggedValue, IsNotMovableOrCopyable)
{
  static_assert(!std::is_copy_constructible_v<LoggedValue<double>>);
  static_assert(!std::is_copy_assignable_v<LoggedValue<double>>);
  static_assert(!std::is_move_constructible_v<LoggedValue<double>>,
                "moving a LoggedValue would leave the channel with a dangling pointer");
  static_assert(!std::is_move_assignable_v<LoggedValue<double>>);
}

TEST(LoggedValue, SharedPtrHandleStillWorks)
{
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<float>("f", 1.0f);
  std::shared_ptr<LoggedValue<float>> moved = std::move(v);
  ASSERT_FALSE(v);
  ASSERT_EQ(moved->get(), 1.0f);
}

TEST(LoggedValue, ScalarTraitSelectsAtomics)
{
  static_assert(is_atomic_scalar_v<double>);
  static_assert(is_atomic_scalar_v<int8_t>);
  static_assert(is_atomic_scalar_v<bool>);
  enum Color : uint8_t { RED };
  static_assert(is_atomic_scalar_v<Color>);
  static_assert(!is_atomic_scalar_v<std::vector<double>>);
  struct Big { double a, b; };
  static_assert(!is_atomic_scalar_v<Big>);
}

TEST(LoggedValue, ScalarSetIsSeenBySnapshotAndDoesNotAllocate)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<double>("v", 1.0);
  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));

  {
    AllocCounter::Scope scope;
    for(int i = 0; i < 1000; i++)
    {
      v->set(double(i));
    }
    ASSERT_EQ(scope.allocations(), 0u);
  }
  ASSERT_EQ(v->get(), 999.0);

  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto snap = sink->latestSnapshot();
  ASSERT_EQ(snap.payload.size(), sizeof(double));
  double stored = 0;
  std::memcpy(&stored, snap.payload.data(), sizeof(double));
  ASSERT_EQ(stored, 999.0);
}

TEST(LoggedValue, ScalarProxiesWriteBackAndHoldCopies)
{
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<float>("f", 3.0f);
  {
    auto p = v->getMutablePtr();
    ASSERT_TRUE(p);
    *p += 1.0f;
    ASSERT_EQ(v->get(), 3.0f);  // write-back happens on destruction
  }
  ASSERT_EQ(v->get(), 4.0f);
  auto c = v->getConstPtr();
  v->set(5.0f);
  ASSERT_EQ(*c, 4.0f);  // copy taken at construction
}

TEST(LoggedValue, SetEnabledFromWriterThreadNeedsNoChannel)
{
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<int32_t>("i", 1);
  channel.reset();  // channel gone; the LoggedValue must still be usable
  v->set(2);
  v->setEnabled(false);
  ASSERT_FALSE(v->isEnabled());
  v->set(3, /*auto_enable=*/true);
  ASSERT_TRUE(v->isEnabled());
  ASSERT_EQ(v->get(), 3);
}

TEST(LoggedValue, AutoEnableOnSetDirtiesMask)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<double>("v", 1.0);
  v->setEnabled(false);
  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(sink->latestPayloadSize(), 0u);

  v->set(2.0);  // auto_enable defaults to true
  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(sink->latestPayloadSize(), sizeof(double));
}

// The race that existed before this plan: a writer thread hammering set()
// while the snapshot thread serializes. Must be clean under TSAN, and every
// snapshot must decode to a value the writer actually wrote.
TEST(LoggedValue, ScalarWriterRacesSnapshotCleanly)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<uint64_t>("v", 0);
  channel->takeSnapshot();

  std::atomic_bool stop{ false };
  std::thread writer([&] {
    uint64_t x = 0;
    while(!stop)
    {
      v->set(++x * 0x0101010101010101ULL);  // every byte identical: a torn read shows up
    }
  });
  for(int i = 0; i < 2000; i++)
  {
    channel->takeSnapshot();
  }
  stop = true;
  writer.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto snap = sink->latestSnapshot();
  ASSERT_EQ(snap.payload.size(), sizeof(uint64_t));
  uint64_t seen = 0;
  std::memcpy(&seen, snap.payload.data(), sizeof(seen));
  for(int b = 1; b < 8; b++)
  {
    ASSERT_EQ((seen >> (8 * b)) & 0xFF, seen & 0xFF) << "torn value " << std::hex << seen;
  }
}

TEST(LoggedValue, NonScalarStillWorksThroughMutablePtr)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<std::vector<double>>("vec", { 1.0, 2.0 });
  {
    auto p = v->getMutablePtr();
    p->push_back(3.0);
  }
  ASSERT_EQ(v->get().size(), 3u);
  channel->takeSnapshot();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(sink->latestPayloadSize(), sizeof(uint32_t) + 3 * sizeof(double));
}
```

Also update `tests/dt_tests.cpp` `DataTamerBasic.LockedPtr`: after the block that assigns `*ptr = val2;`, the comment "we should be able to get it again now that ptr is out of scope" and `EXPECT_TRUE(logged_float->getMutablePtr());` stay valid; no change needed except adding `#include <cstring>` if missing — the test compiles unchanged because `AtomicProxy<float>` has the same `operator bool`/`operator*`.

- [x] **Step 2: Run to see the failures**

Run: `cmake --build --preset debug 2>&1 | grep -E "error" | head -8`
Expected: FAIL — `is_atomic_scalar_v` not declared; `createLoggedValue<std::vector<double>>` initializer-list argument fails only if the constructor signature is wrong (it takes `T initial_value`, so `{1.0, 2.0}` converts); `channel.reset()` then `setEnabled` compiles but the current implementation silently does nothing (the `AutoEnable`/`NeedsNoChannel` tests fail at runtime).

- [x] **Step 3: Rewrite `logged_value.hpp`**

```cpp
#pragma once

#include "data_tamer/types.hpp"
#include "data_tamer/details/locked_reference.hpp"
#include "data_tamer/details/shared_state.hpp"

#include <atomic>
#include <memory>
#include <type_traits>

namespace DataTamer
{

class LogChannel;

/// Scalars small enough for a lock-free std::atomic: stored atomically, so
/// set()/get() are single relaxed stores/loads.
template <typename T>
inline constexpr bool is_atomic_scalar_v =
    IsNumericType<T>() && sizeof(T) <= 8 && std::atomic<T>::is_always_lock_free;

/**
 * @brief The LoggedValue class is a container of a variable that
 * automatically register/unregister to a Channel when created/destroyed.
 *
 * Scalars (arithmetic types, bool, char, small enums) are stored in a
 * std::atomic: set() and get() are wait-free and never take a lock. Other
 * types are written under the channel's transaction mutex (see
 * LogChannel::scopedWrite()).
 *
 * Consistency between several values is opt-in: a lone set() promises only
 * that the value itself is never torn. Values that must be captured together
 * belong in one struct, or inside a LogChannel::scopedWrite() transaction.
 */
template <typename T>
class LoggedValue
{
protected:
  LoggedValue(const std::shared_ptr<LogChannel>& channel, const std::string& name,
              T initial_value);

  friend LogChannel;

public:
  static constexpr bool kAtomic = is_atomic_scalar_v<T>;
  using Storage = std::conditional_t<kAtomic, std::atomic<T>, T>;
  using MutableProxy = std::conditional_t<kAtomic, AtomicProxy<T>, MutablePtr<T>>;
  using ConstProxy = std::conditional_t<kAtomic, AtomicConstProxy<T>, ConstPtr<T>>;

  ~LoggedValue();

  LoggedValue(LoggedValue const& other) = delete;
  LoggedValue& operator=(LoggedValue const& other) = delete;

  // The channel holds a pointer to value_; moving would leave it dangling.
  LoggedValue(LoggedValue&& other) = delete;
  LoggedValue& operator=(LoggedValue&& other) = delete;

  /**
   * @brief set the value of the variable. Wait-free for scalars; takes the
   * channel's transaction mutex for other types (unless the calling thread
   * already holds it through scopedWrite()).
   *
   * @param value        new value
   * @param auto_enable  if true and the value is disabled, enable it
   */
  void set(const T& value, bool auto_enable = true);

  /// @brief get the stored value (a copy).
  [[nodiscard]] T get() const;

  [[deprecated("use getMutablePtr() instead")]] [[nodiscard]] MutableProxy getLockedPtr()
  {
    return getMutablePtr();
  }

  /**
   * Read/write access. For scalars this is a write-back proxy: edits become
   * visible when the proxy is destroyed, and two overlapping proxies are
   * last-writer-wins — prefer set(). For other types the proxy holds the
   * transaction mutex for its lifetime, blocking the snapshot thread: keep it
   * short and allocation-free.
   */
  [[nodiscard]] MutableProxy getMutablePtr();

  /// Read-only access. For scalars: a copy taken now. For other types: the
  /// transaction mutex is held for the proxy's lifetime.
  [[nodiscard]] ConstProxy getConstPtr();

  /// @brief Disabling a LoggedValue means that we will not record it in the snapshot.
  /// Wait-free; callable from any thread, even after the channel is destroyed.
  void setEnabled(bool enabled);

  [[nodiscard]] bool isEnabled() const;

private:
  std::shared_ptr<ChannelSharedState> state_;
  std::weak_ptr<LogChannel> channel_;  // destructor only
  Storage value_;
  RegistrationID id_;
};

}  // namespace DataTamer
```

- [x] **Step 4: Add `registerValue(const std::atomic<T>*)` and `sharedState()` to `LogChannel`**

In `include/data_tamer/channel.hpp`, after the `registerValue(const std::string&, const T*)` declaration (line 78), add:

```cpp
  /**
   * @brief registerValue for an atomic scalar. The value is read with a
   * relaxed load when the snapshot is taken; it serializes exactly like T.
   */
  template <typename T, std::enable_if_t<IsNumericType<T>(), bool> = true>
  RegistrationID registerValue(const std::string& name, const std::atomic<T>* value);
```

after `Mutex& writeMutex();` add:

```cpp
  /// State shared with this channel's LoggedValues (enable flags, write mutex).
  [[nodiscard]] std::shared_ptr<ChannelSharedState> sharedState() const;
```

and `#include "data_tamer/details/shared_state.hpp"` at the top. Add the definition next to the other `registerValue` definitions:

```cpp
template <typename T, std::enable_if_t<IsNumericType<T>(), bool>>
inline RegistrationID LogChannel::registerValue(const std::string& name,
                                                const std::atomic<T>* value_ptr)
{
  return registerValueImpl(name, ValuePtr(value_ptr), {});
}
```

Replace the `LoggedValue` member definitions at the bottom of `channel.hpp` (from `template <typename T> inline LoggedValue<T>::LoggedValue(...)` through the end of `getConstPtr`) with:

```cpp
template <typename T>
inline LoggedValue<T>::LoggedValue(const std::shared_ptr<LogChannel>& channel,
                                   const std::string& name, T initial_value)
  : state_(channel->sharedState())
  , channel_(channel)
  , value_(initial_value)
  , id_(channel->registerValue(name, &value_))
{}

template <typename T>
inline LoggedValue<T>::~LoggedValue()
{
  if(auto channel = channel_.lock())
  {
    channel->unregister(id_);
  }
}

template <typename T>
inline void LoggedValue<T>::setEnabled(bool enabled)
{
  state_->setEnabled(id_, enabled);
}

template <typename T>
inline bool LoggedValue<T>::isEnabled() const
{
  return state_->isEnabled(id_.first_index);
}

template <typename T>
inline void LoggedValue<T>::set(const T& val, bool auto_enable)
{
  if constexpr(kAtomic)
  {
    value_.store(val, std::memory_order_relaxed);
  }
  else
  {
    std::lock_guard<WriteMutex> lk(state_->write_mutex);
    value_ = val;
  }
  if(auto_enable && !isEnabled())
  {
    setEnabled(true);
  }
}

template <typename T>
inline T LoggedValue<T>::get() const
{
  if constexpr(kAtomic)
  {
    return value_.load(std::memory_order_relaxed);
  }
  else
  {
    std::lock_guard<WriteMutex> lk(state_->write_mutex);
    return value_;
  }
}

template <typename T>
inline typename LoggedValue<T>::MutableProxy LoggedValue<T>::getMutablePtr()
{
  if constexpr(kAtomic)
  {
    return AtomicProxy<T>(&value_);
  }
  else
  {
    return MutablePtr<T>(&value_, &state_->write_mutex);
  }
}

template <typename T>
inline typename LoggedValue<T>::ConstProxy LoggedValue<T>::getConstPtr()
{
  if constexpr(kAtomic)
  {
    return AtomicConstProxy<T>(&value_);
  }
  else
  {
    return ConstPtr<T>(&value_, &state_->write_mutex);
  }
}
```

Note on the non-scalar `set()`: Task 5 adds the "already inside a transaction on this thread" check; until then a `set()` inside `scopedWrite()` would deadlock, and no in-tree code does that yet.

- [x] **Step 5: Wire the shared state into `LogChannel::Pimpl`**

In `src/channel.cpp`:

- `Pimpl` gains `std::shared_ptr<ChannelSharedState> shared = std::make_shared<ChannelSharedState>();` and loses `bool mask_dirty` and `ValueHolder::enabled` (the flag now lives in `shared`).
- `registerValueImpl`: for a new series, call `_p->shared->addSeries();` right after `_p->series.emplace_back(...)`; on re-registration replace `instance.enabled = true;` with `_p->shared->setEnabled(index, true);`. Replace `_p->mask_dirty = true;` at the top with `_p->shared->mask_dirty.store(true, std::memory_order_release);`.
- `setEnabled(id, enable)`: body becomes `_p->shared->setEnabled(id, enable);` — **no lock**.
- `unregister(id)`: keep the lock (structure), set `registered = false`, and call `_p->shared->setEnabled(id, false);`.
- `sharedState()`: `return _p->shared;`.
- `takeSnapshot()`: the mask rebuild becomes

  ```cpp
    if(_p->shared->mask_dirty.exchange(false, std::memory_order_acq_rel))
    {
      auto& mask = _p->snapshot.active_mask;
      mask.assign((_p->series.size() + 7) / 8, 0xFF);
      for(size_t i = 0; i < _p->series.size(); i++)
      {
        if(!_p->shared->isEnabled(i))
        {
          SetBit(mask, i, false);
        }
      }
    }
  ```

  and the serialize loop tests the mask instead of `entry.enabled`:

  ```cpp
    for(size_t i = 0; i < _p->series.size(); i++)
    {
      if(GetBit(_p->snapshot.active_mask, i))
      {
        _p->series[i].holder.serialize(payload_buffer);
      }
    }
  ```

  The size pass stays over all series (unchanged behaviour: payload is trimmed after serialization). `_p->mutex` still wraps the whole block, as today.

- [x] **Step 6: Update the T01 example comments**

In `examples/T01_basic_example.cpp`, replace the two comment blocks around `getMutablePtr()` / `getConstPtr()` (lines 36–44) with:

```cpp
  // For a scalar, getMutablePtr() returns a write-back proxy: the new value
  // is stored (atomically) when ptr goes out of scope. Prefer set() for
  // single assignments.
  if(auto ptr = logged_float->getMutablePtr())
  {
    *ptr += 1.1f;
  }

  // getConstPtr() returns a copy taken now; nothing is locked.
```

- [x] **Step 7: Run under all presets**

Run: `cmake --build --preset debug && ctest --preset debug -R "LoggedValue|DataTamerBasic"`, then full `asan`, `tsan`, `release`.
Expected: 10 `LoggedValue.*` tests pass; `ScalarWriterRacesSnapshotCleanly` is TSAN-clean (this is the race fix); every pre-existing test passes unchanged (payload sizes and masks identical).

- [x] **Step 8: Measure**

```bash
cmake --build --preset release
taskset -c 0-5 ./build/release/benchmarks/data_tamer_benchmark --benchmark_filter='DT_LoggedValueSet|DT_SnapshotWithWriter' --benchmark_min_time=0.5s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
taskset -c 0-5 ./build/release/benchmarks/rt_latency --values 1000 --sinks 1 --writers 2 --seconds 10
```

Expected: `DT_LoggedValueSet` items/s up by roughly an order of magnitude versus the baseline (34 M/s); the writer-contention harness p99 no longer shows lock convoying. Paste both outputs into the commit body.

- [x] **Step 9: Commit**

```bash
git add include/data_tamer/logged_value.hpp include/data_tamer/channel.hpp src/channel.cpp tests/logged_value_tests.cpp tests/dt_tests.cpp examples/T01_basic_example.cpp
git commit -m "feat: scalar LoggedValue on std::atomic; enable flags in ChannelSharedState

set()/get() on scalars are single relaxed stores/loads; setEnabled() is
wait-free and works without the channel. Fixes the pre-existing data race
between LoggedValue::set() and takeSnapshot() (spec §4.1, §4.4, §5.2).

Benchmarks (release, pinned):
<paste>

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Deprecate scalar accessors, `get()` const, CHANGELOG-visible API delta

**Files:**
- Modify: `include/data_tamer/logged_value.hpp` (attributes on `getMutablePtr`/`getConstPtr` for scalar `T`)
- Modify: `tests/logged_value_tests.cpp` (compile-time check that the deprecation is scalar-only)

**Interfaces:**
- Produces: `[[deprecated]]` on `getMutablePtr()`/`getConstPtr()` when `kAtomic` is true (spec §4.1); non-scalar overloads undeprecated.

- [x] **Step 1: Write the failing test**

Append to `tests/logged_value_tests.cpp`:

```cpp
// The deprecation must be scalar-only: a non-scalar getMutablePtr() is the
// right tool for in-place edits and must not warn. We compile this file with
// -Werror=deprecated-declarations disabled locally only around the scalar
// call (see the pragma), so an accidental deprecation on the vector overload
// would fail the build.
TEST(LoggedValue, DeprecationIsScalarOnly)
{
  auto channel = LogChannel::create("chan");
  auto vec = channel->createLoggedValue<std::vector<int>>("vec");
  auto p = vec->getMutablePtr();  // must NOT be deprecated
  p->push_back(1);
}
```

and wrap the existing scalar `getMutablePtr()`/`getConstPtr()` calls in `ScalarProxiesWriteBackAndHoldCopies` with:

```cpp
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  ...existing body...
#pragma GCC diagnostic pop
```

Also add `-Werror=deprecated-declarations` for the test target: in `tests/CMakeLists.txt`, after `add_executable(datatamer_test ...)`, add `target_compile_options(datatamer_test PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror=deprecated-declarations>)`.

- [x] **Step 2: Run to see the failure**

Run: `cmake --preset debug && cmake --build --preset debug 2>&1 | grep -E "deprecated|error" | head`
Expected: builds (nothing is deprecated yet) — the RED here is that `dt_tests.cpp`'s `LockedPtr` test and `T01` will fail to build once the attribute is added; Step 3 adds it and Step 4 fixes those call sites, which is the point of the `-Werror`.

- [x] **Step 3: Add the attributes**

In `logged_value.hpp`, split each accessor into two SFINAE overloads:

```cpp
  template <typename U = T, std::enable_if_t<is_atomic_scalar_v<U>, bool> = true>
  [[deprecated("for scalar values use set()/get(); the returned proxy writes back on destruction")]]
  [[nodiscard]] AtomicProxy<T> getMutablePtr();

  template <typename U = T, std::enable_if_t<!is_atomic_scalar_v<U>, bool> = true>
  [[nodiscard]] MutablePtr<T> getMutablePtr();

  template <typename U = T, std::enable_if_t<is_atomic_scalar_v<U>, bool> = true>
  [[deprecated("for scalar values use get(); the returned proxy holds a copy")]]
  [[nodiscard]] AtomicConstProxy<T> getConstPtr();

  template <typename U = T, std::enable_if_t<!is_atomic_scalar_v<U>, bool> = true>
  [[nodiscard]] ConstPtr<T> getConstPtr();
```

and move the four bodies from `channel.hpp` into matching out-of-class definitions (the `if constexpr` versions from Task 3 are replaced by these four). `getLockedPtr()` calls `getMutablePtr()` and inherits the deprecation for scalars, which is fine (it is already deprecated).

- [x] **Step 4: Fix in-tree scalar call sites**

- `tests/dt_tests.cpp` `DataTamerBasic.LockedPtr`: wrap the body in the same `#pragma GCC diagnostic push/ignored/pop` as above (the test documents the proxy semantics and stays).
- `examples/T01_basic_example.cpp`: replace the `getMutablePtr()` block with `logged_float->set(logged_float->get() + 1.1f);` and the `getConstPtr()` block with `std::cout << "logged_float = " << logged_float->get() << "\n";`, updating the comments accordingly.

- [x] **Step 5: Run under all presets and commit**

Run: full `debug`, `asan`, `tsan`, `release`. Expected: green; no deprecation warnings anywhere in the build log (`cmake --build --preset debug 2>&1 | grep -c deprecated` prints `0`).

```bash
git add include/data_tamer/logged_value.hpp include/data_tamer/channel.hpp tests/logged_value_tests.cpp tests/dt_tests.cpp tests/CMakeLists.txt examples/T01_basic_example.cpp
git commit -m "api: deprecate getMutablePtr()/getConstPtr() on scalar LoggedValues

They now return write-back / copy proxies, not locked references; set()
and get() express the same thing without the surprise (spec §4.1, §8).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Transactions — `scopedWrite()`, `takeSnapshot` serializes under the `WriteMutex`, contention counters

**Files:**
- Modify: `include/data_tamer/channel.hpp` — `scopedWrite()`, counters, `Stats`
- Modify: `src/channel.cpp` — `writeMutex()` returns `shared->write_mutex`; `takeSnapshot` locked section; counters
- Modify: `include/data_tamer/logged_value.hpp` / `channel.hpp` — non-scalar `set()`/`get()` skip the lock when the thread already holds it
- Modify: `include/data_tamer/details/shared_state.hpp` — transaction depth tracking
- Create: `tests/transaction_tests.cpp`
- Modify: `benchmarks/data_tamer_benchmark.cpp`, `benchmarks/rt_latency.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `WriteMutex::lockWithSpin()` (Plan 1), `ChannelSharedState` (Task 1).
- Produces:
  ```cpp
  class LogChannel {
    [[nodiscard]] std::lock_guard<WriteMutex> scopedWrite();   // via a Transaction RAII type, see below
    uint64_t writeLockContended() const;
    uint64_t writeLockWaitMaxNs() const;
    struct Stats { uint64_t write_lock_contended; uint64_t write_lock_wait_max_ns; };
    Stats stats() const;
  };
  class ChannelSharedState {
    class Transaction;                // RAII: enters the mutex unless this thread is already inside one
    bool inTransactionOnThisThread() const;
  };
  ```
  Plans 3–4 add more fields to `Stats`.

Why a depth counter rather than a recursive mutex: `PTHREAD_PRIO_INHERIT` combines with `PTHREAD_MUTEX_RECURSIVE`, but a recursive mutex hides accidental nesting from every caller; a thread-local depth makes "am I already in a transaction" an explicit, testable question and keeps the mutex non-recursive.

- [x] **Step 1: Write the failing tests**

`tests/transaction_tests.cpp`:

```cpp
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace DataTamer;

namespace
{
struct Pair
{
  double a = 0;
  double b = 0;
};
template <typename AddField>
std::string_view TypeDefinition(Pair& p, AddField& add)
{
  add("a", &p.a);
  add("b", &p.b);
  return "Pair";
}

// decode two doubles at the start of a payload
std::pair<double, double> firstTwo(const Snapshot& s)
{
  double a = 0, b = 0;
  std::memcpy(&a, s.payload.data(), sizeof(double));
  std::memcpy(&b, s.payload.data() + sizeof(double), sizeof(double));
  return { a, b };
}
}  // namespace

TEST(Transaction, ScopedWriteIsTheWriteMutex)
{
  auto channel = LogChannel::create("chan");
  {
    auto tx = channel->scopedWrite();
    ASSERT_FALSE(channel->writeMutex().try_lock());
  }
  ASSERT_TRUE(channel->writeMutex().try_lock());
  channel->writeMutex().unlock();
}

TEST(Transaction, NestedSetInsideScopedWriteDoesNotDeadlock)
{
  auto channel = LogChannel::create("chan");
  auto vec = channel->createLoggedValue<std::vector<double>>("vec");
  auto scalar = channel->createLoggedValue<double>("s");
  {
    auto tx = channel->scopedWrite();
    vec->set({ 1.0, 2.0 });  // would deadlock on a non-recursive mutex without the depth check
    scalar->set(3.0);
    ASSERT_EQ(vec->get().size(), 2u);
  }
  ASSERT_EQ(scalar->get(), 3.0);
}

// Two scalars written inside one transaction must land in the same snapshot:
// a writer thread flips (a, b) between (1,1) and (2,2); every snapshot must
// decode as a == b.
TEST(Transaction, ValuesWrittenTogetherAppearTogether)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto a = channel->createLoggedValue<double>("a", 1.0);
  auto b = channel->createLoggedValue<double>("b", 1.0);
  channel->takeSnapshot();

  std::atomic_bool stop{ false };
  std::atomic<int> mixed{ 0 };
  std::thread writer([&] {
    double v = 1.0;
    while(!stop)
    {
      auto tx = channel->scopedWrite();
      a->set(v);
      std::this_thread::yield();  // widen the window
      b->set(v);
      v = (v == 1.0) ? 2.0 : 1.0;
    }
  });
  std::thread checker([&] {
    // decode every delivered snapshot by polling the latest one
    for(int i = 0; i < 20000; i++)
    {
      const auto s = sink->latestSnapshot();
      if(s.payload.size() == 2 * sizeof(double))
      {
        auto [x, y] = firstTwo(s);
        if(x != y)
        {
          mixed++;
        }
      }
    }
  });
  for(int i = 0; i < 5000; i++)
  {
    channel->takeSnapshot();
  }
  stop = true;
  writer.join();
  checker.join();
  ASSERT_EQ(mixed.load(), 0);
}

TEST(Transaction, RawPointerUnderWriteMutexIsConsistentToo)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  Pair p;
  channel->registerValue("p", &p);
  channel->takeSnapshot();

  std::atomic_bool stop{ false };
  std::thread writer([&] {
    double v = 1.0;
    while(!stop)
    {
      std::lock_guard<Mutex> lk(channel->writeMutex());  // legacy spelling
      p.a = v;
      std::this_thread::yield();
      p.b = v;
      v = (v == 1.0) ? 2.0 : 1.0;
    }
  });
  int mixed = 0;
  for(int i = 0; i < 5000; i++)
  {
    channel->takeSnapshot();
    const auto s = sink->latestSnapshot();
    if(s.payload.size() == 2 * sizeof(double))
    {
      auto [x, y] = firstTwo(s);
      mixed += (x != y);
    }
  }
  stop = true;
  writer.join();
  ASSERT_EQ(mixed, 0);
}

TEST(Transaction, ContentionIsCountedAndBounded)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);
  auto v = channel->createLoggedValue<double>("v");
  channel->takeSnapshot();
  ASSERT_EQ(channel->writeLockContended(), 0u);

  // hold the transaction for 200 us across a snapshot
  std::atomic_bool held{ false };
  std::thread writer([&] {
    auto tx = channel->scopedWrite();
    held = true;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  });
  while(!held)
  {
  }
  channel->takeSnapshot();
  writer.join();
  ASSERT_EQ(channel->writeLockContended(), 1u);
  ASSERT_GT(channel->writeLockWaitMaxNs(), 0u);
  ASSERT_LT(channel->writeLockWaitMaxNs(), 50'000'000u);  // well under a scheduler quantum
  const auto st = channel->stats();
  ASSERT_EQ(st.write_lock_contended, 1u);
}

TEST(Transaction, LoneScalarSetDoesNotTakeTheMutex)
{
  auto channel = LogChannel::create("chan");
  auto v = channel->createLoggedValue<double>("v");
  channel->writeMutex().lock();  // if set() tried to lock, this would deadlock
  v->set(1.0);
  ASSERT_EQ(v->get(), 1.0);
  channel->writeMutex().unlock();
}
```

- [x] **Step 2: Register and run to see the failures**

Append `transaction_tests.cpp` to `DATATAMER_TEST_SOURCES`.

Run: `cmake --build --preset debug 2>&1 | grep -E "error" | head -5`
Expected: FAIL — no `scopedWrite`, `writeLockContended`, `stats`.

- [x] **Step 3: Transaction depth in `ChannelSharedState`**

Add to `shared_state.hpp`, inside `ChannelSharedState`:

```cpp
  /// RAII transaction: takes write_mutex unless this thread is already inside
  /// a transaction on this state (nested set() calls, or set() inside
  /// scopedWrite()), in which case it is a no-op.
  class Transaction
  {
  public:
    explicit Transaction(ChannelSharedState& state) : state_(&state)
    {
      if(currentDepth() == 0)
      {
        state_->write_mutex.lock();
        owns_ = true;
      }
      ++currentDepth();
    }
    ~Transaction()
    {
      if(state_ == nullptr)
      {
        return;  // moved-from
      }
      --currentDepth();
      if(owns_)
      {
        state_->write_mutex.unlock();
      }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    // Movable so that LogChannel::scopedWrite() can return one by value.
    Transaction(Transaction&& other) noexcept : state_(other.state_), owns_(other.owns_)
    {
      other.state_ = nullptr;
    }
    Transaction& operator=(Transaction&&) = delete;

  private:
    // depth is per (thread, state): a thread-local map would allocate, so we
    // key on the state pointer in a small fixed table (a thread is inside at
    // most a handful of channels' transactions at once).
    int& currentDepth() { return state_->depthFor(); }
    ChannelSharedState* state_;
    bool owns_ = false;
  };

  bool inTransactionOnThisThread() { return depthFor() > 0; }
```

and the private helper:

```cpp
private:
  struct DepthEntry { const ChannelSharedState* state = nullptr; int depth = 0; };
  static constexpr size_t kMaxNested = 8;
  int& depthFor()
  {
    thread_local DepthEntry table[kMaxNested];
    for(auto& e : table)
    {
      if(e.state == this) return e.depth;
    }
    for(auto& e : table)
    {
      if(e.state == nullptr || e.depth == 0) { e.state = this; e.depth = 0; return e.depth; }
    }
    // more than kMaxNested channels in nested transactions on one thread: fall
    // back to the last slot (still correct for lock/unlock pairing of this
    // transaction, only nesting detection across >8 states degrades)
    return table[kMaxNested - 1].depth;
  }
```

- [x] **Step 4: Non-scalar `set()`/`get()` use `Transaction`**

In `channel.hpp`, the non-scalar branches of `LoggedValue<T>::set` and `get` become `ChannelSharedState::Transaction tx(*state_);` instead of `std::lock_guard<WriteMutex>`.

- [x] **Step 5: `scopedWrite()`, `writeMutex()`, counters, locked serialization in `LogChannel`**

`channel.hpp` — declarations:

```cpp
  /// Start a transaction: every write to this channel's values until the
  /// returned object is destroyed lands in one snapshot or in none.
  [[nodiscard]] ChannelSharedState::Transaction scopedWrite();

  /// Times takeSnapshot() found the write mutex held and had to block after
  /// spinning (a writer preempted mid-transaction, or a long transaction).
  [[nodiscard]] uint64_t writeLockContended() const;
  /// Longest such wait, in nanoseconds (measured only on the contended path).
  [[nodiscard]] uint64_t writeLockWaitMaxNs() const;

  struct Stats
  {
    uint64_t write_lock_contended = 0;
    uint64_t write_lock_wait_max_ns = 0;
  };
  [[nodiscard]] Stats stats() const;
```

`channel.cpp`:

- `Pimpl` gains `std::atomic<uint64_t> write_lock_contended{0}; std::atomic<uint64_t> write_lock_wait_max_ns{0};`.
- `writeMutex()`: `return _p->shared->write_mutex;`.
- `scopedWrite()`: `return ChannelSharedState::Transaction(*_p->shared);` — `Transaction` has the move constructor from Step 3 (a moved-from transaction's destructor is a no-op), so returning it by value is well-defined even where copy elision does not apply.
- counters: trivial relaxed loads.
- `takeSnapshot()`: wrap **only** the size pass + serialization (from `size_t payload_size = 0;` through the final `payload.resize(...)`) in:

  ```cpp
    {
      auto& wm = _p->shared->write_mutex;
      std::chrono::steady_clock::time_point t0{};
      const bool contended = !wm.try_lock();
      if(contended)
      {
        t0 = std::chrono::steady_clock::now();
        wm.lockWithSpin();
      }
      ...size pass, serialize...
      wm.unlock();
      if(contended)
      {
        const auto waited = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - t0).count());
        _p->write_lock_contended.fetch_add(1, std::memory_order_relaxed);
        uint64_t prev = _p->write_lock_wait_max_ns.load(std::memory_order_relaxed);
        while(prev < waited && !_p->write_lock_wait_max_ns.compare_exchange_weak(prev, waited, std::memory_order_relaxed)) {}
      }
    }
  ```

  Note: the first `try_lock()` is a `WriteMutex::try_lock`, and `lockWithSpin()` starts with its own `try_lock()` — the double attempt costs one extra uncontended CAS on the contended path only. `write_lock_wait_max_ns` measures lock acquisition + serialization on the contended path (t0 is before the spin, the end is after unlock); document it as "wait plus serialization, contended snapshots only". The old `_p->mutex` remains around the whole block for structure until Plan 4.

- [x] **Step 6: Benchmarks report the new counters and add a transaction writer**

`benchmarks/rt_latency.cpp`: after the percentiles, print `channel->stats()`:

```cpp
  const auto st = channel->stats();
  std::printf("write_lock_contended=%llu write_lock_wait_max_ns=%llu\n",
              (unsigned long long)st.write_lock_contended, (unsigned long long)st.write_lock_wait_max_ns);
```

and add a `--transactions` flag: when set, each writer thread wraps its `set()` loop iteration in `channel->scopedWrite()` (a multi-value transaction) instead of lone sets.

`benchmarks/data_tamer_benchmark.cpp`: add `DT_SnapshotWithTransactionWriter` — same as `DT_SnapshotWithWriter` but the writer does `auto tx = channel->scopedWrite();` around each batch of 100 `set()`s; register it with `BENCHMARK(DT_SnapshotWithTransactionWriter);`.

- [x] **Step 7: Run under all presets**

Run: `cmake --build --preset debug && ctest --preset debug -R "Transaction|LoggedValue"`, then full `asan`, `tsan`, `release`.
Expected: 6 `Transaction.*` tests pass; `ValuesWrittenTogetherAppearTogether` and `RawPointerUnderWriteMutexIsConsistentToo` are the consistency proofs; TSAN clean.

- [x] **Step 8: Measure**

```bash
cmake --build --preset release
taskset -c 0-5 ./build/release/benchmarks/data_tamer_benchmark --benchmark_filter='DT_SnapshotWith' --benchmark_min_time=0.5s --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
taskset -c 0-5 ./build/release/benchmarks/rt_latency --values 1000 --sinks 1 --writers 2 --transactions --seconds 10
```

Expected: `write_lock_contended` small relative to 10 000 snapshots; `write_lock_wait_max_ns` in the tens of µs. Paste into the commit body.

- [x] **Step 9: Commit**

```bash
git add include/data_tamer/channel.hpp include/data_tamer/logged_value.hpp include/data_tamer/details/shared_state.hpp src/channel.cpp tests/transaction_tests.cpp tests/CMakeLists.txt benchmarks/
git commit -m "feat: scopedWrite() transactions on the priority-inheriting WriteMutex

takeSnapshot() serializes under the same mutex (spin-then-lock), so values
written in one transaction appear together or not at all; contention is
counted and its worst wait recorded (spec §3 step 4, §4.2, §7).

Benchmarks (release, pinned):
<paste>

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Sinks behind PIMPL

**Files:**
- Modify: `include/data_tamer/sinks/mcap_sink.hpp`, `src/sinks/mcap_sink.cpp`
- Modify: `include/data_tamer/sinks/ros2_publisher_sink.hpp`, `src/sinks/ros2_publisher_sink.cpp` (ROS build only — by inspection)
- Create: `tests/abi_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: unchanged public API; `sizeof(MCAPSink) == sizeof(DataSinkBase) + sizeof(std::unique_ptr<void>)` pinned by a test so future fields cannot leak into the header.

- [x] **Step 1: Write the failing test**

`tests/abi_tests.cpp`:

```cpp
#include "data_tamer/data_sink.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

#include <gtest/gtest.h>

#include <memory>

using namespace DataTamer;

// Sinks keep their state behind a PIMPL so that adding members does not
// change the object layout downstream binaries were built against.
TEST(ABI, SinksAreOnlyOnePointerLargerThanTheBase)
{
  static_assert(sizeof(MCAPSink) == sizeof(DataSinkBase) + sizeof(std::unique_ptr<int>),
                "MCAPSink grew a member outside its Pimpl");
}
```

- [x] **Step 2: Register and run to see the failure**

Append `abi_tests.cpp` to `DATATAMER_TEST_SOURCES`.

Run: `cmake --build --preset debug 2>&1 | grep "static assertion"`
Expected: `static assertion failed: MCAPSink grew a member outside its Pimpl`.

- [x] **Step 3: Move `MCAPSink` state into a `Pimpl`**

`mcap_sink.hpp`: replace the whole `private:` section with

```cpp
private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  void openFile(std::string const& filepath);
  void restartRecordingImpl(std::string const& filepath, bool do_compression, bool new_file);
```

Remove the now-unneeded `#include <mutex>` and `<unordered_map>` from the header (keep `<chrono>`, `<memory>`, `<string>`). Keep the forward declaration of `mcap::McapWriter`.

`mcap_sink.cpp`: add, after the `kDataTamer` constant,

```cpp
struct MCAPSink::Pimpl
{
  std::string filepath;
  bool compression = false;
  std::unique_ptr<mcap::McapWriter> writer;

  std::unordered_map<uint64_t, uint16_t> hash_to_channel_id;
  std::unordered_map<std::string, Schema> schemas;

  bool create_file_on_reset = false;
  std::string original_filepath;
  size_t file_reset_counter = 1;

  std::chrono::seconds reset_time = std::chrono::seconds(60 * 10);
  std::chrono::system_clock::time_point start_time;

  bool forced_stop_recording = false;
  std::recursive_mutex mutex;
};
```

then mechanically rewrite every member access: `filepath_` → `_p->filepath`, `compression_` → `_p->compression`, `writer_` → `_p->writer`, `hash_to_channel_id_` → `_p->hash_to_channel_id`, `schemas_` → `_p->schemas`, `create_file_on_reset_` → `_p->create_file_on_reset`, `original_filepath_` → `_p->original_filepath`, `file_reset_counter_` → `_p->file_reset_counter`, `reset_time_` → `_p->reset_time`, `start_time_` → `_p->start_time`, `forced_stop_recording_` → `_p->forced_stop_recording`, `mutex_` → `_p->mutex`. The constructor becomes:

```cpp
MCAPSink::MCAPSink(const std::string& filepath, bool do_compression) : _p(new Pimpl)
{
  _p->filepath = filepath;
  _p->compression = do_compression;
  _p->original_filepath = filepath;
  openFile(_p->filepath);
}
```

and the destructor keeps `stopThread();` then `std::scoped_lock lk(_p->mutex);` (as today) — `_p` is destroyed after the body, so the lock is released first. Add `#include <unordered_map>` and `#include <mutex>` to the `.cpp` if not already present.

- [x] **Step 4: Same for `ROS2PublisherSink` (by inspection; not built here)**

`ros2_publisher_sink.hpp`: the private members `schemas_`, `schema_mutex_`, `schema_publisher_`, `data_publisher_`, `schema_changed_`, `data_msg_`, `node_interface_` move into `struct Pimpl; std::unique_ptr<Pimpl> _p;`. Because the constructor template calls `normalize_node` and `create_publishers` inline in the header, `create_publishers` must stay in the header but operate on `_p->...`, and the constructor becomes `: _p(new Pimpl{normalize_node(nodelike)})` with `Pimpl` defined in the header *inside a `details` namespace section guarded by the same includes* — i.e. for the ROS sink the PIMPL cannot hide the ROS types from the header (the constructor is a template). Rule: make `Pimpl` a nested struct **declared in the header, defined at the bottom of the header** (so the layout is still one pointer and members can be appended without changing `sizeof(ROS2PublisherSink)`), and note in a comment that full type hiding is not possible while the constructor is a template. Add the same `static_assert` to `abi_tests.cpp` under `#ifdef USING_ROS2`.

- [x] **Step 5: Run under all presets and commit**

Run: full `debug`, `asan`, `tsan`, `release`; also `./build/release/examples/T03_mcap_writer` runs and produces a file (exercises the MCAP sink end to end). Expected: green, `ABI.*` passes.

```bash
git add include/data_tamer/sinks/ tests/abi_tests.cpp tests/CMakeLists.txt src/sinks/
git commit -m "refactor: MCAPSink and ROS2PublisherSink keep their state behind a Pimpl

Adding fields to a sink no longer changes the object layout downstream
binaries were built against; a static_assert pins the size.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Spec §8 rows, CHANGELOG, measured results document

**Files:**
- Modify: `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md` §8 (repo root `docs/`)
- Modify: `data_tamer_cpp/CHANGELOG.rst` (new "Unreleased" section at the top)
- Create: `docs/benchmarks/2026-09-plan2.md`

- [x] **Step 1: Spec §8**

Add rows (or confirm existing ones now hold): `LogChannel::scopedWrite()` new; `LogChannel::writeLockContended/writeLockWaitMaxNs/stats` new; `Mutex` alias → `WriteMutex` (`lock_shared()` breaks); `LoggedValue::get()` const; scalar `getMutablePtr/getConstPtr` deprecated; `ConstPtr`/`MutablePtr::mutex()` deprecated, `operator bool` explicit; `ROS2PublisherSink::schema_mutex_` type change (private); `MCAPSink`/`ROS2PublisherSink` PIMPL (layout change — ABI break for this release, stable afterwards).

- [x] **Step 2: CHANGELOG**

Prepend to `data_tamer_cpp/CHANGELOG.rst`:

```rst
Unreleased
----------
* Real-time front end, part 1 (see docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md):
  scalar ``LoggedValue`` values are stored atomically — ``set()``/``get()`` are wait-free and the
  ``set()``/``takeSnapshot()`` data race is gone; ``LogChannel::scopedWrite()`` transactions on a
  priority-inheriting mutex; contention counters ``writeLockContended()``/``writeLockWaitMaxNs()``.
* API: ``Mutex`` is now an exclusive ``WriteMutex`` (``lock_shared()`` no longer available);
  ``getMutablePtr()``/``getConstPtr()`` are deprecated for scalar ``LoggedValue`` (use ``set()``/``get()``);
  ``LoggedValue`` is no longer movable; ``DummySink`` exposes accessors instead of public members;
  ``MCAPSink``/``ROS2PublisherSink`` moved their state behind a Pimpl (layout change).
* Build: ``CMakePresets.json`` (debug/release/asan/tsan) and a sanitizer CI job; benchmarks gained
  ``allocs/op`` and a latency harness (``rt_latency``).
```

- [x] **Step 3: Results document**

`docs/benchmarks/2026-09-plan2.md`: same structure as the baseline document; run the same commands (pinned, 5 repetitions, idle) at the Task 6 head and paste; add a table with baseline vs now for `DT_Doubles/1000`, `DT_LoggedValueSet/100`, `DT_SnapshotWithWriter`, and the harness 1-sink / 2-writers / 2-writers-transactions p50/p99/max plus `write_lock_contended` / `write_lock_wait_max_ns`.

- [x] **Step 4: Commit**

```bash
git add ../docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md CHANGELOG.rst ../docs/benchmarks/2026-09-plan2.md
git commit -m "docs: API delta, changelog and measured results for spec steps 4-5

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review against the spec

- **§4.1** atomic scalars, trait, relaxed ordering, proxies with write-back, `get()` const, deprecations: Tasks 2–4. The spec's "falls through to the mutex path when not lock-free" is what `is_atomic_scalar_v` does (`kAtomic == false` → `Transaction` path).
- **§4.2** one `WriteMutex` per channel in `ChannelSharedState`, `scopedWrite()`, nested `set()` via a thread-local depth (not a recursive mutex), `writeMutex()` signature kept, raw pointers covered: Tasks 1, 5. `getMutablePtr()` on non-scalars holds the mutex: Task 2/3.
- **§4.4** `LoggedValue` holds `shared_ptr<ChannelSharedState>`, `weak_ptr` only for the destructor, `enabled_` mirror dropped: Task 3.
- **§3 step 4** spin-then-lock around size+serialize, `write_lock_contended`, `write_lock_wait_max_ns` measured only on the contended path: Task 5. The implementation follows the spec: only blocking acquisition is timed, excluding serialization; see the execution-status corrections above.
- **§5.2** `setEnabled` wait-free, no `weak_ptr::lock`, same function for channel and `LoggedValue`: Tasks 1, 3.
- **§7/§8** counters and API delta: Tasks 5, 7. PIMPL for sinks: Task 6 (user request, not in the spec — §8 row added in Task 7).
- **§9** transaction consistency test (writer + raw pointer variants), nested `set()` test, TSAN writer-vs-snapshot: Tasks 3, 5. PI bound test already exists (Plan 1).
- **§10.1 steps 4–5** measurements: Tasks 3, 5, 7.
- Not in this plan (Plans 3–4): pool/queue wiring, `pushSnapshot` removal, epoch/`control_mutex`, removing `Pimpl::mutex`, `SnapshotRef` in sinks, moodycamel `Traits` for allocation counting.
- Type consistency: `ChannelSharedState::setEnabled(const RegistrationID&, bool)` and `(size_t, bool)` (T1) are what T3 and `channel.cpp` call; `ChannelSharedState::Transaction` (T5) is what `scopedWrite()` returns and non-scalar `set()` uses; `AtomicProxy<T>`/`AtomicConstProxy<T>` (T2) are `LoggedValue::MutableProxy/ConstProxy` for scalars (T3/T4); `DummySink::latestSnapshot()/latestPayloadSize()` (Plan 1) used throughout the tests.
