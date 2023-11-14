#pragma once

#ifdef __linux__
#include <pthread.h>
#else // windows code goes here
#include <mutex>
#endif


#include <cstring>
#include <stdexcept>

namespace DataTamer {
/**
 * Implementation of an mutex with pthread_mutex_t and priority inheritance.
 * Linux only: it falls back to std::mutex otherwise.
 */
class Mutex {

public:

  Mutex();
  ~Mutex();

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void lock();
  void unlock() noexcept;
  bool try_lock() noexcept;

private:
#ifdef __linux__
  pthread_mutex_t m_;
#else // windows code goes here
  std::mutex m_;
#endif
};


//-------------------------------------------
#ifdef __linux__

inline Mutex::Mutex() {
  pthread_mutexattr_t attr;

  int res = pthread_mutexattr_init(&attr);
  if (res != 0) {
    throw std::runtime_error{std::string("cannot pthread_mutexattr_init: ") + std::strerror(res)};
  }

  res = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
  if (res != 0) {
    throw std::runtime_error{std::string("cannot pthread_mutexattr_setprotocol: ") + std::strerror(res)};
  }

  res = pthread_mutex_init(&m_, &attr);
  if (res != 0) {
    throw std::runtime_error{std::string("cannot pthread_mutex_init: ") + std::strerror(res)};
  }
}

inline Mutex::~Mutex() { pthread_mutex_destroy(&m_); }

inline void Mutex::lock() {
  auto res = pthread_mutex_lock(&m_);
  if (res != 0) {
    throw std::runtime_error(std::string("failed pthread_mutex_lock: ") + std::strerror(res));
  }
}

inline void Mutex::unlock() noexcept {
  pthread_mutex_unlock(&m_);
}

inline bool Mutex::try_lock() noexcept {
  return pthread_mutex_trylock(&m_) == 0;
}
#else

inline Mutex::Mutex() {}
inline Mutex::~Mutex() {}

inline void Mutex::lock() { m_.lock(); }
inline void Mutex::unlock() noexcept { m_.unlock(); }
inline bool Mutex::try_lock() noexcept { return m_.try_lock(); }

#endif

}  // namespace DataTamer


