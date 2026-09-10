# Lock-free Front End — Plan 1 (spec steps 0–3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the infrastructure (sanitizer builds, allocation counting, performance baseline) and the three standalone components (`WriteMutex`, `SnapshotPool`/`SnapshotRef`, reworked `ValuePtr`) that the later `takeSnapshot()` rewrite is built from — each as an independently reviewable, sanitizer-verified change that leaves the tree green.

**Architecture:** Nothing in this plan changes the behaviour of `takeSnapshot()`. Tasks 1–5 make the test suite TSAN-clean, add ASAN/UBSAN and TSAN configurations, an `operator new` counting hook, and a committed benchmark baseline. Tasks 6–8 add new, not-yet-wired components under `include/data_tamer/details/` with their own tests. Task 9 makes `ValuePtr`/`LoggedValue` ready for atomic scalars without changing any byte on the wire. Plans 2–4 (spec steps 4–9) will wire these together.

**Tech Stack:** C++17, CMake ≥ 3.16 (presets need ≥ 3.21), GCC 13 (`-fsanitize=address,undefined` / `-fsanitize=thread`), GoogleTest 1.14, Google Benchmark, vendored moodycamel `ConcurrentQueue`, pthread (`PTHREAD_PRIO_INHERIT`).

**Spec:** `docs/superpowers/specs/2026-09-10-lockfree-frontend-design.md` (§2.2 components, §6.1 pool, §4.1/§4.2 mutex and atomics, §9 tests, §10 delivery steps 0–3, §10.1 benchmarks).

## Global Constraints

- Every task ends with the normal build **and** the `asan` **and** `tsan` presets green: `cmake --preset <p> && cmake --build --preset <p> && ctest --preset <p>`.
- TSAN on kernels ≥ 6.x needs ASLR disabled for the process: run test binaries under `setarch x86_64 -R` (the `tsan` test preset does this via a wrapper script, Task 3). GCC needs `-Wno-tsan` in the TSAN configuration because moodycamel uses `atomic_thread_fence`.
- Work in `data_tamer_cpp/` (paths below are relative to it unless they start with `docs/` or `.github/`). Build directories are `data_tamer_cpp/build/<preset>/`.
- Configure with the ROS environment **not** sourced, or pass `-DDATA_TAMER_BUILD_ROS=OFF` (the presets do). Task 1 fixes the test CMake so the ROS gtest vendor is not picked up when ROS is off.
- Compiler flags for the library are `-Wall -Wconversion -Wextra -Wsign-conversion -Werror -Wpedantic -Wno-sign-conversion` (`CMakeLists.txt:86`); new headers must compile clean under them.
- No heap allocation on the snapshot thread after warm-up is the project-wide goal; every new component's tests assert zero allocations with the hook from Task 4 where the spec says so.
- Google Benchmark is not installed on the reference machine: `sudo apt install libbenchmark-dev` (Ubuntu 24.04 ships 1.8.3). The CMake option already skips benchmarks when it is missing.
- Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Spec constants copied verbatim: `kMaxSinks = 8`, `kLockSpinNs = 2000`, `pool_capacity = 64`, `queue_capacity = 1024`, sink wake timeout `50 ms`. Only `kLockSpinNs` and the pool default are used in this plan.

---

## File map

| File | Responsibility | Task |
|---|---|---|
| `tests/CMakeLists.txt` | gate the ament gtest branch on `DATA_TAMER_BUILD_ROS`; register new test sources | 1, 4, 6, 7, 8 |
| `include/data_tamer/sinks/dummy_sink.hpp` | test sink with lock-protected accessors | 2 |
| `tests/*.cpp` (existing) | use the accessors instead of raw members | 2 |
| `CMakePresets.json` | `debug`, `asan`, `tsan` configure/build/test presets | 3 |
| `tests/run_under_tsan.sh` | `setarch x86_64 -R` wrapper for ctest | 3 |
| `.github/workflows/sanitizers.yml` | CI job running both sanitizer presets | 3 |
| `tests/alloc_counter.hpp`, `tests/alloc_counter.cpp` | thread-local `operator new/delete` counting hook | 4 |
| `tests/alloc_counter_tests.cpp` | hook self-test | 4 |
| `conanfile.py` | optional `benchmark/1.8.3` requirement | 5 |
| `benchmarks/CMakeLists.txt`, `benchmarks/data_tamer_benchmark.cpp` | micro-benchmarks with `allocs/op` counter | 5 |
| `benchmarks/rt_latency.cpp` | latency-distribution harness | 5 |
| `docs/benchmarks/2026-09-baseline.md` | numbers from the unmodified library | 5 |
| `include/data_tamer/details/write_mutex.hpp` | `WriteMutex` (PI mutex wrapper) | 6 |
| `tests/write_mutex_tests.cpp` | | 6 |
| `include/data_tamer/details/snapshot_pool.hpp` | `PoolSlot`, `SnapshotPool`, `SnapshotRef` | 7 |
| `tests/snapshot_pool_tests.cpp` | | 7 |
| `include/data_tamer/values.hpp` | `ValuePtr` with function pointers + `std::atomic<T>` constructor | 8 |
| `tests/value_ptr_tests.cpp` | golden-byte serialization tests | 8 |
| `include/data_tamer/logged_value.hpp` | delete move operations | 9 |
| `tests/logged_value_tests.cpp` | | 9 |

---

### Task 1: Gate the ROS gtest branch on `DATA_TAMER_BUILD_ROS`

**Files:**
- Modify: `tests/CMakeLists.txt:1-16`

**Interfaces:**
- Consumes: CMake option `DATA_TAMER_BUILD_ROS` from `CMakeLists.txt:23`.
- Produces: a test target `datatamer_test` that builds with the ROS environment sourced when `DATA_TAMER_BUILD_ROS=OFF`.

- [ ] **Step 1: Reproduce the failure**

With a ROS 2 environment sourced (e.g. `source /opt/ros/jazzy/setup.bash`):

```bash
cmake -S . -B build/repro -DDATA_TAMER_BUILD_ROS=OFF -DDATA_TAMER_BUILD_BENCHMARKS=OFF -DDATA_TAMER_BUILD_EXAMPLES=OFF
cmake --build build/repro -j
```

Expected: FAIL compiling `tests/ros2_publisher_tests.cpp` (`rclcpp/rclcpp.hpp: No such file`) because `gtest_vendor` from ROS was found and the ament branch was taken.

- [ ] **Step 2: Gate the branch**

Replace lines 1–5 of `tests/CMakeLists.txt` with:

```cmake
# look for gtest from ROS, but only when building for ROS 2
if(DATA_TAMER_BUILD_ROS)
    find_package(gtest_vendor QUIET)
    find_package(ament_cmake_gtest QUIET)
endif()

if(DATA_TAMER_BUILD_ROS AND gtest_vendor_FOUND AND ament_cmake_gtest_FOUND)
```

Also add `add_remove_sink_tests.cpp` to the ament source list (lines 6–11) so both branches run the same suite:

```cmake
    ament_add_gtest(datatamer_test
        dt_tests.cpp
        custom_types_tests.cpp
        parser_tests.cpp
        ros2_publisher_tests.cpp
        add_remove_sink_tests.cpp
        trait_tests.cpp)
```

- [ ] **Step 3: Verify both configurations**

```bash
rm -rf build/repro && cmake -S . -B build/repro -DDATA_TAMER_BUILD_ROS=OFF -DDATA_TAMER_BUILD_BENCHMARKS=OFF -DDATA_TAMER_BUILD_EXAMPLES=OFF
cmake --build build/repro -j && (cd build/repro && ctest --output-on-failure)
```

Expected: `100% tests passed, 0 tests failed out of 24`.

If a ROS workspace is available, also `colcon build --packages-select data_tamer_cpp --cmake-args -DCMAKE_BUILD_TYPE=Debug` and `colcon test --packages-select data_tamer_cpp`: expected all tests pass (the ament branch now includes `add_remove_sink_tests.cpp`).

- [ ] **Step 4: Commit**

```bash
git add tests/CMakeLists.txt
git commit -m "build: only look for ROS gtest vendor when DATA_TAMER_BUILD_ROS is on

With a ROS environment sourced, gtest_vendor was found even for non-ROS
builds and the ament test branch compiled ros2_publisher_tests.cpp without
rclcpp. Also run add_remove_sink_tests.cpp in the ament branch.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Make `DummySink` safe to read from the test thread

**Files:**
- Modify: `include/data_tamer/sinks/dummy_sink.hpp`
- Modify: `tests/dt_tests.cpp`, `tests/custom_types_tests.cpp`, `tests/parser_tests.cpp`, `tests/add_remove_sink_tests.cpp`

**Interfaces:**
- Produces: `Snapshot DummySink::latestSnapshot() const`, `long DummySink::snapshotsCount(uint64_t hash) const`, `size_t DummySink::schemasCount() const`, `uint64_t DummySink::firstSchemaHash() const`, `Schema DummySink::schema(uint64_t hash) const`. Later plans' tests use these names.

Why: the existing tests read `sink->latest_snapshot` and `sink->snapshots_count[hash]` while the sink thread writes them under `schema_mutex_` — ThreadSanitizer reports ~50 races in the suite today, all of this kind. The TSAN gate (Task 3) cannot go green until the fixture is fixed.

- [ ] **Step 1: See the races**

```bash
cmake -S . -B build/tsan-check -DCMAKE_BUILD_TYPE=Debug -DDATA_TAMER_BUILD_ROS=OFF -DDATA_TAMER_BUILD_BENCHMARKS=OFF -DDATA_TAMER_BUILD_EXAMPLES=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1 -Wno-tsan" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan-check -j
setarch x86_64 -R build/tsan-check/tests/datatamer_test 2>&1 | grep -c "WARNING: ThreadSanitizer"
```

Expected: a non-zero count (≈ 50) — these are the failures Task 2 removes.

- [ ] **Step 2: Rewrite `dummy_sink.hpp`**

```cpp
#pragma once

#include "data_tamer/data_sink.hpp"

#include <mutex>
#include <unordered_map>
#include <shared_mutex>

namespace DataTamer
{

using Mutex = std::shared_mutex;

/**
 * @brief The DummySink does nothing, only counting the number of snapshots received.
 * Used mostly for testing and debugging.
 *
 * All accessors take the internal mutex, so they may be called from any thread
 * while the sink thread is delivering snapshots.
 */
class DummySink : public DataSinkBase
{
public:
  ~DummySink() override { stopThread(); }

  void addChannel(std::string const& name, Schema const& schema) override
  {
    std::scoped_lock lk(mutex_);
    schemas_[schema.hash] = schema;
    schema_names_[schema.hash] = name;
    snapshots_count_[schema.hash] = 0;
  }

  bool storeSnapshot(const Snapshot& snapshot) override
  {
    std::scoped_lock lk(mutex_);
    latest_snapshot_ = snapshot;
    auto it = snapshots_count_.find(snapshot.schema_hash);
    if(it != snapshots_count_.end())
    {
      it->second++;
    }
    return true;
  }

  /// Copy of the most recent snapshot delivered to storeSnapshot().
  Snapshot latestSnapshot() const
  {
    std::scoped_lock lk(mutex_);
    return latest_snapshot_;
  }

  /// Number of snapshots delivered for the channel with this schema hash (0 if unknown).
  long snapshotsCount(uint64_t hash) const
  {
    std::scoped_lock lk(mutex_);
    auto it = snapshots_count_.find(hash);
    return it == snapshots_count_.end() ? 0 : it->second;
  }

