#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/locked_reference.hpp"
#include "data_tamer_msgs/msg/schemas.hpp"
#include "data_tamer_msgs/msg/snapshot.hpp"
#include <unordered_map>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/node_interfaces/node_interfaces.hpp>
#include <rclcpp/node_interfaces/node_topics_interface.hpp>

namespace DataTamer
{

using PublisherNodeInterfaces =
    rclcpp::node_interfaces::NodeInterfaces<rclcpp::node_interfaces::NodeTopicsInterface>;

// Concept: allow Node, LifecycleNode, or NodeInterface
template <typename NodeLike>
concept NodeInterfaceType = std::same_as<NodeLike, rclcpp::Node> ||
                            std::same_as<NodeLike, rclcpp_lifecycle::LifecycleNode> ||
                            std::same_as<NodeLike, PublisherNodeInterfaces>;

class ROS2PublisherSink : public DataSinkBase
{
public:
  template <typename NodeType>
  ROS2PublisherSink(NodeType interfaces, const std::string& topic_prefix)
    : interfaces_(interfaces)
  {
    create_publishers(topic_prefix);
  }

  void create_publishers(const std::string& topic_prefix)
  {
    rclcpp::QoS schemas_qos{ rclcpp::KeepAll() };
    schemas_qos.reliable();
    schemas_qos.transient_local();  // latch

    const rclcpp::QoS data_qos{ rclcpp::KeepAll() };

    schema_publisher_ = rclcpp::create_publisher<data_tamer_msgs::msg::Schemas>(
        interfaces_, topic_prefix + "/schemas", schemas_qos);
    data_publisher_ = rclcpp::create_publisher<data_tamer_msgs::msg::Snapshot>(
        interfaces_, topic_prefix + "/data", data_qos);
  }

  void addChannel(const std::string& name, const Schema& schema) override;

  bool storeSnapshot(const Snapshot& snapshot) override;

private:
  std::unordered_map<std::string, Schema> schemas_;
  Mutex schema_mutex_;

  rclcpp::Publisher<data_tamer_msgs::msg::Schemas>::SharedPtr schema_publisher_;
  rclcpp::Publisher<data_tamer_msgs::msg::Snapshot>::SharedPtr data_publisher_;

  bool schema_changed_ = true;
  data_tamer_msgs::msg::Snapshot data_msg_;

  // ---- Stored node façade ----
  PublisherNodeInterfaces interfaces_;
};

}  // namespace DataTamer
