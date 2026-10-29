/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamVoiceIsActiveProcess_entry() {
  // Returning 0 here will short-circuit a bunch of voice stuff.
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceIsActiveProcess, kNone, kStub);

dword_result_t XamVoiceCreate_entry(dword_t user_index,
                                    dword_t max_attached_packets,  // 0xF
                                    lpdword_t out_voice_ptr) {
  // Null out the ptr.
  out_voice_ptr.Zero();
  return X_ERROR_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamVoiceCreate, kNone, kStub);

dword_result_t XamVoiceClose_entry(lpunknown_t voice_ptr) { return 0; }
DECLARE_XAM_EXPORT1(XamVoiceClose, kNone, kStub);

dword_result_t XamVoiceHeadsetPresent_entry(lpunknown_t voice_ptr) { return 0; }
DECLARE_XAM_EXPORT1(XamVoiceHeadsetPresent, kNone, kStub);

dword_result_t XamVoiceSubmitPacket_entry(lpdword_t unk1, dword_t unk2,
                                          lpdword_t unk3) {
  // also may return 0xD000009D
  return 0x800700AA;
}
DECLARE_XAM_EXPORT1(XamVoiceSubmitPacket, kNone, kStub);

dword_result_t XamVoiceGetMicArrayStatus_entry() {
  // Returning 0 here tells caller mic is not connected
  return 0;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayStatus, kNone, kStub);

// The Kinect microphone array requests go to the kernel's mic array driver
// and come back as its NT status with the facility bit. With no array that is
// a missing device.
constexpr X_HRESULT kMicArrayNotConnected =
    X_HRESULT_FROM_NT(0xC000009D);  // STATUS_DEVICE_NOT_CONNECTED

// Starts a capture from the microphone array. The request block starts with
// its size; xam resets the status fields at 0x2C..0x3F before sending it.
dword_result_t XamVoiceGetMicArrayAudioEx_entry(lpvoid_t request,
                                                lpvoid_t completion) {
  if (!request) {
    return X_E_INVALIDARG;
  }
  auto fields = request.as_array<uint32_t>();
  if (fields[0] >= 0x40) {
    fields[0x2C / 4] = 0;
    for (uint32_t offset = 0x30; offset < 0x40; offset += 4) {
      fields[offset / 4] = 5;
    }
  }
  return kMicArrayNotConnected;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayAudioEx, kNone, kStub);

dword_result_t XamVoiceGetMicArrayUnderrunStatus_entry(
    lpdword_t underrun_flags, lpdword_t underrun_count) {
  if (underrun_flags) {
    *underrun_flags = 0;
  }
  if (underrun_count) {
    *underrun_count = 0;
  }
  return kMicArrayNotConnected;
}
DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayUnderrunStatus, kNone, kStub);

// Sound source direction from the microphone array behind a voice. Only an
// array voice has one, and XamVoiceCreate never makes one, so the kernel's
// device-not-available result comes back.
dword_result_t XamVoiceGetDirectionalData_entry(lpvoid_t voice_ptr,
                                                lpfloat_t directional_data) {
  return X_RESULT_FROM_WIN32(0x000010DFL);  // ERROR_DEVICE_NOT_AVAILABLE
}
DECLARE_XAM_EXPORT1(XamVoiceGetDirectionalData, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Voice);
