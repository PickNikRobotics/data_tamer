// The real-time API under RealtimeSanitizer (-fsanitize=realtime, Clang 20 or later).
// Each cycle runs in a [[clang::nonblocking]] function, as a control loop would, and
// RTSan aborts the test at any allocation, lock or system call in it. The calls the
// real-time contract allows (CLAUDE.md, "Invariants") are made inside
// details::AllowedRealTimeCall; the death tests check that RTSan is active, again after
// an allowed call, and that nothing else is hidden, a serializer's allocation included.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"
#include "gate.hpp"
#include "hang_watchdog.hpp"
#include "mcap_test_utils.hpp"
#include "observed_thread.hpp"
#include "paused_serializer.hpp"
#include "test_sinks.hpp"
#include "wait_for_sleeping_thread.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(__has_feature) || !__has_feature(realtime_sanitizer)
#error "rtsan_tests.cpp must be built with -fsanitize=realtime"
#endif

using namespace DataTamer2;
using DataTamerTest::CustomValue;

namespace
{
// RTSan prints its report through llvm-symbolizer, which waits for debuginfod (set by
// Ubuntu's profile) on a machine without network access.
const bool kNoDebuginfod = (unsetenv("DEBUGINFOD_URLS"), true);

/// Writes a CustomValue, 8 bytes. While `hold` is set, serialize() spins until the test
/// clears it: the snapshot keeps the write mutex and the epoch without a blocking call.
/// With `allocate` it breaks the contract.
struct HoldingSerializer : DataTamerTest::PausedSerializer
{
  void serialize(const void* source, SerializeMe::SpanBytes& bytes) const override
  {
    if(allocate)
    {
      static std::string* volatile kept;
      kept = new std::string(64, 'x');
      delete kept;
    }
    if(hold)
    {
      holding = true;
      while(hold)
      {
      }
    }
    PausedSerializer::serialize(source, bytes);
  }
  mutable std::atomic<bool> hold{ false };
  mutable std::atomic<bool> holding{ false };
  bool allocate = false;
};

std::set<pid_t> threadIds()
{
  std::set<pid_t> ids;
  for(const auto& entry : std::filesystem::directory_iterator("/proc/self/task"))
  {
    ids.insert(static_cast<pid_t>(std::stoi(entry.path().filename().string())));
  }
  return ids;
}

/// A channel with the three kinds of value: a LoggedValue, a registered double and a
/// custom type.
struct Loop
{
  std::shared_ptr<LogChannel> channel = LogChannel::create("rtsan");
  std::shared_ptr<LoggedValue<double>> value = channel->createLoggedValue<double>("v");
  double plain = 1.0;
  RegistrationID id = channel->registerValue("plain", &plain);
  CustomValue custom;
  std::shared_ptr<HoldingSerializer> serializer = std::make_shared<HoldingSerializer>();
  RegistrationID custom_id = channel->registerCustomValue("custom", &custom, serializer);
  SinkWorker* worker = nullptr;
  MCAPRingSink* ring = nullptr;
  std::atomic<pid_t> worker_tid{ 0 };

  /// Attaches `sink` (by default a DummySink delivered by drain()) and starts logging.
  void start(std::shared_ptr<SinkWorker> sink = DataTamerTest::manual<DummySink>())
  {
    worker = sink.get();
    channel->addDataSink(std::move(sink));
    channel->startLogging();
  }

  /// start() with a DummySink delivered by its own thread, whose id it records.
  void startWithWorkerThread()
  {
    const auto before = threadIds();
    auto sink = DummySink::create();
    for(const auto tid : threadIds())
    {
      if(!before.count(tid))
      {
        worker_tid = tid;
      }
    }
    start(std::move(sink));
  }

