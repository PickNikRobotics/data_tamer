#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/details/snapshot_pool.hpp"
#include "alloc_counter.hpp"
#include "test_sinks.hpp"
#include "geometry_types.hpp"

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace DataTamer2;

namespace
{
class CapacitySink : public DataSink
{
public:
  SnapshotRef last;
  size_t received = 0;
  void onSchema(const Schema&) override {}
  void onSnapshot(const SnapshotRef& snapshot) override
  {
    ++received;
    last = snapshot.clone();
  }
};

/// Manual delivery, with the retained reference released after each drain.
struct CapacityWorker : DataTamerTest::Attached<CapacitySink>
{
  CapacityWorker() : Attached(DataTamerTest::manual<CapacitySink>().worker) {}
  void drain() const
  {
    worker->drain();
    sink->last.reset();
  }
  /// Deliver and keep the last reference, so the slot stays in use.
  void deliver() const { worker->drain(); }
};

struct SizedValue
{
  uint64_t value = 42;
};
class SizedSerializer : public CustomSerializer
{
public:
  size_t size = 8;
  mutable size_t calls = 0;
  bool throw_size = false, throw_serialize = false;
  std::vector<double>* grow = nullptr;  // grown by one element in serialize()
  const std::string& typeName() const override
  {
    static const std::string name = "SizedValue";
    return name;
  }
  bool isFixedSize() const override { return false; }
  size_t serializedSize(const void*) const override
  {
    ++calls;
    if(throw_size)
    {
      throw std::runtime_error("size");
    }
    return size;
  }
  void serialize(const void* source, SerializeMe::SpanBytes& bytes) const override
  {
    if(throw_serialize)
    {
      throw std::runtime_error("serialize");
    }
    if(grow)
    {
      // Within its capacity: the value changes size after sizing, without allocating.
      grow->resize(grow->size() + 1);
    }
    const auto value = static_cast<const SizedValue*>(source)->value;
    std::memcpy(bytes.data(), &value, 8);
    bytes.trimFront(8);
  }
};
}  // namespace

TEST(ChannelCapacity, StartLoggingWithoutSinkReservesAndRejectsSetters)
{
  auto channel = LogChannel::create("capacity");
  auto value = channel->createLoggedValue<std::vector<double>>("value", { 1.0 });
  EXPECT_THROW(channel->setPoolCapacity(0), std::invalid_argument);
  EXPECT_THROW(channel->setPoolCapacity(std::numeric_limits<size_t>::max()),
               std::length_error);
  EXPECT_THROW(channel->setPayloadCapacity(std::numeric_limits<size_t>::max()),
               std::length_error);
  channel->setPoolCapacity(2);
  channel->setPayloadCapacity(0);  // Automatic minimum remains valid.
  // Without sinks nothing is captured and nothing is frozen.
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::no_sinks);
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::no_sinks);
  EXPECT_FALSE(channel->isLoggingStarted());
  channel->setPoolCapacity(2);
  channel->startLogging();  // An explicit start freezes even without sinks.
  EXPECT_TRUE(channel->isLoggingStarted());
  EXPECT_THROW(channel->setPoolCapacity(2), std::runtime_error);
  EXPECT_THROW(channel->setPayloadCapacity(256), std::runtime_error);
  value->set(std::vector<double>(1024, 3.0));
  CapacityWorker sink;
  channel->addDataSink(sink);
  // Must use the reservation made by startLogging().
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::oversize);
  EXPECT_EQ(channel->droppedOversize(), 1u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
}

TEST(ChannelCapacity, TryTakeSnapshotReportsBlockedInsteadOfWaiting)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto value = channel->createLoggedValue<uint64_t>("value", 1);
  channel->setPoolCapacity(1);  // a leaked slot would show as pool_exhausted
  channel->addDataSink(sink);
  channel->startLogging();
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  sink.drain();
  {
    auto tx = channel->scopedWrite();  // holds the write mutex
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::blocked);
  }
  EXPECT_EQ(channel->writeLockContended(), 1u);
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);  // slot was released
  sink.drain();
  EXPECT_EQ(channel->poolExhausted(), 0u);
}