  size_t schemasCount() const
  {
    std::scoped_lock lk(mutex_);
    return schemas_.size();
  }

  /// Hash of the first registered schema. Precondition: schemasCount() >= 1.
  uint64_t firstSchemaHash() const
  {
    std::scoped_lock lk(mutex_);
    return schemas_.begin()->first;
  }

  Schema schema(uint64_t hash) const
  {
    std::scoped_lock lk(mutex_);
    return schemas_.at(hash);
  }

  std::string schemaName(uint64_t hash) const
  {
    std::scoped_lock lk(mutex_);
    return schema_names_.at(hash);
  }

private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Schema> schemas_;
  std::unordered_map<uint64_t, std::string> schema_names_;
  std::unordered_map<uint64_t, long> snapshots_count_;
  Snapshot latest_snapshot_;
};

}  // namespace DataTamer
```

- [ ] **Step 3: Update the tests to the accessors**

Run from `data_tamer_cpp/`:

```bash
sed -i 's/->latest_snapshot\./->latestSnapshot()./g; s/->latest_snapshot)/->latestSnapshot())/g' tests/dt_tests.cpp tests/custom_types_tests.cpp tests/parser_tests.cpp
sed -i 's/->snapshots_count\[hash\]/->snapshotsCount(hash)/g' tests/dt_tests.cpp tests/add_remove_sink_tests.cpp
sed -i 's/->schemas\.size()/->schemasCount()/g; s/->schemas\.begin()->first/->firstSchemaHash()/g' tests/dt_tests.cpp
grep -n "latest_snapshot\b\|snapshots_count\[\|->schemas\." tests/*.cpp
```

Expected: the final `grep` prints nothing (the comment mentioning `snapshots_count` in `add_remove_sink_tests.cpp:67` is fine to leave; it does not match the pattern).

In `tests/dt_tests.cpp`, the repeated `sink->latestSnapshot().payload.size()` inside `checkSize` (formerly lines 203–211) copies the snapshot per call; that is fine for a test.

- [ ] **Step 4: Verify plain build and TSAN**

```bash
cmake --build build/repro -j && (cd build/repro && ctest --output-on-failure)
cmake --build build/tsan-check -j
setarch x86_64 -R build/tsan-check/tests/datatamer_test 2>&1 | grep -c "WARNING: ThreadSanitizer"
```

Expected: 24 tests pass; the TSAN count is `0`. If any report remains, it names the test line — fix it the same way (read through an accessor under the sink mutex), never with a suppression.

- [ ] **Step 5: Commit**

```bash
git add include/data_tamer/sinks/dummy_sink.hpp tests/
git commit -m "test: make DummySink readable from the test thread without races

Tests read latest_snapshot and snapshots_count while the sink thread wrote
them. Replace the public members with mutex-protected accessors so the
suite is clean under ThreadSanitizer.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: Sanitizer presets and CI job

**Files:**
- Create: `CMakePresets.json`
- Create: `tests/run_under_tsan.sh`
- Create: `.github/workflows/sanitizers.yml` (repository root)
- Modify: `tests/CMakeLists.txt:41` (test command goes through the wrapper when `DATA_TAMER_TEST_WRAPPER` is set)

**Interfaces:**
- Produces: configure/build/test presets named `debug`, `asan`, `tsan`; CMake cache variable `DATA_TAMER_TEST_WRAPPER` (path to a script that execs its arguments).

- [ ] **Step 1: Write `CMakePresets.json`**

```json
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 21, "patch": 0 },
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Unix Makefiles",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "DATA_TAMER_BUILD_ROS": "OFF",
        "DATA_TAMER_BUILD_TESTS": "ON",
        "DATA_TAMER_BUILD_EXAMPLES": "OFF",
        "DATA_TAMER_BUILD_BENCHMARKS": "OFF",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
    {
      "name": "debug",
      "inherits": "base",
      "displayName": "Debug, no sanitizers"
    },
    {
      "name": "release",
      "inherits": "base",
      "displayName": "Release with benchmarks",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "DATA_TAMER_BUILD_BENCHMARKS": "ON",
        "DATA_TAMER_BUILD_EXAMPLES": "ON"
      }
    },
    {
      "name": "asan",
      "inherits": "base",
      "displayName": "AddressSanitizer + UndefinedBehaviorSanitizer",
      "cacheVariables": {
        "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g -O1",
        "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=address,undefined",
        "CMAKE_SHARED_LINKER_FLAGS": "-fsanitize=address,undefined"
      }
    },
    {
      "name": "tsan",
      "inherits": "base",
      "displayName": "ThreadSanitizer",
      "cacheVariables": {
        "CMAKE_CXX_FLAGS": "-fsanitize=thread -g -O1 -Wno-tsan",
        "CMAKE_EXE_LINKER_FLAGS": "-fsanitize=thread",
        "CMAKE_SHARED_LINKER_FLAGS": "-fsanitize=thread",
        "DATA_TAMER_TEST_WRAPPER": "${sourceDir}/tests/run_under_tsan.sh"
      }
    }
  ],
  "buildPresets": [
    { "name": "debug",   "configurePreset": "debug",   "jobs": 0 },
    { "name": "release", "configurePreset": "release", "jobs": 0 },
    { "name": "asan",    "configurePreset": "asan",    "jobs": 0 },
    { "name": "tsan",    "configurePreset": "tsan",    "jobs": 0 }
  ],
  "testPresets": [
    {
      "name": "debug",
      "configurePreset": "debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "asan",
      "configurePreset": "asan",
      "output": { "outputOnFailure": true },
      "environment": {
        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1:strict_string_checks=1",
        "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1"
      }
    },
    {
      "name": "tsan",
      "configurePreset": "tsan",
      "output": { "outputOnFailure": true },
      "environment": {
        "TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1:history_size=4"
      }
    }
  ]
}
```

`jobs: 0` means "use all cores" for the Makefile generator in CMake ≥ 3.21.

- [ ] **Step 2: Write the TSAN wrapper**

`tests/run_under_tsan.sh`:

```bash
#!/usr/bin/env bash
# ThreadSanitizer needs a smaller ASLR range than recent kernels use by
# default; -R disables address-space randomisation for this process only.
exec setarch "$(uname -m)" -R "$@"
```

`chmod +x tests/run_under_tsan.sh`.

- [ ] **Step 3: Route ctest through the wrapper**

In `tests/CMakeLists.txt`, replace the non-ROS test registration (currently `gtest_discover_tests(...)` on line 34 and `add_test(NAME datatamer_test ...)` on line 41) with:

```cmake
    set(DATA_TAMER_TEST_WRAPPER "" CACHE FILEPATH "Optional launcher that execs the test binary (used for TSAN)")
    if(DATA_TAMER_TEST_WRAPPER)
        add_test(NAME datatamer_test COMMAND ${DATA_TAMER_TEST_WRAPPER} $<TARGET_FILE:datatamer_test>)
    else()
        gtest_discover_tests(datatamer_test DISCOVERY_MODE PRE_TEST)
    endif()
```

(The previous file registered the tests twice — once discovered, once as a whole binary — which ran every test two times. With the wrapper set, the whole binary runs once under `setarch`; without it, individual tests are discovered as before.)

- [ ] **Step 4: Run all three presets**

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan  && cmake --build --preset asan  && ctest --preset asan
cmake --preset tsan  && cmake --build --preset tsan  && ctest --preset tsan
```

Expected: `debug` — 23 tests pass (discovered individually; the duplicate whole-binary run is gone). `asan` — same, no `ERROR: AddressSanitizer` / `runtime error:` lines. `tsan` — 1 ctest entry (the whole binary) passes with no `WARNING: ThreadSanitizer`.

- [ ] **Step 5: Write the CI workflow**

`.github/workflows/sanitizers.yml` (at the repository root, next to `cmake_ubuntu.yml`):

```yaml
name: sanitizers

on: [push, pull_request]

jobs:
  sanitizers:
    runs-on: ubuntu-24.04
    strategy:
      fail-fast: false
      matrix:
        preset: [asan, tsan]
    steps:
    - uses: actions/checkout@v4

    - name: Install dependencies
      run: sudo apt-get update && sudo apt-get install -y libgtest-dev libzstd-dev liblz4-dev

    - name: Configure
      working-directory: data_tamer_cpp
      run: cmake --preset ${{ matrix.preset }}

    - name: Build
      working-directory: data_tamer_cpp
      run: cmake --build --preset ${{ matrix.preset }}

    - name: Test
      working-directory: data_tamer_cpp
      run: ctest --preset ${{ matrix.preset }}
```

- [ ] **Step 6: Commit**

```bash
git add CMakePresets.json tests/run_under_tsan.sh tests/CMakeLists.txt ../.github/workflows/sanitizers.yml
git commit -m "build: add debug/asan/tsan CMake presets and a sanitizers CI job

TSAN runs the test binary under setarch -R (kernel ASLR conflict) and with
-Wno-tsan (moodycamel's atomic_thread_fence). Tests are no longer
registered twice with ctest.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Allocation-counting hook for tests

**Files:**
- Create: `tests/alloc_counter.hpp`
- Create: `tests/alloc_counter.cpp`
- Create: `tests/alloc_counter_tests.cpp`
- Modify: `tests/CMakeLists.txt` (add the three files to both source lists)

**Interfaces:**
- Produces: `DataTamerTest::AllocCounter` with `static thread_local` counters `allocations`, `deallocations`, `enabled`; RAII `AllocCounter::Scope` that resets and enables counting for the current thread; `AllocCounter::Scope::allocations()` convenience. Later plans' zero-allocation tests use `AllocCounter::Scope`.

Design: counting is **per thread**, so sink threads allocating in the background never pollute a measurement on the test thread. The hook replaces the global `operator new`/`delete` for the whole test binary; outside a `Scope` it only forwards to `malloc`/`free`.

- [ ] **Step 1: Write the self-test first**

`tests/alloc_counter_tests.cpp`:

```cpp
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using DataTamerTest::AllocCounter;

TEST(AllocCounter, CountsNewAndDeleteInsideScope)
{
  AllocCounter::Scope scope;
  {
    std::vector<int> v(100);
    ASSERT_EQ(scope.allocations(), 1u);
    ASSERT_EQ(scope.deallocations(), 0u);
  }
  ASSERT_EQ(scope.allocations(), 1u);
  ASSERT_EQ(scope.deallocations(), 1u);
}

TEST(AllocCounter, DoesNotCountOutsideScope)
{
  {
    AllocCounter::Scope scope;
  }
  // scope ended: allocations after this point are not attributed
  std::vector<int> v(100);
  AllocCounter::Scope scope;
  ASSERT_EQ(scope.allocations(), 0u);
}

TEST(AllocCounter, IsPerThread)
{
  AllocCounter::Scope scope;
  std::thread t([] {
    std::vector<int> v(1000);  // allocation on another thread
    (void)v;
  });
  t.join();
  // std::thread itself allocates on the calling thread (thread state), so we
  // only assert that the 1000-int vector (4000 bytes) was not counted here by
  // checking the count is small, not zero.
  ASSERT_LE(scope.allocations(), 2u);
}

TEST(AllocCounter, ReuseOfCapacityDoesNotAllocate)
{
  std::vector<uint8_t> payload;
  payload.reserve(4096);
  AllocCounter::Scope scope;
  for(int i = 0; i < 1000; i++)
  {
    payload.resize(100 + (i % 10));
  }
  ASSERT_EQ(scope.allocations(), 0u);
}
```

- [ ] **Step 2: Register the files and run to see the failure**

In `tests/CMakeLists.txt`, add `alloc_counter.cpp` and `alloc_counter_tests.cpp` to **both** source lists (the `ament_add_gtest(...)` list and the `add_executable(datatamer_test ...)` list).

```bash
cmake --preset debug && cmake --build --preset debug
```

Expected: FAIL — `alloc_counter.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

`tests/alloc_counter.hpp`:

```cpp
#pragma once

#include <cstddef>

namespace DataTamerTest
{

/**
 * Per-thread allocation counter. Replaces the global operator new/delete
 * (see alloc_counter.cpp). Counting is active only while a Scope object is
 * alive on the current thread.
 */
