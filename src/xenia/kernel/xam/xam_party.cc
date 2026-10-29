/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// XamPartyPartyCreate, XamPartyLeave, XamPartySendInvite, XamPartyJoinEx, &
// XamPartyRemoveLocalUsers share one function

// XPARTY_USER_LIST: a count padded to 8 bytes and 32 XPARTY_USER_INFO of
// 0x78 bytes.
constexpr size_t kPartyUserListSize = 0xF08;

dword_result_t XamPartyGetUserList_entry(dword_t caller,
                                         lpvoid_t party_list_ptr) {
  if (caller != 1) {
    return X_E_NOT_IMPLEMENTED;
  }
  // Titles read the count without checking the result.
  if (party_list_ptr) {
    party_list_ptr.Zero(kPartyUserListSize);
  }
  // 5345085D, 45410923
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartyGetUserList, kNone, kStub);

// The internal list has the same count and 32 users of 0x80 bytes.
constexpr size_t kPartyUserListInternalSize = 0x1008;

dword_result_t XamPartyGetUserListInternal_entry(lpvoid_t party_list_ptr) {
  if (party_list_ptr) {
    party_list_ptr.Zero(kPartyUserListInternalSize);
  }
  // FFFE07D1
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartyGetUserListInternal, kNone, kStub);

// Shares same function as XamPartyAddLocalUsers, XamPartyKickUser,
// XamPartyGetJoinable, XamPartySetJoinable, & XamPartyGetInfoEx
dword_result_t XamPartySendGameInvites_entry(
    dword_t caller, dword_t user_index, dword_t unknown,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [=](uint32_t& extended_error, uint32_t& length) {
    extended_error = X_ERROR_SUCCESS;
    length = 0;

    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    X_RESULT result = run(extended_error, length);

    return result;
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamPartySendGameInvites, kNone, kStub);

dword_result_t XamPartySetCustomData_entry(dword_t caller, dword_t user_index,
                                           lpvoid_t custom_data_ptr) {
  if (caller != 1) {
    return X_E_NOT_IMPLEMENTED;
  } else if (user_index >= XUserMaxUserCount) {
    return 0x807D0002;
  }
  return 0x807D0003;  // X_PARTY_E_NOT_IN_PARTY
}
DECLARE_XAM_EXPORT1(XamPartySetCustomData, kNone, kStub);

dword_result_t XamPartyGetBandwidth_entry(dword_t bandwidth_type,
                                          lpqword_t bandwidth_statstic) {
  if (bandwidth_statstic) {
    *bandwidth_statstic = 0;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamPartyGetBandwidth, kNone, kStub);

// These xam exports pass their request to the party system app. There is no
// party app, so they fail the way xam does when the app can't take the
// request, without touching the overlapped.

dword_result_t XamPartyCreate_entry(dword_t user_index) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamPartyCreate, kNone, kStub);

dword_result_t XamPartySendInvite_entry(dword_t user_index, qword_t xuid,
                                        lpvoid_t message_ptr) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamPartySendInvite, kNone, kStub);

// Up to 100 XUIDs are placed into a formation.
constexpr uint32_t kPartyFormationMaxUsers = 100;

dword_result_t XamPartyGetFormation_entry(
    dword_t user_index, dword_t xuid_count, lpqword_t xuids_ptr,
    lpvoid_t formation_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!xuid_count || xuid_count > kPartyFormationMaxUsers || !xuids_ptr ||
      !formation_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamPartyGetFormation, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Party);