// The RT path never throws for sizes it cannot store: an impossible
// serializedSize() is reported as oversize, and the whole call stays free of
// allocations on the calling thread, including its rejection paths.
TEST(ChannelCapacity, TryTakeSnapshotDoesNotThrowOrAllocateOnRejection)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto serializer = std::make_shared<SizedSerializer>();
  SizedValue value;
  channel->registerCustomValue("value", &value, serializer);
  channel->registerCustomValue("second", &value, serializer);
  channel->setPoolCapacity(1);
  channel->addDataSink(sink);
  channel->startLogging();
  sink.drain();  // The first delivery pass sizes the worker's pass list.
  size_t allocations = 0;
  {
    DataTamerTest::AllocCounter::Scope scope;
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::pool_exhausted);
    sink.drain();
    serializer->size = std::numeric_limits<size_t>::max();  // would overflow the sum
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::oversize);
    serializer->size = 1000;  // fits a vector, not the slot
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::oversize);
    serializer->size = 8;
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
    sink.drain();
    allocations = scope.allocations();
  }
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(channel->droppedOversize(), 2u);
  EXPECT_EQ(channel->poolExhausted(), 1u);
  serializer->size = std::numeric_limits<size_t>::max();
  EXPECT_THROW((void)channel->takeSnapshot(), std::length_error) << "the blocking path "
                                                                    "keeps validating "
                                                                    "sizes";
}

TEST(ChannelCapacity, TwoSlotsExhaustBeforeSizingAndRecover)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto serializer = std::make_shared<SizedSerializer>();
  SizedValue value;
  channel->registerCustomValue("value", &value, serializer);
  channel->setPoolCapacity(2);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  const auto calls = serializer->calls;
  EXPECT_NE(channel->takeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(serializer->calls, calls);
  EXPECT_EQ(channel->poolExhausted(), 1u);
  sink.drain();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(DataTamerTest::droppedBy(*channel, sink), 0u);
}

TEST(ChannelCapacity, TryRejectsOversizeWhileTakeGrowsEachSlot)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto value = channel->createLoggedValue<std::vector<double>>("value", { 1.0 });
  channel->addDataSink(sink);
  channel->setPoolCapacity(2);
  channel->setPayloadCapacity(256);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  value->set(std::vector<double>(1024, 3.0));
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::oversize);
  EXPECT_EQ(channel->droppedOversize(), 1u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(channel->payloadReallocations(), 1u);
  sink.drain();
  EXPECT_EQ(channel->tryTakeSnapshot(),
            SnapshotResult::oversize);  // Other slot is still small.
  ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);  // Grown slot is usable.
  sink.deliver();
  ASSERT_TRUE(sink->last);
  EXPECT_EQ(sink->last->payload.size(), 8196u);
  uint32_t count = 0;
  double first = 0;
  std::memcpy(&count, sink->last->payload.data(), 4);
  std::memcpy(&first, sink->last->payload.data() + 4, 8);
  EXPECT_EQ(count, 1024u);
  EXPECT_EQ(first, 3.0);
  sink->last.reset();
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(channel->payloadReallocations(), 2u);
  sink.drain();
  value->set(std::vector<double>(1500, 4.0));  // Doubled growth provides spare capacity.
  for(int i = 0; i < 4; ++i)
  {
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
    sink.drain();
  }
  EXPECT_EQ(channel->stats().dropped_oversize, 2u);
  EXPECT_EQ(channel->stats().payload_reallocations, 2u);
}

TEST(ChannelCapacity, ReservationCoversHintDoubleInitialSizeAndMinimum)
{
  for(size_t hint : { size_t(0), size_t(4096) })
  {
    auto channel = LogChannel::create("capacity");
    CapacityWorker sink;
    auto value = channel->createLoggedValue<std::vector<double>>(
        "value", std::vector<double>(100));
    channel->setPoolCapacity(2);
    channel->setPayloadCapacity(hint);
    channel->addDataSink(sink);
    ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
    sink.drain();
    value->set(std::vector<double>(hint ? 500 : 200));
    for(int i = 0; i < 4; ++i)
    {
      EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
      sink.drain();
    }
    EXPECT_EQ(channel->payloadReallocations(), 0u);
    EXPECT_EQ(channel->droppedOversize(), 0u);
  }
}

