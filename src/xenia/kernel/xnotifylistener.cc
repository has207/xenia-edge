/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xnotifylistener.h"

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {

// A notification's area and number, without its version.
static uint32_t QueueKey(XNotificationID id) {
  const XNotificationKey key(id);
  return uint32_t(key.mask_index) << 25 | key.local_id;
}

XNotifyListener::XNotifyListener(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XNotifyListener::~XNotifyListener() {}

void XNotifyListener::Initialize(uint64_t mask, uint32_t is_system,
                                 uint32_t max_version) {
  assert_false(wait_handle_);

  // A wait that sees the listener signaled takes the signal, as on the console.
  wait_handle_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(wait_handle_);
  mask_ = mask;
  is_system_ = is_system;
  max_version_ = max_version;

  kernel_state_->RegisterNotifyListener(this);
}

void XNotifyListener::EnqueueNotification(XNotificationID id, uint32_t data,
                                          bool signal) {
  auto key = XNotificationKey(id);
  // Ignore if the notification doesn't match our mask.
  if ((mask_ & uint64_t(1ULL << key.mask_index)) == 0) {
    return;
  }
  // Ignore if the notification is too new.
  if (key.version > max_version_) {
    return;
  }
  // xam 816A8328.
  if (key.system_only && !is_system_) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  // A repeat replaces the pending one's data, and still signals.
  notifications_[QueueKey(id)] = {id, data};
  if (signal) {
    wait_handle_->Set();
  }
}

// The console keeps a queue per area and clears the signal when a read empties
// one, even with notifications left in another area.
void XNotifyListener::RemoveNotification(Notifications::iterator it) {
  const uint32_t area = XNotificationKey(it->first).mask_index;
  notifications_.erase(it);
  auto rest = notifications_.lower_bound(area << 25);
  if (rest == notifications_.end() ||
      XNotificationKey(rest->first).mask_index != area) {
    wait_handle_->Reset();
  }
}

bool XNotifyListener::DequeueNotification(XNotificationID* out_id,
                                          uint32_t* out_data) {
  auto global_lock = global_critical_region_.Acquire();
  if (notifications_.empty()) {
    return false;
  }
  auto it = notifications_.begin();
  *out_id = it->second.first;
  *out_data = it->second.second;
  RemoveNotification(it);
  return true;
}

bool XNotifyListener::DequeueNotification(XNotificationID id,
                                          uint32_t* out_data) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = notifications_.find(QueueKey(id));
  if (it == notifications_.end()) {
    return false;
  }
  *out_data = it->second.second;
  RemoveNotification(it);
  return true;
}

}  // namespace kernel
}  // namespace xe
