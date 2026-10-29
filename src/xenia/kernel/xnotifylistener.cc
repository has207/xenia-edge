/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xnotifylistener.h"

#include <algorithm>

#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {

XNotifyListener::XNotifyListener(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XNotifyListener::~XNotifyListener() {}

void XNotifyListener::Initialize(uint64_t mask, uint32_t is_system,
                                 uint32_t max_version) {
  assert_false(wait_handle_);

  wait_handle_ = xe::threading::Event::CreateManualResetEvent(false);
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
  notifications_.push_back(std::pair<XNotificationID, uint32_t>(id, data));
  wait_handle_->Set();
}

bool XNotifyListener::DequeueNotification(XNotificationID* out_id,
                                          uint32_t* out_data) {
  auto global_lock = global_critical_region_.Acquire();
  bool dequeued = false;
  if (notifications_.size()) {
    dequeued = true;
    auto it = notifications_.begin();
    *out_id = it->first;
    *out_data = it->second;
    notifications_.erase(it);
  }
  // Also reset when nothing was queued: a filtered read may have taken the
  // last notification and left the handle signaled.
  if (notifications_.empty()) {
    wait_handle_->Reset();
  }
  return dequeued;
}

bool XNotifyListener::DequeueNotification(XNotificationID id,
                                          uint32_t* out_data) {
  auto global_lock = global_critical_region_.Acquire();
  bool dequeued = false;
  for (auto it = notifications_.begin(); it != notifications_.end(); ++it) {
    if (it->first != id) {
      continue;
    }
    dequeued = true;
    *out_data = it->second;
    notifications_.erase(it);
    break;
  }
  // A filtered reader never takes the other notifications, so leaving the
  // handle signaled for them makes a wait + XNotifyGetNext(id) loop spin.
  if (std::none_of(notifications_.begin(), notifications_.end(),
                   [id](const auto& n) { return n.first == id; })) {
    wait_handle_->Reset();
  }
  return dequeued;
}

bool XNotifyListener::Save(ByteStream* stream) {
  SaveObject(stream);

  stream->Write(mask_);
  stream->Write(max_version_);
  stream->Write(notifications_.size());
  for (auto pair : notifications_) {
    stream->Write<uint32_t>(pair.first);
    stream->Write<uint32_t>(pair.second);
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
    std::pair<XNotificationID, uint32_t> pair;
    pair.first = stream->Read<uint32_t>();
    pair.second = stream->Read<uint32_t>();
    notify->notifications_.push_back(pair);
  }

  return object_ref<XNotifyListener>(notify);
}

}  // namespace kernel
}  // namespace xe
