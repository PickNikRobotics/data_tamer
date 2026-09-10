#include "data_tamer/data_tamer.hpp"
#include "alloc_counter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace DataTamer;
using DataTamerTest::AllocCounter;

namespace
{
class CheckingSink : public DataSinkBase
{
public:
  enum class Payload
  {
    PAIR,
    VECTOR
  };

  explicit CheckingSink(Payload payload) : payload_(payload) {}
  ~CheckingSink() override { stopThread(); }

  void addChannel(const std::string&, const Schema&) override {}

  bool waitFor(size_t count)
  {
    std::unique_lock lk(mutex_);
    return delivered_.wait_for(lk, std::chrono::seconds(5),
                               [&] { return delivered_count_ >= count; });
  }

  size_t delivered() const
  {
    std::scoped_lock lk(mutex_);
    return delivered_count_;
  }

  size_t errors() const
  {
    std::scoped_lock lk(mutex_);
    return errors_;
  }

protected:
  bool storeSnapshot(const Snapshot& snapshot) override
  {
    bool valid = false;
    if(payload_ == Payload::PAIR && snapshot.payload.size() == 2 * sizeof(double))
    {
      double a = 0;
      double b = 0;
      std::memcpy(&a, snapshot.payload.data(), sizeof(a));
      std::memcpy(&b, snapshot.payload.data() + sizeof(a), sizeof(b));
      valid = (a == b);
    }
    else if(payload_ == Payload::VECTOR && snapshot.payload.size() >= sizeof(uint32_t))
    {
      uint32_t size = 0;
      std::memcpy(&size, snapshot.payload.data(), sizeof(size));
      valid = snapshot.payload.size() == sizeof(size) + size_t(size) * sizeof(double);
      for(size_t i = 0; valid && i < size; ++i)
      {
        double value = 0;
        std::memcpy(&value, snapshot.payload.data() + sizeof(size) + i * sizeof(value),
                    sizeof(value));
        valid = value == double(size);
      }
    }

    {
      std::scoped_lock lk(mutex_);
      ++delivered_count_;
      errors_ += !valid;
    }
    delivered_.notify_all();
    return true;
  }

private:
  Payload payload_;
  mutable std::mutex mutex_;
  std::condition_variable delivered_;
  size_t delivered_count_ = 0;
  size_t errors_ = 0;
};

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

struct CustomValue
{
  uint8_t value = 0;
};

class ProbeSerializer : public CustomSerializer
{
public:
  const std::string& typeName() const override { return name_; }
  size_t serializedSize(const void*) const override
  {
    ++size_calls;
    if(throw_on_size)
    {
      throw std::runtime_error("disabled value was sized");
    }
    return 1;
  }
  bool isFixedSize() const override { return true; }
  void serialize(const void* value, SerializeMe::SpanBytes& dest) const override
  {
    if(throw_on_serialize)
    {
      throw std::runtime_error("serialize failed");
    }
    dest.data()[0] = static_cast<const CustomValue*>(value)->value;
    dest.trimFront(1);
  }

  mutable size_t size_calls = 0;
  bool throw_on_size = false;
  bool throw_on_serialize = false;

private:
  std::string name_ = "CustomValue";
};

void nestTransactions(const std::vector<std::shared_ptr<LogChannel>>& channels, size_t index,
                      bool probe_mutexes)
{
  if(index == channels.size())
  {
    for(const auto& channel : channels)
    {
      ASSERT_TRUE(channel->sharedState()->inTransactionOnThisThread());
    }
    if(probe_mutexes)
    {
      std::atomic_bool all_locked{ true };
      std::thread probe([&] {
        for(const auto& channel : channels)
        {
          if(channel->writeMutex().try_lock())
          {
            all_locked = false;
            channel->writeMutex().unlock();
          }
        }
      });
      probe.join();
      ASSERT_TRUE(all_locked);
    }
    return;
  }
  auto tx = channels[index]->scopedWrite();
  nestTransactions(channels, index + 1, probe_mutexes);
}
}  // namespace

