#pragma once

#include "data_tamer/types.hpp"
#include "data_tamer/details/locked_reference.hpp"

#include <memory>
#include <shared_mutex>

namespace DataTamer
{

class LogChannel;

/**
 * @brief The LoggedValue class is a container of a variable that
 * automatically register/unregister to a Channel when created/destroyed.
 *
 * Setting and getting values is thread-safe.
 */
template <typename T>
class LoggedValue
{
protected:
  LoggedValue(const std::shared_ptr<LogChannel>& channel, const std::string& name,
              T initial_value);

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
  void set(const T& value, bool auto_enable = true);

  /// @brief get the stored value.
  [[nodiscard]] T get();

  /// This method allows us to access a reference to the object in a thread-safe way.
  /// A mutex remain locked as long as LockedRef<> exist, therefore you should destroy
  /// it as soon as you used the reference. Example:
  ///
  /// if(auto ref = value->getMutablePtr())
  /// {
  ///   *ref += 1;
  /// }
  [[nodiscard]] LockedPtr<T> getLockedPtr();

  /// @brief Disabling a LoggedValue means that we will not record it in the snapshot
  void setEnabled(bool enabled);

  [[nodiscard]] bool isEnabled() const { return enabled_; }

private:
  std::weak_ptr<LogChannel> channel_;
  T value_ = {};
  RegistrationID id_;
  bool enabled_ = true;
  std::shared_mutex rw_mutex_;
};

}  // namespace DataTamer
