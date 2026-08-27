/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/threading.h"

#include "xenia/base/assert.h"
#include "xenia/base/chrono_steady_cast.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading_timer_queue.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>

#if defined(__GLIBCXX__)
#include <cxxabi.h>
#endif

#include "xenia/base/logging.h"

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#if XE_PLATFORM_LINUX
#include <semaphore.h>
#endif

#if XE_PLATFORM_ANDROID
#include <dlfcn.h>

#include "xenia/base/main_android.h"
#include "xenia/base/string_util.h"
#endif

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>

#include "xenia/base/cvar.h"
#ifndef HWCAP_EVTSTRM
#define HWCAP_EVTSTRM (1 << 2)
#endif

DEFINE_bool(wfe_precise_sleep, false,
            "ARM64: busy-wait PreciseSleep's sub-millisecond tail on the "
            "generic-timer event stream (WFE) for exact deadlines. Off by "
            "default: the spin holds threads on-CPU (~18% in profiling) for "
            "frame-pacing precision that plain nanosleep matches in practice.",
            "CPU");
#endif

#if XE_PLATFORM_LINUX
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define XE_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() syscall(SYS_gettid)
#endif
#else
#define XE_HAS_SIGEV_THREAD_ID 0
#endif

namespace xe {
namespace threading {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf,
                                          size_t n);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ =
          reinterpret_cast<decltype(android_pthread_getname_np_)>(
              dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Thread interruption is done using user-defined signals
// This implementation uses the SIGRTMAX - SIGRTMIN to signal to a thread
// gdb tip, for SIG = SIGRTMIN + SignalType : handle SIG nostop
// lldb tip, for SIG = SIGRTMIN + SignalType : process handle SIG -s false
enum class SignalType {
  kThreadSuspend,
  kThreadUserCallback,
#if XE_PLATFORM_ANDROID
  // pthread_cancel is not available on Android, using a signal handler for
  // simplified PTHREAD_CANCEL_ASYNCHRONOUS-like behavior - not disabling
  // cancellation currently, so should be enough.
  kThreadTerminate,
#endif
  k_Count
};

#if XE_PLATFORM_MAC
// macOS lacks real-time signals (SIGRTMIN/SIGRTMAX). Use SIGUSR1/SIGUSR2.
int GetSystemSignal(SignalType num) {
  switch (num) {
    case SignalType::kThreadSuspend:
      return SIGUSR1;
    case SignalType::kThreadUserCallback:
      return SIGUSR2;
    default:
      assert_always();
      return SIGUSR1;
  }
}

SignalType GetSystemSignalType(int num) {
  switch (num) {
    case SIGUSR1:
      return SignalType::kThreadSuspend;
    case SIGUSR2:
      return SignalType::kThreadUserCallback;
    default:
      assert_always();
      return SignalType::k_Count;
  }
}
#else
int GetSystemSignal(SignalType num) {
  auto result = SIGRTMIN + static_cast<int>(num);
  assert_true(result < SIGRTMAX);
  return result;
}

SignalType GetSystemSignalType(int num) {
  return static_cast<SignalType>(num - SIGRTMIN);
}
#endif

std::array<std::atomic<bool>, static_cast<size_t>(SignalType::k_Count)>
    signal_handler_installed = {};

static void signal_handler(int signal, siginfo_t* info, void* context);

void install_signal_handler(SignalType type) {
  bool expected = false;
  if (!signal_handler_installed[static_cast<size_t>(type)]
           .compare_exchange_strong(expected, true)) {
    return;
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    signal_handler_installed[static_cast<size_t>(type)] = false;
  }
}

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
#if XE_PLATFORM_MAC
  return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#else
  return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

void MaybeYield() {
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() { __sync_synchronize(); }

static void SleepFor(timespec rqtp) {
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

void Sleep(std::chrono::microseconds duration) {
  SleepFor(DurationToTimeSpec(duration));
}

void NanoSleep(int64_t duration) {
  // Sleep(microseconds) would truncate a sub-microsecond request to zero.
  if (duration <= 0) {
    return;
  }
  SleepFor({static_cast<time_t>(duration / 1000000000LL),
            static_cast<long>(duration % 1000000000LL)});
}

void PreciseSleep(std::chrono::nanoseconds duration) {
  if (duration.count() <= 0) {
    return;
  }
#if defined(__aarch64__) && defined(__linux__)
  static const bool use_wfe =
      cvars::wfe_precise_sleep && (getauxval(AT_HWCAP) & HWCAP_EVTSTRM) != 0;
  if (use_wfe) {
    uint64_t freq, now;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(now));
    // 128-bit intermediate: at a 1GHz counter the 64-bit product wraps for
    // durations beyond ~18s, which would silently drop the precise tail.
    const uint64_t deadline =
        now +
        static_cast<uint64_t>(static_cast<unsigned __int128>(duration.count()) *
                              freq / 1000000000u);
    // Sleep the bulk conventionally and absorb the scheduler's wakeup error
    // in the event-stream tail. The tail must exceed the worst wakeup
    // latency being corrected for; 1.5ms covers CFS + timer slack here.
    constexpr int64_t kTailNs = 1500000;
    if (duration.count() > 2 * kTailNs) {
      Sleep(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::nanoseconds(duration.count() - kTailNs)));
    }
    for (;;) {
      __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(now));
      if (now >= deadline) {
        break;
      }
      __asm__ __volatile__("wfe" ::: "memory");
    }
    // The old path always crossed a syscall (a de-facto full barrier); keep
    // that visible-ordering parity on the pure-WFE return.
    __sync_synchronize();
    return;
  }
#endif
  Sleep(std::chrono::duration_cast<std::chrono::microseconds>(duration));
}

void NanoSleepPrecise(int64_t ns) {
#if XE_PLATFORM_MAC
  // Darwin's nanosleep can oversleep by 100-500us under load. Land precisely
  // on the deadline by using mach_wait_until for the bulk of the wait and
  // busy-waiting the last ~200us.
  if (ns <= 0) {
    return;
  }
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t i;
    mach_timebase_info(&i);
    return i;
  }();
  constexpr uint64_t kSpinTailNs = 200'000;
  const uint64_t deadline =
      mach_absolute_time() +
      static_cast<uint64_t>((static_cast<__uint128_t>(ns) * tb.denom) /
                            tb.numer);
  const uint64_t spin_tail = static_cast<uint64_t>(
      (static_cast<__uint128_t>(kSpinTailNs) * tb.denom) / tb.numer);
  if (deadline > mach_absolute_time() + spin_tail) {
    mach_wait_until(deadline - spin_tail);
  }
  while (mach_absolute_time() < deadline) {
  }
#else
  NanoSleep(ns);
#endif
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  Sleep(duration);
  alertable_state_ = false;
  return SleepResult::kSuccess;
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) { return pthread_key_delete(handle) == 0; }

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(handle));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(handle, reinterpret_cast<void*>(value)) == 0;
}

