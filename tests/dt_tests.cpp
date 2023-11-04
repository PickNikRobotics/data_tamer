#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include <gtest/gtest.h>

using namespace DataTamer;

TEST(DataTamer, SinkAdd)
{
  auto dummy_sink_A = std::make_shared<DummySync>();
  auto dummy_sink_B = std::make_shared<DummySync>();

  ChannelsRegistry::Global().addDefaultSink(dummy_sink_A);

  auto channel = ChannelsRegistry::Global().getChannel("chan");
  channel->addDataSink(dummy_sink_B);

  double var = 3.14;
  int count = 49;
  auto id1 = channel->registerValue("var", &var);
  auto id2 = channel->registerValue("count", &count);

  const int shapshot_count = 10;
  for(int i=0; i<shapshot_count; i++)
  {
    channel->takeSnapshot(std::chrono::milliseconds(i));
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  channel->flush(std::chrono::milliseconds(10));

  const auto hash = channel->getDictionary().hash;

  ASSERT_EQ(dummy_sink_A->dictionaries.size(), 1);
  ASSERT_EQ(dummy_sink_A->dictionaries.begin()->first, hash);
  ASSERT_EQ(dummy_sink_A->snapshots_count[hash], shapshot_count);

  ASSERT_EQ(dummy_sink_B->dictionaries.size(), 1);
  ASSERT_EQ(dummy_sink_B->dictionaries.begin()->first, hash);
  ASSERT_EQ(dummy_sink_B->snapshots_count[hash], shapshot_count);
}
