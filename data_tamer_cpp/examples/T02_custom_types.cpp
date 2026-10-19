// T02 Custom types and containers
// You will learn: describe your structs, adapt a vendor type, and log containers.
// Run: ./T02_custom_types then open T02_joint_states.mcap in PlotJuggler
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/custom_types.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <thread>
#include <vector>

namespace robot
{
struct JointState
{
  double position = 0;
  double velocity = 0;
  double effort = 0;
};

// Put this beside your own type so argument-dependent lookup finds it.
template <typename AddField>
std::string_view TypeDefinition(JointState& joint, AddField& add)
{
  add("position", &joint.position);
  add("velocity", &joint.velocity);
  add("effort", &joint.effort);
  return "JointState";
}
}  // namespace robot

// Imagine this class comes from a vendor header that we cannot edit.
namespace vendor
{
class Vec3
{
public:
  double& x() { return x_; }
  double& y() { return y_; }
  double& z() { return z_; }

private:
  double x_ = 0, y_ = 0, z_ = 0;
};
}  // namespace vendor

// The trait adapts the type without adding anything to the vendor's namespace.
template <>
struct DataTamer2::TypeDefinitionTrait<vendor::Vec3>
{
  template <typename AddField>
  static std::string_view define(vendor::Vec3& acceleration, AddField& add)
  {
    add("x", &acceleration.x());
    add("y", &acceleration.y());
    add("z", &acceleration.z());
    return "Vec3";
  }
};

int main()
{
  std::vector<robot::JointState> joints(2);
  std::array<double, 2> target_positions{};
  vendor::Vec3 tool_acceleration;
  auto mcap = DataTamer2::MCAPSink::create("T02_joint_states.mcap");
  auto channel = DataTamer2::LogChannel::create("controller");
  channel->addDataSink(mcap);
  // Register the vector itself: its elements may move when it grows.
  channel->registerValue("joints", &joints);
  channel->registerValue("target_positions", &target_positions);
  channel->registerValue("tool_acceleration", &tool_acceleration);

  for(int cycle = 0; cycle < 1500; ++cycle)
  {
    const double t = cycle * 0.001;
    for(size_t joint = 0; joint < joints.size(); ++joint)
    {
      const double phase = 4 * t + 0.5 * static_cast<double>(joint);
      joints[joint] = { std::sin(phase), 4 * std::cos(phase), 0.2 * std::sin(phase) };
      target_positions[joint] = std::sin(phase + 0.1);
    }
    tool_acceleration.x() = std::sin(4 * t);
    tool_acceleration.y() = std::cos(4 * t);
    tool_acceleration.z() = 9.81 + 0.1 * std::sin(4 * t);
    channel->takeSnapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  mcap->stop();
}
