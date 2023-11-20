#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/mcap_sink.hpp"
#include <iostream>
#include <cmath>

using namespace DataTamer;


int main()
{
  auto mcap_sink = std::make_shared<MCAPSink>("test_sample.mcap");
  ChannelsRegistry::Global().addDefaultSink(mcap_sink);

  // Create (or get) a channel using the global registry (singleton)
  auto channelA = ChannelsRegistry::Global().getChannel("channelA");
  auto channelB = ChannelsRegistry::Global().getChannel("channelB");

  // logs in channelA
  std::vector<double> v1(10, 0);
  std::array<float, 4> v2 = {1, 2, 3, 4};
  int32_t v3 = 5;
  uint16_t v4 = 6;

  channelA->registerValue("vector_10", &v1);
  channelA->registerValue("array_4", &v2);
  channelA->registerValue("val_int32", &v3);
  channelA->registerValue("val_int16", &v4);

  // logs in channelB
  double v5 = 10;
  uint16_t v6 = 11;
  std::vector<uint8_t> v7(4, 12);
  std::array<uint32_t, 3> v8 = {13, 14, 15};

  channelB->registerValue("real_value", &v5);
  channelB->registerValue("short_int", &v6);
  channelB->registerValue("vector_4", &v7);
  channelB->registerValue("array_3", &v8);

  for(size_t i=0; i<1000; i++)
  {
    channelA->takeSnapshot();
    if(i%2 == 0)
    {
      channelB->takeSnapshot();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
}


