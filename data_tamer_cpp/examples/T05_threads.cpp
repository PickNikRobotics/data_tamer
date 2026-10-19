// T05 Sensor callbacks and snapshots
// You will learn: share a scalar safely and update related values in one transaction.
// Run: ./T05_threads then open T05_joint_states.mcap in PlotJuggler
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_sink.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

int main()
{
  std::array<double, 2> positions{}, velocities{}, efforts{};
  auto mcap = DataTamer2::MCAPSink::create("T05_joint_states.mcap");
  auto channel = DataTamer2::LogChannel::create("controller");
  channel->addDataSink(mcap);
  // A value another thread writes: a LoggedValue owns its storage, a scalar is atomic,
  // and it unregisters itself when destroyed.
  auto motor_temperature = channel->createLoggedValue<double>("motor_temperature", 25);
  // Values that must change together stay plain variables, written in scopedWrite().
  channel->registerValue("positions", &positions);
  channel->registerValue("velocities", &velocities);
  channel->registerValue("efforts", &efforts);
  channel->startLogging();

  std::thread sensor_callback([&] {
    auto next_sample = std::chrono::steady_clock::now();
    for(int sample = 0; sample < 1500; ++sample)
    {
      const double t = sample * 0.001;
      // An independent scalar: set() is an atomic store, safe without a guard.
      motor_temperature->set(25 + std::sin(t));
      // Compute first, so that the transaction only copies and snapshots wait less.
      std::array<double, 2> new_positions{}, new_velocities{}, new_efforts{};
      for(size_t joint = 0; joint < new_positions.size(); ++joint)
      {
        const double phase = 4 * t + 0.5 * static_cast<double>(joint);
        new_positions[joint] = std::sin(phase);
        new_velocities[joint] = 4 * std::cos(phase);
        new_efforts[joint] = 0.2 * std::sin(phase);
      }
      {
        // Raw registered values written by another thread MUST use scopedWrite().
        // One transaction makes position, velocity and effort match one sample.
        auto tx = channel->scopedWrite();
        positions = new_positions;
        velocities = new_velocities;
        efforts = new_efforts;
      }  // Release before sleeping; never block while holding the transaction.
      next_sample += std::chrono::milliseconds(1);
      std::this_thread::sleep_until(next_sample);
    }
  });

  uint64_t missed = 0;
  auto next_cycle = std::chrono::steady_clock::now();
  for(int cycle = 0; cycle < 1500; ++cycle)
  {
    // This thread reads the raw values only through the snapshot API.
    if(!DataTamer2::accepted(channel->tryTakeSnapshot()))
    {
      ++missed;  // A concurrent transaction can yield blocked; do not retry.
    }
    next_cycle += std::chrono::milliseconds(1);
    std::this_thread::sleep_until(next_cycle);
  }
  sensor_callback.join();  // No callback can access locals during their destruction.
  mcap->stop();
  std::cout << "snapshot failures=" << missed << " sink errors=" << mcap->errors()
            << '\n';
  // LoggedValue destruction unregisters here, on the normal thread after the loop.
  return mcap->errors() == 0 ? 0 : 1;
}
