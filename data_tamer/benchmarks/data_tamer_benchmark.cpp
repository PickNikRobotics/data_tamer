#include <benchmark/benchmark.h>
#include "data_tamer/data_sink.hpp"
#include "data_tamer/data_tamer.hpp"

using namespace DataTamer;

class NullSink : public DataSinkBase
{
public:

  void addChannel(std::string const&, Schema const& ) override {}
  bool storeSnapshot(const Snapshot&) override { return true; }
};


static void DT_TakeSnapshot(benchmark::State& state)
{
  std::vector<float> values(size_t(state.range(0)));

  auto channel = ChannelsRegistry::Global().getChannel("channel");
  channel->addDataSink(std::make_shared<NullSink>());

  channel->registerValue("values", &values);

  for (auto _ : state)
  {
    channel->takeSnapshot();
  }
}


BENCHMARK(DT_TakeSnapshot)->Arg(125)->Arg(250)->Arg(500)->Arg(1000)->Arg(2000);

BENCHMARK_MAIN();
