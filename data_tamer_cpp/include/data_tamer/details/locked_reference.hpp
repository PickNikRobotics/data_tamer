#pragma once

#include "data_tamer/details/write_mutex.hpp"

#include <atomic>
#include <utility>

/// The channel's transaction lock. Exclusive and priority-inheriting: a
/// writer holding it is boosted to the priority of a waiting snapshot thread.
/// (Was std::shared_mutex; lock_shared() is no longer available.)
using Mutex = DataTamer::WriteMutex;

namespace DataTamer
{
using ::Mutex;

/**
 * @brief Const pointer that holds the mutex for its lifetime.
 */
template <typename T>
class ConstPtr
{
public:
  ConstPtr(const T* obj, Mutex* mutex);
  ConstPtr(const ConstPtr&) = delete;
  ConstPtr& operator=(const ConstPtr&) = delete;
  ConstPtr(ConstPtr&&) noexcept;
  ConstPtr& operator=(ConstPtr&&) noexcept;
  ~ConstPtr();

  explicit operator bool() const { return obj_ != nullptr; }
  [[deprecated("the lock is held for the lifetime of this object; there is nothing to lock manually")]]
  Mutex* mutex() { return mutex_; }
  const T& operator*() const { return *obj_; }
  const T* operator->() const { return obj_; }

private:
  const T* obj_ = nullptr;
  Mutex* mutex_ = nullptr;
};

/**
 * @brief Mutable pointer that holds the mutex for its lifetime.
 */
template <typename T>
class MutablePtr
{
public:
  MutablePtr(T* obj, Mutex* mutex);
  MutablePtr(const MutablePtr&) = delete;
  MutablePtr& operator=(const MutablePtr&) = delete;
  MutablePtr(MutablePtr&&) noexcept;
  MutablePtr& operator=(MutablePtr&&) noexcept;
  ~MutablePtr();

  explicit operator bool() const { return obj_ != nullptr; }
  [[deprecated("the lock is held for the lifetime of this object; there is nothing to lock manually")]]
  Mutex* mutex() { return mutex_; }
  T& operator*() { return *obj_; }
  T* operator->() { return obj_; }

private:
  T* obj_ = nullptr;
  Mutex* mutex_ = nullptr;
};

/**
 * @brief Write-back proxy for an atomic scalar: holds a local copy, and the
 * destructor stores it back with one atomic store. No lock is involved, so
 * two overlapping proxies on the same value are last-writer-wins.
 */
template <typename T>
class AtomicProxy
{
public:
  explicit AtomicProxy(std::atomic<T>* target)
    : target_(target), copy_(target ? target->load(std::memory_order_relaxed) : T{})
  {}
  AtomicProxy(const AtomicProxy&) = delete;
  AtomicProxy& operator=(const AtomicProxy&) = delete;
  AtomicProxy(AtomicProxy&& other) noexcept : target_(other.target_), copy_(other.copy_)
  {
    other.target_ = nullptr;
  }
  AtomicProxy& operator=(AtomicProxy&& other) noexcept
  {
    if(this != &other)
    {
      commit();
      target_ = other.target_;
      copy_ = other.copy_;
      other.target_ = nullptr;
    }
    return *this;
  }
  ~AtomicProxy() { commit(); }

  explicit operator bool() const { return target_ != nullptr; }
  Mutex* mutex() { return nullptr; }
  T& operator*() { return copy_; }
  T* operator->() { return &copy_; }

private:
  void commit()
  {
    if(target_)
    {
      target_->store(copy_, std::memory_order_relaxed);
    }
  }
  std::atomic<T>* target_;
  T copy_;
};

/// Read-only counterpart of AtomicProxy: a copy taken at construction.
template <typename T>
class AtomicConstProxy
{
public:
  explicit AtomicConstProxy(const std::atomic<T>* target)
    : valid_(target != nullptr), copy_(target ? target->load(std::memory_order_relaxed) : T{})
  {}
  explicit operator bool() const { return valid_; }
  Mutex* mutex() { return nullptr; }
  const T& operator*() const { return copy_; }
  const T* operator->() const { return &copy_; }

private:
  bool valid_;
  T copy_;
};

//----------------------------------------------------

template <typename T>
inline ConstPtr<T>::ConstPtr(const T* obj, Mutex* mutex) : obj_(obj), mutex_(mutex)
{
  if(mutex_)
  {
    mutex_->lock();
  }
}

template <typename T>
inline ConstPtr<T>::ConstPtr(ConstPtr&& other) noexcept : obj_(other.obj_), mutex_(other.mutex_)
{
  other.obj_ = nullptr;
  other.mutex_ = nullptr;
}

template <typename T>
inline ConstPtr<T>& ConstPtr<T>::operator=(ConstPtr&& other) noexcept
{
  if(this != &other)
  {
    if(mutex_)
    {
      mutex_->unlock();
    }
    obj_ = other.obj_;
    mutex_ = other.mutex_;
    other.obj_ = nullptr;
    other.mutex_ = nullptr;
  }
  return *this;
}

template <typename T>
inline ConstPtr<T>::~ConstPtr()
{
  if(mutex_)
  {
    mutex_->unlock();
  }
}

template <typename T>
inline MutablePtr<T>::MutablePtr(T* obj, Mutex* mutex) : obj_(obj), mutex_(mutex)
{
  if(mutex_)
  {
    mutex_->lock();
  }
}

template <typename T>
inline MutablePtr<T>::MutablePtr(MutablePtr&& other) noexcept : obj_(other.obj_), mutex_(other.mutex_)
{
  other.obj_ = nullptr;
  other.mutex_ = nullptr;
}

template <typename T>
inline MutablePtr<T>& MutablePtr<T>::operator=(MutablePtr&& other) noexcept
{
  if(this != &other)
  {
    if(mutex_)
    {
      mutex_->unlock();
    }
    obj_ = other.obj_;
    mutex_ = other.mutex_;
    other.obj_ = nullptr;
    other.mutex_ = nullptr;
  }
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
