// Compile-fail probe (see data_tamer_compile_fail_test in tests/CMakeLists.txt).
// A DataSink is attached through its SinkWorker. As written the probe passes workers,
// also of a class derived from SinkWorker; each DT_* macro passes the bare sink, as 1.x
// code does, which must fail with a message that says how to create the worker.
#include "data_tamer_v2/data_tamer.hpp"
#include "data_tamer_v2/sinks/dummy_sink.hpp"

#include <memory>

namespace probe
{
using namespace DataTamer2;

struct MyWorker : SinkWorker
{
  MyWorker() : SinkWorker(std::make_unique<DummySink>()) {}
};

void attach(LogChannel& channel, ChannelsRegistry& registry)
{
  channel.addDataSink(DummySink::create());
  channel.addDataSink(std::make_shared<MyWorker>());
  channel.removeDataSink(std::make_shared<MyWorker>());
  registry.addDefaultSink(std::make_shared<MyWorker>());
  [[maybe_unused]] auto sink = std::make_shared<DummySink>();
#ifdef DT_ADD_DATA_SINK
  channel.addDataSink(sink);
#endif
#ifdef DT_REMOVE_DATA_SINK
  channel.removeDataSink(sink);
#endif
#ifdef DT_ADD_DEFAULT_SINK
  registry.addDefaultSink(sink);
#endif
}
}  // namespace probe
