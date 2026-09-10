#include "data_tamer/details/write_mutex.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

using DataTamer::WriteMutex;

TEST(WriteMutex, IsLockable)
{
  WriteMutex m;
  {
    std::lock_guard<WriteMutex> lk(m);
  }
  {
    std::unique_lock<WriteMutex> lk(m);
    ASSERT_TRUE(lk.owns_lock());
  }
  {
    std::scoped_lock lk(m);
  }
}

TEST(WriteMutex, TryLockFailsWhileHeld)
{
  WriteMutex m;
  m.lock();
  std::atomic_bool other_got_it{ true };
  std::thread t([&] { other_got_it = m.try_lock(); });
  t.join();
  ASSERT_FALSE(other_got_it);
  m.unlock();
  ASSERT_TRUE(m.try_lock());
  m.unlock();
}

TEST(WriteMutex, PriorityInheritanceIsEnabledOnLinux)
{
#if defined(__linux__)
  ASSERT_TRUE(WriteMutex::kPriorityInheritance);
#else
  ASSERT_FALSE(WriteMutex::kPriorityInheritance);
#endif
}

TEST(WriteMutex, LockWithSpinReturnsFalseWhenUncontended)
{
  WriteMutex m;
  ASSERT_FALSE(m.lockWithSpin());
  m.unlock();
}

TEST(WriteMutex, LockWithSpinAcquiresAfterShortHold)
{
  WriteMutex m;
  std::atomic_bool holder_ready{ false };
  std::thread holder([&] {
    m.lock();
    holder_ready = true;
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    m.unlock();
  });
  while(!holder_ready)
  {
  }
  // 500 us hold vs 2 us spin budget: we must block, and we must still acquire
  const bool blocked = m.lockWithSpin();
  m.unlock();
  holder.join();
  ASSERT_TRUE(blocked);
}

TEST(WriteMutex, LockWithSpinDoesNotBlockForVeryShortHold)
{
  WriteMutex m;
  std::atomic_bool holder_ready{ false };
  std::atomic_bool release{ false };
  std::thread holder([&] {
    m.lock();
    holder_ready = true;
    while(!release)
    {
    }
    m.unlock();
  });
  while(!holder_ready)
  {
  }
  std::atomic_bool blocked{ false };
  std::thread waiter([&] { blocked = m.lockWithSpin(/*spin_ns=*/50'000'000); m.unlock(); });
  std::this_thread::sleep_for(std::chrono::microseconds(100));
  release = true;
  waiter.join();
  holder.join();
  // released well inside the 50 ms spin budget -> acquired by spinning
  ASSERT_FALSE(blocked);
}

// Priority inheritance bound. Needs CAP_SYS_NICE; skipped otherwise.
// A SCHED_OTHER holder shares one core with three SCHED_OTHER CPU hogs, so
// without priority inheritance it gets ~1/4 of the core and its 200 us of
// work spans several CFS slices (milliseconds). With PI, the SCHED_FIFO
// waiter boosts the holder the moment it blocks, so the wait is the holder's
// remaining work plus a wake-up: well under 1 ms.
//
// Note: the FIFO waiter must never spin-wait on this core (a spinning FIFO
// thread starves every CFS thread on it, including the holder); it sleeps.
TEST(WriteMutex, PriorityInheritanceBoundsTheWait)
{
  sched_param fifo{};
  fifo.sched_priority = 50;
  if(pthread_setschedparam(pthread_self(), SCHED_FIFO, &fifo) != 0)
  {
    GTEST_SKIP() << "needs CAP_SYS_NICE for SCHED_FIFO";
  }
  struct RestoreScheduler
  {
    cpu_set_t original_affinity;
    RestoreScheduler() { pthread_getaffinity_np(pthread_self(), sizeof(original_affinity), &original_affinity); }
    ~RestoreScheduler()
    {
      sched_param other{};
      pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
      pthread_setaffinity_np(pthread_self(), sizeof(original_affinity), &original_affinity);
    }
  } restore;

  cpu_set_t one_core;
  CPU_ZERO(&one_core);
  CPU_SET(0, &one_core);
  pthread_setaffinity_np(pthread_self(), sizeof(one_core), &one_core);

  WriteMutex m;
  std::atomic_bool stop{ false };
  std::atomic_bool holder_has_lock{ false };

  auto become_cfs_on_core0 = [&] {
    sched_param other{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &other);
    pthread_setaffinity_np(pthread_self(), sizeof(one_core), &one_core);
  };

  std::vector<std::thread> hogs;
  for(int i = 0; i < 3; i++)
  {
    hogs.emplace_back([&] {
      become_cfs_on_core0();
      while(!stop)
      {
      }
    });
  }

  std::thread holder([&] {
    become_cfs_on_core0();
    m.lock();
    holder_has_lock = true;
    // 200 us of CPU work while holding the mutex
    const auto t0 = std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(200))
    {
    }
    m.unlock();
  });

  while(!holder_has_lock)
  {
    std::this_thread::sleep_for(std::chrono::microseconds(50));  // let CFS threads run
  }
  const auto t0 = std::chrono::steady_clock::now();
  m.lock();
  const auto waited = std::chrono::steady_clock::now() - t0;
  m.unlock();
  stop = true;
  holder.join();
  for(auto& h : hogs)
  {
    h.join();
  }
  ASSERT_LT(std::chrono::duration_cast<std::chrono::microseconds>(waited).count(), 1000)
      << "waited " << std::chrono::duration_cast<std::chrono::microseconds>(waited).count()
      << " us: priority inheritance did not bound the wait";
}
