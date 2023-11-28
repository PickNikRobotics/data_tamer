#pragma once

#include "data_tamer/data_sink.hpp"
#include "data_tamer/details/mutex.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

// Forward declaration. Header will be added in the cpp file
namespace mcap
{
class McapWriter;
}

namespace DataTamer
{

/**
 * @brief The MCAPSink is an implementation of DataSinkBase that
 * will save the data as MCAP file (https://mcap.dev/)
 */
class MCAPSink : public DataSinkBase
{
public:
  /**
   * @brief MCAPSink
   *
   * @param filepath   path of the file to be saved. Should have extension ".mcap"
   * @param do_compression if true, compress the data on the fly. Note that in case of a crash/segfault
   * some of the data may be lost; it is therefore more conservative to leave this to false.
   */
  explicit MCAPSink(std::string const& filepath, bool do_compression = false);

  ~MCAPSink() override;

  void addChannel(std::string const& channel_name, Schema const& schema) override;

  bool storeSnapshot(const Snapshot& snapshot) override;

  /// After a certain amount of time, the MCAP file will be reset
  /// and overwritten. Default value is 600 seconds (10 minutes)
  void setMaxTimeBeforeReset(std::chrono::seconds reset_time);

  // Stop recording (can't be restarted) and save the file
  void stopRecording();

private:
  std::string filepath_;
  bool compression_ = false;
  std::unique_ptr<mcap::McapWriter> writer_;

  std::unordered_map<uint64_t, uint16_t> hash_to_channel_id_;
  std::unordered_map<std::string, Schema> schemas_;

  std::chrono::seconds reset_time_ = std::chrono::seconds(60 * 10);
  std::chrono::system_clock::time_point start_time_;

  bool forced_stop_recording_ = false;
  Mutex schema_mutex_;
  std::recursive_mutex writer_mutex_;

  void openFile(std::string const& filepath);
};

}   // namespace DataTamer