struct AllocCounter
{
  static thread_local bool enabled;
  static thread_local std::size_t allocations;
  static thread_local std::size_t deallocations;

  struct Scope
  {
    Scope()
    {
      AllocCounter::allocations = 0;
      AllocCounter::deallocations = 0;
      AllocCounter::enabled = true;
    }
    ~Scope() { AllocCounter::enabled = false; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    std::size_t allocations() const { return AllocCounter::allocations; }
    std::size_t deallocations() const { return AllocCounter::deallocations; }
  };
};

}  // namespace DataTamerTest
```

- [ ] **Step 4: Write the hook**

`tests/alloc_counter.cpp`:

```cpp
#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace DataTamerTest
{
thread_local bool AllocCounter::enabled = false;
thread_local std::size_t AllocCounter::allocations = 0;
thread_local std::size_t AllocCounter::deallocations = 0;
}  // namespace DataTamerTest

using DataTamerTest::AllocCounter;

namespace
{
void* countedAlloc(std::size_t size)
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  if(size == 0)
  {
    size = 1;
  }
  void* p = std::malloc(size);
  if(p == nullptr)
  {
    throw std::bad_alloc();
  }
  return p;
}

void countedFree(void* p) noexcept
{
  if(p != nullptr && AllocCounter::enabled)
  {
    ++AllocCounter::deallocations;
  }
  std::free(p);
}
}  // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  if(AllocCounter::enabled)
  {
    ++AllocCounter::allocations;
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { countedFree(p); }
```

Over-aligned (`std::align_val_t`) overloads are deliberately not replaced: the only over-aligned allocation in the library after Task 7 is the one-time `PoolSlot[]` array in `SnapshotPool`'s constructor, which never happens inside a measured scope. If a future change adds an over-aligned allocation on the snapshot path it will be invisible to this hook — the zero-allocation tests therefore also assert `deallocations() == 0`, and a reviewer adding `alignas` to anything touched per snapshot must extend the hook.

- [ ] **Step 5: Run the tests under all presets**

```bash
cmake --build --preset debug && ctest --preset debug -R AllocCounter
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Expected: the four `AllocCounter.*` tests pass under `debug`; the full suite passes under `asan` and `tsan`. (ASAN intercepts `malloc`, so the replaced `operator new` still gets leak and overflow checking.)

- [ ] **Step 6: Commit**

```bash
git add tests/alloc_counter.hpp tests/alloc_counter.cpp tests/alloc_counter_tests.cpp tests/CMakeLists.txt
git commit -m "test: add a per-thread allocation counting hook

Replaces global operator new/delete in the test binary; counting is active
only inside an AllocCounter::Scope on the current thread, so background
sink threads do not affect measurements.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Benchmarks and the committed baseline

**Files:**
- Modify: `conanfile.py` (optional `benchmark/1.8.3`)
- Modify: `benchmarks/CMakeLists.txt`
- Modify: `benchmarks/data_tamer_benchmark.cpp`
- Create: `benchmarks/rt_latency.cpp`
- Create: `docs/benchmarks/2026-09-baseline.md` (repository `docs/`, next to `superpowers/`)

**Interfaces:**
- Produces: benchmark targets `dt_benchmark` and `rt_latency`; the harness CLI `rt_latency [--values N] [--sinks K] [--writers W] [--seconds S] [--mcap PATH] [--fifo]`.

The baseline must be recorded on the **unmodified** library, i.e. before Task 8 changes `ValuePtr`. Tasks 6–7 add files but do not touch the hot path, so running this task after them is also valid; running it after Task 8 is not.

- [ ] **Step 1: Make Google Benchmark available**

```bash
sudo apt-get install -y libbenchmark-dev
```

In `conanfile.py`, add an option and requirement so the conan build can also provide it:

```python
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "tests": [True, False],
        "examples": [True, False],
        "benchmarks": [True, False]
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "tests": True,
        "examples": True,
        "benchmarks": False
    }
```

and in `requirements()`:

```python
        if self.options.benchmarks:
            self.requires("benchmark/1.8.3")
```

- [ ] **Step 2: Extend the micro-benchmarks**

Replace `benchmarks/data_tamer_benchmark.cpp` with:

```cpp
#include <benchmark/benchmark.h>
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "../examples/geometry_types.hpp"

#include <atomic>
#include <cstdlib>
#include <new>
#include <thread>

using namespace DataTamer;

// ---- allocation counter (same idea as tests/alloc_counter.cpp, kept local
// so the benchmark does not depend on the test sources) ----
namespace
{
thread_local bool g_count_enabled = false;
thread_local std::size_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t size)
{
  if(g_count_enabled)
  {
    ++g_allocations;
  }
  void* p = std::malloc(size == 0 ? 1 : size);
  if(!p)
  {
    throw std::bad_alloc();
  }
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct AllocScope
{
  AllocScope() { g_allocations = 0; g_count_enabled = true; }
  ~AllocScope() { g_count_enabled = false; }
};

class NullSink : public DataSinkBase
{
public:
  ~NullSink() override { stopThread(); }
  void addChannel(std::string const&, Schema const&) override {}
  bool storeSnapshot(const Snapshot&) override { return true; }
};

static void DT_Doubles(benchmark::State& state)
{
  std::vector<double> values(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  channel->registerValue("values", &values);
  channel->takeSnapshot();  // warm-up
  channel->takeSnapshot();

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    AllocScope scope;
    channel->takeSnapshot();
    allocs += g_allocations;
  }
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
}

static void DT_PoseType(benchmark::State& state)
{
  std::vector<TestTypes::Pose> poses(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  channel->registerValue("values", &poses);
  channel->takeSnapshot();
  channel->takeSnapshot();

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    AllocScope scope;
    channel->takeSnapshot();
    allocs += g_allocations;
  }
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
}

// 1000 doubles, N sinks
static void DT_MultiSink(benchmark::State& state)
{
  std::vector<double> values(1000);
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  for(int i = 0; i < state.range(0); i++)
  {
    channel->addDataSink(std::make_shared<NullSink>());
  }
  channel->registerValue("values", &values);
  channel->takeSnapshot();
  channel->takeSnapshot();

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    AllocScope scope;
    channel->takeSnapshot();
    allocs += g_allocations;
  }
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
}

