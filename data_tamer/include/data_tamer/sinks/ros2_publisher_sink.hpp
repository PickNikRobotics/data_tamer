#pragma once

#include "data_tamer/data_sink.hpp"
#include <unordered_map>
#include <rclcpp/rclcpp.hpp>

namespace DataTamer
{

class ROS2PublisherSink : public DataSinkBase
{
public:
  ROS2PublisherSink(std::shared_ptr<rclcpp::Node> node);
  
  void addChannel(std::string const& name, Schema const& schema) override
  {
  }

  bool storeSnapshot(const Snapshot& snapshot) override
  {

  }
private:
  std::shared_ptr<rclcpp::Node> node_;
};



}  // namespace DataTamer