// Multi-object waits: each signal bumps a generation and notifies this
// condvar so WaitMultiple wakes at once; the timeout cap covers a miss.
static std::mutex g_multi_wait_mutex;
static std::condition_variable g_multi_wait_cv;
static std::atomic<uint64_t> g_multi_wait_gen{0};
static std::atomic<uint32_t> g_multi_waiters{0};
static void PokeMultiWaiters() {
  // Bump before reading the count so a waiter that re-checks gen after
  // registering sees it. Not a guarantee: neither this nor the notify below is
  // ordered against a waiter mid-park. The waiter's 1ms cap is the real bound.
  g_multi_wait_gen.fetch_add(1, std::memory_order_release);
  if (g_multi_waiters.load(std::memory_order_acquire) == 0) {
    // Nobody is parked, so skip the process-global mutex entirely. This path
    // runs from the realtime audio callback, where blocking on a lock held by
    // an ordinary guest thread costs an audio deadline.
    return;
  }
  // Notified without holding the mutex, so a realtime caller never blocks on
  // it. A wakeup lost to the register/sleep race costs at most the 1ms park
  // cap the waiter already applies.
  g_multi_wait_cv.notify_all();
}

// Scopes the parked-waiter count; a cancelled wait must not leak it.
struct ScopedMultiWaiter {
  ScopedMultiWaiter() {
    g_multi_waiters.fetch_add(1, std::memory_order_release);
  }
  ~ScopedMultiWaiter() {
    g_multi_waiters.fetch_sub(1, std::memory_order_release);
  }
};

// Parked on from the suspend signal handler, so it must stay async-signal-safe.
class SuspendGate {
 public:
  SuspendGate() {
#if !XE_PLATFORM_MAC
    sem_ok_ = sem_init(&sem_, 0, 0) == 0;
#endif
  }
  ~SuspendGate() { Destroy(); }

  SuspendGate(const SuspendGate&) = delete;
  SuspendGate& operator=(const SuspendGate&) = delete;