// LoggedValue<double>::set from the calling thread
static void DT_LoggedValueSet(benchmark::State& state)
{
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  std::vector<std::shared_ptr<LoggedValue<double>>> values;
  for(int i = 0; i < state.range(0); i++)
  {
    values.push_back(channel->createLoggedValue<double>("v" + std::to_string(i)));
  }
  double x = 0;
  for(auto _ : state)
  {
    for(auto& v : values)
    {
      v->set(x);
    }
    x += 1.0;
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// takeSnapshot on this thread while another thread hammers set() on 100 LoggedValues
static void DT_SnapshotWithWriter(benchmark::State& state)
{
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  std::vector<std::shared_ptr<LoggedValue<double>>> values;
  for(int i = 0; i < 100; i++)
  {
    values.push_back(channel->createLoggedValue<double>("v" + std::to_string(i)));
  }
  std::vector<double> plain(1000);
  channel->registerValue("plain", &plain);
  channel->takeSnapshot();

  std::atomic_bool run{ true };
  std::thread writer([&] {
    double x = 0;
    while(run)
    {
      for(auto& v : values)
      {
        v->set(x);
      }
      x += 1.0;
    }
  });

  for(auto _ : state)
  {
    channel->takeSnapshot();
  }
  run = false;
  writer.join();
}

BENCHMARK(DT_Doubles)->Arg(125)->Arg(250)->Arg(500)->Arg(1000)->Arg(2000);
BENCHMARK(DT_PoseType)->Arg(125)->Arg(250)->Arg(500)->Arg(1000);
BENCHMARK(DT_MultiSink)->Arg(1)->Arg(2)->Arg(4);
BENCHMARK(DT_LoggedValueSet)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(DT_SnapshotWithWriter);

BENCHMARK_MAIN();
```

- [ ] **Step 3: Write the latency harness**

`benchmarks/rt_latency.cpp`:

```cpp
// Latency-distribution harness: a periodic loop calling takeSnapshot() and
// recording every call's duration. Prints p50/p99/p99.9/max, allocation
// count per call after warm-up, and the channel's counters (when available).
//
// usage: rt_latency [--values N] [--sinks K] [--writers W] [--seconds S]
//                   [--rate HZ] [--mcap PATH] [--fifo]
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <time.h>

using namespace DataTamer;

namespace
{
thread_local bool g_count_enabled = false;
thread_local std::size_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t size)
{
  if(g_count_enabled)
  {
    ++g_allocations;
  }
  void* p = std::malloc(size == 0 ? 1 : size);
  if(!p)
  {
    throw std::bad_alloc();
  }
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

class NullSink : public DataSinkBase
{
public:
  ~NullSink() override { stopThread(); }
  void addChannel(std::string const&, Schema const&) override {}
  bool storeSnapshot(const Snapshot&) override { return true; }
};

struct Options
{
  int values = 1000;
  int sinks = 1;
  int writers = 0;
  int seconds = 10;
  int rate_hz = 1000;
  std::string mcap;
  bool fifo = false;
};

static Options parse(int argc, char** argv)
{
  Options o;
  for(int i = 1; i < argc; i++)
  {
    auto next = [&](int& dst) { dst = std::atoi(argv[++i]); };
    if(!std::strcmp(argv[i], "--values")) next(o.values);
    else if(!std::strcmp(argv[i], "--sinks")) next(o.sinks);
    else if(!std::strcmp(argv[i], "--writers")) next(o.writers);
    else if(!std::strcmp(argv[i], "--seconds")) next(o.seconds);
    else if(!std::strcmp(argv[i], "--rate")) next(o.rate_hz);
    else if(!std::strcmp(argv[i], "--mcap")) o.mcap = argv[++i];
    else if(!std::strcmp(argv[i], "--fifo")) o.fifo = true;
    else
    {
      std::fprintf(stderr, "unknown option %s\n", argv[i]);
      std::exit(1);
    }
  }
  if(o.values < 2)
  {
    o.values = 2;  // half plain, half LoggedValue: need at least one of each
  }
  if(o.rate_hz < 1)
  {
    o.rate_hz = 1;
  }
  return o;
}

static bool trySchedFifo()
{
  sched_param sp{};
  sp.sched_priority = 80;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
}

static void printPercentiles(std::vector<long>& ns)
{
  std::sort(ns.begin(), ns.end());
  auto pct = [&](double p) { return ns[size_t(double(ns.size() - 1) * p)]; };
  std::printf("samples=%zu p50=%ld ns p99=%ld ns p99.9=%ld ns max=%ld ns\n", ns.size(),
              pct(0.50), pct(0.99), pct(0.999), ns.back());
}

int main(int argc, char** argv)
{
  const Options opt = parse(argc, argv);
  std::printf("rt_latency values=%d sinks=%d writers=%d seconds=%d rate=%dHz mcap=%s fifo=%d\n",
              opt.values, opt.sinks, opt.writers, opt.seconds, opt.rate_hz,
              opt.mcap.empty() ? "-" : opt.mcap.c_str(), int(opt.fifo));

  auto channel = LogChannel::create("rt");
  std::vector<std::shared_ptr<DataSinkBase>> sinks;
  for(int i = 0; i < opt.sinks; i++)
  {
    if(!opt.mcap.empty() && i == 0)
    {
      auto mcap = std::make_shared<MCAPSink>(opt.mcap, /*compression*/ true);
      mcap->setMaxTimeBeforeReset(std::chrono::seconds(0));
      sinks.push_back(mcap);
    }
    else
    {
      sinks.push_back(std::make_shared<NullSink>());
    }
    channel->addDataSink(sinks.back());
  }

  // half the values as one registered vector, half as LoggedValues the writers touch
  std::vector<double> plain(size_t(opt.values / 2));
  channel->registerValue("plain", &plain);
  std::vector<std::shared_ptr<LoggedValue<double>>> logged;
  for(int i = 0; i < opt.values / 2; i++)
  {
    logged.push_back(channel->createLoggedValue<double>("lv" + std::to_string(i)));
  }

  std::atomic_bool run{ true };
  std::vector<std::thread> writers;
  for(int w = 0; w < opt.writers; w++)
  {
    writers.emplace_back([&, w] {
      double x = double(w);
      while(run)
      {
        for(size_t i = size_t(w); i < logged.size(); i += size_t(opt.writers))
        {
          logged[i]->set(x);
        }
        x += 1.0;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }

  const bool fifo_ok = opt.fifo && trySchedFifo();
  if(opt.fifo && !fifo_ok)
  {
    std::printf("SCHED_FIFO not available (need CAP_SYS_NICE); running SCHED_OTHER\n");
  }

  // warm-up
  for(int i = 0; i < 10; i++)
  {
    channel->takeSnapshot();
  }

  const long period_ns = 1000000000L / opt.rate_hz;
  const size_t total = size_t(opt.seconds) * size_t(opt.rate_hz);
  std::vector<long> durations;
  durations.reserve(total);
  std::size_t allocations = 0;
  size_t failed = 0;

  timespec next{};
  clock_gettime(CLOCK_MONOTONIC, &next);
  for(size_t i = 0; i < total; i++)
  {
    next.tv_nsec += period_ns;
    while(next.tv_nsec >= 1000000000L)
    {
      next.tv_nsec -= 1000000000L;
      next.tv_sec += 1;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);

    plain[i % plain.size()] = double(i);
    g_allocations = 0;
    g_count_enabled = true;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = channel->takeSnapshot();
    const auto t1 = std::chrono::steady_clock::now();
    g_count_enabled = false;
    allocations += g_allocations;
    if(!ok)
    {
      failed++;
    }
    durations.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  }

  run = false;
  for(auto& t : writers)
  {
    t.join();
  }

  printPercentiles(durations);
  std::printf("allocations per call after warm-up: %.4f\n", double(allocations) / double(total));
  std::printf("takeSnapshot returned false: %zu / %zu\n", failed, total);
  std::printf("fifo=%d\n", int(fifo_ok));
  return 0;
}
```

Later plans append the channel counters (`poolExhausted`, `writeLockContended`, `writeLockWaitMaxNs`) to the summary once those exist; the harness must compile against the unmodified library today, so it prints only what exists.

- [ ] **Step 4: Register the targets**

Replace `benchmarks/CMakeLists.txt` with:

```cmake
add_executable(dt_benchmark data_tamer_benchmark.cpp)
target_include_directories(dt_benchmark
     PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(dt_benchmark data_tamer benchmark::benchmark)

add_executable(rt_latency rt_latency.cpp)
target_include_directories(rt_latency
     PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(rt_latency data_tamer pthread)
```

(`benchmark::benchmark` is the imported target exported by both the apt package and conan; the old file linked the bare name `benchmark`, which only works by accident.)

- [ ] **Step 5: Build and run the baseline**

```bash
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/dt_benchmark --benchmark_min_time=0.5s 2>&1 | tee /tmp/baseline_micro.txt
./build/release/benchmarks/rt_latency --values 1000 --sinks 1 --seconds 10 2>&1 | tee /tmp/baseline_rt_1sink.txt
./build/release/benchmarks/rt_latency --values 1000 --sinks 2 --seconds 10 2>&1 | tee /tmp/baseline_rt_2sink.txt
./build/release/benchmarks/rt_latency --values 1000 --sinks 1 --writers 2 --seconds 10 2>&1 | tee /tmp/baseline_rt_writers.txt
./build/release/benchmarks/rt_latency --values 1000 --sinks 1 --seconds 10 --mcap /tmp/baseline.mcap 2>&1 | tee /tmp/baseline_rt_mcap.txt
sudo ./build/release/benchmarks/rt_latency --values 1000 --sinks 1 --writers 2 --seconds 10 --fifo 2>&1 | tee /tmp/baseline_rt_fifo.txt   # optional: needs CAP_SYS_NICE
```

Expected on the unmodified library: `allocs/op` around `2` for one sink and `2 × sinks` for `DT_MultiSink` (the Snapshot copy into the queue), `allocations per call after warm-up` ≈ 2 in the harness, `takeSnapshot returned false: 0`.

- [ ] **Step 6: Record the baseline**

Create `docs/benchmarks/2026-09-baseline.md` (repository `docs/`) with this structure, pasting the full stdout of each run under its heading:

```markdown
# Performance baseline — before the lock-free front end

Recorded on the unmodified library at commit `<git rev-parse --short HEAD>`.

Machine: `<lscpu | grep "Model name"`>, `<nproc>` cores, kernel `<uname -r>`,
GCC `<g++ --version | head -1>`, build preset `release` (`-O3`, no sanitizers).

How to reproduce: see the commands in
`docs/superpowers/plans/2026-09-10-lockfree-frontend-plan-1.md`, Task 5, Step 5.

## Micro-benchmarks (`dt_benchmark`)

```
<paste /tmp/baseline_micro.txt>
```

## Latency harness (`rt_latency`)

### 1000 values, 1 sink
```
<paste /tmp/baseline_rt_1sink.txt>
```

### 1000 values, 2 sinks
```
<paste /tmp/baseline_rt_2sink.txt>
```

### 1000 values, 1 sink, 2 writer threads
```
<paste /tmp/baseline_rt_writers.txt>
```

### 1000 values, 1 compressed MCAP sink
```
<paste /tmp/baseline_rt_mcap.txt>
```

### 1000 values, 1 sink, 2 writers, SCHED_FIFO (if available)
```
<paste /tmp/baseline_rt_fifo.txt or "not run: no CAP_SYS_NICE">
```
```

- [ ] **Step 7: Commit**

```bash
git add conanfile.py benchmarks/ ../docs/benchmarks/2026-09-baseline.md
git commit -m "bench: add allocs/op, multi-sink, LoggedValue and writer-contention benchmarks; latency harness; baseline

Records p50/p99/p99.9/max of takeSnapshot() at 1 kHz plus allocations per
call on the unmodified library, so later steps have a reference.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: `WriteMutex` — priority-inheriting mutex wrapper

**Files:**
- Create: `include/data_tamer/details/write_mutex.hpp`
- Create: `tests/write_mutex_tests.cpp`
- Modify: `tests/CMakeLists.txt` (add test file to both lists), `CMakeLists.txt:62-80` (add header to the target sources)

**Interfaces:**
- Produces:
  ```cpp
  namespace DataTamer {
  class WriteMutex {  // Lockable
   public:
    static constexpr bool kPriorityInheritance;   // true on Linux
    static constexpr std::int64_t kLockSpinNs = 2000;
    WriteMutex(); ~WriteMutex();
    void lock(); bool try_lock(); void unlock();
    /// spin on try_lock for up to spin_ns, then lock(). Returns true if it had to block.
    bool lockWithSpin(std::int64_t spin_ns = kLockSpinNs);
  };
  }
  ```
  Plan 2 replaces `using Mutex = std::shared_mutex` with `using Mutex = WriteMutex` and gives `ChannelSharedState` one of these.

- [ ] **Step 1: Write the failing tests**

`tests/write_mutex_tests.cpp`:

```cpp
#include "data_tamer/details/write_mutex.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

using DataTamer::WriteMutex;

TEST(WriteMutex, IsLockable)
{
  WriteMutex m;
  {
    std::lock_guard<WriteMutex> lk(m);
  }
  {
    std::unique_lock<WriteMutex> lk(m);
    ASSERT_TRUE(lk.owns_lock());
  }
  {
    std::scoped_lock lk(m);
  }
}

TEST(WriteMutex, TryLockFailsWhileHeld)
{
  WriteMutex m;
  m.lock();
  std::atomic_bool other_got_it{ true };
  std::thread t([&] { other_got_it = m.try_lock(); });
  t.join();
  ASSERT_FALSE(other_got_it);
  m.unlock();
  ASSERT_TRUE(m.try_lock());
  m.unlock();
}

TEST(WriteMutex, PriorityInheritanceIsEnabledOnLinux)
{
#if defined(__linux__)
  ASSERT_TRUE(WriteMutex::kPriorityInheritance);
#else
  ASSERT_FALSE(WriteMutex::kPriorityInheritance);
#endif
}

TEST(WriteMutex, LockWithSpinReturnsFalseWhenUncontended)
{
  WriteMutex m;
  ASSERT_FALSE(m.lockWithSpin());
  m.unlock();
}

TEST(WriteMutex, LockWithSpinAcquiresAfterShortHold)
{
  WriteMutex m;
  std::atomic_bool holder_ready{ false };
  std::thread holder([&] {
    m.lock();
    holder_ready = true;
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    m.unlock();
  });
  while(!holder_ready)
  {
  }
  // 500 us hold vs 2 us spin budget: we must block, and we must still acquire
  const bool blocked = m.lockWithSpin();
  m.unlock();
  holder.join();
  ASSERT_TRUE(blocked);
}

TEST(WriteMutex, LockWithSpinDoesNotBlockForVeryShortHold)
{
  WriteMutex m;
  std::atomic_bool holder_ready{ false };
  std::atomic_bool release{ false };
  std::thread holder([&] {
    m.lock();
    holder_ready = true;
    while(!release)
    {
    }
    m.unlock();
  });
  while(!holder_ready)
  {
  }
  std::atomic_bool blocked{ false };
  std::thread waiter([&] { blocked = m.lockWithSpin(/*spin_ns=*/50'000'000); m.unlock(); });
  std::this_thread::sleep_for(std::chrono::microseconds(100));
  release = true;
  waiter.join();
  holder.join();
  // released well inside the 50 ms spin budget -> acquired by spinning
  ASSERT_FALSE(blocked);
}

// Priority inheritance bound. Needs CAP_SYS_NICE; skipped otherwise.
// A SCHED_OTHER holder shares one core with three SCHED_OTHER CPU hogs, so
// without priority inheritance it gets ~1/4 of the core and its 200 us of
// work spans several CFS slices (milliseconds). With PI, the SCHED_FIFO
// waiter boosts the holder the moment it blocks, so the wait is the holder's
// remaining work plus a wake-up: well under 1 ms.
//
// Note: the FIFO waiter must never spin-wait on this core (a spinning FIFO
// thread starves every CFS thread on it, including the holder); it sleeps.
TEST(WriteMutex, PriorityInheritanceBoundsTheWait)
{
  sched_param fifo{};
  fifo.sched_priority = 50;
  if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &fifo) != 0)
  {
    GTEST_SKIP() << "needs CAP_SYS_NICE for SCHED_FIFO";
  }
  struct RestoreScheduler
  {
    ~RestoreScheduler()
    {
      sched_param other{};
      pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
    }
  } restore;

  cpu_set_t one_core;
  CPU_ZERO(&one_core);
  CPU_SET(0, &one_core);
  pthread_setaffinity_np(pthread_self(), sizeof(one_core), &one_core);

  WriteMutex m;
  std::atomic_bool stop{ false };
  std::atomic_bool holder_has_lock{ false };

  auto become_cfs_on_core0 = [&] {
    sched_param other{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
    pthread_setaffinity_np(pthread_self(), sizeof(one_core), &one_core);
  };

  std::vector<std::thread> hogs;
  for(int i = 0; i < 3; i++)
  {
    hogs.emplace_back([&] {
      become_cfs_on_core0();
      while(!stop)
      {
      }
    });
  }

  std::thread holder([&] {
    become_cfs_on_core0();
    m.lock();
    holder_has_lock = true;
    // 200 us of CPU work while holding the mutex
    const auto t0 = std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(200))
    {
    }
    m.unlock();
  });

  while(!holder_has_lock)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(50));  // let CFS threads run
  }
  const auto t0 = std::chrono::steady_clock::now();
  m.lock();
  const auto waited = std::chrono::steady_clock::now() - t0;
  m.unlock();
  stop = true;
  holder.join();
  for(auto& h : hogs)
  {
    h.join();
  }
  ASSERT_LT(std::chrono::duration_cast<std::chrono::microseconds>(waited).count(), 1000)
      << "waited " << std::chrono::duration_cast<std::chrono::microseconds>(waited).count()
      << " us: priority inheritance did not bound the wait";
}
```

- [ ] **Step 2: Register and run to verify it fails**

Add `write_mutex_tests.cpp` to both source lists in `tests/CMakeLists.txt`, and `include/data_tamer/details/write_mutex.hpp` to the `add_library(data_tamer ...)` list in `CMakeLists.txt`.

```bash
cmake --preset debug && cmake --build --preset debug
```

Expected: FAIL — `data_tamer/details/write_mutex.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

`include/data_tamer/details/write_mutex.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <system_error>

#if defined(__linux__)
#include <pthread.h>
#define DATA_TAMER_HAS_PI_MUTEX 1
#else
#define DATA_TAMER_HAS_PI_MUTEX 0
#pragma message("data_tamer: no PTHREAD_PRIO_INHERIT on this platform; WriteMutex is a plain mutex " \
                "and the snapshot thread's wait for a writer is not bounded by priority inheritance")
#endif

namespace DataTamer
{

/**
 * @brief Exclusive mutex with priority inheritance (PTHREAD_PRIO_INHERIT) where
 * the platform supports it. Satisfies the C++ Lockable requirements.
 *
 * Shared by the writer threads of a channel and by the snapshot thread. A
 * writer holding it is boosted to the priority of any waiter, so the snapshot
 * thread's wait is bounded by the writer's critical section rather than by
 * the scheduler.
 */
class WriteMutex
{
public:
  static constexpr bool kPriorityInheritance = (DATA_TAMER_HAS_PI_MUTEX == 1);

  /// Spin budget used by lockWithSpin() when called without an argument.
  /// Longer than any legal writer critical section, so the futex sleep is
  /// only reached when a writer was preempted mid-transaction.
  static constexpr std::int64_t kLockSpinNs = 2000;

  WriteMutex()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    const int rc = pthread_mutex_init(&mutex_, &attr);
    pthread_mutexattr_destroy(&attr);
    if(rc != 0)
    {
      throw std::system_error(rc, std::generic_category(), "pthread_mutex_init");
    }
#endif
  }

  ~WriteMutex()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutex_destroy(&mutex_);
#endif
  }

  WriteMutex(const WriteMutex&) = delete;
  WriteMutex& operator=(const WriteMutex&) = delete;
  WriteMutex(WriteMutex&&) = delete;
  WriteMutex& operator=(WriteMutex&&) = delete;

  void lock()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutex_lock(&mutex_);
#else
    mutex_.lock();
#endif
  }

  bool try_lock()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    return pthread_mutex_trylock(&mutex_) == 0;
#else
    return mutex_.try_lock();
#endif
  }

  void unlock()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutex_unlock(&mutex_);
#else
    mutex_.unlock();
#endif
  }

  /**
   * @brief Spin on try_lock() for at most spin_ns, then block in lock().
   * @return true if the call had to block (i.e. a writer held the mutex for
   *         longer than the spin budget). Callers use this to count contention.
   */
  bool lockWithSpin(std::int64_t spin_ns = kLockSpinNs)
  {
    if(try_lock())
    {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);
    while(std::chrono::steady_clock::now() < deadline)
    {
      if(try_lock())
      {
        return false;
      }
    }
    lock();
    return true;
  }

private:
#if DATA_TAMER_HAS_PI_MUTEX
  pthread_mutex_t mutex_;
#else
  std::mutex mutex_;
#endif
};

}  // namespace DataTamer
```

- [ ] **Step 4: Run the tests under all presets**

```bash
cmake --build --preset debug && ctest --preset debug -R WriteMutex
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Expected: 6 tests pass, `PriorityInheritanceBoundsTheWait` is SKIPPED without `CAP_SYS_NICE` (run `sudo setcap cap_sys_nice+ep build/debug/tests/datatamer_test` once to exercise it locally; it must then pass — and, as a control, temporarily changing `PTHREAD_PRIO_INHERIT` to `PTHREAD_PRIO_NONE` in the header must make it fail with a multi-millisecond wait). ASAN and TSAN clean — TSAN understands `pthread_mutex_*`, so no annotations are needed. Do not run the PI test under the sanitizers with the capability set; it is a scheduling test, not a memory test.

- [ ] **Step 5: Commit**

```bash
git add include/data_tamer/details/write_mutex.hpp tests/write_mutex_tests.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add WriteMutex, a priority-inheriting Lockable with spin-then-lock

Standalone component for the lock-free front end (spec §4.2). Not wired yet.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: `SnapshotPool` and `SnapshotRef`

**Files:**
- Create: `include/data_tamer/details/snapshot_pool.hpp`
- Create: `tests/snapshot_pool_tests.cpp`
- Modify: `tests/CMakeLists.txt`, `CMakeLists.txt:62-80`

**Interfaces:**
- Consumes: `DataTamer::Snapshot` from `data_sink.hpp`.
- Produces:
  ```cpp
  namespace DataTamer {
  struct PoolSlot { Snapshot snapshot; alignas(64) std::atomic<uint32_t> refs{0}; };
  class SnapshotPool : public std::enable_shared_from_this<SnapshotPool> {
   public:
    static constexpr size_t kDefaultCapacity = 64;
    SnapshotPool(size_t capacity, size_t payload_capacity, size_t mask_bytes);
    PoolSlot* tryAcquire();            // snapshot thread only; refs 0 -> 1; nullptr + exhausted++ if none
    static void addRef(PoolSlot*);     // refs++ (relaxed)
    static void release(PoolSlot*);    // refs-- (release)
    size_t capacity() const; size_t inUse() const;
    uint64_t exhausted() const;
  };
  class SnapshotRef {  // move-only handle; keeps slot and pool alive
   public:
    SnapshotRef(); SnapshotRef(std::shared_ptr<SnapshotPool>, PoolSlot*);  // takes one count
    SnapshotRef(SnapshotRef&&) noexcept; SnapshotRef& operator=(SnapshotRef&&) noexcept; ~SnapshotRef();
    SnapshotRef clone() const;         // refs++
    const Snapshot& operator*() const; const Snapshot* operator->() const;
    explicit operator bool() const; void reset();
  };
  }
  ```
  Plan 3 puts `SnapshotRef` into `BlockingConcurrentQueue<SnapshotRef>`; Plan 4 makes `takeSnapshot` serialize into `tryAcquire()->snapshot`.

- [ ] **Step 1: Write the failing tests**

`tests/snapshot_pool_tests.cpp`:

```cpp
#include "data_tamer/details/snapshot_pool.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

using namespace DataTamer;
using DataTamerTest::AllocCounter;

static std::shared_ptr<SnapshotPool> makePool(size_t capacity = 4)
{
  return std::make_shared<SnapshotPool>(capacity, /*payload_capacity=*/256, /*mask_bytes=*/2);
}

TEST(SnapshotPool, SlotsArePreallocated)
{
  auto pool = makePool(3);
  ASSERT_EQ(pool->capacity(), 3u);
  ASSERT_EQ(pool->inUse(), 0u);
  PoolSlot* s = pool->tryAcquire();
  ASSERT_NE(s, nullptr);
  ASSERT_GE(s->snapshot.payload.capacity(), 256u);
  ASSERT_EQ(s->snapshot.active_mask.size(), 2u);
  ASSERT_EQ(s->refs.load(), 1u);
  SnapshotPool::release(s);
  ASSERT_EQ(pool->inUse(), 0u);
}

TEST(SnapshotPool, AcquireExhaustsAndCounts)
{
  auto pool = makePool(2);
  PoolSlot* a = pool->tryAcquire();
  PoolSlot* b = pool->tryAcquire();
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(a, b);
  ASSERT_EQ(pool->tryAcquire(), nullptr);
  ASSERT_EQ(pool->exhausted(), 1u);
  SnapshotPool::release(a);
  ASSERT_EQ(pool->tryAcquire(), a);
}

TEST(SnapshotPool, AcquireIsRoundRobin)
{
  auto pool = makePool(3);
  PoolSlot* first = pool->tryAcquire();
  SnapshotPool::release(first);
  PoolSlot* second = pool->tryAcquire();
  // a freed slot is not immediately reused when others are free: the scan
  // continues from the last index, spreading wear and keeping recently
  // released slots readable a little longer for slow sinks.
  ASSERT_NE(second, first);
  SnapshotPool::release(second);
}

TEST(SnapshotPool, AcquireDoesNotAllocate)
{
  auto pool = makePool(8);
  AllocCounter::Scope scope;
  for(int i = 0; i < 1000; i++)
  {
    PoolSlot* s = pool->tryAcquire();
    ASSERT_NE(s, nullptr);
    s->snapshot.payload.resize(100);  // within capacity
    SnapshotPool::release(s);
  }
  ASSERT_EQ(scope.allocations(), 0u);
  ASSERT_EQ(scope.deallocations(), 0u);
}

TEST(SnapshotRef, IsMoveOnlyAndReleasesOnDestruction)
{
  static_assert(!std::is_copy_constructible_v<SnapshotRef>);
  static_assert(std::is_nothrow_move_constructible_v<SnapshotRef>);
  auto pool = makePool();
  PoolSlot* s = pool->tryAcquire();
  {
    SnapshotRef ref(pool, s);
    ASSERT_TRUE(ref);
    ASSERT_EQ(&*ref, &s->snapshot);
    ASSERT_EQ(s->refs.load(), 1u);
    SnapshotRef moved(std::move(ref));
    ASSERT_FALSE(ref);
    ASSERT_TRUE(moved);
    ASSERT_EQ(s->refs.load(), 1u);
  }
  ASSERT_EQ(s->refs.load(), 0u);
  ASSERT_EQ(pool->inUse(), 0u);
}

TEST(SnapshotRef, CloneAddsAReference)
{
  auto pool = makePool();
  PoolSlot* s = pool->tryAcquire();
  SnapshotRef a(pool, s);
  {
    SnapshotRef b = a.clone();
    ASSERT_EQ(s->refs.load(), 2u);
  }
  ASSERT_EQ(s->refs.load(), 1u);
  a.reset();
  ASSERT_EQ(s->refs.load(), 0u);
  ASSERT_FALSE(a);
}

TEST(SnapshotRef, KeepsPoolAliveAfterOwnerDropsIt)
{
  SnapshotRef survivor;
  {
    auto pool = makePool();
    PoolSlot* s = pool->tryAcquire();
    s->snapshot.payload.assign({ 1, 2, 3 });
    survivor = SnapshotRef(pool, s);
  }  // pool shared_ptr dropped here; the ref must keep it alive
  ASSERT_TRUE(survivor);
  ASSERT_EQ(survivor->payload.size(), 3u);
  survivor.reset();  // last reference: pool freed here (ASAN verifies)
}

// One producer acquiring slots and handing refs to N consumers that release
// them; run under TSAN. Also checks the "producer keeps its own hold until
// all consumers have a ref" protocol from spec §3 step 7.
TEST(SnapshotPool, ProducerAndConsumersUnderContention)
{
  constexpr int kConsumers = 3;
  constexpr int kRounds = 20000;
  auto pool = makePool(8);

  std::vector<std::vector<SnapshotRef>> mailboxes(kConsumers);
  std::vector<std::mutex> mailbox_mutex(kConsumers);
  std::atomic_bool done{ false };
  std::atomic<long> consumed{ 0 };

  std::vector<std::thread> consumers;
  for(int c = 0; c < kConsumers; c++)
  {
    consumers.emplace_back([&, c] {
      while(true)
      {
        std::vector<SnapshotRef> batch;
        {
          std::lock_guard lk(mailbox_mutex[c]);
          batch.swap(mailboxes[c]);
        }
        if(batch.empty())
        {
          if(done)
          {
            return;
          }
          std::this_thread::yield();
          continue;
        }
        for(auto& ref : batch)
        {
          // read the payload the producer wrote (EXPECT: ASSERT cannot abort
          // a thread body, only the enclosing function)
          EXPECT_EQ(ref->payload.size(), 8u);
          consumed++;
        }
      }  // refs released when batch is destroyed
    });
  }

  long produced = 0;
  long dropped = 0;
  for(int i = 0; i < kRounds; i++)
  {
    PoolSlot* s = pool->tryAcquire();
    if(!s)
    {
      dropped++;
      std::this_thread::yield();
      continue;
    }
    s->snapshot.payload.resize(8);
    s->snapshot.payload[0] = uint8_t(i);
    // producer holds refs == 1 while handing out clones
    for(int c = 0; c < kConsumers; c++)
    {
      SnapshotPool::addRef(s);
      std::lock_guard lk(mailbox_mutex[c]);
      mailboxes[c].emplace_back(pool, s);
    }
    SnapshotPool::release(s);  // producer's own hold, released last
    produced++;
  }
  done = true;
  for(auto& t : consumers)
  {
    t.join();
  }
  ASSERT_EQ(consumed.load(), produced * kConsumers);
  ASSERT_EQ(pool->inUse(), 0u);
  ASSERT_EQ(pool->exhausted(), uint64_t(dropped));
}
```

- [ ] **Step 2: Register and run to verify it fails**

Add `snapshot_pool_tests.cpp` to both lists in `tests/CMakeLists.txt` and `include/data_tamer/details/snapshot_pool.hpp` to the library sources in `CMakeLists.txt`.

```bash
cmake --preset debug && cmake --build --preset debug
```

Expected: FAIL — `data_tamer/details/snapshot_pool.hpp: No such file or directory`.

- [ ] **Step 3: Write the header**

`include/data_tamer/details/snapshot_pool.hpp`:

```cpp
#pragma once

#include "data_tamer/data_sink.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace DataTamer
{

/// One pre-allocated snapshot plus its intrusive reference count.
/// refs == 0 means free. Only the snapshot thread makes the 0 -> 1 transition.
struct PoolSlot
{
  Snapshot snapshot;
  alignas(64) std::atomic<uint32_t> refs{ 0 };
};

/**
 * @brief Fixed-size pool of PoolSlot, owned by a LogChannel (shared with every
 * SnapshotRef handed to a sink). Allocation happens only in the constructor.
 */
class SnapshotPool
{
public:
  static constexpr size_t kDefaultCapacity = 64;

  SnapshotPool(size_t capacity, size_t payload_capacity, size_t mask_bytes)
    : capacity_(capacity), slots_(new PoolSlot[capacity])
  {
    for(size_t i = 0; i < capacity_; i++)
    {
      slots_[i].snapshot.payload.reserve(payload_capacity);
      slots_[i].snapshot.active_mask.resize(mask_bytes);
    }
  }

  SnapshotPool(const SnapshotPool&) = delete;
  SnapshotPool& operator=(const SnapshotPool&) = delete;

  /**
   * @brief Find a free slot and take one reference on it.
   * Must be called from a single thread (the snapshot thread).
   * @return the slot, or nullptr (and exhausted() incremented) if all are in use.
   */
  PoolSlot* tryAcquire()
  {
    for(size_t n = 0; n < capacity_; n++)
    {
      scan_from_ = (scan_from_ + 1) % capacity_;
      PoolSlot& slot = slots_[scan_from_];
      // acquire: synchronizes with the last release() by a consumer, so that
      // consumer's reads of the slot happen-before our next writes into it.
      if(slot.refs.load(std::memory_order_acquire) == 0)
      {
        slot.refs.store(1, std::memory_order_relaxed);
        return &slot;
      }
    }
    exhausted_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }

  static void addRef(PoolSlot* slot) { slot->refs.fetch_add(1, std::memory_order_relaxed); }

  static void release(PoolSlot* slot) { slot->refs.fetch_sub(1, std::memory_order_release); }

  size_t capacity() const { return capacity_; }

  /// Number of slots with refs != 0. Diagnostic only; racy by nature.
  size_t inUse() const
  {
    size_t count = 0;
    for(size_t i = 0; i < capacity_; i++)
    {
      if(slots_[i].refs.load(std::memory_order_relaxed) != 0)
      {
        count++;
      }
    }
    return count;
  }

  uint64_t exhausted() const { return exhausted_.load(std::memory_order_relaxed); }

private:
  const size_t capacity_;
  std::unique_ptr<PoolSlot[]> slots_;
  size_t scan_from_ = 0;  // snapshot thread only
  std::atomic<uint64_t> exhausted_{ 0 };
};

/**
 * @brief Move-only handle to a PoolSlot. Holds one reference on the slot and a
 * shared_ptr to the pool, so a sink may keep it for as long as it likes: the
 * slot is simply not reused until the last handle is destroyed, and the pool
 * outlives the channel if necessary.
 */
class SnapshotRef
{
public:
  SnapshotRef() = default;

  /// Takes ownership of one already-counted reference on `slot`.
  SnapshotRef(std::shared_ptr<SnapshotPool> pool, PoolSlot* slot)
    : pool_(std::move(pool)), slot_(slot)
  {}

  SnapshotRef(SnapshotRef&& other) noexcept
    : pool_(std::move(other.pool_)), slot_(other.slot_)
  {
    other.slot_ = nullptr;
  }

  SnapshotRef& operator=(SnapshotRef&& other) noexcept
  {
    if(this != &other)
    {
      reset();
      pool_ = std::move(other.pool_);
      slot_ = other.slot_;
      other.slot_ = nullptr;
    }
    return *this;
  }

  SnapshotRef(const SnapshotRef&) = delete;
  SnapshotRef& operator=(const SnapshotRef&) = delete;

  ~SnapshotRef() { reset(); }

  /// Explicit copy: adds a reference.
  SnapshotRef clone() const
  {
    if(slot_)
    {
      SnapshotPool::addRef(slot_);
    }
    return SnapshotRef(pool_, slot_);
  }

  void reset()
  {
    if(slot_)
    {
      SnapshotPool::release(slot_);
      slot_ = nullptr;
    }
    pool_.reset();
  }

  const Snapshot& operator*() const { return slot_->snapshot; }
  const Snapshot* operator->() const { return &slot_->snapshot; }
  explicit operator bool() const { return slot_ != nullptr; }

private:
  std::shared_ptr<SnapshotPool> pool_;
  PoolSlot* slot_ = nullptr;
};

}  // namespace DataTamer
```

Note on alignment: the `alignas(64)` member makes `PoolSlot` itself over-aligned (`alignof(PoolSlot) == 64`), so `new PoolSlot[n]` goes through the C++17 aligned `operator new[](size_t, std::align_val_t)`. That happens once, in the constructor, outside any `AllocCounter::Scope`; the hook from Task 4 deliberately does not replace the aligned overloads because nothing on the per-snapshot path is over-aligned. `AcquireDoesNotAllocate` confirms that acquire/release and in-capacity `resize` allocate nothing at all.

- [ ] **Step 4: Run the tests under all presets**

```bash
cmake --build --preset debug && ctest --preset debug -R "SnapshotPool|SnapshotRef"
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Expected: 8 tests pass; ASAN clean (in particular `KeepsPoolAliveAfterOwnerDropsIt` would report a use-after-free if the ref did not pin the pool); TSAN clean on `ProducerAndConsumersUnderContention` (release/acquire on `refs` is what makes the consumers' reads and the producer's next writes ordered).

- [ ] **Step 5: Commit**

```bash
git add include/data_tamer/details/snapshot_pool.hpp tests/snapshot_pool_tests.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add SnapshotPool and SnapshotRef (pre-allocated, refcounted snapshot slots)

Standalone component for the lock-free front end (spec §6.1). Not wired yet.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: `ValuePtr` on function pointers, plus an `std::atomic<T>` constructor

**Files:**
- Modify: `include/data_tamer/values.hpp` (whole file)
- Create: `tests/value_ptr_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `CustomSerializer`, `SerializeMe::SerializeIntoBuffer/BufferSize`, `GetBasicType`, `IsNumericType`.
- Produces: `ValuePtr` with the same public API as today (`serialize`, `getSerializedSize`, `type`, `isVector`, `vectorSize`, `operator==`) and one new constructor `template <typename T> ValuePtr(const std::atomic<T>*)` for arithmetic/enum `T`. Plan 2 registers `LoggedValue<T>::value_` (an `std::atomic<T>`) through it.

Bytes on the wire do not change. The golden tests pin them.

- [ ] **Step 1: Write the failing tests**

`tests/value_ptr_tests.cpp`:

```cpp
#include "data_tamer/values.hpp"
#include "data_tamer/custom_types.hpp"
#include "../examples/geometry_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <vector>

using namespace DataTamer;

static std::vector<uint8_t> serialize(const ValuePtr& ptr)
{
  std::vector<uint8_t> out(ptr.getSerializedSize());
  SerializeMe::SpanBytes span(out);
  ptr.serialize(span);
  EXPECT_EQ(span.size(), 0u) << "serializer wrote fewer bytes than getSerializedSize()";
  return out;
}

TEST(ValuePtrGolden, Double)
{
  const double v = 1.5;  // 0x3FF8000000000000 little-endian
  ValuePtr ptr(&v);
  ASSERT_EQ(ptr.type(), BasicType::FLOAT64);
  ASSERT_FALSE(ptr.isVector());
  const std::vector<uint8_t> expected = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, Int16AndBoolAndEnum)
{
  const int16_t i = -2;  // 0xFFFE
  ValuePtr pi(&i);
  ASSERT_EQ(serialize(pi), (std::vector<uint8_t>{ 0xFE, 0xFF }));

  const bool b = true;
  ValuePtr pb(&b);
  ASSERT_EQ(pb.type(), BasicType::BOOL);
  ASSERT_EQ(serialize(pb), (std::vector<uint8_t>{ 0x01 }));

  enum Color : uint8_t { RED = 0, GREEN = 1, BLUE = 2 };
  const Color c = BLUE;
  ValuePtr pc(&c);
  ASSERT_EQ(pc.type(), BasicType::UINT8);
  ASSERT_EQ(serialize(pc), (std::vector<uint8_t>{ 0x02 }));
}

TEST(ValuePtrGolden, VectorOfFloat)
{
  const std::vector<float> v = { 1.0f, 2.0f };  // 0x3F800000, 0x40000000
  ValuePtr ptr(&v);
  ASSERT_EQ(ptr.type(), BasicType::FLOAT32);
  ASSERT_TRUE(ptr.isVector());
  ASSERT_EQ(ptr.vectorSize(), 0);  // dynamic
  const std::vector<uint8_t> expected = { 0x02, 0x00, 0x00, 0x00,  //
                                          0x00, 0x00, 0x80, 0x3F,  //
                                          0x00, 0x00, 0x00, 0x40 };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, ArrayOfUint16)
{
  const std::array<uint16_t, 3> a = { 1, 256, 0xABCD };
  ValuePtr ptr(&a);
  ASSERT_TRUE(ptr.isVector());
  ASSERT_EQ(ptr.vectorSize(), 3);
  // fixed-size arrays carry no length prefix
  const std::vector<uint8_t> expected = { 0x01, 0x00, 0x00, 0x01, 0xCD, 0xAB };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, CustomTypeThroughSerializer)
{
  TypesRegistry registry;
  auto serializer = registry.getSerializer<TestTypes::Point3D>();
  const TestTypes::Point3D p{ 1.5, -0.0, 2.0 };
  ValuePtr ptr(&p, serializer);
  ASSERT_EQ(ptr.type(), BasicType::OTHER);
  const std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F,  // 1.5
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,  // -0.0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40   // 2.0
  };
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrGolden, VectorOfCustomType)
{
  TypesRegistry registry;
  auto serializer = registry.getSerializer<TestTypes::Point3D>();
  const std::vector<TestTypes::Point3D> v = { { 1.5, 0, 0 } };
  ValuePtr ptr(&v, serializer);
  ASSERT_TRUE(ptr.isVector());
  std::vector<uint8_t> expected = { 0x01, 0x00, 0x00, 0x00 };  // count
  const uint8_t one_point[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F, 0, 0, 0, 0,
                                0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0, 0 };
  expected.insert(expected.end(), std::begin(one_point), std::end(one_point));
  ASSERT_EQ(serialize(ptr), expected);
}

TEST(ValuePtrAtomic, ScalarSerializesLikePlainValue)
{
  const std::atomic<double> a{ 1.5 };
  const double d = 1.5;
  ValuePtr pa(&a);
  ValuePtr pd(&d);
  ASSERT_EQ(pa.type(), BasicType::FLOAT64);
  ASSERT_EQ(pa.getSerializedSize(), sizeof(double));
  ASSERT_EQ(serialize(pa), serialize(pd));
  // same schema identity as the plain value
  ASSERT_TRUE(pa == pd);
}

TEST(ValuePtrAtomic, EnumAndInt)
{
  enum Mode : int32_t { A = 7 };
  const std::atomic<Mode> m{ A };
  ValuePtr pm(&m);
  ASSERT_EQ(pm.type(), BasicType::INT32);
  ASSERT_EQ(serialize(pm), (std::vector<uint8_t>{ 0x07, 0x00, 0x00, 0x00 }));

  const std::atomic<uint8_t> u{ 200 };
  ValuePtr pu(&u);
  ASSERT_EQ(serialize(pu), (std::vector<uint8_t>{ 0xC8 }));
}

TEST(ValuePtrAtomic, SeesLatestStore)
{
  std::atomic<int32_t> a{ 1 };
  ValuePtr ptr(&a);
  a.store(42, std::memory_order_relaxed);
  ASSERT_EQ(serialize(ptr), (std::vector<uint8_t>{ 0x2A, 0x00, 0x00, 0x00 }));
}

TEST(ValuePtr, IsMoveOnlyAndSmall)
{
  static_assert(!std::is_copy_constructible_v<ValuePtr>);
  static_assert(std::is_nothrow_move_constructible_v<ValuePtr>);
  // two function pointers + serializer shared_ptr + data pointer + small fields
  static_assert(sizeof(ValuePtr) <= 64, "ValuePtr grew; std::function crept back?");
}
```

- [ ] **Step 2: Register and run to see the failures**

Add `value_ptr_tests.cpp` to both lists in `tests/CMakeLists.txt`.

```bash
cmake --preset debug && cmake --build --preset debug 2>&1 | grep -E "error" | head -5
```

Expected: compile errors in `value_ptr_tests.cpp` — no `ValuePtr(const std::atomic<T>*)` constructor, and `sizeof(ValuePtr) <= 64` fails (two `std::function`s are 32 bytes each on libstdc++). The golden tests would pass on the current code, which is the point: they pin today's bytes.

- [ ] **Step 3: Rewrite `values.hpp`**

```cpp
#pragma once

#include <atomic>
#include <cstring>
#include <typeindex>

#include "data_tamer/custom_types.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

namespace DataTamer
{
using SerializeMe::has_TypeDefinition;

/**
 * @brief The ValuePtr is a non-owning pointer to a variable, together with
 * the two functions needed to serialize it. Type-erased through plain
 * function pointers (no std::function, no heap): the serializer for custom
 * types is kept alive by a shared_ptr member.
 */
class ValuePtr
{
public:
  using SerializeFn = void (*)(const void* value, const CustomSerializer* serializer,
                               SerializeMe::SpanBytes& dest);
  using SizeFn = size_t (*)(const void* value, const CustomSerializer* serializer);

  ValuePtr() = default;

  template <typename T, bool = true>
  ValuePtr(const T* pointer, CustomSerializer::Ptr type_info = {});

  /// Atomic scalar: serialized with a relaxed load. Same BasicType and wire
  /// bytes as the plain T.
  template <typename T, std::enable_if_t<IsNumericType<T>(), bool> = true>
  ValuePtr(const std::atomic<T>* pointer);

  template <template <class, class> class Container, class T, class... TArgs,
            std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
  ValuePtr(const Container<T, TArgs...>* vect);

  template <template <class, class> class Container, class T, class... TArgs,
            std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool> = true>
  ValuePtr(const Container<T, TArgs...>* vect, CustomSerializer::Ptr type_info);

  template <typename T, size_t N,
            std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
  ValuePtr(const std::array<T, N>* vect);

  template <typename T, size_t N,
            std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool> = true>
  ValuePtr(const std::array<T, N>* vect, CustomSerializer::Ptr type_info);

  ValuePtr(ValuePtr const& other) = delete;
  ValuePtr& operator=(ValuePtr const& other) = delete;

  ValuePtr(ValuePtr&& other) noexcept = default;
  ValuePtr& operator=(ValuePtr&& other) noexcept = default;

  [[nodiscard]] bool operator==(const ValuePtr& other) const;
  [[nodiscard]] bool operator!=(const ValuePtr& other) const { return !(*this == other); }

  void serialize(SerializeMe::SpanBytes& dest) const;

  [[nodiscard]] size_t getSerializedSize() const;

  [[nodiscard]] BasicType type() const { return type_; }
  [[nodiscard]] bool isVector() const { return is_vector_; }
  [[nodiscard]] uint16_t vectorSize() const { return array_size_; }

private:
  const void* v_ptr_ = nullptr;
  SerializeFn serialize_fn_ = nullptr;
  SizeFn size_fn_ = nullptr;
  CustomSerializer::Ptr serializer_;  // keeps the custom serializer alive
  std::type_index type_index_ = typeid(void);
  BasicType type_ = BasicType::OTHER;
  std::uint8_t memory_size_ = 0;
  bool is_vector_ = false;
  uint16_t array_size_ = 0;

  // ---- the type-erased implementations ----
  template <typename T>
  static void serializeNumeric(const void* v, const CustomSerializer*, SerializeMe::SpanBytes& dst)
  {
    std::memcpy(dst.data(), v, sizeof(T));
    dst.trimFront(sizeof(T));
  }
  template <typename T>
  static size_t sizeNumeric(const void*, const CustomSerializer*)
  {
    return sizeof(T);
  }

  template <typename T>
  static void serializeAtomic(const void* v, const CustomSerializer*, SerializeMe::SpanBytes& dst)
  {
    const T tmp = static_cast<const std::atomic<T>*>(v)->load(std::memory_order_relaxed);
    std::memcpy(dst.data(), &tmp, sizeof(T));
    dst.trimFront(sizeof(T));
  }

  static void serializeCustom(const void* v, const CustomSerializer* s, SerializeMe::SpanBytes& dst)
  {
    s->serialize(v, dst);
  }
  static size_t sizeCustom(const void* v, const CustomSerializer* s)
  {
    return s->serializedSize(v);
  }

  template <typename C>
  static void serializeContainer(const void* v, const CustomSerializer*, SerializeMe::SpanBytes& dst)
  {
    SerializeMe::SerializeIntoBuffer(dst, *static_cast<const C*>(v));
  }
  template <typename C>
  static size_t sizeContainer(const void* v, const CustomSerializer*)
  {
    return SerializeMe::BufferSize(*static_cast<const C*>(v));
  }

  template <typename C>
  static void serializeContainerCustom(const void* v, const CustomSerializer* s,
                                       SerializeMe::SpanBytes& dst)
  {
    const auto& vect = *static_cast<const C*>(v);
    SerializeMe::SerializeIntoBuffer(dst, uint32_t(vect.size()));
    for(const auto& value : vect)
    {
      s->serialize(&value, dst);
    }
  }
  template <typename C>
  static size_t sizeContainerCustom(const void* v, const CustomSerializer* s)
  {
    const auto& vect = *static_cast<const C*>(v);
    if(vect.empty())
    {
      return sizeof(uint32_t);
    }
    if(s->isFixedSize())
    {
      return sizeof(uint32_t) + vect.size() * s->serializedSize(&vect.front());
    }
    size_t tot = sizeof(uint32_t);
    for(const auto& value : vect)
    {
      tot += s->serializedSize(&value);
    }
    return tot;
  }

  template <typename A>
  static void serializeArrayCustom(const void* v, const CustomSerializer* s, SerializeMe::SpanBytes& dst)
  {
    for(const auto& value : *static_cast<const A*>(v))
    {
      s->serialize(&value, dst);
    }
  }
  template <typename A>
  static size_t sizeArrayCustom(const void* v, const CustomSerializer* s)
  {
    const auto& arr = *static_cast<const A*>(v);
    if(s->isFixedSize())
    {
      return arr.size() * s->serializedSize(&arr.front());
    }
    size_t tot = 0;
    for(const auto& value : arr)
    {
      tot += s->serializedSize(&value);
    }
    return tot;
  }
};

//------------------------------------------------------------
//------------------------------------------------------------
//------------------------------------------------------------

template <typename T, bool>
inline ValuePtr::ValuePtr(const T* pointer, CustomSerializer::Ptr type_info)
  : v_ptr_(pointer)
  , serializer_(std::move(type_info))
  , type_index_(typeid(T))
  , type_(GetBasicType<T>())
  , memory_size_(sizeof(T))
  , is_vector_(false)
{
  if(serializer_)
  {
    serialize_fn_ = &ValuePtr::serializeCustom;
    size_fn_ = &ValuePtr::sizeCustom;
  }
  else
  {
    serialize_fn_ = &ValuePtr::serializeNumeric<T>;
    size_fn_ = &ValuePtr::sizeNumeric<T>;
  }
}

template <typename T, std::enable_if_t<IsNumericType<T>(), bool>>
inline ValuePtr::ValuePtr(const std::atomic<T>* pointer)
  : v_ptr_(pointer)
  , serialize_fn_(&ValuePtr::serializeAtomic<T>)
  , size_fn_(&ValuePtr::sizeNumeric<T>)
  , type_index_(typeid(T))  // identical identity to the plain T, on purpose
  , type_(GetBasicType<T>())
  , memory_size_(sizeof(T))
  , is_vector_(false)
{
  static_assert(std::atomic<T>::is_always_lock_free, "atomic scalar must be lock-free");
}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect)
  : v_ptr_(vect)
  , serialize_fn_(&ValuePtr::serializeContainer<Container<T, TArgs...>>)
  , size_fn_(&ValuePtr::sizeContainer<Container<T, TArgs...>>)
  , type_index_(typeid(Container<T, TArgs...>))
  , type_(GetBasicType<T>())
  , memory_size_(sizeof(T))
  , is_vector_(true)
{}

template <template <class, class> class Container, class T, class... TArgs,
          std::enable_if_t<!has_TypeDefinition<Container<T, TArgs...>>::value, bool>>
inline ValuePtr::ValuePtr(const Container<T, TArgs...>* vect, CustomSerializer::Ptr type_info)
  : v_ptr_(vect)
  , serialize_fn_(&ValuePtr::serializeContainerCustom<Container<T, TArgs...>>)
  , size_fn_(&ValuePtr::sizeContainerCustom<Container<T, TArgs...>>)
  , serializer_(std::move(type_info))
  , type_index_(typeid(Container<T, TArgs...>))
  , type_(GetBasicType<T>())
  , memory_size_(sizeof(T))
  , is_vector_(true)
{}

template <typename T, size_t N, std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline ValuePtr::ValuePtr(const std::array<T, N>* array)
  : v_ptr_(array)
  , serialize_fn_(&ValuePtr::serializeContainer<std::array<T, N>>)
  , size_fn_(&ValuePtr::sizeContainer<std::array<T, N>>)
  , type_index_(typeid(std::array<T, N>))
  , type_(GetBasicType<T>())
  , is_vector_(true)
  , array_size_(N)
{}

template <typename T, size_t N, std::enable_if_t<!has_TypeDefinition<std::array<T, N>>::value, bool>>
inline ValuePtr::ValuePtr(const std::array<T, N>* array, CustomSerializer::Ptr type_info)
  : v_ptr_(array)
  , serialize_fn_(&ValuePtr::serializeArrayCustom<std::array<T, N>>)
  , size_fn_(&ValuePtr::sizeArrayCustom<std::array<T, N>>)
  , serializer_(std::move(type_info))
  , type_index_(typeid(std::array<T, N>))
  , type_(GetBasicType<T>())
  , is_vector_(true)
  , array_size_(N)
{}

inline bool ValuePtr::operator==(const ValuePtr& other) const
{
  return type_ == other.type_ && type_index_ == other.type_index_ &&
         is_vector_ == other.is_vector_ && array_size_ == other.array_size_;
}

inline void ValuePtr::serialize(SerializeMe::SpanBytes& dest) const
{
  serialize_fn_(v_ptr_, serializer_.get(), dest);
}

inline size_t ValuePtr::getSerializedSize() const
{
  return size_fn_(v_ptr_, serializer_.get());
}

}  // namespace DataTamer
```

Behavioural notes for the reviewer:
- Today's scalar path did `memcpy(dest, v_ptr_, memory_size_)` with a runtime size; the new `serializeNumeric<T>` uses `sizeof(T)`, a compile-time constant, so the compiler emits a single store. Bytes are identical (golden tests).
- Today's `std::array` of numerics goes through `SerializeMe::SerializeIntoBuffer(buffer, *array)`, whose `sizeof(T) == 1` branch has a pre-existing bug: `memcpy(vect.data(), buffer.data(), N)` has its arguments swapped (`SerializeMe.hpp:558`) — it copies the buffer *into* the const array, and since `vect.data()` is `const T*` it does not even compile when instantiated, which is why no test registers a `std::array<uint8_t, N>` today. This task keeps calling the same function, so nothing changes here; Plan 2 fixes it with its own golden test (`std::array<uint8_t, 3>`). Mention it in this commit's message so the reviewer is not surprised by the missing case.
- The `member initializer order` must match declaration order (`v_ptr_, serialize_fn_, size_fn_, serializer_, type_index_, type_, memory_size_, is_vector_, array_size_`) or `-Werror=reorder` fires.

- [ ] **Step 4: Run the whole suite under all presets**

```bash
cmake --build --preset debug && ctest --preset debug
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Expected: every existing test still passes (registration, custom types, parser round-trips all exercise `ValuePtr`), plus the 10 new `ValuePtr*` tests. Clean under both sanitizers.

- [ ] **Step 5: Re-run the micro-benchmark and note the delta**

```bash
cmake --build --preset release && ./build/release/benchmarks/dt_benchmark --benchmark_filter='DT_Doubles|DT_PoseType' --benchmark_min_time=0.5s
```

Expected: equal or slightly faster than the baseline; `allocs/op` unchanged (still 2 — the queue copy is untouched in this plan). Paste the output into the commit message body.

- [ ] **Step 6: Commit**

```bash
git add include/data_tamer/values.hpp tests/value_ptr_tests.cpp tests/CMakeLists.txt
git commit -m "refactor: ValuePtr uses function pointers instead of std::function; add std::atomic<T> constructor

Wire bytes are unchanged (pinned by golden tests). sizeof(ValuePtr) drops
from 112 to <= 64 bytes. The atomic constructor serializes with a relaxed
load and carries the same schema identity as the plain scalar.

Benchmark (DT_Doubles/1000, DT_PoseType/1000) before/after:
<paste>

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Delete `LoggedValue` move operations

**Files:**
- Modify: `include/data_tamer/logged_value.hpp:37-38`
- Create: `tests/logged_value_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `LoggedValue<T>` is neither copyable nor movable. `createLoggedValue` already returns `std::shared_ptr`, so no call site changes.

Why now: today a moved-from `LoggedValue` leaves the channel holding `&value_` of the dead object (review finding). Plan 2 changes `value_`'s type; deleting the move first keeps that change from silently depending on it.

- [ ] **Step 1: Write the failing test**

`tests/logged_value_tests.cpp`:

```cpp
#include "data_tamer/data_tamer.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using namespace DataTamer;

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
  std::shared_ptr<LoggedValue<float>> moved = std::move(v);  // moving the handle is fine
  ASSERT_FALSE(v);
  ASSERT_EQ(moved->get(), 1.0f);
}
```

- [ ] **Step 2: Register and run to verify it fails**

Add `logged_value_tests.cpp` to both lists in `tests/CMakeLists.txt`.

```bash
cmake --preset debug && cmake --build --preset debug 2>&1 | grep -E "static assertion failed" | head -2
```

Expected: `static assertion failed: moving a LoggedValue would leave the channel with a dangling pointer`.

- [ ] **Step 3: Delete the move operations**

In `include/data_tamer/logged_value.hpp`, replace lines 37–38:

```cpp
  LoggedValue(LoggedValue&& other) = default;
  LoggedValue& operator=(LoggedValue&& other) = default;
```

with:

```cpp
  // The channel holds a pointer to value_; moving would leave it dangling.
  LoggedValue(LoggedValue&& other) = delete;
  LoggedValue& operator=(LoggedValue&& other) = delete;
```

- [ ] **Step 4: Run the suite under all presets**

```bash
cmake --build --preset debug && ctest --preset debug
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Expected: all pass. If any in-tree code moved a `LoggedValue` by value, it now fails to compile — replace it with the `shared_ptr` handle as in the second test.

- [ ] **Step 5: Commit**

```bash
git add include/data_tamer/logged_value.hpp tests/logged_value_tests.cpp tests/CMakeLists.txt
git commit -m "fix: delete LoggedValue move operations

A moved-from LoggedValue left the channel pointing at its old value_.
createLoggedValue already returns a shared_ptr, which remains movable.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review against the spec

- **§10 step 0** (sanitizer CI, allocation hook): Tasks 1–4. The pre-existing TSAN races in the fixture and the ROS-gating bug were not in the spec; both are prerequisites for a green gate and are fixed here.
- **§10.1 step 0** (baseline: `set` loop, 1/2/4 sinks, concurrent writer, `allocs/op`, harness, committed numbers): Task 5. The harness omits the channel counters that do not exist yet; Plans 2–4 add them.
- **§10 step 1** (`WriteMutex`, attribute check, Lockable, spin-then-lock, PI bound test skipped without privilege): Task 6. The "attribute check" is expressed as `kPriorityInheritance` plus the behavioural PI test, since a `pthread_mutex_t`'s protocol cannot be queried after init.
- **§10 step 2** (`SnapshotPool` + `SnapshotRef`, exhaustion, move semantics, producer + N consumers under TSAN, zero allocations): Task 7. `kDefaultCapacity = 64` matches the spec's revised default.
- **§10 step 3** (`ValuePtr` function pointers, atomic ctor, golden buffers, `LoggedValue` move deleted): Tasks 8–9.
- **§10.1 steps 1–3** (micro-benchmarks for the mutex and pool, re-run after `ValuePtr`): dedicated mutex/pool measurements were deferred from Plan 1. Plan 2 closes this item in `a14c1ff` with ten standalone cases: uncontended locks, 0/1/5 µs writer holds, three pool occupancy levels, reference clone/destruction and a two-consumer round trip. Results are in `docs/benchmarks/2026-09-plan2.md`; timing-free unit tests are correctness checks, not substitutes for these measurements.
- Type consistency: `WriteMutex::lockWithSpin(std::int64_t)` returns `bool` (Task 6, used in Plan 2); `SnapshotPool::tryAcquire()` returns `PoolSlot*`, `addRef`/`release` are static and take `PoolSlot*` (Task 7, used in Plan 3/4); `ValuePtr(const std::atomic<T>*)` is enabled for `IsNumericType<T>()` (Task 8, used in Plan 2); `DummySink::latestSnapshot()/snapshotsCount()/schemasCount()/firstSchemaHash()` (Task 2, used by every later plan's tests); `AllocCounter::Scope::allocations()` (Task 4).
- Placeholders: the baseline document has `<paste …>` markers by design — they are filled with tool output at execution time, not with content the plan could know in advance.
