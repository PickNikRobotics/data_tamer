// T06 Flight recorder
// You will learn: keep recent joint history in RAM and dump it after a fault.
// Run: ./T06_flight_recorder then open fault_*.mcap in PlotJuggler
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <thread>

using namespace DataTamer2;
using namespace std::chrono_literals;

int main()
{
  MCAPRingOptions options;
  options.filepath = "fault.mcap";  // dumps: fault_1.mcap, fault_2.mcap, ...
  options.window = 1s;
  options.capacity_bytes = 4 * 1024 * 1024;
  auto recorder = MCAPRingSink::create(options);
  recorder->sink().setDumpCallback([](const MCAPRingDump& dump) {
    std::cout << (dump.ok ? "wrote " : "failed ") << dump.path << " (" << dump.messages
              << " messages) " << dump.error << std::endl;
  });

  std::array<double, 2> positions{}, velocities{}, efforts{};
  auto channel = LogChannel::create("controller");
  channel->registerValue("positions", &positions);
  channel->registerValue("velocities", &velocities);
  channel->registerValue("efforts", &efforts);
  channel->addDataSink(recorder);
  channel->startLogging();  // before the loop, so that every cycle is real-time

  // A 1 kHz control loop; a fault at t = 1 s.
  for(int i = 0; i < 1500; i++)
  {
    const double t = i * 0.001;
    for(size_t joint = 0; joint < positions.size(); ++joint)
    {
      const double phase = 4 * t + 0.5 * static_cast<double>(joint);
      positions[joint] = std::sin(phase);
      velocities[joint] = 4 * std::cos(phase);
      efforts[joint] = 0.2 * std::sin(phase);
    }
    (void)channel->tryTakeSnapshot();
    if(i == 1000)
    {
      // Real-time safe: one atomic compare-exchange, no allocation. The result is false
      // when an earlier request is still active, which cannot happen here.
      (void)recorder->sink().requestDump(250ms);
    }
    std::this_thread::sleep_for(1ms);
  }

  // Deliver what is queued, then write a dump requested just before shutdown, if
  // any, and wait for the writer.
  recorder->stop();
  return 0;
}