  // Must run before the signal is raised, never inside the handler.
  bool Arm() {
#if XE_PLATFORM_MAC
    std::lock_guard guard(create_mutex_);
    if (destroyed_) {
      return false;
    }
    if (fds_[0] >= 0) {
      return true;
    }
    if (pipe(fds_) != 0) {
      XELOGE("SuspendGate: pipe() failed: {}", strerror(errno));
      fds_[0] = fds_[1] = -1;
      return false;
    }
    fcntl(fds_[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds_[1], F_SETFD, FD_CLOEXEC);
    // Post() writes under state_mutex_, so the write end must never block.
    fcntl(fds_[1], F_SETFL, fcntl(fds_[1], F_GETFL, 0) | O_NONBLOCK);
    return true;
#else
    return sem_ok_ && !destroyed_;
#endif
  }

  bool Wait() {
#if XE_PLATFORM_MAC
    if (fds_[0] < 0) {
      return false;
    }
    uint8_t token;
    ssize_t n;
    do {
      n = read(fds_[0], &token, 1);  // Flawfinder: ignore
    } while (n < 0 && errno == EINTR);
#else
    if (!sem_ok_) {
      return false;
    }
    int ret;
    do {
      ret = sem_wait(&sem_);
    } while (ret == -1 && errno == EINTR);
#endif
    return true;
  }

  void Post() {
#if XE_PLATFORM_MAC
    if (fds_[1] < 0) {
      return;
    }
    const uint8_t token = 1;
    ssize_t n;
    do {
      n = write(fds_[1], &token, 1);
    } while (n < 0 && errno == EINTR);
    // EAGAIN means a token is already pending, which wakes the gate just as
    // well; WaitSuspended loops on suspend_count_, so an extra one is fine.
#else
    if (sem_ok_) {
      sem_post(&sem_);
    }
#endif
  }

  void Destroy() {
#if XE_PLATFORM_MAC
    std::lock_guard guard(create_mutex_);
    destroyed_ = true;
    if (fds_[0] >= 0) {
      close(fds_[0]);
      fds_[0] = -1;
    }
    if (fds_[1] >= 0) {
      close(fds_[1]);
      fds_[1] = -1;
    }
#else
    if (!destroyed_ && sem_ok_) {
      destroyed_ = true;
      sem_destroy(&sem_);
    }
#endif
  }

 private:
#if XE_PLATFORM_MAC
  int fds_[2] = {-1, -1};
  bool destroyed_ = false;
  std::mutex create_mutex_;
#else
  sem_t sem_;
  bool sem_ok_ = false;
  bool destroyed_ = false;
#endif
};

class PosixConditionBase {
 public:
  PosixConditionBase() {
#if !XE_PLATFORM_MAC
    // Initialize as robust mutex to handle thread termination gracefully.
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);

    // Get the native handle and set it as robust.
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    pthread_mutex_destroy(native_mutex);      // Destroy default mutex.
    pthread_mutex_init(native_mutex, &attr);  // Reinit as robust.
    pthread_mutexattr_destroy(&attr);
#endif
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    bool executed;
    auto predicate = [this] { return this->signaled(); };
#if XE_PLATFORM_MAC
    // Standard locking on macOS (no robust mutex support).
    std::unique_lock<std::mutex> lock(mutex_);
#else
    // Handle robust mutex locking.
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
#endif
    if (predicate()) {
      executed = true;
    } else {
      ScopedParked parked(*this);
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;  // Did not time out;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
    }
    if (executed) {
      post_execution();
      lock.unlock();
      post_execution_unlocked();
      return WaitResult::kSuccess;
    }
    return WaitResult::kTimeout;
  }

  // The shared condvar is a notify_all, so poking it for an object nobody
  // multi-waits on wakes every parked thread to re-check handles it does not
  // care about.
  void NotifyAll() {
    // bionic's pthread_cond_broadcast issues the futex syscall even with no
    // sleeper. Under the cooperative scheduler guest waits are fibers, so
    // nearly every signal is uncontended and paid a syscall for nothing.
    // parked_waiters_ is only touched under mutex_, held by every caller.
    if (parked_waiters_ != 0) {
      cond_.notify_all();
    }
    if (multi_wait_refs_.load(std::memory_order_acquire) != 0) {
      PokeMultiWaiters();
    }
  }

  // Number of parked WaitMultiple threads that include this object.
  std::atomic<uint32_t> multi_wait_refs_{0};

  // Registers the caller against every handle it is waiting on, so a signal
  // knows whether waking the shared condvar can possibly help.
  class MultiWaitRegistration {
   public:
    explicit MultiWaitRegistration(
        const std::vector<PosixConditionBase*>& handles)
        : handles_(handles) {
      for (auto* h : handles_) {
        h->multi_wait_refs_.fetch_add(1, std::memory_order_release);
      }
    }
    ~MultiWaitRegistration() {
      for (auto* h : handles_) {
        h->multi_wait_refs_.fetch_sub(1, std::memory_order_release);
      }
    }

   private:
    const std::vector<PosixConditionBase*>& handles_;
  };

  static std::pair<WaitResult, size_t> WaitMultiple(
      std::vector<PosixConditionBase*>&& handles, bool wait_all,
      std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    // For single handle, just use the normal Wait path.
    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }

    MultiWaitRegistration registration(handles);

    // For multiple handles, we need to poll since we can't wait on multiple
    // condition variables simultaneously. This is a limitation of the POSIX
    // condition variable API.
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    while (true) {
#if !XE_PLATFORM_ANDROID
      // Cancellation point, clear of the alloc below.
      pthread_testcancel();
#endif
      // Snapshot the gen before checking so a racing signal isn't missed.
      uint64_t wait_gen = g_multi_wait_gen.load(std::memory_order_acquire);

      // Check all handles to see if any/all are signaled.
      // Use try_lock to avoid deadlocks from lock ordering issues.
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;

      // Try to acquire all locks without blocking.
      std::vector<std::unique_lock<std::mutex>> locks;
      locks.reserve(handles.size());
      bool all_locked = true;

      for (size_t i = 0; i < handles.size(); ++i) {
#if XE_PLATFORM_MAC
        // macOS: no robust mutex support.
        std::unique_lock<std::mutex> lk(handles[i]->mutex_, std::try_to_lock);
        if (!lk.owns_lock()) {
          all_locked = false;
          break;
        }
        locks.emplace_back(std::move(lk));
#else
        // Linux/Android: robust-aware trylock.
        auto native_mutex =
            static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
        int result = pthread_mutex_trylock(native_mutex);
        if (result == 0 || result == EOWNERDEAD) {
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          }
          locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
        } else {
          all_locked = false;
          break;
        }
#endif
      }

      // If we couldn't acquire all locks, release what we have and retry.
      if (!all_locked) {
        locks.clear();
        std::this_thread::yield();
        continue;
      }