  /// True once the worker thread sleeps, waiting for a snapshot.
  bool workerSleeps() const
  {
    const std::atomic<bool> finished{ false };
    return DataTamerTest::waitForSleepingThread(worker_tid, finished);
  }
};

/// One control cycle: every call of the real-time API (llm_user_manual.md, "Threading").
SnapshotResult cycle(const Loop& loop) noexcept [[clang::nonblocking]]
{
  loop.value->set(loop.value->get() + 1.0);
  (void)loop.channel->trySetEnabled(loop.id, true);
  (void)loop.channel->isEnabled(loop.id);
  loop.value->setEnabled(true);
  (void)loop.value->isEnabled();
  (void)loop.channel->writeLockContended();
  (void)loop.channel->writeLockWaitMaxNs();
  (void)loop.channel->payloadReallocations();
  (void)loop.channel->droppedOversize();
  (void)loop.channel->serializeFailures();
  (void)loop.channel->snapshotAttempts();
  (void)loop.channel->snapshotsAccepted();
  (void)loop.channel->poolExhausted();
  if(loop.worker)
  {
    (void)loop.worker->delivered();
    (void)loop.worker->errors();
    (void)loop.worker->queueHighWater();
  }
  if(loop.ring)
  {
    (void)loop.ring->requestDump();
  }
  const auto result = loop.channel->tryTakeSnapshot();
  (void)accepted(result);
  return result;
}

/// Runs `count` cycles on a new thread, as a control loop does, and counts the results.
std::array<int, 8> runCycles(const Loop& loop, int count)
{
  std::array<int, 8> results{};
  std::thread([&] {
    for(int i = 0; i < count; ++i)
    {
      results[static_cast<size_t>(cycle(loop))]++;
    }
  }).join();
  return results;
}

int count(const std::array<int, 8>& results, SnapshotResult result)
{
  return results[static_cast<size_t>(result)];
}

/// Runs one cycle on this thread. Its serializer holds the snapshot, with the write
/// mutex and the epoch, until `op`, started on another thread, sleeps.
template <typename Op>
SnapshotResult holdSnapshotWhile(const Loop& loop, Op op)
{
  loop.serializer->holding = false;
  loop.serializer->hold = true;
  std::thread helper([&] {
    while(!loop.serializer->holding)
    {
    }
    DataTamerTest::ObservedThread blocked(op);
    EXPECT_TRUE(blocked.sleeps());
    loop.serializer->hold = false;
  });
  const auto result = cycle(loop);
  helper.join();
  return result;
}
}  // namespace

// Logging disabled by configuration: no sink is ever attached.
TEST(RealtimeSanitizer, NothingIsReportedWithoutSinks)
{
  Loop loop;
  EXPECT_EQ(count(runCycles(loop, 100), SnapshotResult::no_sinks), 100);
  EXPECT_FALSE(loop.channel->isLoggingStarted());
}

TEST(RealtimeSanitizer, NothingIsReportedWithAManualSink)
{
  Loop loop;
  loop.start();
  for(int round = 0; round < 10; ++round)
  {
    EXPECT_EQ(count(runCycles(loop, 50), SnapshotResult::ok), 50);
    loop.worker->drain();
  }
}

// The worker sleeps before each cycle: its push owes the futex wake, which comes after
// the write mutex is released (SinkWorker::Push::wake_owed).
TEST(RealtimeSanitizer, WakingASleepingWorkerIsAllowed)
{
  Loop loop;
  loop.startWithWorkerThread();
  for(int round = 0; round < 5; ++round)
  {
    ASSERT_TRUE(loop.workerSleeps());
    EXPECT_EQ(count(runCycles(loop, 1), SnapshotResult::ok), 1);
  }
  loop.worker->drain();
  EXPECT_EQ(loop.worker->delivered(), 5u);
}

// Two sinks: the first push gets a clone of the snapshot, the second the snapshot.
TEST(RealtimeSanitizer, NothingIsReportedWithMCAPSinks)
{
  DataTamerTest::ScratchDir dir("rtsan");
  Loop loop;
  auto file = MCAPSink::create(dir.file("run.mcap"));
  MCAPRingOptions options;
  options.filepath = dir.file("ring.mcap");
  options.capacity_bytes = size_t(1) << 20;
  auto ring = MCAPRingSink::create(options);
  loop.channel->addDataSink(file);
  loop.channel->addDataSink(ring);
  loop.channel->startLogging();
  loop.ring = &ring->as<MCAPRingSink>();
  EXPECT_EQ(count(runCycles(loop, 200), SnapshotResult::ok), 200);
  ring->stop();
  file->stop();
  EXPECT_EQ(file->errors(), 0u);
}

