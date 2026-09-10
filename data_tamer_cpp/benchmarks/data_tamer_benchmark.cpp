#include <benchmark/benchmark.h>
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"
#include "../examples/geometry_types.hpp"
#include "alloc_counter.hpp"

#include <atomic>
#include <thread>

using namespace DataTamer;

class NullSink : public DataSinkBase
{
public:
  ~NullSink() override { stopThread(); }
  void addChannel(std::string const&, Schema const&) override {}
  bool storeSnapshot(const Snapshot&) override { return true; }
};

static void DT_Doubles(benchmark::State& state)
{
  std::vector<double> values(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  channel->registerValue("values", &values);
  channel->takeSnapshot();  // warm-up
  channel->takeSnapshot();

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    DataTamerTest::AllocCounter::Scope scope;
    channel->takeSnapshot();
    allocs += scope.allocations();
  }
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
}

static void DT_PoseType(benchmark::State& state)
{
  std::vector<TestTypes::Pose> poses(size_t(state.range(0)));
  auto registry = ChannelsRegistry();
  auto channel = registry.getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());
  channel->registerValue("values", &poses);
  channel->takeSnapshot();
  channel->takeSnapshot();

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    DataTamerTest::AllocCounter::Scope scope;
    channel->takeSnapshot();
    allocs += scope.allocations();
  }
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
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
  channel->takeSnapshot();
  channel->takeSnapshot();

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    DataTamerTest::AllocCounter::Scope scope;
    channel->takeSnapshot();
    allocs += scope.allocations();
  }
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
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
static void DT_SnapshotWithWriter(benchmark::State& state)
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
  channel->takeSnapshot();

  std::atomic_bool run{ true };
  std::thread writer([&] {
    double x = 0;
    while(run)
    {
      for(auto& v : values)
      {
        v->set(x);
      }
      x += 1.0;
    }
  });

  std::size_t allocs = 0;
  for(auto _ : state)
  {
    DataTamerTest::AllocCounter::Scope scope;
    channel->takeSnapshot();
    allocs += scope.allocations();
  }
  run = false;
  writer.join();
  state.counters["allocs/op"] = double(allocs) / double(state.iterations());
}

BENCHMARK(DT_Doubles)->Arg(125)->Arg(250)->Arg(500)->Arg(1000)->Arg(2000);
BENCHMARK(DT_PoseType)->Arg(125)->Arg(250)->Arg(500)->Arg(1000);
BENCHMARK(DT_MultiSink)->Arg(1)->Arg(2)->Arg(4);
BENCHMARK(DT_LoggedValueSet)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK(DT_SnapshotWithWriter);

BENCHMARK_MAIN();
