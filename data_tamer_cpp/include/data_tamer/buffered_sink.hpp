#pragma once

#include "data_tamer/data_sink.hpp"
#include <memory>

namespace DataTamer
{

enum BufferPolicy {
  MAX_TIME_MS = 0, // time in milliseconds
  MAX_MEMORY_KB = 1, // size in kilobytes
  MAX_BUFFER_SIZE = 2 // number of elements in the buffer
};

/**
 * @brief The BufferedSink is a class to
 * create sinks that implement a "rolling buffer"
 * to keep the snapshots in memory and flush them to
 * file (or publish them through inter process communication)
 * when requested.
 *
 * This is a wrapper to a specific implementation.
 * For instance, you can do:
 *
 * auto mcap_sink = std::make_shared<MCAPSink>("saved_data.mcap");
 * auto buffered_sink = std::make_shared<BufferedSinkBase>(mcap_sink);
 */
class BufferedSink: public DataSinkBase
{
public:

  BufferedSink(std::shared_ptr<DataSinkBase> sink_impl);

  uint32_t bufferPolicy(BufferPolicy p) const;
  void setBufferPolicy(BufferPolicy p, uint32_t value);

  bool pushSnapshot(const Snapshot& snapshot) override final;

  bool flush();

  void clear();

protected:
  uint32_t _max_count = std::numeric_limits<uint32_t>::max();
  uint32_t _max_time_ms = std::numeric_limits<uint32_t>::max();
  uint32_t _max_size_kn = std::numeric_limits<uint32_t>::max();

  struct Pimpl;
  std::unique_ptr<Pimpl> _p;
};

}   // namespace DataTamer
