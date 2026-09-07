#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <new>
#include <thread>

using namespace DataTamer;

//------------------------------------------------------------------
// Count heap allocations performed by the calling thread only.
// Replacing the global operator new affects the whole test binary, but the
// counter is only incremented while t_count_allocs is true on this thread.
static std::atomic<long> g_alloc_count{ 0 };
static thread_local bool t_count_allocs = false;

void* operator new(std::size_t size)
{
  if(t_count_allocs)
  {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  }
  if(void* ptr = std::malloc(size == 0 ? 1 : size))
  {
    return ptr;
  }
  throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept
{
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
  std::free(ptr);
}

//------------------------------------------------------------------
// A sink whose consumer thread can be held back, to fill the queue.
class BlockingSink : public DataSinkBase
{
public:
  explicit BlockingSink(size_t queue_size, size_t reserved_bytes = 0)
    : DataSinkBase(queue_size, reserved_bytes)
  {}

  ~BlockingSink() override
  {
    release = true;
    stopThread();
  }

  void addChannel(std::string const&, Schema const&) override {}

  bool storeSnapshot(const Snapshot&) override
  {
    while(!release)
    {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    stored_count++;
    return true;
  }

  std::atomic<bool> release{ false };
  std::atomic<int> stored_count{ 0 };
};

static void waitFor(std::function<bool()> predicate,
                    std::chrono::milliseconds timeout = std::chrono::seconds(2))
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while(!predicate() && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

//------------------------------------------------------------------

TEST(RealTime, StartRecordingRegistersSchemaBeforeFirstSnapshot)
{
  auto channel = LogChannel::create("rt_start");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  double value = 1.0;
  channel->registerValue("value", &value);

  {
    std::scoped_lock lk(sink->schema_mutex_);
    ASSERT_EQ(sink->schemas.count(channel->getSchema().hash), 0u);
  }

  channel->startRecording();

  {
    std::scoped_lock lk(sink->schema_mutex_);
    ASSERT_EQ(sink->schemas.count(channel->getSchema().hash), 1u);
    ASSERT_EQ(sink->snapshots_count[channel->getSchema().hash], 0);
  }

  // schema is frozen
  int late = 0;
  ASSERT_THROW(channel->registerValue("late", &late), std::runtime_error);

  // a second call is a no-op
  channel->startRecording();

  // a sink added afterwards gets the schema immediately
  auto late_sink = std::make_shared<DummySink>();
  channel->addDataSink(late_sink);
  {
    std::scoped_lock lk(late_sink->schema_mutex_);
    ASSERT_EQ(late_sink->schemas.count(channel->getSchema().hash), 1u);
  }
  ASSERT_EQ(channel->getNumberOfSinks(), 2u);
}

TEST(RealTime, TryTakeSnapshotDoesNotBlockOnWriteMutex)
{
  auto channel = LogChannel::create("rt_try");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  double value = 1.0;
  channel->registerValue("value", &value);
  channel->startRecording();

  {
    std::unique_lock lk(channel->writeMutex());
    // would deadlock if tryTakeSnapshot blocked
    ASSERT_FALSE(channel->tryTakeSnapshot());
  }
  ASSERT_TRUE(channel->tryTakeSnapshot());

  const auto hash = channel->getSchema().hash;
  waitFor([&] {
    std::scoped_lock lk(sink->schema_mutex_);
    return sink->snapshots_count[hash] == 1;
  });
  std::scoped_lock lk(sink->schema_mutex_);
  ASSERT_EQ(sink->snapshots_count[hash], 1);
}

TEST(RealTime, FullQueueDropsSnapshotsInsteadOfGrowing)
{
  constexpr size_t queue_size = 4;
  auto channel = LogChannel::create("rt_queue");
  auto sink = std::make_shared<BlockingSink>(queue_size);
  channel->addDataSink(sink);

  double value = 1.0;
  channel->registerValue("value", &value);
  channel->startRecording();

  ASSERT_EQ(sink->queueSize(), queue_size);

  int accepted = 0;
  for(int i = 0; i < 10; i++)
  {
    accepted += channel->takeSnapshot() ? 1 : 0;
  }
  // the consumer is blocked inside storeSnapshot, so exactly queue_size
  // snapshots could be stored in the pool. The others were dropped.
  ASSERT_EQ(accepted, int(queue_size));
  ASSERT_EQ(sink->droppedSnapshotsCount(), 10u - queue_size);

  sink->release = true;
  waitFor([&] { return sink->stored_count == int(queue_size); });
  ASSERT_EQ(sink->stored_count, int(queue_size));

  // once drained, pushing works again
  ASSERT_TRUE(channel->takeSnapshot());
  waitFor([&] { return sink->stored_count == int(queue_size) + 1; });
  ASSERT_EQ(sink->stored_count, int(queue_size) + 1);
}

TEST(RealTime, SteadyStateSnapshotDoesNotAllocate)
{
  constexpr size_t queue_size = 8;
  auto channel = LogChannel::create("rt_alloc");
  // reserve enough bytes for the payload below, so that no slot has to grow
  auto sink = std::make_shared<BlockingSink>(queue_size, 4096);
  sink->release = true;
  channel->addDataSink(sink);

  std::vector<double> vect(100, 1.0);
  std::array<float, 16> arr{};
  double scalar = 0;
  int32_t counter = 0;
  channel->registerValue("vect", &vect);
  channel->registerValue("arr", &arr);
  channel->registerValue("scalar", &scalar);
  channel->registerValue("counter", &counter);
  channel->startRecording();

  // warm up: registers this thread as a producer in the queue and lets every
  // slot see the payload once.
  for(size_t i = 0; i < queue_size * 4; i++)
  {
    channel->takeSnapshot();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  waitFor([&] { return sink->stored_count == int(queue_size * 4); });

  g_alloc_count = 0;
  t_count_allocs = true;
  int taken = 0;
  for(int i = 0; i < 1000; i++)
  {
    scalar += 1.0;
    counter++;
    taken += channel->tryTakeSnapshot() ? 1 : 0;
    if((i % 4) == 3)
    {
      // leave room for the consumer, so that most snapshots are accepted
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  }
  t_count_allocs = false;

  ASSERT_GT(taken, 0);
  ASSERT_EQ(g_alloc_count.load(), 0) << "takeSnapshot allocated on the caller thread";
}
