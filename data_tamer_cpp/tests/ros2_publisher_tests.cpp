#include "data_tamer/data_tamer.hpp"
#include "data_tamer/sinks/dummy_sink.hpp"
#include "data_tamer/sinks/ros2_publisher_sink.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <variant>
#include <string>
#include <thread>

using namespace DataTamer;

TEST(DataTamerROS2Publisher, SharedPointer)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer");
  auto ros2_sink = std::make_shared<ROS2PublisherSink>(node, "test");

  auto channel = ChannelsRegistry::Global().getChannel("channel");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  auto id_value = channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());
}

TEST(DataTamerROS2Publisher, SharedPointerLifeCycle)
{
  auto lifecycle_node = std::make_shared<rclcpp_lifecycle::LifecycleNode>("test_"
                                                                          "datatamer");
  auto ros2_sink = std::make_shared<ROS2PublisherSink>(lifecycle_node, "test");

  auto channel = ChannelsRegistry::Global().getChannel("channel");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  auto id_value = channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());
}

TEST(DataTamerROS2Publisher, Dereference)
{
  auto node = std::make_shared<rclcpp::Node>("test_datatamer");
  auto ros2_sink = std::make_shared<ROS2PublisherSink>(*node, "test");

  auto channel = ChannelsRegistry::Global().getChannel("channel");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  auto id_value = channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());
}

TEST(DataTamerROS2Publisher, DereferenceLifeCycle)
{
  auto lifecycle_node = std::make_shared<rclcpp_lifecycle::LifecycleNode>("test_"
                                                                          "datatamer");
  auto ros2_sink = std::make_shared<ROS2PublisherSink>(*lifecycle_node, "test");

  auto channel = ChannelsRegistry::Global().getChannel("channel");

  channel->addDataSink(ros2_sink);

  double const value = 1.;
  auto id_value = channel->registerValue("value", &value);

  EXPECT_TRUE(channel->takeSnapshot());
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  int ret = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return ret;
}
