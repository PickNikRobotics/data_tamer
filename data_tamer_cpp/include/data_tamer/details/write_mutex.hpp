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
namespace details
{
#if DATA_TAMER_HAS_PI_MUTEX
/// pthread mutex created with PTHREAD_PRIO_INHERIT. The only place that
/// touches the platform primitive.
class PriorityInheritingMutex
{
public:
  PriorityInheritingMutex()
  {
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
  }
  ~PriorityInheritingMutex() { pthread_mutex_destroy(&mutex_); }

  PriorityInheritingMutex(const PriorityInheritingMutex&) = delete;
  PriorityInheritingMutex& operator=(const PriorityInheritingMutex&) = delete;

  void lock() { pthread_mutex_lock(&mutex_); }
  bool try_lock() { return pthread_mutex_trylock(&mutex_) == 0; }
  void unlock() { pthread_mutex_unlock(&mutex_); }

private:
  pthread_mutex_t mutex_;
};
using PlatformWriteMutex = PriorityInheritingMutex;
#else
using PlatformWriteMutex = std::mutex;
#endif
}  // namespace details

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

  WriteMutex() = default;

  WriteMutex(const WriteMutex&) = delete;
  WriteMutex& operator=(const WriteMutex&) = delete;
  WriteMutex(WriteMutex&&) = delete;
  WriteMutex& operator=(WriteMutex&&) = delete;

  void lock() { mutex_.lock(); }
  bool try_lock() { return mutex_.try_lock(); }
  void unlock() { mutex_.unlock(); }

  /**
   * @brief Spin on try_lock() for at most spin_ns, then block in lock().
   * @return true if the call had to block (i.e. a writer held the mutex for
   *         longer than the spin budget). Callers use this to count contention.
   */
  bool lockWithSpin(std::int64_t spin_ns = kLockSpinNs,
                    std::uint64_t* blocked_wait_ns = nullptr)
  {
    if(blocked_wait_ns)
    {
      *blocked_wait_ns = 0;
    }
    if(try_lock())
    {
      return false;
    }
    // The clock is read once per kTriesPerClockCheck attempts: a try_lock is a
    // few ns, a clock read tens of ns, so checking every iteration would spend
    // most of the budget on the clock instead of on the lock.
    constexpr int kTriesPerClockCheck = 16;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(spin_ns);
    do
    {
      for(int i = 0; i < kTriesPerClockCheck; i++)
      {
        if(try_lock())
        {
          return false;
        }
      }
    } while(std::chrono::steady_clock::now() < deadline);
    const auto wait_start = std::chrono::steady_clock::now();
    lock();
    if(blocked_wait_ns)
    {
      *blocked_wait_ns = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - wait_start)
                                          .count());
    }
    return true;
  }

private:
  details::PlatformWriteMutex mutex_;
};

}  // namespace DataTamer
