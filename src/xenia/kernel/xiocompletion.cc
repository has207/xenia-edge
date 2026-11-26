/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xiocompletion.h"

#include "xenia/base/clock.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XIOCompletion::XIOCompletion(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {
  notification_semaphore_ = threading::Semaphore::Create(0, kMaxNotifications);
  assert_not_null(notification_semaphore_);
}

XIOCompletion::~XIOCompletion() = default;

void XIOCompletion::QueueNotification(IONotification& notification) {
  {
    std::unique_lock<std::mutex> lock(notification_lock_);
    notifications_.push(notification);
    notification_semaphore_->Release(1, nullptr);
  }
  kernel_state()->guest_scheduler()->WakeAll();
}

bool XIOCompletion::WaitForNotification(const uint64_t* timeout,
                                        IONotification* notify) {
  auto ms = timeout ? std::chrono::milliseconds(TimeoutTicksToMs(*timeout))
                    : std::chrono::milliseconds::max();

  if (XThread::GetCurrentFiberThread()) {
    // Acquire at zero timeout and yield between polls, rather than blocking
    // the dispatch host thread.
    auto* scheduler = kernel_state()->guest_scheduler();
    uint64_t deadline_ms = Clock::QueryHostUptimeMillis() + ms.count();
    while (true) {
      auto poll = threading::Wait(notification_semaphore_.get(),
                                  std::chrono::milliseconds(0));
      if (poll == threading::WaitResult::kSuccess) {
        break;
      }
      if (timeout && Clock::QueryHostUptimeMillis() >= deadline_ms) {
        return false;
      }
      scheduler->BlockCurrentThread();
    }
  } else {
    auto res = threading::Wait(notification_semaphore_.get(), ms);
    if (res != threading::WaitResult::kSuccess) {
      return false;
    }
  }

  std::unique_lock<std::mutex> lock(notification_lock_);
  assert_false(notifications_.empty());

  std::memcpy(notify, &notifications_.front(), sizeof(IONotification));
  notifications_.pop();

  return true;
}

}  // namespace kernel
}  // namespace xe
