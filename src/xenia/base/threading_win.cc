/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <winternl.h>
#include "xenia/base/assert.h"
#include "xenia/base/chrono_steady_cast.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/threading.h"
#include "xenia/base/threading_timer_queue.h"
#if defined(__clang__)
// chrispy: i do not understand why this is an error for clang here
// something about the quoted __FUNCTION__ freaks it out (clang 14.0.1)
#define LOG_LASTERROR()                                                       \
  do {                                                                        \
    XELOGI("Win32 Error 0x{:08X} in {} (...)", GetLastError(), __FUNCTION__); \
  } while (false)
#else
#define LOG_LASTERROR()                                                      \
  do {                                                                       \
    XELOGI("Win32 Error 0x{:08X} in " __FUNCTION__ "(...)", GetLastError()); \
  } while (false)
#endif
typedef HANDLE (*SetThreadDescriptionFn)(HANDLE hThread,
                                         PCWSTR lpThreadDescription);

// sys function for ntyieldexecution, by calling it we sidestep
// RtlGetCurrentUmsThread
XE_NTDLL_IMPORT(NtYieldExecution, cls_NtYieldExecution,
                NtYieldExecutionPointer);
// sidestep the activation context/remapping special windows handles like stdout
XE_NTDLL_IMPORT(NtWaitForSingleObject, cls_NtWaitForSingleObject,
                NtWaitForSingleObjectPointer);

XE_NTDLL_IMPORT(NtSetEvent, cls_NtSetEvent, NtSetEventPointer);
XE_NTDLL_IMPORT(NtSetEventBoostPriority, cls_NtSetEventBoostPriority,
                NtSetEventBoostPriorityPointer);
// difference between NtClearEvent and NtResetEvent is that NtResetEvent returns
// the events state prior to the call, but we dont need that. might need to
// check whether one or the other is faster in the kernel though yeah, just
// checked, the code in ntoskrnl is way simpler for clearevent than resetevent
XE_NTDLL_IMPORT(NtClearEvent, cls_NtClearEvent, NtClearEventPointer);
XE_NTDLL_IMPORT(NtPulseEvent, cls_NtPulseEvent, NtPulseEventPointer);

// heavily called, we dont skip much garbage by calling this, but every bit
// counts
XE_NTDLL_IMPORT(NtReleaseSemaphore, cls_NtReleaseSemaphore,
                NtReleaseSemaphorePointer);

XE_NTDLL_IMPORT(NtDelayExecution, cls_NtDelayExecution,
                NtDelayExecutionPointer);
XE_NTDLL_IMPORT(NtQueryEvent, cls_NtQueryEvent, NtQueryEventPointer);
namespace xe {
namespace threading {

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(GetCurrentThreadId());
}

// https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
#pragma pack(push, 8)
struct THREADNAME_INFO {
  DWORD dwType;      // Must be 0x1000.
  LPCSTR szName;     // Pointer to name (in user addr space).
  DWORD dwThreadID;  // Thread ID (-1=caller thread).
  DWORD dwFlags;     // Reserved for future use, must be zero.
};
#pragma pack(pop)

void raise_thread_name_exception(HANDLE thread, const std::string& name) {
  if (!IsDebuggerPresent()) {
    return;
  }
  THREADNAME_INFO info;
  info.dwType = 0x1000;
  info.szName = name.c_str();
  info.dwThreadID = ::GetThreadId(thread);
  info.dwFlags = 0;
  __try {
    RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR),
                   reinterpret_cast<ULONG_PTR*>(&info));
  } __except (EXCEPTION_EXECUTE_HANDLER) {  // NOLINT
  }
}

static void set_name(HANDLE thread, const std::string_view name) {
  auto kernel = GetModuleHandleW(L"kernel32.dll");
  if (kernel) {
    auto func =
        (SetThreadDescriptionFn)GetProcAddress(kernel, "SetThreadDescription");
    if (func) {
      auto u16name = xe::to_utf16(name);
      func(thread, reinterpret_cast<PCWSTR>(u16name.c_str()));
    }
  }
  raise_thread_name_exception(thread, std::string(name));
}

void set_name(const std::string_view name) {
  set_name(GetCurrentThread(), name);
}

