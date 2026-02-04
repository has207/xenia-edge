/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/mutex.h"
#if XE_PLATFORM_WIN32 == 1
#include "xenia/base/platform_win.h"
#elif XE_PLATFORM_LINUX == 1
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace xe {
#if XE_PLATFORM_WIN32 == 1 && XE_ENABLE_FAST_WIN32_MUTEX == 1
// default spincount for entercriticalsection is insane on windows, 0x20007D0i64
// (33556432 times!!) when a lock is highly contended performance degrades
// sharply on some processors todo: perhaps we should have a set of optional
// jobs that processors can do instead of spinning, for instance, sorting a list
// so we have better locality later or something
#define XE_CRIT_SPINCOUNT 128
/*
chrispy: todo, if a thread exits before releasing the global mutex we need to
check this and release the mutex one way to do this is by using FlsAlloc and
PFLS_CALLBACK_FUNCTION, which gets called with the fiber local data when a
thread exits
*/

static CRITICAL_SECTION* global_critical_section(xe_global_mutex* mutex) {
  return reinterpret_cast<CRITICAL_SECTION*>(mutex);
}

xe_global_mutex::xe_global_mutex() {
  InitializeCriticalSectionEx(global_critical_section(this), XE_CRIT_SPINCOUNT,
                              CRITICAL_SECTION_NO_DEBUG_INFO);
}
xe_global_mutex ::~xe_global_mutex() {
  DeleteCriticalSection(global_critical_section(this));
}

void xe_global_mutex::lock() {
  EnterCriticalSection(global_critical_section(this));
}
void xe_global_mutex::unlock() {
  LeaveCriticalSection(global_critical_section(this));
}
bool xe_global_mutex::try_lock() {
  BOOL success = TryEnterCriticalSection(global_critical_section(this));
  return success;
}

CRITICAL_SECTION* fast_crit(xe_fast_mutex* mutex) {
  return reinterpret_cast<CRITICAL_SECTION*>(mutex);
}
xe_fast_mutex::xe_fast_mutex() {
  InitializeCriticalSectionEx(fast_crit(this), XE_CRIT_SPINCOUNT,
                              CRITICAL_SECTION_NO_DEBUG_INFO);
}
xe_fast_mutex::~xe_fast_mutex() { DeleteCriticalSection(fast_crit(this)); }

void xe_fast_mutex::lock() { EnterCriticalSection(fast_crit(this)); }
void xe_fast_mutex::unlock() { LeaveCriticalSection(fast_crit(this)); }
bool xe_fast_mutex::try_lock() {
  return TryEnterCriticalSection(fast_crit(this));
}
#elif XE_PLATFORM_LINUX == 1 && XE_ENABLE_FAST_LINUX_MUTEX == 1

namespace {

inline int futex_wait(std::atomic<uint32_t>* addr, uint32_t expected) {
  return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, nullptr,
                 nullptr, 0);
}

inline int futex_wake(std::atomic<uint32_t>* addr, int count) {
  return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr,
                 0);
}

inline pid_t gettid() { return static_cast<pid_t>(syscall(SYS_gettid)); }

}  // namespace

// xe_global_mutex implementation (recursive)
void xe_global_mutex::lock() {
  pid_t self = gettid();

  // Fast path: check if we already own it (recursive lock)
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return;
  }

  // Try to acquire with a simple CAS first (uncontended case)
  uint32_t expected = 0;
  if (XE_LIKELY(state_.compare_exchange_strong(
          expected, 1, std::memory_order_acquire, std::memory_order_relaxed))) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    return;
  }

  lock_slow();
}

void xe_global_mutex::lock_slow() {
  pid_t self = gettid();

  // Spin phase
  for (int i = 0; i < XE_LINUX_MUTEX_SPINCOUNT; ++i) {
#if XE_ARCH_AMD64 == 1
    _mm_pause();
#endif
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }
  }

  // Slow path: use futex
  while (true) {
    // Mark as contended (state = 2) and wait
    uint32_t state = state_.exchange(2, std::memory_order_acquire);
    if (state == 0) {
      // We got the lock while marking contended
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }

    // Wait on futex
    futex_wait(&state_, 2);

    // Try to acquire after wakeup
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      owner_.store(self, std::memory_order_relaxed);
      recursion_count_ = 1;
      return;
    }
  }
}

void xe_global_mutex::unlock() {
  if (--recursion_count_ > 0) {
    return;  // Still have recursive locks
  }

  owner_.store(0, std::memory_order_relaxed);

  // If state was 2 (contended), we need to wake a waiter
  if (state_.exchange(0, std::memory_order_release) == 2) {
    futex_wake(&state_, 1);
  }
}

bool xe_global_mutex::try_lock() {
  pid_t self = gettid();

  // Check for recursive lock
  if (owner_.load(std::memory_order_relaxed) == self) {
    ++recursion_count_;
    return true;
  }

  uint32_t expected = 0;
  if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                     std::memory_order_relaxed)) {
    owner_.store(self, std::memory_order_relaxed);
    recursion_count_ = 1;
    return true;
  }
  return false;
}

// xe_fast_mutex implementation (non-recursive)
void xe_fast_mutex::lock() {
  // Fast path: uncontended
  uint32_t expected = 0;
  if (XE_LIKELY(state_.compare_exchange_strong(
          expected, 1, std::memory_order_acquire, std::memory_order_relaxed))) {
    return;
  }

  lock_slow();
}

void xe_fast_mutex::lock_slow() {
  // Spin phase
  for (int i = 0; i < XE_LINUX_MUTEX_SPINCOUNT; ++i) {
#if XE_ARCH_AMD64 == 1
    _mm_pause();
#endif
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      return;
    }
  }

  // Slow path: use futex
  while (true) {
    // Mark as contended (state = 2) and wait
    uint32_t state = state_.exchange(2, std::memory_order_acquire);
    if (state == 0) {
      // We got the lock while marking contended
      return;
    }

    // Wait on futex
    futex_wait(&state_, 2);

    // Try to acquire after wakeup
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                       std::memory_order_relaxed)) {
      return;
    }
  }
}

void xe_fast_mutex::unlock() {
  // If state was 2 (contended), we need to wake a waiter
  if (state_.exchange(0, std::memory_order_release) == 2) {
    futex_wake(&state_, 1);
  }
}

bool xe_fast_mutex::try_lock() {
  uint32_t expected = 0;
  return state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                        std::memory_order_relaxed);
}

#endif
// chrispy: moved this out of body of function to eliminate the initialization
// guards. Heap-allocated and intentionally leaked to avoid static destruction
// order issues on macOS/POSIX where threads may outlive static destructors.
static global_mutex_type* global_mutex = new global_mutex_type();
global_mutex_type& global_critical_region::mutex() { return *global_mutex; }

}  // namespace xe
