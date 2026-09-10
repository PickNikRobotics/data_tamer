#include <benchmark/benchmark.h>
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "../examples/geometry_types.hpp"
#include "alloc_counter.hpp"
#include "null_sink.hpp"

#include <atomic>
#include <thread>

using namespace DataTamer;

/// Warm up, then time takeSnapshot() while counting the allocations it makes
/// on this thread; reports them as the "allocs/op" counter.
static void measureSnapshots(benchmark::State& state, LogChannel& channel)
{
  channel.takeSnapshot();  // warm-up: buffers reach their steady-state capacity
  channel.takeSnapshot();

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    DataTamerTest::AllocCounter::Scope scope;
    channel.takeSnapshot();
    allocs += scope.allocations();
  }
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
}

static void DT_Doubles(benchmark::State& state)
{
  std::vector<double> values(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  channel->registerValue("values", &values);
  measureSnapshots(state, *channel);
}

static void DT_PoseType(benchmark::State& state)
{
  std::vector<TestTypes::Pose> poses(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  channel->registerValue("values", &poses);
  measureSnapshots(state, *channel);
}

// 1000 doubles, N sinks
static void DT_MultiSink(benchmark::State& state)
{
  std::vector<double> values(1000);
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  for(int i = 0; i < state.range(0); i++)
  {
    channel->addDataSink(std::make_shared<NullSink>());
  }
  channel->registerValue("values", &values);
  measureSnapshots(state, *channel);
}

// LoggedValue<double>::set from the calling thread
static void DT_LoggedValueSet(benchmark::State& state)
{
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  std::vector<std::shared_ptr<LoggedValue<double>>> values;
  for(int i = 0; i < state.range(0); i++)
  {
    values.push_back(channel->createLoggedValue<double>("v" + std::to_string(i)));
  }
  double x = 0;
  for(auto _ : state)
  {
    for(auto& v : values)
    {
      v->set(x);
    }
    x += 1.0;
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// takeSnapshot on this thread while another thread hammers set() on 100 LoggedValues
static void snapshotWithWriter(benchmark::State& state, bool transactions)
{
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  std::vector<std::shared_ptr<LoggedValue<double>>> values;
  for(int i = 0; i < 100; i++)
  {
    values.push_back(channel->createLoggedValue<double>("v" + std::to_string(i)));
  }
  std::vector<double> plain(1000);
  channel->registerValue("plain", &plain);

  std::atomic_bool run{ true };
  std::thread writer([&] {
    double x = 0;
    while(run)
    {
      if(transactions)
      {
        auto transaction = channel->scopedWrite();
        for(auto& v : values)
        {
          v->set(x);
        }
      }
      else
      {
        for(auto& v : values)
        {
          v->set(x);
        }
      }
      x += 1.0;
    }
  });

  measureSnapshots(state, *channel);
  run = false;
  writer.join();
}

static void DT_SnapshotWithWriter(benchmark::State& state)
{
  snapshotWithWriter(state, false);
}

static void DT_SnapshotWithTransactionWriter(benchmark::State& state)
{
  snapshotWithWriter(state, true);
}

BENCHMARK(DT_Doubles)->Arg(125)->Arg(250)->Arg(500)->Arg(1000)->Arg(2000);
BENCHMARK(DT_PoseType)->Arg(125)->Arg(250)->Arg(500)->Arg(1000);
BENCHMARK(DT_MultiSink)->Arg(1)->Arg(2)->Arg(4);
BENCHMARK(DT_LoggedValueSet)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(DT_SnapshotWithWriter);
BENCHMARK(DT_SnapshotWithTransactionWriter);

BENCHMARK_MAIN();
