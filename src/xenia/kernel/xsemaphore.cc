/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsemaphore.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {

XSemaphore::XSemaphore(KernelState* kernel_state, bool host_object)
    : XObject(kernel_state, kObjectType, host_object) {}

XSemaphore::~XSemaphore() = default;

bool XSemaphore::Initialize(int32_t initial_count, int32_t maximum_count) {
  assert_false(semaphore_);

  CreateNative(sizeof(X_KSEMAPHORE));
  auto* ksem = memory()->TranslateVirtual<X_KSEMAPHORE*>(guest_object());
  // Don't touch header.wait_list: SetNativePointer stashes the handle there.
  ksem->header.type = X_OBJECT_TYPES::SemaphoreObject;
  ksem->header.signal_state = initial_count;
  host_count_ = initial_count;
  ksem->limit = maximum_count;

  maximum_count_ = maximum_count;
  semaphore_ = xe::threading::Semaphore::Create(initial_count, maximum_count);
  return !!semaphore_;
}

bool XSemaphore::InitializeNative(void* native_ptr,
                                  const X_DISPATCH_HEADER* header) {
  assert_false(semaphore_);

  auto semaphore = reinterpret_cast<X_KSEMAPHORE*>(native_ptr);
  maximum_count_ = semaphore->limit;
  host_count_ = static_cast<int32_t>(semaphore->header.signal_state);
  semaphore_ = xe::threading::Semaphore::Create(semaphore->header.signal_state,
                                                semaphore->limit);
  if (!semaphore_) {
    return false;
  }
  SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);
  return true;
}

bool XSemaphore::ReleaseSemaphore(int32_t release_count,
                                  int32_t* out_previous_count) {
  int32_t previous_count = 0;
  bool success = false;
  {
    auto global_lock = global_critical_region_.Acquire();
    success = semaphore_->Release(release_count, &previous_count);
    if (success) {
      host_count_ += release_count;
      memory()
          ->TranslateVirtual<X_KSEMAPHORE*>(guest_object())
          ->header.signal_state = host_count_;
    }
  }
  if (out_previous_count) {
    *out_previous_count = previous_count;
  }
  if (success) {
    WakeCooperativeWaiters();
  }
  return success;
}

void XSemaphore::CooperativeWaitBegin(XThread* thread) { waiters_.Add(thread); }

void XSemaphore::CooperativeWaitEnd(XThread* thread) {
  // Poke the new front so it re-polls now.
  if (waiters_.Remove(thread)) {
    WakeCooperativeWaiters();
  }
}

bool XSemaphore::CooperativeMayAcquire(XThread* thread) {
  return waiters_.MayAcquire(thread);
}

void XSemaphore::WaitCallback() {
  auto global_lock = global_critical_region_.Acquire();
  host_count_ -= 1;
  memory()
      ->TranslateVirtual<X_KSEMAPHORE*>(guest_object())
      ->header.signal_state = host_count_;
}

void XSemaphore::SyncFromGuest() {
  if (!guest_object()) {
    return;
  }
  bool released = false;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto& signal_state = memory()
                             ->TranslateVirtual<X_KSEMAPHORE*>(guest_object())
                             ->header.signal_state;
    int32_t guest_count = static_cast<int32_t>(signal_state);
    if (guest_count == host_count_) {
      return;
    }
    // The header only holds what this kernel last put there, so anything else
    // came from the guest, such as an in-place re-initialize.
    if (guest_count > host_count_) {
      int32_t delta = guest_count - host_count_;
      if (!semaphore_->Release(delta, nullptr)) {
        return;  // over the limit, leave the host count alone
      }
      host_count_ += delta;
      released = true;
    } else {
      // Take back permits the guest dropped, never blocking for one a waiter
      // already claimed.
      int32_t delta = host_count_ - guest_count;
      int32_t drained = 0;
      while (
          drained < delta &&
          xe::threading::Wait(semaphore_.get(), std::chrono::milliseconds(0)) ==
              xe::threading::WaitResult::kSuccess) {
        ++drained;
      }
      host_count_ -= drained;
    }
    signal_state = host_count_;
  }
  if (released) {
    WakeCooperativeWaiters();
  }
}

}  // namespace kernel
}  // namespace xe