      // Now we have all locks, check the condition.
      if (wait_all) {
        // For wait_all, check if ALL are signaled.
        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;
      } else {
        // For wait_any, check if ANY is signaled.
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        // Execute post_execution for the signaled handle(s).
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        locks.clear();
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution_unlocked();
          }
        } else {
          handles[first_signaled]->post_execution_unlocked();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      // Release locks before sleeping.
      locks.clear();

      // Check timeout.
      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      // Park on the shared condvar (any signal wakes us); 1ms caps a lost poke.
      auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(end_time - now);
      auto park = std::min(remaining, std::chrono::milliseconds(1));
      {
        ScopedMultiWaiter parked;
        std::unique_lock<std::mutex> mw_lock(g_multi_wait_mutex);
        g_multi_wait_cv.wait_for(mw_lock, park, [&] {
          return g_multi_wait_gen.load(std::memory_order_acquire) != wait_gen;
        });
      }
    }
  }

  [[nodiscard]] virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  [[nodiscard]] inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;
  // Threads currently inside a cond_ wait on this object. Guarded by mutex_
  // (the wait sites hold it on both sides of the park, so the counter is
  // exact whenever NotifyAll reads it).
  uint32_t parked_waiters_ = 0;
  // Scopes a cond_ wait for the parked-waiter count.
  struct ScopedParked {
    explicit ScopedParked(PosixConditionBase& c) : c_(c) {
      ++c_.parked_waiters_;
    }
    ~ScopedParked() { --c_.parked_waiters_; }
    PosixConditionBase& c_;
  };

  // Runs after mutex_ is released.
  inline virtual void post_execution_unlocked() {}
  std::condition_variable cond_;
  std::mutex mutex_;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  ~PosixCondition() override = default;

  bool Signal() override {
    auto lock = std::unique_lock(mutex_);
    signal_ = true;
    NotifyAll();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock(mutex_);
    signal_ = false;
  }

 private:
  [[nodiscard]] bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> final : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock(mutex_);
    // Validate that releasing would not exceed the maximum count
    if (count_ + release_count > maximum_count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    NotifyAll();
    return true;
  }

 private:
  [[nodiscard]] bool signaled() const override { return count_ > 0; }
  void post_execution() override {
    count_--;
    NotifyAll();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> final : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        NotifyAll();
      }
      return true;
    }
    return false;
  }

  [[nodiscard]] void* native_handle() const override {
    return const_cast<std::mutex&>(mutex_).native_handle();
  }

 private:
  [[nodiscard]] bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> final : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  ~PosixCondition() override { Cancel(); }

  bool Signal() override {
    std::lock_guard lock(mutex_);
    signal_ = true;
    NotifyAll();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time,
               std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period,
                    std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ =
        QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() const {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  [[nodiscard]] void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = static_cast<PosixCondition*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

  [[nodiscard]] bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> final : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0),
        joined_(false) {
#if XE_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params,
                  ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;

    if (params.create_suspended && !suspend_gate_.Arm()) {
      XELOGE("Thread::Create: could not arm the suspend gate");
      return false;
    }

    auto attempt_create = [&](size_t stack_size, bool use_custom_stack_size) {
      pthread_attr_t attr;
      int result = pthread_attr_init(&attr);
      if (result != 0) {
        return result;
      }

      if (use_custom_stack_size) {
        result = pthread_attr_setstacksize(&attr, stack_size);
        if (result != 0) {
          pthread_attr_destroy(&attr);
          return result;
        }
      }

      if (params.initial_priority != 0) {
        sched_param sched{};
#if XE_PLATFORM_MAC
        // Remap into Darwin's SCHED_FIFO range (see set_priority).
        static const int fifo_min = sched_get_priority_min(SCHED_FIFO);
        static const int fifo_max = sched_get_priority_max(SCHED_FIFO);
        sched.sched_priority = fifo_min + (params.initial_priority - 1) *
                                              (fifo_max - fifo_min) / 31;
#else
        sched.sched_priority = params.initial_priority + 1;
#endif
        result = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        if (result != 0) {
          pthread_attr_destroy(&attr);
          return result;
        }
        result = pthread_attr_setschedparam(&attr, &sched);
        if (result != 0) {
          pthread_attr_destroy(&attr);
          return result;
        }
      }

      result = pthread_create(&thread_, &attr, ThreadStartRoutine, start_data);
      pthread_attr_destroy(&attr);
      return result;
    };

    int result = attempt_create(params.stack_size, true);
    if (result == 0) {
      return true;
    }

#if XE_PLATFORM_IOS
    XELOGW(
        "pthread_create failed (stack=0x{:X}, err={} '{}'); "
        "retrying with smaller stack",
        static_cast<uint32_t>(params.stack_size), result,
        std::strerror(result));
    // iOS can fail thread creation under memory pressure if requested stack
    // size is too large. Retry with progressively smaller stacks before giving
    // up.
    size_t retry_stack_size = params.stack_size / 2;
    constexpr size_t kMinRetryStackSize = size_t(1) * 1024 * 1024;
    while (retry_stack_size >= kMinRetryStackSize) {
      result = attempt_create(retry_stack_size, true);
      if (result == 0) {
        XELOGW("pthread_create succeeded with fallback stack size 0x{:X}",
               static_cast<uint32_t>(retry_stack_size));
        return true;
      }
      retry_stack_size /= 2;
    }

    // Final attempt using platform default stack size.
    result = attempt_create(0, false);
    if (result == 0) {
      XELOGW(
          "pthread_create succeeded using platform default stack size after "
          "fallback retries");
      return true;
    }
#endif

    XELOGE("pthread_create failed (stack=0x{:X}, err={} '{}')",
           static_cast<uint32_t>(params.stack_size), result,
           std::strerror(result));
    return false;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
#if XE_PLATFORM_LINUX
        tid_(static_cast<pid_t>(syscall(SYS_gettid))),
#endif
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0),
        joined_(false) {
#if XE_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }

  ~PosixCondition() override {
    // FIXME(RodoMa92): This causes random crashes.
    //  The proper way to handle them according to the webs is properly shutdown
    //  instead on relying on killing them using pthread_cancel.
    /*
    if (thread_ && !signaled_) {
#if XE_PLATFORM_ANDROID
      if (pthread_kill(thread_,
                       GetSystemSignal(SignalType::kThreadTerminate)) != 0) {
        assert_always();
      }
#else
      if (pthread_cancel(thread_) != 0) {
        assert_always();
      }
#endif
      if (pthread_join(thread_, nullptr) != 0) {
        assert_always();
      }
    }
    */
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if XE_PLATFORM_ANDROID
      // pthread_getname_np was added in API 26 - below that, store the name in
      // this object, which may be only modified through Xenia threading, but
      // should be enough in most cases.
      if (android_pthread_getname_np_) {
        if (android_pthread_getname_np_(thread_, result.data(),
                                        result.size() - 1) != 0) {
          assert_always();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        std::strcpy(result.data(), android_pre_api_26_name_);
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        assert_always();
      }
#endif
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) const {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if XE_PLATFORM_MAC
      // macOS can only set the current thread's name.
      if (pthread_self() == thread_) {
        pthread_setname_np(std::string(name).c_str());
      }
#else
      pthread_setname_np(thread_, std::string(name).c_str());
#if XE_PLATFORM_ANDROID
      SetAndroidPreApi26Name(name);
#endif
#endif
    }
  }

#if XE_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    xe::string_util::copy_truncating(android_pre_api_26_name_, name,
                                     xe::countof(android_pre_api_26_name_));
  }
