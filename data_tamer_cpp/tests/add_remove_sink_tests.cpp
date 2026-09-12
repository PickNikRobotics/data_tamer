#include "data_tamer/channel.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"

#include <gtest/gtest.h>
#include <string>
#include <thread>

using namespace DataTamer;

void take_snapshots(std::shared_ptr<LogChannel> channel, int count)
{
  for(int i = 0; i < count; i++)
  {
    channel->takeSnapshot();
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

TEST(DataTamerSinkRegistry, AddSinkIncreasesCountAndRef)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  std::vector<double> dummyData = { 10, 11, 12 };
  channel->registerValue("valsA", &dummyData);

  ASSERT_EQ(channel->getNumberOfSinks(), 1);
}

TEST(DataTamerSinkRegistry, SnapshotsAreRecordedWhileSinkPresent)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  std::vector<double> dummyData = { 10, 11, 12 };
  channel->registerValue("valsA", &dummyData);

  const int snapshot_count = 10;
  take_snapshots(channel, snapshot_count);

  const auto hash = channel->getSchema().hash;
  ASSERT_EQ(sink->snapshots_count[hash], snapshot_count);
}

TEST(DataTamerSinkRegistry, RemoveSinkStopsRecording)
{
  auto channel = LogChannel::create("chan");
  auto sink = std::make_shared<DummySink>();
  channel->addDataSink(sink);

  std::vector<double> dummyData = { 10, 11, 12 };
  channel->registerValue("valsA", &dummyData);

  const int snapshot_count = 10;
  take_snapshots(channel, snapshot_count);

  const auto hash = channel->getSchema().hash;
  ASSERT_EQ(sink->snapshots_count[hash], snapshot_count);

  channel->removeDataSink(sink);

  ASSERT_EQ(channel->getNumberOfSinks(), 0);

  // Taking more snapshots, should not be recorded in the sink (i.e does not increase snapshots_count)
  take_snapshots(channel, snapshot_count);

  ASSERT_EQ(sink->snapshots_count[hash], snapshot_count);
}