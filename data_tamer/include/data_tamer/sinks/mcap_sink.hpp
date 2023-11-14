#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/mutex.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>

// Forward declaration. Header will be added in the cpp file
namespace mcap {
class McapWriter;
}

namespace DataTamer {

/**
 * @brief The MCAPSink is an implementation of DataSinkBase that
 * will save the data as MCAP file (https://mcap.dev/)
 */
class MCAPSink : public DataSinkBase {
 public:
  explicit MCAPSink(std::string const& filepath);

  ~MCAPSink() override;

  void addChannel(std::string const& channel_name,
                  Schema const& schema) override;

  bool storeSnapshot(const Snapshot& snapshot) override;

  /// After a certain amount of time, the MCAP file will be reset
  /// and overwritten. Default value is 600 seconds (10 minutes)
  void setMaxTimeBeforeReset(std::chrono::seconds reset_time);

 private:
  std::string filepath_;
  std::unique_ptr<mcap::McapWriter> writer_;

  std::unordered_map<uint64_t, uint16_t> hash_to_channel_id_;
  std::unordered_map<std::string, Schema> schemas_;

  std::chrono::seconds reset_time_ = std::chrono::seconds(60 * 10);
  std::chrono::system_clock::time_point start_time_;

  Mutex schema_mutex_;

  void openFile(std::string const& filepath);
};

}  // namespace DataTamer