TEST(ChannelCapacity, DisabledOversizedAndDeadFieldsNeverSizeOrSerialize)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto serializer = std::make_shared<SizedSerializer>();
  auto value = std::make_unique<SizedValue>();
  auto id = channel->registerCustomValue("value", value.get(), serializer);
  serializer->throw_size = serializer->throw_serialize = true;
  channel->setEnabled(id, false);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  channel->unregister(id);
  value.reset();
  channel->setEnabled(id, true);
  ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  sink.deliver();
  ASSERT_TRUE(sink->last);
  EXPECT_FALSE(GetBit(sink->last->active_mask, 0));
  EXPECT_TRUE(sink->last->payload.empty());
  EXPECT_EQ(serializer->calls, 0u);
  EXPECT_EQ(channel->droppedOversize(), 0u);
}

static_assert(noexcept(std::declval<LogChannel&>().tryTakeSnapshot()));

// A serializer that throws, while sizing or serializing, breaks its contract:
// tryTakeSnapshot() returns serialize_failed, takeSnapshot() lets the exception through.
// Either way nothing is queued, and the slot and the snapshot epoch are released.
TEST(ChannelCapacity, ExceptionsReleaseSingleSlotAndEpochOnBothPaths)
{
  for(bool real_time : { false, true })
  {
    for(bool sizing : { false, true })
    {
      auto channel = LogChannel::create("capacity");
      const auto take = [&] {
        return real_time ? channel->tryTakeSnapshot() : channel->takeSnapshot();
      };
      CapacityWorker sink;
      auto serializer = std::make_shared<SizedSerializer>();
      SizedValue value;
      auto id = channel->registerCustomValue("value", &value, serializer);
      channel->setPoolCapacity(1);
      channel->addDataSink(sink);
      channel->startLogging();
      ASSERT_EQ(take(), SnapshotResult::ok);
      sink.drain();
      serializer->throw_size = sizing;
      serializer->throw_serialize = !sizing;
      if(real_time)
      {
        EXPECT_EQ(take(), SnapshotResult::serialize_failed);
      }
      else
      {
        try
        {
          take();
          ADD_FAILURE() << "takeSnapshot() did not throw";
        }
        catch(const std::runtime_error& error)
        {
          EXPECT_STREQ(error.what(), sizing ? "size" : "serialize");
        }
      }
      EXPECT_EQ(channel->snapshotAttempts(), 2u);
      EXPECT_EQ(channel->snapshotsAccepted(), 1u);
      EXPECT_EQ(channel->serializeFailures(), real_time ? 1u : 0u);
      sink.drain();
      EXPECT_EQ(sink->received, 1u);  // nothing queued
      serializer->throw_size = serializer->throw_serialize = false;
      channel->unregister(id);  // A leaked odd epoch would block here.
      channel->registerCustomValue("value", &value, serializer);
      EXPECT_EQ(take(), SnapshotResult::ok);
      EXPECT_EQ(channel->poolExhausted(), 0u);
    }
  }
}

