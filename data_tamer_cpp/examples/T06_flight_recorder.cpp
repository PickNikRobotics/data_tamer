// T06 Flight recorder
// You will learn: keep recent joint history in RAM and dump it after a fault.
// Run: ./T06_flight_recorder then open fault_*.mcap in PlotJuggler
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"

#include <chrono>
#include <cmath>
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
  auto worker = MCAPRingSink::create(options);
  auto& recorder = worker->as<MCAPRingSink>();
  recorder.setDumpCallback([](const MCAPRingDump& dump) {
    std::cout << (dump.ok ? "wrote " : "failed ") << dump.path << " (" << dump.messages
              << " messages) " << dump.error << std::endl;
  });

  double shoulder_position = 0, shoulder_velocity = 0, shoulder_effort = 0;
  double elbow_position = 0, elbow_velocity = 0, elbow_effort = 0;
  auto channel = LogChannel::create("controller");
  channel->registerValue("shoulder/position", &shoulder_position);
  channel->registerValue("shoulder/velocity", &shoulder_velocity);
  channel->registerValue("shoulder/effort", &shoulder_effort);
  channel->registerValue("elbow/position", &elbow_position);
  channel->registerValue("elbow/velocity", &elbow_velocity);
  channel->registerValue("elbow/effort", &elbow_effort);
  channel->addDataSink(worker);
  channel->startLogging();

  // A 1 kHz control loop; a fault at t = 1 s.
  for(int i = 0; i < 1500; i++)
  {
    const double t = i * 0.001;
    shoulder_position = std::sin(4 * t);
    shoulder_velocity = 4 * std::cos(4 * t);
    shoulder_effort = 0.2 * std::sin(4 * t);
    elbow_position = std::sin(4 * t + 0.5);
    elbow_velocity = 4 * std::cos(4 * t + 0.5);
    elbow_effort = 0.2 * std::sin(4 * t + 0.5);
    (void)channel->tryTakeSnapshot();
    if(i == 1000)
    {
      // Real-time safe: one atomic compare-exchange, no allocation. The result is false
      // when an earlier request is still active, which cannot happen here.
      (void)recorder.requestDump(250ms);
    }
    std::this_thread::sleep_for(1ms);
  }

  // Deliver what is queued, then write a dump requested just before shutdown, if
  // any, and wait for the writer.
  worker->stop();
  return 0;
}
