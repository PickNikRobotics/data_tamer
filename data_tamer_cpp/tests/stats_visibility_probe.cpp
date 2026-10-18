// Built as a shared object by tests/CMakeLists.txt; check_no_stats_symbol.cmake
// then asserts that its dynamic symbol table does not export any stats().
#include "data_tamer_v2/channel.hpp"
#include "data_tamer_v2/data_sink.hpp"
#include "data_tamer_v2/sinks/mcap_ring_sink.hpp"

extern "C" void data_tamer_stats_probe(DataTamer2::LogChannel* channel,
                                       DataTamer2::SinkWorker* worker,
                                       DataTamer2::MCAPRingSink* ring)
{
  (void)channel->stats();
  (void)worker->stats();
  (void)ring->stats();
}