// checked ntoskrnl, it does not modify delay, so we can place this as a
// constant and avoid creating a stack variable
static const LARGE_INTEGER sleepdelay0_for_maybeyield{{0LL}};

void MaybeYield() {
#if 0
#if defined(XE_USE_NTDLL_FUNCTIONS)
	
  NtYieldExecutionPointer.invoke();
#else
  SwitchToThread();
#endif
#else
  // chrispy: SwitchToThread will only switch to a ready thread on the current
  // processor, so if one is not ready we end up spinning, constantly calling
  // switchtothread without doing any work, heating up the users cpu sleep(0)
  // however will yield to threads on other processors and surrenders the
  // current timeslice
#if defined(XE_USE_NTDLL_FUNCTIONS)
  NtDelayExecutionPointer.invoke(0, &sleepdelay0_for_maybeyield);
#else
  ::Sleep(0);
#endif
#endif
  // memorybarrier is really not necessary here...
  // MemoryBarrier();
}
// Expires closer to its due time than NtDelayExecution does.
struct ThreadSleepTimer {
  // Null before Windows 10 1803.
  HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr,
                                         CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_MODIFY_STATE | SYNCHRONIZE);
  ~ThreadSleepTimer() {
    if (handle) {
      CloseHandle(handle);
    }
  }
};

void NanoSleep(int64_t ns) {
  // nanosleep is done in 100 nanosecond increments
  int64_t in_nt_increments = ns / 100LL;
  if (in_nt_increments == 0 && ns != 0) {
    // if we're explicitly requesting a delay of 0 ns, let it go through,
    // otherwise if it was less than a 100ns increment we round up to 100ns
    in_nt_increments = 1;
  }
  in_nt_increments = -in_nt_increments;
  if (ns > 0) {
    thread_local ThreadSleepTimer timer;
    LARGE_INTEGER due_time;
    due_time.QuadPart = in_nt_increments;
    if (timer.handle &&
        SetWaitableTimer(timer.handle, &due_time, 0, nullptr, nullptr, false)) {
      WaitForSingleObject(timer.handle, INFINITE);
      return;
    }
  }
  NtDelayExecutionPointer.invoke(0, &in_nt_increments);
}
void SyncMemory() { MemoryBarrier(); }

void Sleep(std::chrono::microseconds duration) {
  if (duration.count() < 100) {
    MaybeYield();
  } else {
    ::Sleep(static_cast<DWORD>(duration.count() / 1000));
  }
}

void PreciseSleep(std::chrono::nanoseconds duration) {
  // The overshoot this exists to correct is an ARM64-Linux scheduler problem.
  NanoSleep(duration.count());
}

void NanoSleepPrecise(int64_t ns) { NanoSleep(ns); }

TlsHandle AllocateTlsHandle() { return TlsAlloc(); }

bool FreeTlsHandle(TlsHandle handle) { return TlsFree(handle) ? true : false; }

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(TlsGetValue(handle));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return TlsSetValue(handle, reinterpret_cast<void*>(value)) ? true : false;
}

template <typename T>
class Win32Handle : public T {
 public:
  explicit Win32Handle(HANDLE handle) : handle_(handle) {
    assert_not_null(handle);
  }
  ~Win32Handle() override {
    CloseHandle(handle_);
    handle_ = nullptr;
  }

 protected:
  void* native_handle() const override { return handle_; }

  HANDLE handle_ = nullptr;
};

