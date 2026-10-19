#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>

// The umbrella header: ChannelsRegistry plus LogChannel, LoggedValue and the sink
// interface (channel.hpp includes data_sink.hpp). A header that only names these types
// can include data_tamer_v2/fwd.hpp instead.
#include "data_tamer_v2/channel.hpp"

namespace DataTamer2
{

/**
 * @brief Settings that ChannelsRegistry::getChannel() applies to the channels it
 * creates, before attaching the default sinks. Zero keeps the library default. Set
 * the pool as a count or in time, not both.
 *
 * ABI: passed by value, so a change of its layout requires a SONAME bump.
 */
struct ChannelDefaults
{
  /// LogChannel::setPoolCapacity(count): pool slots per channel.
  size_t pool_capacity = 0;
  /// LogChannel::setPoolCapacity(stall_tolerance, snapshot_period): the pool
  /// sized in time. Set both or neither.
  std::chrono::nanoseconds pool_stall_tolerance{ 0 };
  std::chrono::nanoseconds pool_snapshot_period{ 0 };
  /// LogChannel::setPayloadCapacity(bytes): minimum payload bytes per slot.
  size_t payload_capacity = 0;

private:
  friend class ChannelsRegistry;
  friend class LogChannel;

  /// The settings as LogChannel sizes; zero leaves the library default.
  struct Sizes
  {
    /// For LogChannel::setPoolCapacity(count).
    size_t pool_slots = 0;
    /// For LogChannel::setPayloadCapacity(bytes).
    size_t payload_bytes = 0;
  };

  /// Validates the settings and converts the pool in time to a slot count. Throws
  /// std::invalid_argument if both pool forms are set or a duration is not positive,
  /// and std::length_error if a size is too large for the LogChannel setters.
  [[nodiscard]] Sizes resolve() const;
};

/// Creates channels by name and gives each the same default settings and default sinks.
/// Members may be called from any thread.
class ChannelsRegistry
{
public:
  ChannelsRegistry();

  // Defined in the .cpp, where Pimpl is complete.
  ~ChannelsRegistry();

  /// The process-wide registry.
  static ChannelsRegistry& Global();

  /// Attaches the sink to every channel of the registry: those that exist (a started
  /// channel sends its schema to the sink at once, see LogChannel::addDataSink()) and
  /// those getChannel() creates later. If a channel refuses it (eight sinks already,
  /// onSchema() throws, or a callback of this sink calls it: see DataSink),
  /// the call is undone, the sink is not a default sink and the exception propagates.
  /// At most eight default sinks: another throws std::runtime_error.
  void addDefaultSink(std::shared_ptr<SinkWorker> sink);

  /// A DataSink is not attached directly: this overload only makes the error readable.
  template <typename T, std::enable_if_t<details::kIsBareDataSink<T>, bool> = true>
  void addDefaultSink(const std::shared_ptr<T>&)
  {
    static_assert(!details::kIsBareDataSink<T>, "addDefaultSink() takes a SinkWorker: "
                                                "create the sink with "
                                                "MCAPSink::create(...) or "
                                                "SinkWorker::create<YourSink>(...)");
  }

  /// Sets the defaults for the channels getChannel() creates after this call; existing
  /// channels are not changed. Throws std::invalid_argument if the pool is set both as
  /// a count and in time, or a duration is not positive, and std::length_error if a
  /// size is too large; the previous defaults are then kept.
  void setChannelDefaults(const ChannelDefaults& defaults);

  /// Returns the channel with this name, creating it on first use with the channel
  /// defaults and the default sinks. The registry keeps it until clear() or shutdown().
  /// Throws std::runtime_error for a name LogChannel::create() rejects.
  [[nodiscard]] std::shared_ptr<LogChannel> getChannel(std::string const& channel_name);

  /// Calls SinkWorker::stop() once on each sink the registry reaches: the default sinks
  /// and the sinks attached to channels created by getChannel(). Sinks of channels made
  /// with LogChannel::create() are reached only if they are default sinks. The stops
  /// run on the calling thread in no particular order; each delivers what is queued,
  /// then finishes the sink (DataSink::onStop()). The sinks stay attached: restart one
  /// with SinkWorker::start(). Never call it from a sink callback.
  void stopAll();

  /// Drops the registry's references to all channels and default sinks, and resets the
  /// channel defaults. Dropping the last reference to a worker stops it, which can block.
  void clear();

  /// stopAll(), then clear(): every sink the registry reaches has delivered and
  /// finished, and no channel is left borrowing pointers to your variables. Call it at
  /// the end of main() for the global registry. getChannel() afterwards creates a
  /// fresh channel. Never call it from a sink callback.
  void shutdown();

private:
  struct Pimpl;
  std::unique_ptr<Pimpl> _p;
};

}  // namespace DataTamer2