#endif

  uint32_t system_id() const {
#if XE_PLATFORM_MAC
    return static_cast<uint32_t>(pthread_mach_thread_np(thread_));
#else
    return static_cast<uint32_t>(thread_);
#endif
  }

  uint64_t affinity_mask() const {
    WaitStarted();
#if XE_PLATFORM_MAC
    // Thread affinity is not supported on macOS.
    return 0;
#else
    cpu_set_t cpu_set;
#if XE_PLATFORM_ANDROID
    if (sched_getaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t),
                          &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
    uint64_t result = 0;
    auto cpu_count = std::min(CPU_SETSIZE, 64);
    for (auto i = 0u; i < cpu_count; i++) {
      if (CPU_ISSET(i, &cpu_set)) {
        result |= uint64_t(1) << i;
      }
    }
    return result;
#endif
  }

  void set_affinity_mask(uint64_t mask) const {
    WaitStarted();
#if XE_PLATFORM_MAC
    // Thread affinity is not supported on macOS.
    (void)mask;
    return;
#else
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (auto i = 0u; i < 64; i++) {
      if (mask & (uint64_t(1) << i)) {
        CPU_SET(i, &cpu_set);
      }
    }
#if XE_PLATFORM_ANDROID
    if (sched_setaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t),
                          &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
#endif
  }

  int priority() const {
    WaitStarted();
#if XE_PLATFORM_LINUX
    if (fifo_failed_) {
      // Report nice values mapped back into the SCHED_FIFO 1..32 range so
      // callers see a consistent priority space.
      int nice_val = getpriority(PRIO_PROCESS, tid_);
      return 16 - nice_val;
    }
#endif
    int policy;
    sched_param param{};
    int ret = pthread_getschedparam(thread_, &policy, &param);
    if (ret != 0) {
      return -1;
    }
#if XE_PLATFORM_MAC
    // Reverse the mapping applied in set_priority so callers see xenia-space
    // values 1..32 regardless of Darwin's SCHED_FIFO range (typically 15..47).
    static const int fifo_min = sched_get_priority_min(SCHED_FIFO);
    static const int fifo_max = sched_get_priority_max(SCHED_FIFO);
    const int fifo_range = fifo_max - fifo_min;
    if (fifo_range <= 0) {
      return param.sched_priority;
    }
    return 1 + (param.sched_priority - fifo_min) * 31 / fifo_range;
#else
    return param.sched_priority;
#endif
  }

  void set_priority(int new_priority) const {
    WaitStarted();
#if XE_PLATFORM_LINUX
    if (!fifo_failed_) {
#endif
      sched_param param{};
#if XE_PLATFORM_MAC
      // Xenia's POSIX ThreadPriority tiers are 1/8/16/24/32. Darwin's
      // SCHED_FIFO range is typically 15..47, so linearly remap xenia 1..32
      // into that range to keep all five tiers distinct and monotonically
      // ordered. The guest kernel emulates Xenon quantum decay on its own
      // and re-applies priorities via this path as they change.
      static const int fifo_min = sched_get_priority_min(SCHED_FIFO);
      static const int fifo_max = sched_get_priority_max(SCHED_FIFO);
      param.sched_priority =
          fifo_min + (new_priority - 1) * (fifo_max - fifo_min) / 31;
#else
    param.sched_priority = new_priority;
#endif
      int res = pthread_setschedparam(thread_, SCHED_FIFO, &param);
      if (res == 0) {
        return;
      }
#if XE_PLATFORM_LINUX
      // SCHED_FIFO requires CAP_SYS_NICE or root on Linux; fall back to
      // nice values under SCHED_OTHER for unprivileged runs.
      if (res == EPERM) {
        fifo_failed_ = true;
      } else {
        XELOGW("Unexpected error {} while setting SCHED_FIFO priority", res);
        fifo_failed_ = true;
      }
    }
    // Map SCHED_FIFO range (1..32) to nice range (19..-19), fifo 16 → nice 0.
    int nice_val = 16 - new_priority;
    if (nice_val < -20) {
      nice_val = -20;
    }
    if (nice_val > 19) {
      nice_val = 19;
    }
    if (tid_ > 0) {
      setpriority(PRIO_PROCESS, tid_, nice_val);
    }
#else
    XELOGW("pthread_setschedparam failed: err={} '{}'", res,
           std::strerror(res));
#endif
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    std::unique_lock lock(callback_mutex_);
    user_callback_ = std::move(callback);
#if XE_PLATFORM_MAC
    // No pthread_sigqueue on macOS, use pthread_kill (no si_value payload).
    pthread_kill(thread_, GetSystemSignal(SignalType::kThreadUserCallback));
#elif XE_PLATFORM_ANDROID
    sigval value{};
    value.sival_ptr = this;
    sigqueue(pthread_gettid_np(thread_),
             GetSystemSignal(SignalType::kThreadUserCallback), value);
#else
    sigval value{};
    value.sival_ptr = this;
    pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback),
                     value);