TEST(Transaction, ScopedWriteOwnsTheSharedWriteMutex)
{
  auto channel = LogChannel::create("chan");
  {
    auto tx = channel->scopedWrite();
    ASSERT_TRUE(channel->sharedState()->inTransactionOnThisThread());
    std::atomic_bool acquired{ false };
    std::thread probe([&] {
      acquired = channel->writeMutex().try_lock();
      if(acquired)
      {
        channel->writeMutex().unlock();
      }
    });
    probe.join();
    ASSERT_FALSE(acquired);
  }
  ASSERT_FALSE(channel->sharedState()->inTransactionOnThisThread());
  ASSERT_TRUE(channel->writeMutex().try_lock());
  channel->writeMutex().unlock();
}

TEST(Transaction, NestedSetAndGetDoNotRelock)
{
  auto channel = LogChannel::create("chan");
  auto vec = channel->createLoggedValue<std::vector<double>>("vec");
  auto scalar = channel->createLoggedValue<double>("scalar");
  {
    auto tx = channel->scopedWrite();
    vec->set({ 1.0, 2.0 });
    scalar->set(3.0);
    ASSERT_EQ(vec->get(), (std::vector<double>{ 1.0, 2.0 }));
  }
  ASSERT_EQ(scalar->get(), 3.0);
}

TEST(Transaction, NestingAcrossMoreThanEightChannelsDoesNotAllocateOrAlias)
{
  static_assert(!std::is_move_constructible_v<ChannelSharedState::Transaction>);
  std::vector<std::shared_ptr<LogChannel>> channels;
  for(int i = 0; i < 10; ++i)
  {
    channels.push_back(LogChannel::create("chan" + std::to_string(i)));
  }

  size_t allocation_count = 0;
  {
    AllocCounter::Scope allocations;
    nestTransactions(channels, 0, false);
    allocation_count = allocations.allocations();
  }
  ASSERT_EQ(allocation_count, 0u);
  nestTransactions(channels, 0, true);
}

TEST(Transaction, ValuesWrittenTogetherAppearTogetherInDeliveredSnapshots)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  auto a = channel->createLoggedValue<double>("a", 1.0);
  auto b = channel->createLoggedValue<double>("b", 1.0);

  std::atomic_bool stop{ false };
  std::thread writer([&] {
    double value = 1.0;
    while(!stop.load(std::memory_order_relaxed))
    {
      auto tx = channel->scopedWrite();
      a->set(value);
      std::this_thread::yield();
      b->set(value);
      value = value == 1.0 ? 2.0 : 1.0;
    }
  });

  size_t accepted = 0;
  for(int i = 0; i < 2000; ++i)
  {
    accepted += channel->takeSnapshot();
  }
  stop = true;
  writer.join();

  ASSERT_GT(accepted, 0u);
  ASSERT_TRUE(sink->waitFor(accepted));
  ASSERT_EQ(sink->delivered(), accepted);
  ASSERT_EQ(sink->errors(), 0u);
}

TEST(Transaction, RawPointerWritesAppearTogetherInDeliveredSnapshots)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  Pair pair{ 1.0, 1.0 };
  channel->registerValue("pair", &pair);

  std::atomic_bool stop{ false };
  std::thread writer([&] {
    double value = 1.0;
    while(!stop.load(std::memory_order_relaxed))
    {
      std::lock_guard<Mutex> lock(channel->writeMutex());
      pair.a = value;
      std::this_thread::yield();
      pair.b = value;
      value = value == 1.0 ? 2.0 : 1.0;
    }
  });

  size_t accepted = 0;
  for(int i = 0; i < 2000; ++i)
  {
    accepted += channel->takeSnapshot();
  }
  stop = true;
  writer.join();

  ASSERT_GT(accepted, 0u);
  ASSERT_TRUE(sink->waitFor(accepted));
  ASSERT_EQ(sink->delivered(), accepted);
  ASSERT_EQ(sink->errors(), 0u);
}

