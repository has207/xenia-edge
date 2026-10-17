/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsession.h"

#include <algorithm>
#include <span>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/net/system_link.h"

namespace xe {
namespace kernel {

namespace {

constexpr uint32_t kContextGameType = 0x800A;
constexpr uint32_t kContextGameMode = 0x800B;
constexpr uint32_t kGameTypeRanked = 0;

// The join and invite flags, which XSessionModify may change.
constexpr uint32_t kModifiableFlags =
    SessionFlags::INVITES_DISABLED | SessionFlags::JOIN_VIA_PRESENCE_DISABLED |
    SessionFlags::JOIN_IN_PROGRESS_DISABLED |
    SessionFlags::JOIN_VIA_PRESENCE_FRIENDS_ONLY;

constexpr uint32_t kMemberPrivateSlot = 1;

struct XSESSION_LOCAL_DETAILS {
  xe::be<uint32_t> user_index_host;
  xe::be<uint32_t> game_type;
  xe::be<uint32_t> game_mode;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> max_public_slots;
  xe::be<uint32_t> max_private_slots;
  xe::be<uint32_t> available_public_slots;
  xe::be<uint32_t> available_private_slots;
  xe::be<uint32_t> actual_member_count;
  xe::be<uint32_t> returned_member_count;
  xe::be<uint32_t> state;
  xe::be<uint32_t> padding;
  xe::be<uint64_t> nonce;
  XSESSION_INFO session_info;
  net::KeyId arbitration_id;
  xe::be<uint32_t> members_ptr;
};
static_assert_size(XSESSION_LOCAL_DETAILS, 0x80);

struct XSESSION_MEMBER {
  xe::be<uint64_t> online_xuid;
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> flags;
};
static_assert_size(XSESSION_MEMBER, 0x10);

}  // namespace

XSession::XSession(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

X_STATUS XSession::Initialize() {
  // ObDereferenceObject finds the object by the handle CreateNative stashes.
  if (!CreateNative(sizeof(X_DISPATCH_HEADER))) {
    return X_STATUS_NO_MEMORY;
  }
  return X_STATUS_SUCCESS;
}

object_ref<XSession> XSession::FromGuest(KernelState* kernel_state,
                                         uint32_t guest_ptr) {
  if (!guest_ptr) {
    return nullptr;
  }
  auto header =
      kernel_state->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(guest_ptr);
  if (header->wait_list.flink_ptr != kXObjSignature) {
    return nullptr;
  }
  auto session = kernel_state->object_table()->LookupObject<XSession>(
      header->wait_list.blink_ptr);
  // The handle may have been recycled for another object.
  if (!session || session->guest_object() != guest_ptr) {
    return nullptr;
  }
  return session;
}

X_RESULT XSession::Create(uint32_t flags, uint32_t user_index,
                          uint32_t max_public_slots, uint32_t max_private_slots,
                          uint32_t session_info_ptr, uint32_t nonce_ptr) {
  {
    std::lock_guard lock(mutex_);
    flags_ = flags;
    user_index_ = user_index;
    max_public_slots_ = max_public_slots;
    max_private_slots_ = max_private_slots;
    game_type_ = CreatorContext(kContextGameType);
    if (flags & SessionFlags::HOST) {
      net::Random(
          std::span(reinterpret_cast<uint8_t*>(&nonce_), sizeof(nonce_)));
      if (nonce_ptr) {
        xe::store_and_swap<uint64_t>(memory()->TranslateVirtual(nonce_ptr),
                                     nonce_);
      }
    }
  }
  if (!session_info_ptr) {
    return X_ERROR_SUCCESS;
  }

  auto info = memory()->TranslateVirtual<XSESSION_INFO*>(session_info_ptr);
  const bool peer_network = flags & SessionFlags::PEER_NETWORK;
  // Only a peer network session needs System Link.
  auto link = peer_network ? xam::GetSystemLink(kernel_state_) : nullptr;
  if (flags & SessionFlags::HOST) {
    net::SystemLink::CreateKey(info->session_id, info->key_exchange_key);
    info->host_address = link ? link->identity() : net::secure::Address{};
  } else if (!peer_network) {
    *info = {};
  }
  {
    std::lock_guard lock(mutex_);
    info_ = *info;
  }
  if (!link) {
    return X_ERROR_SUCCESS;
  }

  // The title may have registered the key itself.
  const uint32_t error =
      link->RegisterKey(info->session_id, info->key_exchange_key);
  if (error && error != net::kWsaAlready) {
    XELOGW("XSession: System Link refused the session key, error {}", error);
    return 0x8015520D;
  }
  system_link_ = link;
  registered_key_ = info->session_id;
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::DeleteSession() {
  {
    std::lock_guard lock(mutex_);
    state_ = kDeleted;
  }
  if (registered_key_) {
    if (auto link = system_link_.lock()) {
      link->UnregisterKey(*registered_key_);
    }
    registered_key_.reset();
  }
  return X_ERROR_SUCCESS;
}

void XSession::Join(uint32_t count, uint32_t xuids_ptr,
                    uint32_t user_indices_ptr, uint32_t private_slots_ptr) {
  auto* xam = kernel_state_->xam_state();
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < count; ++i) {
    Member member;
    if (user_indices_ptr) {
      const uint32_t user_index = xe::load_and_swap<uint32_t>(
          memory()->TranslateVirtual(user_indices_ptr + i * 4));
      const auto* profile = xam->GetUserProfile(user_index);
      if (!profile) {
        continue;
      }
      member.xuid = profile->GetLogonXUID();
      member.user_index = user_index;
    } else if (xuids_ptr) {
      member.xuid = xe::load_and_swap<uint64_t>(
          memory()->TranslateVirtual(xuids_ptr + i * 8));
      member.user_index = XUserIndexNone;
    } else {
      return;
    }
    // A private slot request takes a public slot once none are left.
    if (private_slots_ptr && UsedSlots().second < max_private_slots_) {
      member.private_slot =
          xe::load_and_swap<uint32_t>(
              memory()->TranslateVirtual(private_slots_ptr + i * 4)) != 0;
    }
    if (std::ranges::none_of(members_, [&](const Member& joined) {
          return joined.xuid == member.xuid;
        })) {
      members_.push_back(member);
    }
  }
}

void XSession::Leave(uint32_t count, uint32_t xuids_ptr,
                     uint32_t user_indices_ptr) {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < count; ++i) {
    if (user_indices_ptr) {
      const uint32_t user_index = xe::load_and_swap<uint32_t>(
          memory()->TranslateVirtual(user_indices_ptr + i * 4));
      std::erase_if(members_, [&](const Member& member) {
        return member.user_index == user_index;
      });
    } else if (xuids_ptr) {
      const uint64_t xuid = xe::load_and_swap<uint64_t>(
          memory()->TranslateVirtual(xuids_ptr + i * 8));
      std::erase_if(members_,
                    [&](const Member& member) { return member.xuid == xuid; });
    }
  }
}

void XSession::Start() {
  std::lock_guard lock(mutex_);
  if (state_ >= kInGame) {
    return;
  }
  if (flags_ & SessionFlags::STATS) {
    game_mode_ = CreatorContext(kContextGameMode);
  }
  state_ = kInGame;
}

void XSession::End() {
  std::lock_guard lock(mutex_);
  if (state_ == kInGame) {
    state_ = kLobby;
  }
}

X_RESULT XSession::Modify(uint32_t flags, uint32_t max_public_slots,
                          uint32_t max_private_slots) {
  std::lock_guard lock(mutex_);
  uint32_t changed = flags ^ flags_;
  if (changed & ~(kModifiableFlags | SessionFlags::ARBITRATION)) {
    return 0x80155204;  // X_ONLINE_E_SESSION_INVALID_FLAGS
  }
  if (state_ == kDeleted) {
    return 0x80155206;  // X_ONLINE_E_SESSION_WRONG_STATE
  }
  // Only an unranked session may change these, arbitration only in the lobby.
  const bool ranked =
      game_type_ == kGameTypeRanked && (flags_ & SessionFlags::ARBITRATION);
  if (!ranked) {
    changed &= ~kModifiableFlags;
    if (state_ == kLobby) {
      changed &= ~uint32_t(SessionFlags::ARBITRATION);
    }
  }
  if (changed) {
    return 0x80155204;  // X_ONLINE_E_SESSION_INVALID_FLAGS
  }
  const auto [used_public, used_private] = UsedSlots();
  if (max_public_slots < used_public || max_private_slots < used_private) {
    return 0x80155202;  // X_ONLINE_E_SESSION_FULL
  }
  flags_ = flags;
  max_public_slots_ = max_public_slots;
  max_private_slots_ = max_private_slots;
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetDetails(uint32_t size_ptr, uint32_t details_ptr) {
  if (!size_ptr || !details_ptr) {
    return X_E_INVALIDARG;
  }
  const uint32_t size =
      xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(size_ptr));
  if (size < sizeof(XSESSION_LOCAL_DETAILS)) {
    return X_E_INVALIDARG;
  }

  std::lock_guard lock(mutex_);
  auto* details =
      memory()->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(details_ptr);
  details->user_index_host =
      flags_ & SessionFlags::HOST ? user_index_ : XUserIndexNone;
  details->game_type = game_type_;
  // The mode follows the creator's context while the session is in the lobby.
  if (state_ == kLobby) {
    game_mode_ = CreatorContext(kContextGameMode);
  }
  details->game_mode = game_mode_;
  details->flags = flags_;
  details->max_public_slots = max_public_slots_;
  details->max_private_slots = max_private_slots_;

  // Only a matchmaking session open to joiners reports free slots.
  uint32_t available_public = 0;
  uint32_t available_private = 0;
  const bool closed = (flags_ & (SessionFlags::JOIN_IN_PROGRESS_DISABLED |
                                 SessionFlags::ARBITRATION)) &&
                      state_ == kInGame;
  if (!closed && (flags_ & SessionFlags::MATCHMAKING) && state_ != kDeleted) {
    const auto [used_public, used_private] = UsedSlots();
    available_public =
        max_public_slots_ - std::min(used_public, max_public_slots_);
    available_private =
        max_private_slots_ - std::min(used_private, max_private_slots_);
  }
  details->available_public_slots = available_public;
  details->available_private_slots = available_private;
  details->state = state_;
  details->nonce = nonce_;
  details->session_info = info_;
  details->arbitration_id = {};

  // Members follow the details and those that don't fit are only counted.
  const uint32_t members_ptr =
      details_ptr + uint32_t(sizeof(XSESSION_LOCAL_DETAILS));
  const uint32_t room = uint32_t((size - sizeof(XSESSION_LOCAL_DETAILS)) /
                                 sizeof(XSESSION_MEMBER));
  auto* members = memory()->TranslateVirtual<XSESSION_MEMBER*>(members_ptr);
  uint32_t returned = 0;
  for (const auto& member : members_) {
    if (returned == room) {
      break;
    }
    members[returned].online_xuid = member.xuid;
    members[returned].user_index = member.user_index;
    members[returned].flags = member.private_slot ? kMemberPrivateSlot : 0;
    ++returned;
  }
  details->actual_member_count = uint32_t(members_.size());
  details->returned_member_count = returned;
  details->members_ptr = members_ptr;
  return X_E_SUCCESS;
}

std::pair<uint32_t, uint32_t> XSession::UsedSlots() const {
  const auto used_private = uint32_t(std::ranges::count_if(
      members_, [](const Member& member) { return member.private_slot; }));
  return {uint32_t(members_.size()) - used_private, used_private};
}

uint32_t XSession::CreatorContext(uint32_t id) const {
  auto* xam = kernel_state_->xam_state();
  const auto* profile = xam->GetUserProfile(user_index_);
  if (!profile) {
    return 0;
  }
  return xam->user_tracker()->GetUserContext(profile->xuid(), id).value_or(0);
}

}  // namespace kernel
}  // namespace xe
