#pragma once

#include "data_tamer/channel.hpp"

namespace DataTamer {

class ChannelsRegistry {
public:

  // global instance (similar to singleton)
  static ChannelsRegistry& Global();

  /// Once added here, this sink will be automatically connected to
  /// any new channel created with getChannel()
  void addDefaultSink(std::shared_ptr<DataSinkBase> sink);

  /// Create a new channel or get a previously create one.
  [[nodiscard]] std::shared_ptr<LogChannel> getChannel(
      std::string const& channel_name);

private:
  std::unordered_map<std::string, std::shared_ptr<LogChannel>> channels_;
  std::unordered_set<std::shared_ptr<DataSinkBase>> default_sinks_;
};


}  // namespace DataTamer