WaitResult Wait(WaitHandle* wait_handle, std::chrono::milliseconds timeout) {
  HANDLE handle = wait_handle->native_handle();
  DWORD result;
  DWORD timeout_dw = DWORD(timeout.count());
#if XE_USE_NTDLL_FUNCTIONS == 1
  LARGE_INTEGER timeout_big;
  timeout_big.QuadPart = -10000LL * static_cast<int64_t>(timeout_dw);
  result = NtWaitForSingleObjectPointer.invoke<NTSTATUS>(
      handle, false, timeout_dw == INFINITE ? nullptr : &timeout_big);
#else
  result = WaitForSingleObject(handle, timeout_dw);
#endif
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal,
                         WaitHandle* wait_handle_to_wait_on,
                         std::chrono::milliseconds timeout) {
  HANDLE handle_to_signal = wait_handle_to_signal->native_handle();
  HANDLE handle_to_wait_on = wait_handle_to_wait_on->native_handle();
  DWORD result = SignalObjectAndWait(handle_to_signal, handle_to_wait_on,
                                     DWORD(timeout.count()), false);
  switch (result) {
    case WAIT_OBJECT_0:
      return WaitResult::kSuccess;
    case WAIT_ABANDONED:
      return WaitResult::kAbandoned;
    case WAIT_TIMEOUT:
      return WaitResult::kTimeout;
    default:
    case WAIT_FAILED:
      return WaitResult::kFailed;
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[],
                                           size_t wait_handle_count,
                                           bool wait_all,
                                           std::chrono::milliseconds timeout) {
  xenia_assert(wait_handle_count <= 64);
  HANDLE handles[64];

  for (size_t i = 0; i < wait_handle_count; ++i) {
    handles[i] = wait_handles[i]->native_handle();
  }
  DWORD result =
      WaitForMultipleObjects(static_cast<DWORD>(wait_handle_count), handles,
                             wait_all ? true : false, DWORD(timeout.count()));
  if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + wait_handle_count) {
    return std::pair<WaitResult, size_t>(WaitResult::kSuccess,
                                         result - WAIT_OBJECT_0);
  } else if (result >= WAIT_ABANDONED_0 &&
             result < WAIT_ABANDONED_0 + wait_handle_count) {
    return std::pair<WaitResult, size_t>(WaitResult::kAbandoned,
                                         result - WAIT_ABANDONED_0);
  }
  switch (result) {
    case WAIT_TIMEOUT:
      return std::pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
    default:
    case WAIT_FAILED:
      return std::pair<WaitResult, size_t>(WaitResult::kFailed, 0);
  }
}

