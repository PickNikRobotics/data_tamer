// T01 Hello MCAP
// You will learn: record changing joint values and close the recording.
// Run: ./T01_hello_mcap then open T01_joint_states.mcap in PlotJuggler
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"

#include <chrono>
#include <cmath>
#include <thread>

int main()
{
  // The channel borrows these variables: declare them first so they outlive it.
  double shoulder_position = 0, shoulder_velocity = 0, shoulder_effort = 0;
  double elbow_position = 0, elbow_velocity = 0, elbow_effort = 0;

  // A worker that owns the sink writing the file (mcap->sink() is the MCAPSink). The
  // channel takes snapshots and hands them to its sinks.
  auto mcap = DataTamer2::MCAPSink::create("T01_joint_states.mcap");
  auto channel = DataTamer2::LogChannel::create("controller");
  channel->addDataSink(mcap);

  channel->registerValue("shoulder/position", &shoulder_position);
  channel->registerValue("shoulder/velocity", &shoulder_velocity);
  channel->registerValue("shoulder/effort", &shoulder_effort);
  channel->registerValue("elbow/position", &elbow_position);
  channel->registerValue("elbow/velocity", &elbow_velocity);
  channel->registerValue("elbow/effort", &elbow_effort);

  for(int cycle = 0; cycle < 1500; ++cycle)
  {
    const double t = cycle * 0.001;
    shoulder_position = std::sin(4 * t);
    shoulder_velocity = 4 * std::cos(4 * t);
    shoulder_effort = 0.2 * std::sin(4 * t);
    elbow_position = std::sin(4 * t + 0.5);
    elbow_velocity = 4 * std::cos(4 * t + 0.5);
    elbow_effort = 0.2 * std::sin(4 * t + 0.5);
    channel->takeSnapshot();  // One sample of every registered value.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  mcap->stop();  // Deliver queued snapshots and close the file, including its footer.
}
