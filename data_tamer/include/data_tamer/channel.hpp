#pragma once

#include "data_tamer/values.hpp"
#include "data_tamer/data_sink.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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
class ChannelsRegistry;

/**
 * @brief The Value class is a container of a variable that
 * automatically register and unregister to a Channel.
 *
 * Setting and getting values is thread-safe.
 */
template <typename T>
class LoggedValue {

protected:
  LoggedValue(const std::shared_ptr<LogChannel>& channel, const std::string& name, T initial_value);

  friend LogChannel;

public:

  LoggedValue() {}

  ~LoggedValue();

  LoggedValue(LoggedValue const& other) = delete;
  LoggedValue& operator=(LoggedValue const& other) = delete;

  LoggedValue(LoggedValue&& other) = default;
  LoggedValue& operator=(LoggedValue&& other) = default;

  /**
   * @brief set the value of the variable.
   *
   * @param value   new value
   * @param auto_enable  if true and the current instance is disabled, call setEnabled(true)
   */
  void set(const T& value, bool auto_enable = false);

  /// @brief get the stored value.
  T get();

  /// @brief Disabling a LoggedValue means that we will not record it in the snapshot
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

/**
 * @brief A LogChannel is a class used to record multiple values in a single
 * "snapshot". Taking a snapshot is done usually in a periodic loop.
 * Usually, instances of LogChannel are accessed through ChannelsRegistry. *
 *
 * There is no limit to the number of tracked values, but sometimes you may
 * want to use different LogChannels in your application wither to:
 *
 * - aggregate them logically or
 * - record them at different frequencies.
 *
 * Use the methods LogChannel::registerValue or LogChannel::createLoggedValue
 * to add a new value.
 * All you values must be registered before calling takeSnapshot for the first time.
 *
 */
class LogChannel : public std::enable_shared_from_this<LogChannel> {

protected:
  // We make this private because the object must be wrapped
  // inside a std::shared_ptr.
  // This allows us to use std::weak_ptr in LoggedValue
  LogChannel(std::string name);

public:
  /// Use this static mentod do create an instance of LogChannel
  static std::shared_ptr<LogChannel> create(std::string name)
  {
    return std::shared_ptr<LogChannel>(new LogChannel(name));
  }

  ~LogChannel();

  LogChannel(const LogChannel&) = delete;
  LogChannel& operator=(const LogChannel&) = delete;

  LogChannel(LogChannel&&) = delete;
  LogChannel& operator=(LogChannel&&) = delete;

  template <typename T>
  RegistrationID registerValue(const std::string& name, T* value);

  template <typename T, typename TArg>
  RegistrationID registerValue(const std::string& name, std::vector<T, TArg>* value);

  template <typename T, size_t N>
  RegistrationID registerValue(const std::string& name, std::array<T, N>* value);

  template <typename T = double>
  [[nodiscard]] std::shared_ptr<LoggedValue<T>> createLoggedValue(
      std::string const& name, T initial_value = T{});

  [[nodiscard]] const std::string& channelName() const;

  /** Enabling / disabling a value is much faster than
   *  registering / unregistering.
   *  It should be prefered when we want to temporary remove a
   *  variable from the snapshot.
   */
  void setEnabled(const RegistrationID& id, bool enable);

  /// NOTE: the unregistered value will not be removed from the Schema
  void unregister(const RegistrationID& id);

  void addDataSink(std::shared_ptr<DataSinkBase> sink);

  /**
   * @brief takeSnapshot copies the current value of all your registered values
   *  and send an instance of Snapshot to all your Sinks.
   *
   * @param timestamp is the time since epoch, by default.
   *
   * @return true is succesfully pushed to all its sinks.
   */
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

  /** You will need to use this if:
   *
  * - your variables were registered using LogChannel::registerValue AND
  * - the variables are being modified in a thread different than the one calling takeSnapshot()
  *
  * No need to worry about LoggedValues (they use the mutex internally)
  */
  std::mutex& writeMutex();

private:

  struct Pimpl;
  std::unique_ptr<Pimpl> _p;

  RegistrationID registerValueImpl(const std::string& name,
                                   ValuePtr&& value_ptr);
};

//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

template <typename T> inline
RegistrationID LogChannel::registerValue(const std::string& name, T* value_ptr) {

  if constexpr (GetBasicType<T>() != BasicType::OTHER )
  {
    return registerValueImpl(name, ValuePtr(value_ptr));
  }
  else {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Only trivialy copyable objects can be registered");
    return RegisterVariable(*this, name, value_ptr);
  }
}

template <typename T, typename TArg> inline
    RegistrationID LogChannel::registerValue(const std::string& prefix, std::vector<T, TArg>* vect)
{
  if(vect->empty())
  {
    return {};
  }
  auto id = registerValue(prefix + "[0]", &(*vect)[0]);
  for(size_t i=1; i<vect->size(); i++)
  {
    id += registerValue(prefix + "[" + std::to_string(i) + "]", &(*vect)[i]);
  }
  return id;
}

template <typename T, size_t N> inline
    RegistrationID LogChannel::registerValue(const std::string& prefix, std::array<T, N>* vect)
{
  auto id = registerValue(prefix + "[0]", &(*vect)[0]);
  for(size_t i=1; i<N; i++)
  {
    id += registerValue(prefix + "[" + std::to_string(i) + "]", &(*vect)[i]);
  }
  return id;
}

template <typename T> inline
std::shared_ptr<LoggedValue<T>> LogChannel::createLoggedValue(
    std::string const& name, T initial_value)
{
  auto val = new LoggedValue<T>(shared_from_this(), name, initial_value);
  return std::shared_ptr<LoggedValue<T>>(val);
}

template <typename T> inline
LoggedValue<T>::LoggedValue(const std::shared_ptr<LogChannel> &channel,
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
void LoggedValue<T>::set(const T& val, bool auto_enable)
{
  if(auto channel = channel_.lock())
  {
    std::lock_guard const lock(channel->writeMutex());
    value_ = val;
    if(!enabled_ && auto_enable) {
      channel->setEnabled(id_, true);
      enabled_ = true;
    }
  }
  else {
    value_ = val;
    if(!enabled_ && auto_enable) {
      enabled_ = true;
    }
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
