#pragma once

#include "data_tamer/data_sink.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

// Forward declaration
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
   * @brief MCAPSink.
   * IMPORTANT: if you want the recorder to be more robust to crash/segfault,
   * set `do_compression` to false.
   * Compression is dafe if your application is closing cleanly.
   *
   * @param filepath   path of the file to be saved. Should have extension ".mcap"
   * @param do_compression if true, compress the data on the fly.
   */
  explicit MCAPSink(std::string const& filepath, bool do_compression = false);

  ~MCAPSink() override;

  void addChannel(std::string const& channel_name, Schema const& schema) override;

  bool storeSnapshot(const Snapshot& snapshot) override;

  /// After a certain amount of time, the MCAP file will be reset
  /// and overwritten. Default value is 600 seconds (10 minutes)
  void setMaxTimeBeforeReset(std::chrono::seconds reset_time);

  /// Stop recording and save the file
  void stopRecording();

  /**
   * @brief restartRecording saves the current file (unless we did it already,
   * calling stopRecording) and start recording into a new one.
   * Note that all the registered channels and their schemas will be copied into the new file.
   *
   * @param filepath   file path of the new file (should be ".mcap" extension)
   * @param do_compression if true, compress the data on the fly.
   */
  void restartRecording(std::string const& filepath, bool do_compression = false);

private:
  std::string filepath_;
  bool compression_ = false;
  std::unique_ptr<mcap::McapWriter> writer_;

  std::unordered_map<uint64_t, uint16_t> hash_to_channel_id_;
  std::unordered_map<std::string, Schema> schemas_;

  std::chrono::seconds reset_time_ = std::chrono::seconds(60 * 10);
  std::chrono::system_clock::time_point start_time_;

  bool forced_stop_recording_ = false;
  std::recursive_mutex mutex_;

  void openFile(std::string const& filepath);
};

}   // namespace DataTamer
