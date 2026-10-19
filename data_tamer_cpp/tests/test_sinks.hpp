#pragma once

#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace DataTamerTest
{

/// A SinkWorker owning a T, with direct access to the T. Converts to the
/// shared_ptr<SinkWorker> that LogChannel::addDataSink takes.
template <typename T>
struct Attached
{
  explicit Attached(std::shared_ptr<DataTamer2::SinkWorker> worker_)
    : worker(std::move(worker_)), sink(&worker->template as<T>())
  {}

  template <typename... Args>
  explicit Attached(Args&&... args)
    : Attached(std::shared_ptr<DataTamer2::SinkWorker>(
          DataTamer2::SinkWorker::create<T>(std::forward<Args>(args)...)))
  {}

  T* operator->() const { return sink; }
  T& operator*() const { return *sink; }
  operator const std::shared_ptr<DataTamer2::SinkWorker>&() const { return worker; }

  /// Deliver everything queued so far on this thread (see SinkWorker::drain).
  void drain() const { worker->drain(); }

  std::shared_ptr<DataTamer2::SinkWorker> worker;
  T* sink;
};

/// Worker with an explicit delivery mode. Manual delivery makes tests
/// deterministic: snapshots are delivered only by drain().
template <typename T, typename... Args>
Attached<T> attach(DataTamer2::SinkWorker::Delivery delivery, Args&&... args)
{
  return Attached<T>(std::make_shared<DataTamer2::SinkWorker>(
      std::make_unique<T>(std::forward<Args>(args)...), delivery));
}

template <typename T, typename... Args>
Attached<T> manual(Args&&... args)
{
  return attach<T>(DataTamer2::SinkWorker::Delivery::Manual, std::forward<Args>(args)...);
}

/// A channel with one registered value, attached to `sink` (logging not started).
template <typename T>
std::shared_ptr<DataTamer2::LogChannel>
channelWith(const std::shared_ptr<DataTamer2::SinkWorker>& sink, T* value,
            const std::string& name = "test_channel")
{
  auto channel = DataTamer2::LogChannel::create(name);
  channel->registerValue("value", value);
  channel->addDataSink(sink);
  return channel;
}

/// A channel named "chan" with a DummySink attached, for the tests of what registered
/// values record. The delivery is manual: snapshot() delivers what it takes.
struct Recording
{
  Attached<DataTamer2::DummySink> sink = manual<DataTamer2::DummySink>();
  std::shared_ptr<DataTamer2::LogChannel> channel = DataTamer2::LogChannel::create("cha"
                                                                                   "n");

  Recording() { channel->addDataSink(sink); }

  /// A field of the schema, counted in the order of registration.
  DataTamer2::TypeField field(size_t index = 0) const
  {
    return channel->getSchema().fields.at(index);
  }

  /// Takes a snapshot, delivers it and returns it as the sink received it.
  DataTamer2::Snapshot snapshot()
  {
    EXPECT_EQ(channel->takeSnapshot(), DataTamer2::SnapshotResult::ok);
    sink.drain();
    return sink->latestSnapshot();
  }

  DataTamer2::PayloadVector payload() { return snapshot().payload; }
  size_t payloadSize() { return snapshot().payload.size(); }
};

/// Snapshots a started channel accepts until its pool is exhausted. With
/// nothing delivered meanwhile, that is the pool capacity.
inline size_t acceptedUntilExhausted(DataTamer2::LogChannel& channel)
{
  size_t accepted = 0;
  while(true)
  {
    const auto result = channel.tryTakeSnapshot();
    if(result != DataTamer2::SnapshotResult::ok)
    {
      EXPECT_EQ(result, DataTamer2::SnapshotResult::pool_exhausted);
      return accepted;
    }
    ++accepted;
  }
}

/// Publications `sink` refused, from stats.dropped_by_sink (0 if the sink is not in it).
inline uint64_t droppedBy(const DataTamer2::LogChannel::Stats& stats,
                          const std::shared_ptr<DataTamer2::SinkWorker>& sink)
{
  for(const auto& entry : stats.dropped_by_sink)
  {
    if(entry.sink == sink.get())
    {
      return entry.dropped;
    }
  }
  return 0;
}

/// Publications `sink` refused on `channel`, from stats().dropped_by_sink (0 if the
/// sink is not attached).
inline uint64_t droppedBy(const DataTamer2::LogChannel& channel,
                          const std::shared_ptr<DataTamer2::SinkWorker>& sink)
{
  return droppedBy(channel.stats(), sink);
}

/// True while the calling thread (or any other) holds the channel's write mutex:
/// a probe thread's tryTakeSnapshot() reports `blocked`. The channel must be
/// started with a sink attached (the probe takes a snapshot when not blocked).
inline bool writeMutexHeld(DataTamer2::LogChannel& channel)
{
  bool held = false;
  std::thread([&] {
    held = channel.tryTakeSnapshot() == DataTamer2::SnapshotResult::blocked;
  }).join();
  return held;
}

}  // namespace DataTamerTest
