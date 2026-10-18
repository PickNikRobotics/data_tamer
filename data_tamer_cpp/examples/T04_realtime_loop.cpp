// T04 Preparing a real-time loop
// You will learn: prepare outside the loop, count snapshot results, and read stats.
// Run: ./T04_realtime_loop then open T04_joint_states.mcap in PlotJuggler
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
  auto sink = DataTamer2::MCAPSink::create("T04_joint_states.mcap");
  auto channel = DataTamer2::LogChannel::create("controller");
  channel->addDataSink(sink);
  channel->registerValue("positions", &positions);
  channel->registerValue("velocities", &velocities);
  channel->registerValue("efforts", &efforts);
  // On this normal thread: freeze the schema, allocate, and announce to sinks.
  channel->startLogging();

  uint64_t missed = 0, partial = 0;
  // A portable timing simulation; a robot would use its controller's scheduler.
  auto next_cycle = std::chrono::steady_clock::now();
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
    // takeSnapshot() may wait for a writer or grow a slot. This call gives up.
    // Count failures: printing, retrying or allocating here would delay control.
    switch(channel->tryTakeSnapshot())
    {
      case DataTamer2::SnapshotResult::ok:  // Every sink accepted the snapshot.
        break;
      case DataTamer2::SnapshotResult::partial:  // Some stopped sinks refused it.
        ++partial;
        break;
      case DataTamer2::SnapshotResult::rejected:        // Every sink stopped.
      case DataTamer2::SnapshotResult::no_sinks:        // No sink attached.
      case DataTamer2::SnapshotResult::not_started:     // Preparation was skipped.
      case DataTamer2::SnapshotResult::pool_exhausted:  // Sinks hold every slot.
      case DataTamer2::SnapshotResult::oversize:        // Payload outgrew a slot.
      case DataTamer2::SnapshotResult::blocked:         // A writer holds the mutex.
        ++missed;
        break;
    }
    next_cycle += std::chrono::milliseconds(1);
    std::this_thread::sleep_until(next_cycle);
  }

  sink->stop();
  // stats() can lock and allocate: monitor from a normal thread, or after the loop.
  const auto channel_stats = channel->stats();
  const auto sink_stats = sink->stats();
  std::cout << "missed=" << missed << " partial=" << partial
            << " accepted=" << channel_stats.accepted
            << " pool_exhausted=" << channel_stats.pool_exhausted
            << " oversize=" << channel_stats.dropped_oversize
            << " sink_errors=" << sink_stats.errors << '\n';
  return sink_stats.errors == 0 ? 0 : 1;
}
