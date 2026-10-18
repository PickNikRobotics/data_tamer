#include <benchmark/benchmark.h>
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/data_tamer_parser.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"
#include "data_tamer_v2/types.hpp"
#include "geometry_types.hpp"
#include "alloc_counter.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// Decoding cost of the standalone parser, on snapshots taken by the library.

using namespace DataTamer2;

struct Recorded
{
  DataTamerParser::Schema schema;
  Snapshot snapshot;
};

/// The schema and one snapshot of a channel whose values `register_values` registers.
template <typename RegisterValues>
static Recorded record(const RegisterValues& register_values)
{
  auto worker = SinkWorker::create<DummySink>();
  auto channel = LogChannel::create("channel");
  channel->addDataSink(worker);
  register_values(*channel);
  (void)channel->takeSnapshot();
  worker->drain();
  Recorded recorded;
  recorded.snapshot = worker->as<DummySink>().latestSnapshot();
  recorded.schema = DataTamerParser::BuildSchemaFromText(ToStr(channel->getSchema()));
  return recorded;
}

/// Times ParseSnapshot() with a callback that uses every name and value, as a reader
/// that stores the values by name does; reports the allocations per decode.
static void measureParse(benchmark::State& state, const Recorded& recorded)
{
  DataTamerParser::SnapshotView view;
  view.schema_hash = recorded.snapshot.schema_hash;
  view.active_mask = { recorded.snapshot.active_mask.data(),
                       recorded.snapshot.active_mask.size() };
  view.payload = { recorded.snapshot.payload.data(), recorded.snapshot.payload.size() };

  size_t values = 0;
  const auto parse = [&] {
    double sum = 0;
    size_t name_chars = 0;
    values = 0;
    const bool ok = DataTamerParser::ParseSnapshot(
        recorded.schema, view,
        [&](const std::string& name, const DataTamerParser::VarNumber& value) {
          name_chars += name.size();
          sum += std::visit([](auto number) { return double(number); }, value);
          values++;
        });
    benchmark::DoNotOptimize(sum);
    benchmark::DoNotOptimize(name_chars);
    return ok;
  };
  if(!parse())
  {
    state.SkipWithError("the snapshot does not match its schema");
    return;
  }

  DataTamerTest::AllocCounter::Scope scope;
  for(auto _ : state)
  {
    benchmark::DoNotOptimize(parse());
  }
  const auto iterations = double(state.iterations());
  state.counters["allocs/op"] = double(scope.allocations()) / iterations;
  state.counters["values/s"] =
      benchmark::Counter(double(values) * iterations, benchmark::Counter::kIsRate);
  state.SetBytesProcessed(int64_t(state.iterations()) *
                          int64_t(recorded.snapshot.payload.size()));
}

// N doubles registered one by one: top-level names only.
static void DTP_Scalars(benchmark::State& state)
{
  std::vector<double> values(size_t(state.range(0)), 1.5);
  measureParse(state, record([&](LogChannel& channel) {
                 for(size_t i = 0; i < values.size(); i++)
                 {
                   channel.registerValue("v_" + std::to_string(i), &values[i]);
                 }
               }));
}

// One vector of N doubles: an indexed name per element.
static void DTP_Vector(benchmark::State& state)
{
  std::vector<double> values(size_t(state.range(0)), 1.5);
  measureParse(state, record([&](LogChannel& channel) {
                 channel.registerValue("values", &values);
               }));
}

// A vector of N poses: nested custom types inside an indexed element.
static void DTP_Poses(benchmark::State& state)
{
  std::vector<TestTypes::Pose> poses(size_t(state.range(0)));
  measureParse(state, record([&](LogChannel& channel) {
                 channel.registerValue("poses", &poses);
               }));
}

// 20 points, then a vector of N poses: more fields of a custom type than the parser
// keeps in place before the poses.
static void DTP_ManyTypes(benchmark::State& state)
{
  std::vector<TestTypes::Point3D> points(20);
  std::vector<TestTypes::Pose> poses(size_t(state.range(0)));
  measureParse(state, record([&](LogChannel& channel) {
                 for(size_t i = 0; i < points.size(); i++)
                 {
                   channel.registerValue("p_" + std::to_string(i), &points[i]);
                 }
                 channel.registerValue("poses", &poses);
               }));
}

BENCHMARK(DTP_Scalars)->Arg(6)->Arg(125)->Arg(1000);
BENCHMARK(DTP_Vector)->Arg(125)->Arg(1000);
BENCHMARK(DTP_Poses)->Arg(10)->Arg(100);
BENCHMARK(DTP_ManyTypes)->Arg(100);

BENCHMARK_MAIN();
