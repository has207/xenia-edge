/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

dword_result_t XUsbcamCreate_entry(dword_t buffer,
                                   dword_t buffer_size,  // 0x4B000 640x480?
                                   lpdword_t handle_out) {
  // This function should return success.
  // It looks like it only allocates space for usbcam support.
  // returning error code might cause games to initialize incorrectly.
  // "Carcassonne" initalization function checks for result from this
  // function. If value is different than 0 instead of loading
  // rest of the game it returns from initalization function and tries
  // to run game normally which causes crash, due to uninitialized data.
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamCreate, kNone, kStub);

enum class XUsbCamState : uint32_t {
  CamNotConnected = 0x00000000,
  CamInitilizied = 0x00000001,
  CamConnected = 0x00000002,
};

dword_result_t XUsbcamGetState_entry() {
  return static_cast<uint32_t>(XUsbCamState::CamNotConnected);
}
DECLARE_XBOXKRNL_EXPORT1(XUsbcamGetState, kNone, kStub);

// No camera is ever attached, so every request against the camera fails the
// same way the kernel fails it for an unplugged device: synchronously with
// ERROR_DEVICE_NOT_CONNECTED, never ERROR_IO_PENDING, so the completion
// routines passed in are never called.

dword_result_t XUsbcamDestroy_entry(lpvoid_t handle) { return X_ERROR_SUCCESS; }
DECLARE_XBOXKRNL_EXPORT1(XUsbcamDestroy, kNone, kStub);

dword_result_t XUsbcamSetConfig_entry(lpvoid_t handle, dword_t unk1,
                                      dword_t unk2, lpvoid_t unk3, dword_t unk4,
                                      function_t completion, lpvoid_t context) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT2(XUsbcamSetConfig, kNone, kStub, kSketchy);

dword_result_t XUsbcamSetView_entry(lpvoid_t handle, dword_t unk1, dword_t unk2,
                                    dword_t unk3, function_t completion,
                                    lpvoid_t context) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT2(XUsbcamSetView, kNone, kStub, kSketchy);

dword_result_t XUsbcamSetCaptureMode_entry(lpvoid_t handle, dword_t mode,
                                           lpvoid_t buffer, dword_t width,
                                           dword_t height, dword_t unk5,
                                           dword_t unk6,
                                           function_t completion) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT2(XUsbcamSetCaptureMode, kNone, kStub, kSketchy);

dword_result_t XUsbcamReadFrame_entry(lpvoid_t handle, dword_t unk1,
                                      dword_t unk2, function_t completion,
                                      lpvoid_t context) {
  return X_ERROR_DEVICE_NOT_CONNECTED;
}
DECLARE_XBOXKRNL_EXPORT2(XUsbcamReadFrame, kNone, kStub, kSketchy);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Usbcam);