TEST(RealtimeSanitizer, AWriterInScopedWriteMakesItBlocked)
{
  Loop loop;
  loop.start();
  DataTamerTest::Gate gate;
  std::thread writer([&] {
    auto transaction = loop.channel->scopedWrite();
    gate.pause();
  });
  EXPECT_TRUE(gate.waitEntered());
  EXPECT_EQ(count(runCycles(loop, 20), SnapshotResult::blocked), 20);
  gate.release();
  writer.join();
}

// The snapshot holds the write mutex while a writer sleeps on it: the release is a
// FUTEX_UNLOCK_PI call.
TEST(RealtimeSanitizer, ReleasingTheWriteMutexToASleepingWriterIsAllowed)
{
  DataTamerTest::expectFinishes([] {
    Loop loop;
    loop.start();
    EXPECT_EQ(holdSnapshotWhile(loop, [&] { auto tx = loop.channel->scopedWrite(); }),
              SnapshotResult::ok);
  });
}

// unregister() waits for the snapshot in progress: the snapshot notifies it.
TEST(RealtimeSanitizer, NotifyingAWaitingControlOperationIsAllowed)
{
  DataTamerTest::expectFinishes([] {
    Loop loop;
    loop.start();
    EXPECT_EQ(holdSnapshotWhile(loop, [&] { loop.channel->unregister(loop.id); }),
              SnapshotResult::ok);
  });
}

namespace
{
SnapshotResult blockingSnapshot(LogChannel& channel) noexcept [[clang::nonblocking]]
{
  // Not real-time safe (the compile-time check, -Wfunction-effects, rejects it).
  return channel.takeSnapshot();
}

void setVector(LoggedValue<std::vector<double>>& value) noexcept [[clang::nonblocking]]
{
  value.set({});  // locks the write mutex
}
}  // namespace

TEST(RealtimeSanitizerDeathTest, AnAllocatingSerializerIsReported)
{
  DataTamerTest::useThreadsafeDeathTests();
  EXPECT_DEATH(
      {
        Loop loop;
        loop.start();
        loop.serializer->allocate = true;
        (void)cycle(loop);
      },
      "unsafe function `malloc`");
}

// An allowed call turns RTSan back on. The first cycle makes all three, on this thread:
// it wakes the sleeping worker, notifies the waiting unregister() and releases the
// write mutex.
TEST(RealtimeSanitizerDeathTest, AnAllocationAfterAnAllowedCallIsReported)
{
  DataTamerTest::useThreadsafeDeathTests();
  auto body = [] {
    Loop loop;
    loop.startWithWorkerThread();
    ASSERT_TRUE(loop.workerSleeps());
    EXPECT_EQ(holdSnapshotWhile(loop, [&] { loop.channel->unregister(loop.id); }),
              SnapshotResult::ok);
    loop.serializer->allocate = true;
    (void)cycle(loop);
  };
  const auto limit = std::chrono::seconds(10);
  EXPECT_DEATH(DataTamerTest::runWithWatchdog(limit, body), "unsafe function `malloc`");
}

TEST(RealtimeSanitizerDeathTest, TakeSnapshotIsReported)
{
  DataTamerTest::useThreadsafeDeathTests();
  EXPECT_DEATH(
      {
        Loop loop;
        loop.start();
        (void)blockingSnapshot(*loop.channel);
      },
      "blocking function `DataTamer2::LogChannel::takeSnapshot`");
}

// Before startLogging() it is not real-time: the first call starts logging.
TEST(RealtimeSanitizerDeathTest, TryTakeSnapshotBeforeStartLoggingIsReported)
{
  DataTamerTest::useThreadsafeDeathTests();
  EXPECT_DEATH(
      {
        Loop loop;
        loop.channel->addDataSink(DataTamerTest::manual<DummySink>());
        (void)cycle(loop);
      },
      "blocking function `DataTamer2::LogChannel::tryTakeSnapshot`");
}

TEST(RealtimeSanitizerDeathTest, NonScalarLoggedValueSetIsReported)
{
  DataTamerTest::useThreadsafeDeathTests();
  EXPECT_DEATH(
      {
        auto channel = LogChannel::create("rtsan");
        auto value = channel->createLoggedValue<std::vector<double>>("vector");
        setVector(*value);
      },
      "unsafe function `pthread_mutex_lock`");
}