// A vector that grows between sizing and serialization (here from the serializer of the
// value before it; in a program, a write outside scopedWrite()) no longer fits its bytes,
// though the slot has room: SerializeMe throws std::runtime_error, which takeSnapshot()
// lets through and tryTakeSnapshot() turns into serialize_failed. The next snapshot
// takes the released slot, without allocating.
TEST(ChannelCapacity, AVectorThatGrowsAfterSizingFailsTheSnapshot)
{
  for(bool real_time : { false, true })
  {
    auto channel = LogChannel::create("capacity");
    const auto take = [&] {
      return real_time ? channel->tryTakeSnapshot() : channel->takeSnapshot();
    };
    CapacityWorker sink;
    auto serializer = std::make_shared<SizedSerializer>();
    SizedValue value;
    std::vector<double> later(2, 1.0);
    later.reserve(16);
    auto id = channel->registerCustomValue("value", &value, serializer);
    channel->registerValue("later", &later);  // serialized after "value"
    channel->setPoolCapacity(1);  // a leaked slot would show as pool_exhausted
    channel->addDataSink(sink);
    channel->startLogging();
    sink.drain();  // The first delivery pass sizes the worker's pass list.
    serializer->grow = &later;
    if(real_time)
    {
      EXPECT_EQ(take(), SnapshotResult::serialize_failed);
    }
    else
    {
      EXPECT_THROW(take(), std::runtime_error);
    }
    serializer->grow = nullptr;
    sink.drain();
    EXPECT_EQ(sink->received, 0u);  // nothing queued
    size_t allocations = 0;
    {
      DataTamerTest::AllocCounter::Scope scope;
      EXPECT_EQ(take(), SnapshotResult::ok);
      allocations = scope.allocations();
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(channel->serializeFailures(), real_time ? 1u : 0u);
    EXPECT_EQ(channel->stats().serialize_failures, real_time ? 1u : 0u);
    EXPECT_EQ(channel->droppedOversize(), 0u);
    EXPECT_EQ(channel->poolExhausted(), 0u);
    sink.drain();
    channel->unregister(id);  // A leaked odd epoch would block here.
    EXPECT_EQ(take(), SnapshotResult::ok);
  }
}

TEST(ChannelCapacity, ImpossibleSizesFailSafelyAndStartLoggingCanRetry)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto serializer = std::make_shared<SizedSerializer>();
  SizedValue value;
  auto id = channel->registerCustomValue("value", &value, serializer);
  channel->registerCustomValue("second", &value, serializer);
  channel->setPoolCapacity(1);
  channel->addDataSink(sink);
  serializer->size = std::numeric_limits<size_t>::max();
  EXPECT_THROW((void)channel->takeSnapshot(), std::length_error);
  EXPECT_FALSE(channel->isLoggingStarted());  // a failed start leaves the channel open
  channel->setPayloadCapacity(256);           // so sizes can still be tuned
  serializer->size = 8;
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  // Each size fits the byte vector, but their sum cannot be doubled.
  serializer->size = std::vector<uint8_t>().max_size() / 2;
  EXPECT_THROW((void)channel->takeSnapshot(), std::length_error);
  serializer->size = std::vector<uint8_t>().max_size();
  EXPECT_THROW((void)channel->takeSnapshot(), std::length_error);  // Sum overflow/limit.
  serializer->size = 8;
  channel->unregister(id);
  EXPECT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
  EXPECT_EQ(channel->poolExhausted(), 0u);
}

