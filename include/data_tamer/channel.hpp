#pragma once

#include "data_tamer/values.hpp"
#include "data_tamer/data_sink.hpp"

#include <atomic>
#include <deque>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace DataTamer {

// Utility
inline std::chrono::nanoseconds NsecSinceEpoch()
{
  auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch);
}

class DataSinkBase;
class LogChannel;

/**
 * @brief The Value class is a container of a variable that
 * automatically register and unregister to a Channel.
 *
 * Setting and getting values is thread-safe.
 */
template <typename T>
class LoggedValue {

protected:
  LoggedValue(std::shared_ptr<LogChannel> channel, const std::string& name, T initial_value);

  friend LogChannel;

public:

  LoggedValue() = default;
  ~LoggedValue();

  LoggedValue(LoggedValue const& other) = delete;
  LoggedValue& operator=(LoggedValue const& other) = delete;

  LoggedValue(LoggedValue&& other) = default;
  LoggedValue& operator=(LoggedValue&& other) = default;

  void set(const T& value);

  T get();

  void setEnabled(bool enabled);

  bool isEnabled() const { return enabled_; }

private:
  std::weak_ptr<LogChannel> channel_;
  T value_ = {};
  RegistrationID id_;
  bool enabled_ = true;
};

//---------------------------------------------------------

template <typename T>
RegistrationID RegisterVariable(LogChannel& channel, const std::string& name, T* value);

class LogChannel : public std::enable_shared_from_this<LogChannel> {
public:

  LogChannel(std::string name);
  ~LogChannel();

  LogChannel(const LogChannel&) = delete;
  LogChannel& operator=(const LogChannel&) = delete;

  LogChannel(LogChannel&&) = delete;
  LogChannel& operator=(LogChannel&&) = delete;

  template <typename T>
  RegistrationID registerValue(const std::string& name, T* value);

  template <typename T = double>
  [[nodiscard]] std::shared_ptr<LoggedValue<T>> createLoggedValue(
      std::string const& name, T initial_value = T{});

  [[nodiscard]] const std::string& channelName() const { return channel_name_; }

  void setEnabled(const RegistrationID& id, bool enable);

  void unregister(const RegistrationID& id);

  void addDataSink(std::shared_ptr<DataSinkBase> sink);

  bool takeSnapshot(std::chrono::nanoseconds timestamp = NsecSinceEpoch());

  /**
   * @brief getActiveFlags returns a serialized buffer, where
   * each bit represents if a series is enabled or not.
   * Therefore the vector size will be ceiling(series_count/8).
   *
   * Even if technically we may use vector<bool>, this data structure
   * is already serialized.
   */
  [[nodiscard]] const ActiveMask& getActiveFlags();

  /**
   * @brief getSchema. See description of class Schema
   */
  [[nodiscard]] Schema getSchema() const;


  std::mutex& writeMutex() { return mutex_; }

private:

  struct ValueHolder {
    std::string name;
    bool enabled = true;
    bool registered = true;
    ValuePtr holder;
  };

  std::string channel_name_;

  mutable std::mutex mutex_;

  size_t payload_buffer_size_ = 0;

  std::vector<ValueHolder> series_;
  std::unordered_map<std::string, size_t> registered_values_;

  std::atomic_bool active_flags_dirty_ = true;
  ActiveMask active_flags_;

  std::vector<uint8_t> snapshot_buffer_;
  uint64_t prev_schema_hash_ = 0;

  Schema schema_;

  std::unordered_set<std::shared_ptr<DataSinkBase>> sinks_;

  void updateActiveMask();

  RegistrationID registerValueImpl(const std::string& name,
                                   ValuePtr&& value_ptr);

  void writerThreadLoop();
};


//----------------------------------------------------------------------

template <typename T> inline
RegistrationID LogChannel::registerValue(const std::string& name, T* value_ptr) {

  if constexpr (GetBasicType<T>() != BasicType::OTHER )
  {
    return registerValueImpl(name, ValuePtr(value_ptr));
  }
  else {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Only trivialy copiable objects can be registered");
    return RegisterVariable(*this, name, value_ptr);
  }
}

template <typename T> inline
std::shared_ptr<LoggedValue<T>> LogChannel::createLoggedValue(
    std::string const& name, T initial_value)
{
  auto val = new LoggedValue<T>(shared_from_this(), name, initial_value);
  return std::shared_ptr<LoggedValue<T>>(val);
}

template <typename T> inline
LoggedValue<T>::LoggedValue(std::shared_ptr<LogChannel> channel,
                            const std::string& name,
                            T initial_value):
  channel_(channel),
  value_(initial_value),
  id_(channel->registerValue(name, &value_))
{}


template <typename T> inline
LoggedValue<T>::~LoggedValue()
{
  if(auto channel = channel_.lock())
  {
    channel->unregister(id_);
  }
}

template <typename T> inline
void LoggedValue<T>::setEnabled(bool enabled)
{
  if(auto channel = channel_.lock())
  {
    channel->setEnabled(id_, enabled);
  }
  enabled_ = enabled;
}

template <typename T> inline
void LoggedValue<T>::set(const T& val)
{
  if(auto channel = channel_.lock())
  {
    std::lock_guard const lock(channel->writeMutex());
    value_ = val;
  }
  else {
    value_ = val;
  }
}

template <typename T> inline
T LoggedValue<T>::get()
{
  if(auto channel = channel_.lock())
  {
    std::lock_guard const lock(channel->writeMutex());
    return value_;
  }
  return value_;
}

}  // namespace DataTamer
