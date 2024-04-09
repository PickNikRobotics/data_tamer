#pragma once

#include "data_tamer/details/mutex.hpp"
#include <shared_mutex>
#include <utility>

namespace DataTamer
{

/**
 * @brief The ConstPtr is a wrapper to a
 * pointer that locks a shared_mutex in "read-only" mode
 * in the constructor and unlocks it in the destructor.
 * It allows the user to access the constant reference/pointer.
 */
template <typename T>
class ConstPtr
{
public:
  ConstPtr(const T* obj, std::shared_mutex* mutex);
  ConstPtr(const ConstPtr&) = delete;
  ConstPtr& operator=(const ConstPtr&) = delete;
  ConstPtr(ConstPtr&&);
  ConstPtr& operator=(ConstPtr&&);
  ~ConstPtr();

  /// True if the object is not nullptr
  operator bool() const { return obj_ != nullptr; }
  /// Return the const reference
  const T& operator*() const { return *obj_; }
  /// Return the const pointer
  const T* operator->() const { return obj_; }

private:
  const T* obj_ = nullptr;
  std::shared_mutex* mutex_ = nullptr;
};

/**
 * @brief The MutablePtr is a wrapper to a
 * pointer that locks a mutex in the constructor
 * and unlocks it in the destructor.
 * It allows the user to access the reference/pointer and modify the object.
 */
template <typename T>
class MutablePtr
{
public:
  MutablePtr(T* obj, Mutex* mutex);
  MutablePtr(const MutablePtr&) = delete;
  MutablePtr& operator=(const MutablePtr&) = delete;
  MutablePtr(MutablePtr&&);
  MutablePtr& operator=(MutablePtr&&);
  ~MutablePtr();

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
inline ConstPtr<T>::ConstPtr(const T* obj, std::shared_mutex* mutex)
  : obj_(obj), mutex_(mutex)
{
  if(mutex_)
  {
    mutex_->lock_shared();
  }
}

template <typename T>
inline ConstPtr<T>::ConstPtr(ConstPtr&& other) : obj_(other.obj_), mutex_(other.mutex_)
{}

template <typename T>
inline ConstPtr<T>& ConstPtr<T>::operator=(ConstPtr&& other)
{
  mutex_ = &other.mutex_;
  std::swap(obj_, other.obj_);
  return *this;
}

template <typename T>
inline ConstPtr<T>::~ConstPtr()
{
  if(mutex_)
  {
    mutex_->unlock_shared();
  }
}

//----------------------------------------------------

template <typename T>
inline MutablePtr<T>::MutablePtr(T* obj, Mutex* mutex) : obj_(obj), mutex_(mutex)
{
  if(mutex_)
  {
    mutex_->lock();
  }
}

template <typename T>
inline MutablePtr<T>::MutablePtr(MutablePtr&& other) : mutex_(other.mutex_)
{
  std::swap(obj_, other.obj_);
}

template <typename T>
inline MutablePtr<T>& MutablePtr<T>::operator=(MutablePtr<T>&& other)
{
  mutex_ = &other.mutex_;
  std::swap(obj_, other.obj_);
  return *this;
}

template <typename T>
inline MutablePtr<T>::~MutablePtr()
{
  if(mutex_)
  {
    mutex_->unlock();
  }
}

}  // namespace DataTamer
