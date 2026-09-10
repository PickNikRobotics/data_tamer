#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <system_error>

#if defined(__linux__)
#include <pthread.h>
#define DATA_TAMER_HAS_PI_MUTEX 1
#else
#define DATA_TAMER_HAS_PI_MUTEX 0
#pragma message("data_tamer: no PTHREAD_PRIO_INHERIT on this platform; WriteMutex is a plain mutex " \
                "and the snapshot thread's wait for a writer is not bounded by priority inheritance")
#endif

namespace DataTamer
{

/**
 * @brief Exclusive mutex with priority inheritance (PTHREAD_PRIO_INHERIT) where
 * the platform supports it. Satisfies the C++ Lockable requirements.
 *
 * Shared by the writer threads of a channel and by the snapshot thread. A
 * writer holding it is boosted to the priority of any waiter, so the snapshot
 * thread's wait is bounded by the writer's critical section rather than by
 * the scheduler.
 */
class WriteMutex
{
public:
  static constexpr bool kPriorityInheritance = (DATA_TAMER_HAS_PI_MUTEX == 1);

  /// Spin budget used by lockWithSpin() when called without an argument.
  /// Longer than any legal writer critical section, so the futex sleep is
  /// only reached when a writer was preempted mid-transaction.
  static constexpr std::int64_t kLockSpinNs = 2000;

  WriteMutex()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    int rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if(rc == 0)
    {
      rc = pthread_mutex_init(&mutex_, &attr);
    }
    pthread_mutexattr_destroy(&attr);
    if(rc != 0)
    {
      throw std::system_error(rc, std::generic_category(),
                              "WriteMutex: priority-inheriting mutex not available");
    }
#endif
  }

  ~WriteMutex()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutex_destroy(&mutex_);
#endif
  }

  WriteMutex(const WriteMutex&) = delete;
  WriteMutex& operator=(const WriteMutex&) = delete;
  WriteMutex(WriteMutex&&) = delete;
  WriteMutex& operator=(WriteMutex&&) = delete;

  void lock()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutex_lock(&mutex_);
#else
    mutex_.lock();
#endif
  }

  bool try_lock()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    return pthread_mutex_trylock(&mutex_) == 0;
#else
    return mutex_.try_lock();
#endif
  }

  void unlock()
  {
#if DATA_TAMER_HAS_PI_MUTEX
    pthread_mutex_unlock(&mutex_);
#else
    mutex_.unlock();
#endif
  }

  /**
   * @brief Spin on try_lock() for at most spin_ns, then block in lock().
   * @return true if the call had to block (i.e. a writer held the mutex for
   *         longer than the spin budget). Callers use this to count contention.
   */
  bool lockWithSpin(std::int64_t spin_ns = kLockSpinNs)
  {
    if(try_lock())
    {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);
    while(std::chrono::steady_clock::now() < deadline)
    {
      if(try_lock())
      {
        return false;
      }
    }
    lock();
    return true;
  }

private:
#if DATA_TAMER_HAS_PI_MUTEX
  pthread_mutex_t mutex_;
#else
  std::mutex mutex_;
#endif
};

}  // namespace DataTamer
