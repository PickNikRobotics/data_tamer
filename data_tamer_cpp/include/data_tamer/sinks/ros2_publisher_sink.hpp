#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/locked_reference.hpp"
#include "data_tamer_msgs/msg/schemas.hpp"
#include "data_tamer_msgs/msg/snapshot.hpp"
#include <unordered_map>
#include <type_traits>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <rclcpp/node_interfaces/node_interfaces.hpp>
#include <rclcpp/node_interfaces/node_topics_interface.hpp>

namespace DataTamer
{

using PublisherNodeInterfaces =
    rclcpp::node_interfaces::NodeInterfaces<rclcpp::node_interfaces::NodeTopicsInterface>;

class ROS2PublisherSink : public DataSinkBase
{
public:
  template <typename NodeT>
  ROS2PublisherSink(NodeT&& nodelike, const std::string& topic_prefix)
    : node_interface_(normalize_node(nodelike))
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
        node_interface_, topic_prefix + "/schemas", schemas_qos);
    data_publisher_ = rclcpp::create_publisher<data_tamer_msgs::msg::Snapshot>(
        node_interface_, topic_prefix + "/data", data_qos);
  }

  void addChannel(const std::string& name, const Schema& schema) override;

  bool storeSnapshot(const Snapshot& snapshot) override;

private:
  template <typename NodeT>
  static PublisherNodeInterfaces normalize_node(NodeT&& nodelike)
  {
    using D = std::decay_t<NodeT>;

    if constexpr(std::is_same_v<D, PublisherNodeInterfaces>)
      return nodelike;
    else if constexpr(std::is_same_v<D, std::shared_ptr<rclcpp::Node>> ||
                      std::is_same_v<D, std::shared_ptr<rclcpp_lifecycle::LifecycleNode>>)
      return PublisherNodeInterfaces(*nodelike);
    else
      return PublisherNodeInterfaces(nodelike);
  }

  std::unordered_map<std::string, Schema> schemas_;
  Mutex schema_mutex_;

  rclcpp::Publisher<data_tamer_msgs::msg::Schemas>::SharedPtr schema_publisher_;
  rclcpp::Publisher<data_tamer_msgs::msg::Snapshot>::SharedPtr data_publisher_;

  bool schema_changed_ = true;
  data_tamer_msgs::msg::Snapshot data_msg_;

  // ---- Stored node façade ----
  PublisherNodeInterfaces node_interface_;
};

}  // namespace DataTamer