TEST(Transaction, VectorSetAndSnapshotRaceDeliversValidPayloads)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::VECTOR);
  channel->addDataSink(sink);
  auto vec = channel->createLoggedValue<std::vector<double>>("vec");

  std::atomic_bool stop{ false };
  std::thread writer([&] {
    size_t size = 1;
    while(!stop.load(std::memory_order_relaxed))
    {
      vec->set(std::vector<double>(size, double(size)));
      size = size == 64 ? 1 : size + 1;
    }
  });

  size_t accepted = 0;
  for(int i = 0; i < 2000; ++i)
  {
    accepted += channel->takeSnapshot();
  }
  stop = true;
  writer.join();

  ASSERT_GT(accepted, 0u);
  ASSERT_TRUE(sink->waitFor(accepted));
  ASSERT_EQ(sink->delivered(), accepted);
  ASSERT_EQ(sink->errors(), 0u);
}

TEST(Transaction, ContentionCountersAdvanceOnlyAfterSpinExhaustion)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::PAIR);
  channel->addDataSink(sink);
  auto a = channel->createLoggedValue<double>("a", 1.0);
  auto b = channel->createLoggedValue<double>("b", 1.0);
  ASSERT_TRUE(channel->takeSnapshot());

  std::atomic_bool held{ false };
  std::atomic_bool snapshot_started{ false };
  std::atomic_bool release{ false };
  std::thread writer([&] {
    auto tx = channel->scopedWrite();
    held = true;
    while(!snapshot_started.load(std::memory_order_acquire))
    {
    }
    while(!release.load(std::memory_order_acquire))
    {
      std::this_thread::yield();
    }
  });
  while(!held.load(std::memory_order_acquire))
  {
  }

  std::thread snapshot([&] {
    snapshot_started.store(true, std::memory_order_release);
    channel->takeSnapshot();
  });
  while(!snapshot_started.load(std::memory_order_acquire))
  {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  release.store(true, std::memory_order_release);
  snapshot.join();
  writer.join();

  ASSERT_EQ(channel->writeLockContended(), 1u);
  ASSERT_GT(channel->writeLockWaitMaxNs(), 0u);
  const auto stats = channel->stats();
  ASSERT_EQ(stats.write_lock_contended, 1u);
  ASSERT_EQ(stats.write_lock_wait_max_ns, channel->writeLockWaitMaxNs());
}

TEST(Transaction, LoneScalarSetDoesNotTakeWriteMutex)
{
  auto channel = LogChannel::create("chan");
  auto value = channel->createLoggedValue<double>("value");
  channel->writeMutex().lock();
  value->set(1.0);
  ASSERT_EQ(value->get(), 1.0);
  channel->writeMutex().unlock();
}

TEST(Transaction, DisabledAndDestroyedValuesAreNotSized)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::VECTOR);
  channel->addDataSink(sink);
  CustomValue value;
  auto serializer = std::make_shared<ProbeSerializer>();
  const auto id = channel->registerCustomValue("value", &value, serializer);
  auto vec = channel->createLoggedValue<std::vector<double>>("vec");
  ASSERT_TRUE(channel->takeSnapshot());
  ASSERT_GT(serializer->size_calls, 0u);

  channel->unregister(id);
  serializer->size_calls = 0;
  serializer->throw_on_size = true;
  ASSERT_NO_THROW(channel->takeSnapshot());
  ASSERT_EQ(serializer->size_calls, 0u);

  vec.reset();
  ASSERT_NO_THROW(channel->takeSnapshot());
}

TEST(Transaction, SerializationExceptionReleasesWriteMutex)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<CheckingSink>(CheckingSink::Payload::VECTOR);
  channel->addDataSink(sink);
  CustomValue value;
  auto serializer = std::make_shared<ProbeSerializer>();
  serializer->throw_on_serialize = true;
  channel->registerCustomValue("value", &value, serializer);

  ASSERT_THROW(channel->takeSnapshot(), std::runtime_error);
  ASSERT_TRUE(channel->writeMutex().try_lock());
  channel->writeMutex().unlock();
}
