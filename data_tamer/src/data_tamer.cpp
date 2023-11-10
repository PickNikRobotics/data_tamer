#include "data_tamer/data_tamer.hpp"

namespace DataTamer
{

ChannelsRegistry &ChannelsRegistry::Global()
{
  static ChannelsRegistry obj;
  return obj;
}

void ChannelsRegistry::addDefaultSink(std::shared_ptr<DataSinkBase> sink) {
  std::scoped_lock lk(mutex_);
  default_sinks_.insert(sink);
}

std::shared_ptr<LogChannel> ChannelsRegistry::getChannel(
    std::string const& channel_name) {
  std::scoped_lock lk(mutex_);
  auto it = channels_.find(channel_name);
  if (it == channels_.end()) {
    auto new_channel = LogChannel::create(channel_name);
    for (auto const& sink : default_sinks_) {
      new_channel->addDataSink(sink);
    }
    channels_.insert({channel_name, new_channel});
    return new_channel;
  }
  return it->second;
}

}
