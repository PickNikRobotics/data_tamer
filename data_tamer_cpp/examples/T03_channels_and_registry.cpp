// T03 Channels and the registry
// You will learn: share a sink across rates, toggle a value, and own a LoggedValue.
// Run: ./T03_channels_and_registry then open T03_joint_states.mcap in PlotJuggler
#include "data_tamer_v2/data_tamer.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>

int main()
{
  std::array<double, 2> positions{}, velocities{}, efforts{}, target_positions{};
  auto& registry = DataTamer2::ChannelsRegistry::Global();
  registry.addDefaultSink(DataTamer2::MCAPSink::create("T03_joint_states.mcap"));
  // Each rate gets its own schema and snapshots; both channels share the file.
  auto controller = registry.getChannel("controller");
  auto planner = registry.getChannel("planner");
  controller->registerValue("positions", &positions);
  controller->registerValue("velocities", &velocities);
  const auto effort_id = controller->registerValue("efforts", &efforts);
  planner->registerValue("target_positions", &target_positions);

  // Owns its storage and unregisters on destruction. Release the last handle on
  // a normal thread: unregistering can wait for a snapshot in progress.
  auto tracking_error = controller->createLoggedValue<double>("tracking_error");
  for(int cycle = 0; cycle < 1500; ++cycle)
  {
    const double t = cycle * 0.001;
    for(size_t joint = 0; joint < positions.size(); ++joint)
    {
      const double phase = 4 * t + 0.5 * static_cast<double>(joint);
      positions[joint] = std::sin(phase);
      velocities[joint] = 4 * std::cos(phase);
      efforts[joint] = 0.2 * std::sin(phase);
    }
    if(cycle % 100 == 0)  // Planner: 10 Hz, controller: 1 kHz.
    {
      target_positions = { std::sin(4 * t + 0.1), std::sin(4 * t + 0.6) };
      planner->takeSnapshot();
    }
    tracking_error->set(target_positions[0] - positions[0]);
    // Disabling keeps the registration and only omits the value from the snapshots.
    if(cycle == 500)
    {
      controller->setEnabled(effort_id, false);
    }
    if(cycle == 1000)
    {
      controller->setEnabled(effort_id, true);
    }
    controller->takeSnapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  registry.stopAll();
  // The global registry otherwise keeps channels borrowing our locals after main.
  registry.clear();
}
