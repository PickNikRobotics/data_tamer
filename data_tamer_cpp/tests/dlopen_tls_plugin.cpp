// A plugin linked to libdata_tamer, loaded with dlopen() by dlopen_tls_tests.cpp, so
// that libdata_tamer arrives with it, as with pluginlib or ros2_control. On fresh
// threads it makes the first accesses of the thread-local transaction chain.
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"

#include <atomic>
#include <memory>
#include <thread>

using namespace DataTamer2;

namespace
{
void waitFor(const std::atomic<int>& step, int value)
{
  while(step.load() < value)
  {
    std::this_thread::yield();
  }
}
}  // namespace

// Each first access runs between count(true) and count(false), on a thread created
// before count(true). Returns false if the snapshot did not find the write mutex held.
extern "C" __attribute__((visibility("default"))) bool
dataTamerFirstTransactionAccess(void (*count)(bool))
{
  auto channel = LogChannel::create("dlopen_tls");
  double value = 0;
  channel->registerValue("value", &value);
  channel->addDataSink(std::make_shared<SinkWorker>(std::make_unique<DummySink>(),
                                                    SinkWorker::Delivery::Manual));
  channel->startLogging();

  // A snapshot while another thread holds the write mutex: try_lock() fails and
  // inTransactionOnThisThread() reads the chain.
  std::atomic<int> step{ 0 };
  std::thread holder([&] {
    auto transaction = channel->scopedWrite();
    step = 1;
    waitFor(step, 3);
  });
  waitFor(step, 1);
  auto result = SnapshotResult::ok;
  std::thread snapshot([&] {
    waitFor(step, 2);
    count(true);
    result = channel->tryTakeSnapshot();
    count(false);
    step = 3;
  });
  step = 2;
  snapshot.join();
  holder.join();

  // A transaction: its constructor and destructor update the chain.
  std::atomic<int> go{ 0 };
  std::thread writer([&] {
    waitFor(go, 1);
    count(true);
    {
      auto transaction = channel->scopedWrite();
    }
    count(false);
  });
  go = 1;
  writer.join();
  return result == SnapshotResult::blocked;
}
