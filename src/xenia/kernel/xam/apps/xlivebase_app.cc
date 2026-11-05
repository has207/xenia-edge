/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xenumerator.h"

#include <asio.hpp>

struct XONLINE_SERVICE_INFO {
  xe::be<uint32_t> id;
  in_addr ip;
  xe::be<uint16_t> port;
  xe::be<uint16_t> reserved;
};

struct X_ARGUMENT_ENTRY {
  xe::be<uint32_t> native_size;
  xe::be<uint64_t> argument_value_ptr;
};
static_assert_size(X_ARGUMENT_ENTRY, 0x10);

struct X_CREATE_FRIENDS_ENUMERATOR {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY friends_starting_index;
  X_ARGUMENT_ENTRY friends_amount;
  X_ARGUMENT_ENTRY buffer_size_ptr;
  X_ARGUMENT_ENTRY handle_ptr;
};

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

constexpr uint32_t kOnlineMaxFriends = 100;
constexpr uint32_t kOnlineFriendSize = 0xC4;  // X_ONLINE_FRIEND

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      /* Notes:
         - Called on startup of dashboard (netplay build)
         - used by other internet funtions to check if online (e.g.
         XamGetLiveHiveValueA)
         - Return is Saved elsewhere and used here
      */
      XELOGD("XLiveBaseLogonGetHR, implemented in netplay");
      return 0x001510F1;  // X_ONLINE_S_LOGON_DISCONNECTED
    }
    case 0x00058004: {
      /* Notes:
         - Called on startup, seems to just return a bool in the buffer.
         - It is Saved elsewhere and used here
      */
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      // Buffer only set when online
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("XLiveBaseOnlineGetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x0005800E: {
      // XUserMuteListQuery. The buffer ends in the result code and
      // buffer_length holds the address of the BOOL result. No mute list is
      // kept so nobody is muted.
      if (buffer_ptr) {
        xe::store_and_swap<uint32_t>(buffer + 0x10, X_ERROR_SUCCESS);
      }
      if (buffer_length) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_length),
                                     0);
      }
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      XELOGD("XFriendsCreateEnumerator({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return XFriendsCreateEnumerator(buffer_length);
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058024: {
      // A Live service request. The 17559 dashboard polls its unread message
      // count with one every few seconds.
      XELOGD("XLiveBaseServiceRequest({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058037: {
      XELOGD("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      XELOGD("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058017: {
      XELOGD("UserFindUsers({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      // Fixes Xbox Live error for 513107D9
      // Required for 534507D4
      XELOGD("XLiveBaseUnk58035({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050036: {
      XELOGD("XOnlineQuerySearch({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050009: {
      // XStorageDownloadToMemory. There is no storage server so no file is
      // ever found. Success would leave the title's buffer unfilled and
      // 434D083D parses that as its server setup file.
      XELOGD("XStorageDownloadToMemory({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x8015C004;  // X_ONLINE_E_STORAGE_FILE_NOT_FOUND
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

X_HRESULT XLiveBaseApp::XFriendsCreateEnumerator(uint32_t args_ptr) {
  if (!args_ptr) {
    return X_E_INVALIDARG;
  }
  const auto args =
      memory_->TranslateVirtual<X_CREATE_FRIENDS_ENUMERATOR*>(args_ptr);
  const uint32_t user_index_ptr =
      static_cast<uint32_t>(args->user_index.argument_value_ptr);
  const uint32_t starting_index_ptr =
      static_cast<uint32_t>(args->friends_starting_index.argument_value_ptr);
  const uint32_t amount_ptr =
      static_cast<uint32_t>(args->friends_amount.argument_value_ptr);
  const uint32_t buffer_size_ptr =
      static_cast<uint32_t>(args->buffer_size_ptr.argument_value_ptr);
  const uint32_t handle_ptr =
      static_cast<uint32_t>(args->handle_ptr.argument_value_ptr);
  if (!user_index_ptr || !starting_index_ptr || !amount_ptr ||
      !buffer_size_ptr || !handle_ptr) {
    return X_E_INVALIDARG;
  }

  const uint32_t user_index =
      xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(user_index_ptr));
  const uint32_t starting_index = xe::load_and_swap<uint32_t>(
      memory_->TranslateVirtual(starting_index_ptr));
  const uint32_t amount =
      xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(amount_ptr));
  if (user_index >= XUserMaxUserCount || starting_index >= kOnlineMaxFriends ||
      amount > kOnlineMaxFriends) {
    return X_E_INVALIDARG;
  }
  if (!kernel_state_->xam_state()->IsUserSignedIn(user_index)) {
    return X_E_NO_SUCH_USER;
  }

  // No friends list is kept so the enumerator is always empty.
  auto e = make_object<XStaticUntypedEnumerator>(kernel_state_, amount,
                                                 kOnlineFriendSize);
  auto result = e->Initialize(user_index, app_id(), 0x58021, 0x58022, 0);
  if (XFAILED(result)) {
    return result;
  }

  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_size_ptr),
                               amount * kOnlineFriendSize);
  xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr),
                               e->handle());
  return X_E_SUCCESS;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
