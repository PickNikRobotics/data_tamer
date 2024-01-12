#pragma once

#include <memory>
#include <mutex>

namespace DataTamer
{
/**
 * @brief The LockedRef class is used to share a pointer to an object
 * and a mutex that protects the read/write access to that object.
 *
 * As long as the object remains in scope, the mutex is locked, therefore
 * you must destroy this object as soon as the pointer was used.
 */
template <typename T, class Mutex>
class LockedRef
{
public:
  LockedRef() = default;

  LockedRef(T* obj, Mutex* obj_mutex) : ref_(obj), mutex_(obj_mutex) { mutex_->lock(); }

  ~LockedRef()
  {
    if (mutex_)
    {
      mutex_->unlock();
    }
  }

  LockedRef(LockedRef const&) = delete;
  LockedRef& operator=(LockedRef const&) = delete;

  LockedRef(LockedRef&& other)
  {
    std::swap(ref_, other.ref_);
    std::swap(mutex_, other.mutex_);
  }

  LockedRef& operator=(LockedRef&& other)
  {
    std::swap(ref_, other.ref_);
    std::swap(mutex_, other.mutex_);
  }

  operator bool() const { return ref_ != nullptr; }

  void lock()
  {
    if (mutex_)
    {
      mutex_->lock();
    }
  }

  void unlock()
  {
    if (mutex_)
    {
      mutex_->unlock();
    }
  }

  bool empty() const { return ref_ == nullptr; }

  const T& operator()() const { return *ref_; }

  T& operator()() { return *ref_; }

private:
  T* ref_ = nullptr;
  Mutex* mutex_ = nullptr;
};

}   // namespace DataTamer