#endif
  }

  void CallUserCallback() const {
    std::unique_lock lock(callback_mutex_);
    user_callback_();
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock lock(state_mutex_);
    if (state_ != State::kSuspended) {
      return false;
    }
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0) {
      state_ = State::kRunning;
      suspend_gate_.Post();
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();

    // Check if we're trying to suspend ourselves.
    bool is_current_thread = pthread_self() == thread_;
    bool already_suspended = false;

    if (!suspend_gate_.Arm()) {
      return false;
    }

    {
      std::unique_lock lock(state_mutex_);
      if (state_ == State::kFinished) {
        // Don't resurrect an exiting thread to kSuspended and signal it.
        return false;
      }
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      already_suspended = (state_ == State::kSuspended);
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    // If already suspended, just increment the count — don't send another
    // signal. A second pthread_kill while the thread is in its suspend wait
    // would nest signal handlers, but Resume only wakes once when count
    // reaches 0.
    if (already_suspended) {
      return true;
    }

    if (is_current_thread) {
      // Self-suspension: directly call WaitSuspended instead of signalling
      // ourselves, avoiding signal-handler nesting.
      WaitSuspended();
      return true;
    }

    int result =
        pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend));
    return result == 0;
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          // The other thread's pthread_cancel may never arrive; exit here.
          XELOGW("Terminate: thread was already marked finished; exiting");
          lock.unlock();
#if XE_PLATFORM_MAC && defined(__aarch64__)
          pthread_jit_write_protect_np(1);
#endif
          pthread_exit(nullptr);
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      NotifyAll();
    }
    if (is_current_thread) {
#if XE_PLATFORM_MAC && defined(__aarch64__)
      pthread_jit_write_protect_np(1);
#endif
      pthread_exit(reinterpret_cast<void*>(exit_code));
    }
#ifdef XE_PLATFORM_ANDROID
    if (pthread_kill(thread_, GetSystemSignal(SignalType::kThreadTerminate)) !=
        0) {
      assert_always();
    }
#else
    if (pthread_cancel(thread_) != 0) {
      assert_always();
    }
#endif
  }

  void WaitStarted() const {
    std::unique_lock lock(state_mutex_);
    state_signal_.wait(lock,
                       [this] { return state_ != State::kUninitialized; });
  }

  /// Runs in the signal handler: only SuspendGate may block here.
  void WaitSuspended() {
    // SIGUSR1 does not queue, so a spare token is possible: re-check the count.
    while (suspend_count_.load(std::memory_order_acquire) > 0) {
      if (!suspend_gate_.Wait()) {
        struct timespec ts = {0, 1000 * 1000};
        nanosleep(&ts, nullptr);
      }
    }
  }

  void* native_handle() const override {
    return reinterpret_cast<void*>(thread_);
  }

 private:
  static void* ThreadStartRoutine(void* parameter);
  bool signaled() const override { return signaled_; }
  void post_execution() override {}

  void post_execution_unlocked() override {
    // Terminate() can signal before the thread exits; join unlocked, once.
    bool expected = false;
    if (thread_ && joined_.compare_exchange_strong(expected, true)) {
      pthread_join(thread_, nullptr);
      suspend_gate_.Destroy();
    }
  }
  pthread_t thread_;
#if XE_PLATFORM_LINUX
  pid_t tid_ = 0;                     // Kernel TID for setpriority() fallback.
  mutable bool fifo_failed_ = false;  // True after SCHED_FIFO was rejected.
#endif
  bool signaled_;
  int exit_code_;
  State state_;  // Protected by state_mutex_
  // Atomic: read from the signal handler.
  std::atomic<uint32_t> suspend_count_;
  std::atomic<bool> joined_;
  SuspendGate suspend_gate_;
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::function<void()> user_callback_;
#if XE_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

// Out-of-line destructor to ensure proper RTTI/vtable emission.
PosixWaitHandle::~PosixWaitHandle() = default;

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  [[nodiscard]] void* native_handle() const override {
    return handle_.native_handle();
  }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner)
    : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset)
    : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset,
                                                  bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread)
    : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable,
                std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  auto result = posix_wait_handle->condition().Wait(timeout);
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal,
                         WaitHandle* wait_handle_to_wait_on, bool is_alertable,
                         std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal =
      dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on =
      dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr ||
      posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  if (posix_wait_handle_to_signal->condition().Signal()) {
    result = posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[],
                                           size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (is_alertable) {
    alertable_state_ = true;
  }
  auto result = PosixConditionBase::WaitMultiple(std::move(conditions),
                                                 wait_all, timeout);
  if (is_alertable) {
    alertable_state_ = false;
  }
  return result;
}

