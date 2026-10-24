#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/sinks/ros2_publisher_sink.hpp"
#include "data_tamer_v2/types.hpp"

#include <rclcpp/rclcpp.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>

// Usage: ros2_publisher [--aggregate] [--yaml]
//   --aggregate  publish SnapshotBatch on /test/data_batch (100 snapshots or 100 ms)
//                instead of one Snapshot per sample on /test/data
//   --yaml       write the schemas as YAML (wire format section 2.1)
// python/ros2_subscriber.py decodes either.
class JointPublisher : public rclcpp::Node
{
public:
  explicit JointPublisher(const DataTamer2::ROS2PublisherOptions& options)
    : Node("joint_controller")
  {
    publisher_ = DataTamer2::ROS2PublisherSink::create(*this, "/test", options);
    channel_ = DataTamer2::LogChannel::create("controller");
    channel_->addDataSink(publisher_);
    channel_->registerValue("positions", &positions_);
    channel_->registerValue("velocities", &velocities_);
    channel_->registerValue("efforts", &efforts_);
    channel_->startLogging();  // Publish the schema before the timer starts.
    timer_ = create_wall_timer(std::chrono::milliseconds(5), [this] { sample(); });
  }

  // Drain queued snapshots and flush a pending batch.
  void stop() { publisher_->stop(); }

private:
  void sample()
  {
    const double t = cycle_++ * 0.005;
    for(size_t joint = 0; joint < positions_.size(); ++joint)
    {
      const double phase = 4 * t + 0.5 * static_cast<double>(joint);
      positions_[joint] = std::sin(phase);
      velocities_[joint] = 4 * std::cos(phase);
      efforts_[joint] = 0.2 * std::sin(phase);
    }
    if(channel_->takeSnapshot() != DataTamer2::SnapshotResult::ok)
    {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 1000,
                           "Snapshot was not accepted by every sink");
    }
  }

  // Borrowed values precede the channel; the timer is destroyed before either.
  std::array<double, 2> positions_{}, velocities_{}, efforts_{};
  int cycle_ = 0;
  std::shared_ptr<DataTamer2::SinkWorkerOf<DataTamer2::ROS2PublisherSink>> publisher_;
  std::shared_ptr<DataTamer2::LogChannel> channel_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char* argv[])
{
  rclcpp::init(argc, argv);
  DataTamer2::ROS2PublisherOptions options;
  for(int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    options.aggregate |= (arg == "--aggregate");
    if(arg == "--yaml")
    {
      options.schema_format = DataTamer2::SchemaFormat::Yaml;
    }
  }
  auto node = std::make_shared<JointPublisher>(options);
  rclcpp::spin(node);
  node->stop();
  rclcpp::shutdown();
}
