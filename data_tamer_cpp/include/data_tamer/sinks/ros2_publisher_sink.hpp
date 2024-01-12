#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/mutex.hpp"
#include "data_tamer_msgs/msg/schemas.hpp"
#include "data_tamer_msgs/msg/snapshot.hpp"
#include <unordered_map>
#include <rclcpp/rclcpp.hpp>

namespace DataTamer
{

class ROS2PublisherSink : public DataSinkBase
{
public:
  ROS2PublisherSink(std::shared_ptr<rclcpp::Node> node, const std::string& topic_prefix);

  void addChannel(const std::string& name, const Schema& schema) override;

  bool storeSnapshot(const Snapshot& snapshot) override;

private:
  std::shared_ptr<rclcpp::Node> node_;

  std::unordered_map<std::string, Schema> schemas_;
  Mutex schema_mutex_;

  rclcpp::Publisher<data_tamer_msgs::msg::Schemas>::SharedPtr schema_publisher_;
  rclcpp::Publisher<data_tamer_msgs::msg::Snapshot>::SharedPtr data_publisher_;

  bool schema_changed_ = true;
  data_tamer_msgs::msg::Snapshot data_msg_;
};

}   // namespace DataTamer
