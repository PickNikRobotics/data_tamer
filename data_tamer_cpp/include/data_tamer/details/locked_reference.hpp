#pragma once

#include "data_tamer/details/mutex.hpp"
#include <utility>

namespace DataTamer
{

/**
 * @brief The MutablePtr is a wrapper to a
 * pointer that locks a mutex in the constructor
 * and unlocks it in the destructor.
 * It allows the user to access the reference/pointer and modify the object.
 */
template <typename T>
class LockedPtr
{
public:
  LockedPtr(T* obj, Mutex* mutex);
  LockedPtr(const LockedPtr&) = delete;
  LockedPtr& operator=(const LockedPtr&) = delete;
  LockedPtr(LockedPtr&&);
  LockedPtr& operator=(LockedPtr&&);
  ~LockedPtr();

  /// True if the object is not nullptr
  operator bool() const { return obj_ != nullptr; }
  /// Return the reference
  T& operator*() { return *obj_; }
  /// Return the pointer
  T* operator->() { return obj_; }

private:
  T* obj_ = nullptr;
  Mutex* mutex_ = nullptr;
};

//----------------------------------------------------
//----------------------------------------------------
//----------------------------------------------------

template <typename T>
inline LockedPtr<T>::LockedPtr(T* obj, Mutex* mutex) : obj_(obj), mutex_(mutex)
{
  if(mutex_)
  {
    mutex_->lock();
  }
}

template <typename T>
inline LockedPtr<T>::LockedPtr(LockedPtr&& other) : mutex_(other.mutex_)
{
  std::swap(obj_, other.obj_);
}

template <typename T>
inline LockedPtr<T>& LockedPtr<T>::operator=(LockedPtr<T>&& other)
{
  mutex_ = &other.mutex_;
  std::swap(obj_, other.obj_);
  return *this;
}

template <typename T>
inline LockedPtr<T>::~LockedPtr()
{
  if(mutex_)
  {
    mutex_->unlock();
  }
}

}  // namespace DataTamer
