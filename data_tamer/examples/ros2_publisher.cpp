#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/ros2_publisher_sink.hpp"
#include <iostream>

using namespace DataTamer;

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("test_datatamer");

  auto ros2_sink = std::make_shared<ROS2PublisherSink>(node, node->name());
  ChannelsRegistry::Global().addDefaultSink(ros2_sink);


  // Create (or get) a channel using the global registry (singleton)
  auto channel = ChannelsRegistry::Global().getChannel("chan");

  auto const vect_size = 100;
  std::vector<double> real64(vect_size);
  std::vector<float> real32(vect_size);
  std::vector<int16_t> int16(vect_size);

  channel->registerValue("real64", &real64);
  channel->registerValue("real32", &real32);
  channel->registerValue("int16", &int16);

  while(rclcpp::ok())
  {
    writers[i] = std::thread(WritingThread, std::string("channel_") + std::to_string(i));
  }

  for(int i=0; i<N; i++)
  {
    writers[i].join();
  }
}
