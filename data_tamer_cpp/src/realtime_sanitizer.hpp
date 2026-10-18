#pragma once

// RealtimeSanitizer's C interface, as weak references: they resolve in a program linked
// with the RTSan runtime (-fsanitize=realtime), also when this library is not built
// with it, and are null otherwise. ELF only: elsewhere RTSan sees no allowed call.
// __rtsan_notify_blocking_call() is what Clang calls at the entry of a
// [[clang::blocking]] function built with -fsanitize=realtime.
#if defined(__ELF__) && (defined(__GNUC__) || defined(__clang__))
#define DATA_TAMER_HAS_RTSAN_HOOKS 1
extern "C" void __rtsan_disable() __attribute__((weak));
extern "C" void __rtsan_enable() __attribute__((weak));
extern "C" void __rtsan_notify_blocking_call(const char* function) __attribute__((weak));
#endif

namespace DataTamer2::details
{
/**
 * @brief Tells RealtimeSanitizer that the scope makes a call the real-time contract
 * allows (CLAUDE.md, "Invariants"): the release of the write mutex, a FUTEX_UNLOCK_PI
 * call when a writer waits for it, and the futex wake of a sleeping worker or of a
 * registered waiter. RTSan reports every pthread_mutex_unlock() and syscall() under a
 * [[clang::nonblocking]] caller; the rest of the real-time path, serializers included,
 * stays checked. Without the runtime: a load and a branch.
 */
class AllowedRealTimeCall
{
public:
#if defined(DATA_TAMER_HAS_RTSAN_HOOKS)
  AllowedRealTimeCall() noexcept : active_(__rtsan_disable != nullptr)
  {
    if(active_)
    {
      __rtsan_disable();
    }
  }
  ~AllowedRealTimeCall()
  {
    if(active_)
    {
      __rtsan_enable();
    }
  }
#else
  AllowedRealTimeCall() noexcept {}  // user-provided: no unused-variable warning
#endif
  AllowedRealTimeCall(const AllowedRealTimeCall&) = delete;
  AllowedRealTimeCall& operator=(const AllowedRealTimeCall&) = delete;

#if defined(DATA_TAMER_HAS_RTSAN_HOOKS)
private:
  // No member without the hooks: GCC 11 and 12 reject [[maybe_unused]] on one, and
  // Clang warns about an unused private field.
  bool active_ = false;
#endif
};

/// Reports a call of `function` (a blocking one) to RTSan, which aborts if the caller
/// is [[clang::nonblocking]]: what Clang does for a [[clang::blocking]] function.
inline void notifyBlockingCall([[maybe_unused]] const char* function) noexcept
{
#if defined(DATA_TAMER_HAS_RTSAN_HOOKS)
  if(__rtsan_notify_blocking_call)
  {
    __rtsan_notify_blocking_call(function);
  }
#endif
}
}  // namespace DataTamer2::details
