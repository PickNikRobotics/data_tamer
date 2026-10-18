#pragma once

namespace DataTamer2
{

/// Defined in the library only: consumers hold it through pointers.
class ChannelSharedState;

/**
 * @brief Scoped ownership of a channel's write mutex, returned by
 * LogChannel::scopedWrite() and held by the guards below and by the set() and get()
 * of a non-scalar LoggedValue. A nested transaction on the same channel and thread
 * does nothing.
 *
 * The constructor and destructor are defined in the library, with the thread-local
 * chain they maintain: it is initial-exec TLS, so no access allocates, also when the
 * library is loaded with dlopen(). Only the size of this class reaches consumers.
 * Destroy it on the creating thread (never across a coroutine suspension) and before
 * the channel or LoggedValue it came from: it does not keep the state alive.
 * Out-of-order destruction is supported: the mutex passes to the next younger
 * transaction on the same channel.
 */
class WriteTransaction
{
public:
  /// Locks the state's write mutex unless this thread holds a transaction on it.
  explicit WriteTransaction(ChannelSharedState& state);
  ~WriteTransaction();

  WriteTransaction(const WriteTransaction&) = delete;
  WriteTransaction& operator=(const WriteTransaction&) = delete;
  WriteTransaction(WriteTransaction&&) = delete;
  WriteTransaction& operator=(WriteTransaction&&) = delete;

private:
  friend class ChannelSharedState;

  ChannelSharedState* state_;
  WriteTransaction* previous_;
  bool owns_ = false;
};

/**
 * @brief Read-only access to a non-scalar LoggedValue. Holds the channel's write
 * transaction while it lives, so it nests inside scopedWrite() and other guards, and
 * must be destroyed on the thread that created it. Not movable:
 * `auto p = value->getConstPtr();` is fine, storing it is not.
 */
template <typename T>
class ConstPtr
{
public:
  ConstPtr(const T* obj, ChannelSharedState& state) : obj_(obj), tx_(state) {}
  ConstPtr(const ConstPtr&) = delete;
  ConstPtr& operator=(const ConstPtr&) = delete;

  const T& operator*() const { return *obj_; }
  const T* operator->() const { return obj_; }

private:
  const T* obj_;
  WriteTransaction tx_;
};

/// Mutable counterpart of ConstPtr. The snapshot thread waits while it lives: keep the
/// scope short and allocation-free.
template <typename T>
class MutablePtr
{
public:
  MutablePtr(T* obj, ChannelSharedState& state) : obj_(obj), tx_(state) {}
  MutablePtr(const MutablePtr&) = delete;
  MutablePtr& operator=(const MutablePtr&) = delete;

  T& operator*() { return *obj_; }
  T* operator->() { return obj_; }

private:
  T* obj_;
  WriteTransaction tx_;
};

}  // namespace DataTamer2