class Win32Event : public Win32Handle<Event> {
 public:
  explicit Win32Event(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Event() override = default;
#if XE_USE_NTDLL_FUNCTIONS == 1
  void Set() override { NtSetEventPointer.invoke(handle_, nullptr); }
  void Reset() override { NtClearEventPointer.invoke(handle_); }
  void Pulse() override { NtPulseEventPointer.invoke(handle_, nullptr); }
  void SetBoostPriority() override {
    // no previous state for boostpriority
    // Boost priority is unimplemented under wine probably because it's not used
    // anywhere in user mode except by us. Maybe some Windows internals uses it
    // see:
    // https://discord.com/channels/308194948048486401/308207592482668545/1027178776599216228
    if (NtSetEventBoostPriorityPointer) {
      NtSetEventBoostPriorityPointer.invoke(handle_);
    } else {
      NtSetEventPointer.invoke(handle_, nullptr);
    }
  }
#else
  void Set() override { SetEvent(handle_); }
  void Reset() override { ResetEvent(handle_); }
  void Pulse() override { PulseEvent(handle_); }

  void SetBoostPriority() override {
    // no win32 version of boostpriority
    SetEvent(handle_);
  }
#endif

  EventInfo Query() override {
    EventInfo result{};
    NtQueryEventPointer.invoke(handle_, 0, &result, sizeof(EventInfo), nullptr);
    return result;
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  HANDLE handle =
      CreateEvent(nullptr, true, initial_state ? true : false, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();

    return nullptr;
  }
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  HANDLE handle =
      CreateEvent(nullptr, false, initial_state ? true : false, nullptr);
  if (handle) {
    return std::make_unique<Win32Event>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Semaphore : public Win32Handle<Semaphore> {
 public:
  explicit Win32Semaphore(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Semaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
#if XE_USE_NTDLL_FUNCTIONS == 1
    return NtReleaseSemaphorePointer.invoke<NTSTATUS>(handle_, release_count,
                                                      out_previous_count) >= 0;
#else
    return ReleaseSemaphore(handle_, release_count,
                            reinterpret_cast<LPLONG>(out_previous_count))
               ? true
               : false;
#endif
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count,
                                             int maximum_count) {
  HANDLE handle =
      CreateSemaphore(nullptr, initial_count, maximum_count, nullptr);
  if (handle) {
    return std::make_unique<Win32Semaphore>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Mutant : public Win32Handle<Mutant> {
 public:
  explicit Win32Mutant(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Mutant() = default;
  bool Release() override { return ReleaseMutex(handle_) ? true : false; }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  HANDLE handle = CreateMutex(nullptr, initial_owner ? true : false, nullptr);
  if (handle) {
    return std::make_unique<Win32Mutant>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Timer : public Win32Handle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit Win32Timer(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Timer() { Cancel(); }

  bool SetOnceAfter(xe::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback) override {
    return SetOnceAt(WClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback) override {
    return SetOnceAt(date::clock_cast<WClock_>(due_time),
                     std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback) override {
    return Set(due_time, std::chrono::milliseconds::zero(),
               std::move(opt_callback));
  }

  bool SetRepeatingAfter(
      xe::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(WClock_::now() + rel_time, period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(date::clock_cast<WClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time,
                      std::chrono::milliseconds period,
                      std::function<void()> opt_callback) override {
    return Set(due_time, period, std::move(opt_callback));
  }

  bool Cancel() override {
    // Disarm outside mutex_, a running callback takes it to copy callback_.
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
    return CancelWaitableTimer(handle_) ? true : false;
  }

 private:
  bool Set(WClock_::time_point due_time, std::chrono::milliseconds period,
           std::function<void()> opt_callback) {
    // The handle carries the signaled state, the timer queue delivers the
    // callback, an APC routine would need the arming thread to wait alertably.
    Cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    LARGE_INTEGER due_time_li;
    // SetWaitableTimer treats a negative due time as relative.
    due_time_li.QuadPart =
        std::max(due_time.time_since_epoch().count(), WClock_::rep(0));
    if (!SetWaitableTimer(handle_, &due_time_li, int32_t(period.count()), NULL,
                          NULL, false)) {
      return false;
    }
    if (opt_callback) {
      callback_ = std::move(opt_callback);
      auto due = date::clock_cast<GClock_>(due_time);
      if (period.count()) {
        wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due, period);
      } else {
        wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due);
      }
    }
    return true;
  }

  static void CompletionRoutine(void* userdata) {
    auto timer = static_cast<Win32Timer*>(userdata);
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

  std::mutex mutex_;
  std::function<void()> callback_;
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, true, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  HANDLE handle = CreateWaitableTimer(NULL, false, NULL);
  if (handle) {
    return std::make_unique<Win32Timer>(handle);
  } else {
    LOG_LASTERROR();
    return nullptr;
  }
}

class Win32Thread : public Win32Handle<Thread> {
 public:
  explicit Win32Thread(HANDLE handle) : Win32Handle(handle) {}
  ~Win32Thread() = default;

  void set_name(std::string name) override {
    // this can actually happen in some debug builds
    if (&name == nullptr) {
      return;
    }
    xe::threading::set_name(handle_, name);
    Thread::set_name(name);
  }

  uint32_t system_id() const override { return GetThreadId(handle_); }

  void set_priority(int32_t new_priority) override {
    SetThreadPriority(handle_, new_priority);
  }

  void Resume() override { ResumeThread(handle_); }

 private:
  void AssertCallingThread() {
    assert_true(GetCurrentThreadId() == GetThreadId(handle_));
  }
};

thread_local std::unique_ptr<Win32Thread> current_thread_ = nullptr;

struct ThreadStartData {
  std::function<void()> start_routine;
};
DWORD WINAPI ThreadStartRoutine(LPVOID parameter) {
  current_thread_ = std::make_unique<Win32Thread>(::GetCurrentThread());

  auto start_data = reinterpret_cast<ThreadStartData*>(parameter);
  start_data->start_routine();
  delete start_data;
  return 0;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  auto start_data = new ThreadStartData({std::move(start_routine)});
  HANDLE handle =
      CreateThread(NULL, params.stack_size, ThreadStartRoutine, start_data,
                   params.create_suspended ? CREATE_SUSPENDED : 0, NULL);
  if (handle) {
    return std::make_unique<Win32Thread>(handle);
  } else {
    LOG_LASTERROR();
    delete start_data;
    return nullptr;
  }
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_.get();
  }

  HANDLE handle = ::GetCurrentThread();
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }

  current_thread_ = std::make_unique<Win32Thread>(handle);
  return current_thread_.get();
}

void Thread::Exit(int exit_code) { ExitThread(exit_code); }

}  // namespace threading
}  // namespace xe
