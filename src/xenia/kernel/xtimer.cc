/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

namespace {
// Leaked so they outlive static destruction.
std::mutex& LiveTimersLock() {
  static auto* lock = new std::mutex;
  return *lock;
}
std::vector<XTimer*>& LiveTimers() {
  static auto* timers = new std::vector<XTimer*>;
  return *timers;
}
}  // namespace

XTimer::XTimer(KernelState* kernel_state, bool host_object)
    : XObject(kernel_state, kObjectType, host_object) {
  std::lock_guard<std::mutex> lock(LiveTimersLock());
  LiveTimers().push_back(this);
}

XTimer::~XTimer() {
  {
    std::lock_guard<std::mutex> lock(LiveTimersLock());
    auto& timers = LiveTimers();
    timers.erase(std::remove(timers.begin(), timers.end(), this), timers.end());
  }
  if (timer_) {
    timer_->Cancel();
  }
  RemoveApc();
  memory()->SystemHeapFree(apc_ptr_);
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      signal_ = xe::threading::Event::CreateManualResetEvent(false);
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      signal_ = xe::threading::Event::CreateAutoResetEvent(false);
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

void XTimer::InitializeNative(void* native_ptr,
                              const X_DISPATCH_HEADER* header) {
  Initialize(header->type == X_OBJECT_TYPES::TimerSynchronizationObject ? 1
                                                                        : 0);
  SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);
}

void XTimer::CancelAll() {
  // Global lock first, as ~XTimer is entered holding it.
  auto global_lock = xe::global_critical_region::AcquireDirect();
  std::lock_guard<std::mutex> lock(LiveTimersLock());
  for (XTimer* timer : LiveTimers()) {
    if (timer->timer_) {
      timer->Cancel();
    }
  }
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume,
                          uint32_t dpc) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  // The console expires timers only on its 1 ms clock tick and counts a
  // relative due time from the last tick. An expiry rounds up to a tick and one
  // already due at the last tick fires at once.
  constexpr uint64_t kClockTick = 10000;
  const uint64_t now = XSystemClock::to_file_time(XSystemClock::now());
  const uint64_t last_tick = now - now % kClockTick;
  const uint64_t due = due_time < 0
                           ? last_tick + (0 - static_cast<uint64_t>(due_time))
                           : static_cast<uint64_t>(due_time);
  uint64_t expiry = now;
  if (due > last_tick) {
    expiry = (due + kClockTick - 1) / kClockTick * kClockTick;
  }
  WinSystemClock::time_point due_tp =
      date::clock_cast<WinSystemClock>(XSystemClock::from_file_time(expiry));

  // Clamp past times before the host timers convert them.
  auto now_wsc = WinSystemClock::now();
  if (due_tp < now_wsc) {
    due_tp = now_wsc;
  }

  // The previous expiry must not be able to queue the APC while it is reused.
  timer_->Cancel();
  RemoveApc();
  // KeSetTimer clears the signal state.
  signal_->Reset();

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  XThread* cb_thread = nullptr;
  uint32_t cb_apc = 0;
  if (routine) {
    if (!apc_ptr_) {
      apc_ptr_ = memory()->SystemHeapAlloc(XAPC::kSize);
      if (!apc_ptr_) {
        return X_STATUS_NO_MEMORY;
      }
    }
    apc_thread_ = retain_object(XThread::GetCurrentThread());
    xboxkrnl::xeKeInitializeApc(memory()->TranslateVirtual<XAPC*>(apc_ptr_),
                                apc_thread_->guest_object(),
                                XAPC::kOwnedKernelRoutine, 0, routine, 1,
                                routine_arg);
    cb_thread = apc_thread_.get();
    cb_apc = apc_ptr_;
  }
  const uint32_t native = guest_object();
  // Signal and unwait cooperative waiters, as the timer DPC does.
  xe::threading::Event* signal = signal_.get();
  const bool periodic = period_ms != 0;
  std::function<void()> callback = [this, signal, cb_thread, cb_apc, routine,
                                    routine_arg, dpc, native, periodic]() {
    signal->Set();
    if (native) {
      // Signal the guest KTIMER. A one-shot expiry also leaves the timer
      // queue.
      auto header = memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native);
      header->signal_state = 1;
      if (!periodic) {
        header->inserted = 0;
      }
    }
    if (dpc) {
      // The time it expired at is the DPC's system arguments.
      const uint64_t time = xe::Clock::QueryGuestSystemTime();
      kernel_state()->QueueDpc(dpc, uint32_t(time), uint32_t(time >> 32));
    }
    if (cb_thread) {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      XELOGD(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, "
          "{:08X})",
          routine, routine_arg, time_low, time_high);
      cb_thread->InsertOwnedApc(cb_apc, time_low, time_high);
    }
    WakeCooperativeWaiters();
  };

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  bool result = timer_->Cancel();
  RemoveApc();
  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

bool XTimer::IsSignaled() { return signal_->Query().state != 0; }

void XTimer::RemoveApc() {
  if (apc_thread_) {
    apc_thread_->RemoveOwnedApc(apc_ptr_);
    apc_thread_.reset();
  }
}

}  // namespace kernel
}  // namespace xe