class PosixEvent final : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  EventInfo Query() override {
    EventInfo result{};
    assert_always();
    return result;
  }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore final : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count),
                           out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count,
                                             int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count ||
      maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant final : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner)
      : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer final : public PosixConditionHandle<Timer> {
  using WClock_ = WClock_;
  using GClock_ = GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(xe::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(date::clock_cast<GClock_>(due_time),
                     std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(
      xe::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(date::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread final : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params,
                  std::function<void()> start_routine) {
    auto start_data =
        new ThreadStartData({std::move(start_routine), false, this});
    if (!handle_.Initialize(params, start_data)) {
      delete start_data;
      return false;
    }
    return true;
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    std::lock_guard lock(name_mutex_);
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override {
    handle_.set_affinity_mask(mask);
  }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override {
    handle_.set_priority(new_priority);
  }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }

 private:
  mutable std::mutex name_mutex_;
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !XE_PLATFORM_ANDROID
  // Deferred so cancellation unwinds only at safe points, never mid-malloc.
  if (pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr) != 0) {
    assert_always();
  }
#endif
  threading::set_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  // Signal handlers must be able to run when a fiber stack is exhausted, so
  // give every thread an alternate signal stack.
  static thread_local std::unique_ptr<uint8_t[]> signal_stack(
      new uint8_t[64 * 1024]);
  stack_t altstack = {};
  altstack.ss_sp = signal_stack.get();
  altstack.ss_size = 64 * 1024;
  sigaltstack(&altstack, nullptr);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
#if XE_PLATFORM_LINUX
  thread->handle_.tid_ = static_cast<pid_t>(syscall(SYS_gettid));
#endif
  {
    std::unique_lock lock(thread->handle_.state_mutex_);
    // Arm suspend_count_ in the same lock that publishes kSuspended, else a
    // Resume() can land between, see count==0, bail, and wedge us at the gate.
    if (create_suspended) {
      thread->handle_.state_ = State::kSuspended;
      thread->handle_.suspend_count_ = 1;
    } else {
      thread->handle_.state_ = State::kRunning;
    }
    thread->handle_.state_signal_.notify_all();
  }

  // Park where Resume() signals (sem on Linux) so its post can't leak.
  if (create_suspended) {
    thread->handle_.WaitSuspended();
  }

  try {
    start_routine();
  }
#if defined(__GLIBCXX__)
  // pthread_exit/cancel sentinel; must propagate.
  catch (abi::__forced_unwind&) {
    throw;
  }
#endif
  catch (const std::exception& e) {
    XELOGE("Host thread '{}' terminated by uncaught exception: {}",
           thread->handle_.name(), e.what());
  } catch (...) {
    XELOGE("Host thread '{}' terminated by unknown exception",
           thread->handle_.name());
  }

  {
    std::unique_lock lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  std::unique_lock lock(thread->handle_.mutex_);
  thread->handle_.exit_code_ = 0;
  thread->handle_.signaled_ = true;
  // Not cond_ directly: a WaitMultiple on this thread handle needs the poke.
  thread->handle_.NotifyAll();

  current_thread_ = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if XE_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine))) {
    return nullptr;
  }
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
#if XE_PLATFORM_MAC && defined(__aarch64__)
    pthread_jit_write_protect_np(1);
#endif
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_name(const std::string_view name) {
#if XE_PLATFORM_MAC
  pthread_setname_np(std::string(name).c_str());
#else
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#if XE_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
#endif
#endif
}

static void signal_handler(int signal, siginfo_t* info, void* /*context*/) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      if (!current_thread_) {
        // current_thread_ is NULL - this can happen if the signal arrives
        // before the thread has initialized or after it has exited.
        return;
      }
      current_thread_->WaitSuspended();
    } break;
    case SignalType::kThreadUserCallback: {
#if XE_PLATFORM_MAC
      // macOS: no si_value payload when using pthread_kill.
      if (alertable_state_ && current_thread_) {
        auto& condition =
            static_cast<PosixCondition<Thread>&>(current_thread_->condition());
        condition.CallUserCallback();
      }
#else
      assert_not_null(info->si_value.sival_ptr);
      auto p_thread =
          static_cast<PosixCondition<Thread>*>(info->si_value.sival_ptr);
      if (alertable_state_) {
        p_thread->CallUserCallback();
      }
#endif
    } break;
#if XE_PLATFORM_ANDROID
    case SignalType::kThreadTerminate: {
      pthread_exit(reinterpret_cast<void*>(-1));
    } break;
#endif
    default:
      assert_always();
  }
}

}  // namespace threading
}  // namespace xe
