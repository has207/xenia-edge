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
#include "xenia/base/byte_stream.h"
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

void XNotifyListener::EnqueueNotification(XNotificationID id, uint32_t data) {
  auto key = XNotificationKey(id);
  // Ignore if the notification doesn't match our mask.
  if ((mask_ & uint64_t(1ULL << key.mask_index)) == 0) {
    return;
  }
  // Ignore if the notification is too new.
  if (key.version > max_version_) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  // A repeat replaces the pending one's data, and still signals.
  notifications_[QueueKey(id)] = {id, data};
  wait_handle_->Set();
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

bool XNotifyListener::Save(ByteStream* stream) {
  SaveObject(stream);

  stream->Write(mask_);
  stream->Write(max_version_);
  stream->Write(notifications_.size());
  for (const auto& [key, notification] : notifications_) {
    stream->Write<uint32_t>(notification.first);
    stream->Write<uint32_t>(notification.second);
  }

  return true;
}

object_ref<XNotifyListener> XNotifyListener::Restore(KernelState* kernel_state,
                                                     ByteStream* stream) {
  auto notify = new XNotifyListener(nullptr);
  notify->kernel_state_ = kernel_state;

  notify->RestoreObject(stream);

  auto mask = stream->Read<uint64_t>();
  auto is_system = stream->Read<uint32_t>();
  auto max_version = stream->Read<uint32_t>();
  notify->Initialize(mask, is_system, max_version);

  auto notification_count_ = stream->Read<size_t>();
  for (size_t i = 0; i < notification_count_; i++) {
    const XNotificationID id = stream->Read<uint32_t>();
    const uint32_t data = stream->Read<uint32_t>();
    notify->notifications_[QueueKey(id)] = {id, data};
  }

  return object_ref<XNotifyListener>(notify);
}

}  // namespace kernel
}  // namespace xe