TEST(ChannelCapacity, TenThousandDirtySnapshotsWithControlChurnDoNotAllocate)
{
  auto channel = LogChannel::create(std::string(128, 'c'));
  CapacityWorker first;
  CapacityWorker second;
  CapacityWorker changing;
  uint64_t value = 42;
  auto id = channel->registerValue("value", &value);
  auto logged = channel->createLoggedValue<uint64_t>("changing", 42);
  channel->addDataSink(first);
  channel->addDataSink(second);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  first.drain();
  second.drain();
  std::atomic<bool> start{ false }, done{ false };
  std::atomic<size_t> churn{ 0 };
  std::thread control([&] {
    while(!start)
    {
      std::this_thread::yield();
    }
    do
    {
      logged.reset();
      logged = channel->createLoggedValue<uint64_t>("changing", 42);
      channel->addDataSink(changing);
      channel->removeDataSink(changing);
      changing.drain();
      ++churn;
    } while(!done);
  });
  start = true;
  size_t allocations = 0, deallocations = 0, successes = 0;
  size_t completed_churn = 0;
  for(int batch = 0; batch < 10; ++batch)
  {
    for(int i = 0; i < 1000; ++i)
    {
      channel->setEnabled(id, (batch * 1000 + i) % 2 == 0);
      {
        DataTamerTest::AllocCounter::Scope scope;
        successes += channel->takeSnapshot() == SnapshotResult::ok;
        allocations += scope.allocations();
        deallocations += scope.deallocations();
      }
      first.drain();
      second.drain();
    }
    if(batch != 9)
    {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while(churn.load() <= completed_churn &&
            std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
      }
      ASSERT_GT(churn.load(), completed_churn) << "control thread stalled";
      completed_churn = churn.load();
    }
  }
  done = true;
  control.join();
  EXPECT_GT(churn.load(), 0u);
  EXPECT_GT(successes, 0u);
  EXPECT_EQ(allocations, 0u);
  EXPECT_EQ(deallocations, 0u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
}

TEST(ChannelCapacity, AutomaticMinimumAndTryDropRecoverAfterShrink)
{
  auto channel = LogChannel::create("capacity");
  CapacityWorker sink;
  auto value = channel->createLoggedValue<std::vector<double>>("value");
  channel->setPoolCapacity(2);
  channel->addDataSink(sink);
  ASSERT_EQ(channel->takeSnapshot(), SnapshotResult::ok);
  sink.drain();
  value->set(std::vector<double>(31));  // 4-byte length + 248 bytes fits the minimum.
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  sink.drain();
  value->set(std::vector<double>(32));  // 260 bytes exceeds both initial slots.
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::oversize);
  value->set(std::vector<double>(1));
  EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  EXPECT_EQ(channel->droppedOversize(), 1u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
}

using DataTamerTest::acceptedUntilExhausted;

// Without setPoolCapacity(): 8 MiB of slots, between 64 and 1024 of them.
TEST(ChannelCapacity, DefaultPoolFillsEightMiBBetween64And1024Slots)
{
  const auto slots = [](auto* value) {
    CapacityWorker sink;
    auto channel = LogChannel::create("default");
    channel->registerValue("value", value);
    channel->addDataSink(sink);
    channel->startLogging();
    return acceptedUntilExhausted(*channel);
  };
  uint64_t small = 1;
  EXPECT_EQ(slots(&small), 1024u);  // 1 s at 1 kHz
  // 32 KiB per slot: 8 MiB holds 255 of them, with the slot overhead.
  auto medium = std::make_unique<std::array<double, 4096>>();
  EXPECT_EQ(slots(medium.get()), SnapshotPool::defaultCapacity(sizeof(*medium), 1));
  EXPECT_NEAR(double(slots(medium.get())), 256.0, 2.0);
  // 160 KB per slot: the floor, 10 MB.
  auto large = std::make_unique<std::array<double, 20000>>();
  EXPECT_EQ(slots(large.get()), 64u);
}

TEST(ChannelCapacity, PoolSizedInTimeIsTheCeilingOfStallOverPeriod)
{
  using std::chrono::microseconds;
  using std::chrono::milliseconds;
  using std::chrono::nanoseconds;
  const auto slots = [](nanoseconds stall, nanoseconds period) {
    uint64_t value = 1;
    CapacityWorker sink;
    auto channel = LogChannel::create("timed");
    channel->registerValue("value", &value);
    channel->addDataSink(sink);
    channel->setPoolCapacity(stall, period);
    channel->startLogging();
    return acceptedUntilExhausted(*channel);
  };
  EXPECT_EQ(slots(milliseconds(200), milliseconds(1)), 200u);
  EXPECT_EQ(slots(milliseconds(9), milliseconds(3)), 3u);   // exact
  EXPECT_EQ(slots(milliseconds(10), milliseconds(3)), 4u);  // 3.33 rounds up
  EXPECT_EQ(slots(microseconds(1), milliseconds(1)), 1u);   // under one period
  EXPECT_EQ(slots(nanoseconds::max(), nanoseconds::max()), 1u);
  EXPECT_EQ(slots(nanoseconds::max(), nanoseconds::max() - nanoseconds(1)), 2u);
}

TEST(ChannelCapacity, PoolSizedInTimeValidatesItsArguments)
{
  using std::chrono::milliseconds;
  using std::chrono::nanoseconds;
  uint64_t value = 1;
  CapacityWorker sink;
  auto channel = LogChannel::create("timed");
  channel->registerValue("value", &value);
  channel->addDataSink(sink);
  channel->setPoolCapacity(3);
  EXPECT_THROW(channel->setPoolCapacity(nanoseconds(0), milliseconds(1)),
               std::invalid_argument);
  EXPECT_THROW(channel->setPoolCapacity(milliseconds(1), nanoseconds(0)),
               std::invalid_argument);
  EXPECT_THROW(channel->setPoolCapacity(milliseconds(-1), milliseconds(1)),
               std::invalid_argument);
  EXPECT_THROW(channel->setPoolCapacity(milliseconds(1), milliseconds(-1)),
               std::invalid_argument);
  // About 9.2e18 slots, computed without overflow: more than a pool can address.
  EXPECT_THROW(channel->setPoolCapacity(nanoseconds::max(), nanoseconds(1)),
               std::length_error);
  channel->startLogging();  // the failed calls kept the capacity set before
  EXPECT_EQ(acceptedUntilExhausted(*channel), 3u);
  EXPECT_THROW(channel->setPoolCapacity(milliseconds(10), milliseconds(1)),
               std::runtime_error);
}

TEST(ChannelCapacity, FixedSizeSchemaReservesExactlyItsFullPayload)
{
  CapacityWorker sink;
  auto channel = LogChannel::create("exact");
  double scalar = 1;
  std::array<float, 3> array{};
  std::atomic<int32_t> atomic{ 0 };
  TestTypes::Pose pose;
  auto scalar_id = channel->registerValue("scalar", &scalar);
  channel->registerValue("array", &array);
  channel->registerValue("atomic", &atomic);
  channel->registerValue("pose", &pose);
  constexpr size_t kFull = 8 + 3 * 4 + 4 + 7 * 8;
  // Disabled at startLogging(): its bytes are still reserved, for when it comes back.
  channel->setEnabled(scalar_id, false);
  channel->addDataSink(sink);
  channel->startLogging();
  ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
  sink.deliver();
  EXPECT_EQ(sink->last->payload.size(), kFull - 8);
  EXPECT_EQ(sink->last->payload.capacity(), kFull);
  sink.drain();
  channel->setEnabled(scalar_id, true);
  for(int i = 0; i < 4; ++i)  // every slot
  {
    ASSERT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
    sink.deliver();
    EXPECT_EQ(sink->last->payload.size(), kFull);
    EXPECT_EQ(sink->last->payload.capacity(), kFull);
    sink.drain();
  }
  EXPECT_EQ(channel->droppedOversize(), 0u);
  EXPECT_EQ(channel->payloadReallocations(), 0u);
}

namespace ReserveTypes
{
struct VariableSample
{
  double a = 0;
  std::vector<int32_t> v;
};

template <typename AddField>
std::string_view TypeDefinition(VariableSample& sample, AddField& add)
{
  add("a", &sample.a);
  add("v", &sample.v);
  return "VariableSample";
}
}  // namespace ReserveTypes

TEST(ChannelCapacity, PayloadReserveRulePerSchemaKind)
{
  const auto reserved = [](auto&& setup, size_t hint) {
    CapacityWorker sink;
    auto channel = LogChannel::create("reserve");
    setup(*channel);
    channel->setPayloadCapacity(hint);
    channel->setPoolCapacity(1);
    channel->addDataSink(sink);
    channel->startLogging();
    EXPECT_EQ(channel->tryTakeSnapshot(), SnapshotResult::ok);
    sink.deliver();
    return sink->last->payload.capacity();
  };
  static double a = 1, b = 2;
  static std::vector<double> dynamic(100);
  const auto fixed = [](LogChannel& channel) {
    channel.registerValue("a", &a);
    channel.registerValue("b", &b);
  };
  const auto mixed = [](LogChannel& channel) {
    channel.registerValue("a", &a);
    channel.registerValue("dynamic", &dynamic);
  };
  const auto with_unregistered = [](LogChannel& channel) {
    channel.registerValue("a", &a);
    channel.unregister(channel.registerValue("b", &b));  // its size is not known
  };
  EXPECT_EQ(reserved(fixed, 0), 16u);                 // exact
  EXPECT_EQ(reserved(fixed, 1000), 1000u);            // the hint is a floor
  EXPECT_EQ(reserved(mixed, 0), 2u * (8 + 4 + 800));  // twice, for growth
  EXPECT_EQ(reserved(mixed, 4096), 4096u);
  EXPECT_EQ(reserved(with_unregistered, 0), 256u);  // the dynamic minimum

  // A custom type with a variable-size member, alone or in a std::array: its
  // serializer is not fixed-size, so the reserve is twice the payload.
  static ReserveTypes::VariableSample variable{ 1.0, std::vector<int32_t>(100) };
  static std::array<ReserveTypes::VariableSample, 2> variables{ variable, variable };
  const auto custom_variable = [](LogChannel& channel) {
    channel.registerValue("variable", &variable);
  };
  const auto array_of_variable = [](LogChannel& channel) {
    channel.registerValue("variables", &variables);
  };
  EXPECT_EQ(reserved(custom_variable, 0), 2u * (8 + 4 + 400));
  EXPECT_EQ(reserved(array_of_variable, 0), 2u * 2 * (8 + 4 + 400));
}
