/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xecrypt_sha.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/xbox.h"

DECLARE_bool(allow_mic_initialization);

namespace xe {
namespace kernel {
namespace xam {

using xe::hid::X_INPUT_CAPABILITIES;
using xe::hid::X_INPUT_CAPABILITIES_EX;
using xe::hid::X_INPUT_FLAG;
using xe::hid::X_INPUT_KEYSTROKE;
using xe::hid::X_INPUT_STATE;
using xe::hid::X_INPUT_VIBRATION;
using xe::hid::X_USER_DEVICE_CLASS;
using xe::hid::X_USER_DEVICE_TYPE;

// Input flag of system callers, which see the guide button.
constexpr uint32_t kInputFlagSystem = 0x80000000;

dword_result_t XAutomationpUnbindController_entry(dword_t user_index) {
  if (user_index >= XUserMaxUserCount) {
    return 0;
  }

  return 1;
}
DECLARE_XAM_EXPORT1(XAutomationpUnbindController, kInput, kStub);

void XamResetInactivity_entry() {
  // Do we need to do anything?
}
DECLARE_XAM_EXPORT1(XamResetInactivity, kInput, kStub);

dword_result_t XamEnableInactivityProcessing_entry(dword_t inactivity_index,
                                                   dword_t enable) {
  // Enables/disables screen saver and auto shutoff
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamEnableInactivityProcessing, kInput, kStub);

dword_result_t XamInputGetCapabilitiesEx_entry(
    dword_t unk, dword_t user_index, dword_t flags,
    pointer_t<X_INPUT_CAPABILITIES_EX> caps) {
  if (unk > 1) {
    return X_ERROR_NOT_SUPPORTED;
  }

  // Fail-safe check
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  caps.Zero();

  if ((flags & X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER) != 0) {
    // should trap
  }

  if ((flags & 4) != 0) {
    // should trap
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & XUserIndexAny) == XUserIndexAny ||
      (flags & X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  uint32_t actual_flags = flags;
  if (!flags) {
    actual_flags = X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD |
                   X_INPUT_FLAG::X_INPUT_FLAG_KEYBOARD;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  auto lock = input_system->lock();
  return input_system->GetCapabilities(actual_user_index, actual_flags, caps);
}
DECLARE_XAM_EXPORT1(XamInputGetCapabilitiesEx, kInput, kSketchy);

// https://learn.microsoft.com/en-gb/windows/win32/api/xinput/nf-xinput-xinputgetcapabilities
dword_result_t XamInputGetCapabilities_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_CAPABILITIES> caps) {
  X_HRESULT result;
  memset(caps, 0x0, sizeof(X_INPUT_CAPABILITIES));
  X_INPUT_CAPABILITIES_EX caps_ex = {};
  result = XamInputGetCapabilitiesEx_entry(1, user_index, flags, &caps_ex);
  if (!result) {
    std::memcpy(caps, &caps_ex, sizeof(X_INPUT_CAPABILITIES));
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamInputGetCapabilities, kInput, kSketchy);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
dword_result_t XamInputGetState_entry(dword_t user_index, dword_t flags,
                                      pointer_t<X_INPUT_STATE> input_state) {
  if (input_state) {
    memset((void*)input_state.host_address(), 0, sizeof(X_INPUT_STATE));
  }
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Games call this with a NULL state ptr, probably as a query.
  X_INPUT_STATE query_state;
  X_INPUT_STATE* state =
      input_state ? static_cast<X_INPUT_STATE*>(input_state) : &query_state;

  uint32_t actual_user_index = user_index;
  // chrispy: change this, logic is not right
  if ((actual_user_index & XUserIndexAny) == XUserIndexAny ||
      (flags & X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  X_RESULT result;
  auto input_system = kernel_state()->emulator()->input_system();
  {
    auto lock = input_system->lock();
    auto xam_state = kernel_state()->xam_state();
    bool ui_active = xam_state->IsUIActive();
    result = input_system->GetState(
        user_index, !flags ? X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD : flags, state,
        ui_active);
    if (input_state && result == X_ERROR_SUCCESS) {
      // While xam's UI holds the input, the title reads the packet number from
      // before.
      uint32_t& packet_number =
          xam_state->title_input_packet_numbers_[user_index];
      if (ui_active) {
        input_state->packet_number = packet_number;
      } else {
        packet_number = input_state->packet_number;
      }
    }
  }

  if (input_state && result == X_ERROR_SUCCESS) {
    // Only system callers see the guide button, which belongs to the console.
    if (!(flags & kInputFlagSystem)) {
      input_state->gamepad.buttons =
          input_state->gamepad.buttons & ~hid::X_INPUT_GAMEPAD_GUIDE;
    }
    if (auto patch = kernel_state()->xmp_volume_patch()) {
      patch->OnInputPoll(input_state->packet_number);
    }
  }

  return result;
}
DECLARE_XAM_EXPORT2(XamInputGetState, kInput, kImplemented, kHighFrequency);

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
dword_result_t XamInputSetState_entry(
    dword_t user_index,
    dword_t flags, /* flags, as far as i can see, is not used*/
    pointer_t<X_INPUT_VIBRATION> vibration) {
  if (user_index >= XUserMaxUserCount) {
    return X_E_DEVICE_NOT_CONNECTED;
  }
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto input_system = kernel_state()->emulator()->input_system();
  auto lock = input_system->lock();
  return input_system->SetState(user_index, vibration);
}
DECLARE_XAM_EXPORT1(XamInputSetState, kInput, kImplemented);

// https://learn.microsoft.com/en-gb/windows/win32/api/xinput/nf-xinput-xinputgetkeystroke
// Same as non-ex, just takes a pointer to user index.
dword_result_t XamInputGetKeystrokeEx_entry(
    lpdword_t user_index_ptr, dword_t flags,
    pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  keystroke.Zero();

  uint32_t user_index = *user_index_ptr;
  bool ui_active = kernel_state()->xam_state()->IsUIActive();
  auto input_system = kernel_state()->emulator()->input_system();
  auto lock = input_system->lock();
  if ((user_index & XUserIndexAny) == XUserIndexAny) {
    // Always pin user to 0.
    user_index = 0;
  }

  // Only system callers get keystrokes of the guide button.
  auto get_keystroke = [&](uint32_t user) {
    while (true) {
      X_RESULT result =
          input_system->GetKeystroke(user, flags, keystroke, ui_active);
      if (result != X_ERROR_SUCCESS || (flags & kInputFlagSystem) ||
          keystroke->virtual_key != uint16_t(ui::VirtualKey::kXInputPadGuide)) {
        return result;
      }
      keystroke.Zero();
    }
  };

  if (flags & X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER) {
    // That flag means we should iterate over every connected controller and
    // check which one have pending request.
    X_RESULT result = X_ERROR_DEVICE_NOT_CONNECTED;
    for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
      const X_RESULT user_result = get_keystroke(i);

      // Return result from first user that have pending request
      if (user_result == X_ERROR_SUCCESS) {
        *user_index_ptr = keystroke->user_index;
        return user_result;
      }
      // A user that answered with nothing pending makes the result EMPTY.
      if (user_result == X_ERROR_EMPTY) {
        result = X_ERROR_EMPTY;
      }
    }
    return result;
  }

  auto result = get_keystroke(user_index);

  // XSUCCEEDED would also pass EMPTY, a Win32 code without the error bit.
  if (result == X_ERROR_SUCCESS) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamInputGetKeystrokeEx, kInput, kImplemented);

dword_result_t XamInputGetKeystroke_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  // Ex reads the index through a guest pointer, so it must be big-endian.
  xe::be<uint32_t> actual_user_index = static_cast<uint32_t>(user_index);
  return XamInputGetKeystrokeEx_entry(
      reinterpret_cast<uint32_t*>(&actual_user_index), flags, keystroke);
}
DECLARE_XAM_EXPORT1(XamInputGetKeystroke, kInput, kImplemented);

// The guide's keystroke read, as on the console: any device, and any user for
// XUserIndexAny or with 0x10000000. 0x20000000 takes precedence and skips only
// the big button remap and keyboard translation, which xenia has neither of.
// As a system read it keeps the guide button.
dword_result_t XamInputGetKeystrokeHudEx_entry(
    dword_t user_index, dword_t flags, pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  uint32_t input_flags =
      X_INPUT_FLAG::X_INPUT_FLAG_ANYDEVICE | kInputFlagSystem;
  if (static_cast<uint32_t>(user_index) == XUserIndexAny ||
      (flags & 0x30000000) == 0x10000000) {
    input_flags |= X_INPUT_FLAG::X_INPUT_FLAG_ANY_USER;
  }
  return XamInputGetKeystroke_entry(user_index, input_flags, keystroke);
}
DECLARE_XAM_EXPORT1(XamInputGetKeystrokeHudEx, kInput, kImplemented);

dword_result_t XamInputGetKeystrokeHud_entry(
    dword_t user_index, pointer_t<X_INPUT_KEYSTROKE> keystroke) {
  return XamInputGetKeystrokeHudEx_entry(user_index, 0, keystroke);
}
DECLARE_XAM_EXPORT1(XamInputGetKeystrokeHud, kInput, kImplemented);

X_HRESULT_result_t XamUserGetDeviceContext_entry(dword_t user_index,
                                                 dword_t device_type,
                                                 lpdword_t out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  if (kernel_state()->xam_state()->IsUserSignedIn(user_index) ||
      (user_index & XUserIndexAny) == XUserIndexAny) {
    if (device_type == X_USER_DEVICE_CLASS::DEVICE_CLASS_MIC &&
        cvars::allow_mic_initialization) {  // Microphone
      *out_ptr = X_USER_DEVICE_TYPE::DEVICE_TYPE_MIC_2;
    } else {
      *out_ptr = (uint32_t)user_index;
    }
    return X_E_SUCCESS;
  } else {
    return X_E_DEVICE_NOT_CONNECTED;
  }
}
DECLARE_XAM_EXPORT1(XamUserGetDeviceContext, kInput, kStub);

X_HRESULT_result_t XamInputNonControllerGetRawEx_entry(
    dword_t device_id, lpdword_t buffer_ptr, lpdword_t buffer_length_ptr,
    lpword_t state_ptr) {
  if (device_id != 5 && device_id != 6) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!state_ptr || !buffer_length_ptr || !buffer_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (*buffer_length_ptr == 0 || *buffer_length_ptr > hid::kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto portal = kernel_state()->emulator()->input_system()->GetPortal();
  if (!portal) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t bytes_read = *buffer_length_ptr;
  uint16_t state = 0;

  const auto result = portal->Read(
      {kernel_memory()->TranslateVirtual(buffer_ptr.guest_address()),
       *buffer_length_ptr},
      bytes_read, state);

  if (XSUCCEEDED(result)) {
    *buffer_length_ptr = bytes_read;
    *state_ptr = state;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamInputNonControllerGetRawEx, kInput, kSketchy);

X_HRESULT_result_t XamInputNonControllerSetRawEx_entry(dword_t device_id,
                                                       lpdword_t buffer_ptr,
                                                       dword_t buffer_length) {
  if (device_id != 5 && device_id != 6) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!buffer_ptr || !buffer_length || buffer_length > hid::kPortalBufferSize) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto portal = kernel_state()->emulator()->input_system()->GetPortal();
  if (!portal) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  return portal->Write(
      {kernel_memory()->TranslateVirtual(buffer_ptr.guest_address()),
       buffer_length});
}
DECLARE_XAM_EXPORT1(XamInputNonControllerSetRawEx, kInput, kSketchy);

X_HRESULT_result_t XamInputNonControllerGetRaw_entry(
    lpword_t state_ptr, lpdword_t buffer_length_ptr, lpdword_t buffer_ptr) {
  return XamInputNonControllerGetRawEx_entry(5, buffer_ptr, buffer_length_ptr,
                                             state_ptr);
}
DECLARE_XAM_EXPORT1(XamInputNonControllerGetRaw, kInput, kSketchy);

X_HRESULT_result_t XamInputNonControllerSetRaw_entry(dword_t buffer_length,
                                                     lpdword_t buffer_ptr) {
  // Normally there are handled separatelly with different first param, but
  // whatever.
  return XamInputNonControllerSetRawEx_entry(5, buffer_ptr, buffer_length);
}
DECLARE_XAM_EXPORT1(XamInputNonControllerSetRaw, kInput, kSketchy);

// A raw USB report of a user's controller or one of its attachments, as the
// kernel input driver exchanges it. The caller sets how many report bytes it
// wants; reads also return the device's packet number.
struct X_INPUT_RAW_STATE {
  uint8_t report[0x20];
  uint8_t report_size;
  uint8_t pad[3];
  xe::be<uint32_t> packet_number;
};
static_assert_size(X_INPUT_RAW_STATE, 0x28);

dword_result_t XamInputRawState_entry(dword_t user_index, dword_t flags,
                                      dword_t mode,
                                      pointer_t<X_INPUT_RAW_STATE> raw_state) {
  // The flags pick the device of the user: the controller itself, or the
  // attachments 1, 3 and 4. xenia's controllers have no attachments.
  uint32_t device = 0;
  if (flags & 0x08) {
    device = 3;
  } else if (flags & 0x10) {
    device = 1;
  } else if (flags & 0x20) {
    device = 4;
  }
  if (user_index >= XUserMaxUserCount || device != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!raw_state) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint32_t report_size =
      std::min<uint32_t>(raw_state->report_size, sizeof(raw_state->report));

  auto input_system = kernel_state()->emulator()->input_system();
  auto lock = input_system->lock();
  switch (static_cast<uint32_t>(mode)) {
    case 0: {
      // Read the last input report.
      auto xam_state = kernel_state()->xam_state();
      const bool ui_active = xam_state->IsUIActive();
      X_INPUT_STATE state = {};
      if (input_system->GetState(user_index, X_INPUT_FLAG::X_INPUT_FLAG_GAMEPAD,
                                 &state, ui_active) != X_ERROR_SUCCESS) {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
      // While xam's UI holds the input the title gets an empty report and the
      // packet number from before, as with XamInputGetState.
      uint32_t& packet_number =
          xam_state->title_input_packet_numbers_[user_index];
      std::memset(raw_state->report, 0, report_size);
      if (ui_active) {
        raw_state->packet_number = packet_number;
        return X_ERROR_SUCCESS;
      }
      packet_number = state.packet_number;
      raw_state->packet_number = state.packet_number;

      // The wired controller's input report: type 0, length 0x14, then the
      // gamepad fields little-endian.
      uint16_t buttons = state.gamepad.buttons;
      if (!(flags & kInputFlagSystem)) {
        buttons = uint16_t(buttons & ~hid::X_INPUT_GAMEPAD_GUIDE);
      }
      uint8_t report[sizeof(raw_state->report)] = {};
      report[1] = 0x14;
      report[2] = uint8_t(buttons);
      report[3] = uint8_t(buttons >> 8);
      report[4] = state.gamepad.left_trigger;
      report[5] = state.gamepad.right_trigger;
      const int16_t thumbs[] = {state.gamepad.thumb_lx, state.gamepad.thumb_ly,
                                state.gamepad.thumb_rx, state.gamepad.thumb_ry};
      for (size_t i = 0; i < std::size(thumbs); i++) {
        report[6 + i * 2] = uint8_t(uint16_t(thumbs[i]));
        report[7 + i * 2] = uint8_t(uint16_t(thumbs[i]) >> 8);
      }
      std::memcpy(raw_state->report, report, report_size);
      return X_ERROR_SUCCESS;
    }
    case 1: {
      // Send an output report. Only the rumble report has a meaning here.
      const uint8_t* report = raw_state->report;
      if (report_size >= 5 && report[0] == 0x00 && report[1] == 0x08) {
        X_INPUT_VIBRATION vibration;
        vibration.left_motor_speed = uint16_t(report[3] * 0x101);
        vibration.right_motor_speed = uint16_t(report[4] * 0x101);
        input_system->SetState(user_index, &vibration);
      }
      return X_ERROR_SUCCESS;
    }
    default:
      // Other report kinds aren't modeled; the console also reports success
      // for kinds the device doesn't take.
      return X_ERROR_SUCCESS;
  }
}
DECLARE_XAM_EXPORT1(XamInputRawState, kInput, kSketchy);

// Input from a remote control client (LRC). None is ever connected, so report
// an empty queue; returning 0 makes callers dispatch the uninitialized input.
dword_result_t XamLrcGetInputWithSeek_entry(dword_t unk, lpqword_t input_ptr) {
  return X_ERROR_EMPTY;
}
DECLARE_XAM_EXPORT1(XamLrcGetInputWithSeek, kInput, kStub);

// The rest of LRC (SmartGlass). Its service never starts in xenia, which is
// how the console behaves with LRC turned off: media state and title port
// updates are dropped, client checks and message crypto fail, and remote
// keyboards can't register.
dword_result_t XamLrcSetMediaState_entry(lpvoid_t media_state) {
  if (!media_state) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcSetMediaState, kInput, kStub);

dword_result_t XamLrcSetTitlePort_entry(dword_t port, dword_t unk) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcSetTitlePort, kInput, kStub);

dword_result_t XamLrcVerifyClientId_entry(dword_t client_id, dword_t unk) {
  return X_ERROR_NO_SUCH_USER;
}
DECLARE_XAM_EXPORT1(XamLrcVerifyClientId, kInput, kStub);

dword_result_t XamLrcEncryptDecryptTitleMessage_entry(dword_t unk1,
                                                      dword_t unk2,
                                                      dword_t unk3,
                                                      dword_t unk4) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamLrcEncryptDecryptTitleMessage, kInput, kStub);

void XamLrcLogError_entry() {
  // Telemetry for the LRC service, which isn't running.
}
DECLARE_XAM_EXPORT1(XamLrcLogError, kInput, kStub);

void XamLrcLogSessionSummary_entry() {
  // Telemetry for the LRC service, which isn't running.
}
DECLARE_XAM_EXPORT1(XamLrcLogSessionSummary, kInput, kStub);

// Size of the buffer a title hands XamLrcKeyboardRegister.
constexpr uint32_t kLrcKeyboardBufferSize = 0x2D58;
// Keyboards are registered in two slots; this handle means none was given.
constexpr uint32_t kLrcKeyboardInvalidHandle = 2;

dword_result_t XamLrcKeyboardGetRequiredBufferSize_entry() {
  return kLrcKeyboardBufferSize;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardGetRequiredBufferSize, kInput, kImplemented);

// Takes nine arguments; the ones past the buffer only matter to a running
// LRC service.
dword_result_t XamLrcKeyboardRegister_entry(dword_t unk, lpvoid_t buffer) {
  return kLrcKeyboardInvalidHandle;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardRegister, kInput, kStub);

void XamLrcKeyboardUnregister_entry(dword_t keyboard_handle) {}
DECLARE_XAM_EXPORT1(XamLrcKeyboardUnregister, kInput, kStub);

dword_result_t XamLrcKeyboardGetInput_entry(dword_t keyboard_handle,
                                            lpvoid_t text_change) {
  if (!text_change) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // No pending input reads as a failure here.
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardGetInput, kInput, kStub);

void XamLrcKeyboardUpdateText_entry(dword_t keyboard_handle, lpvoid_t state,
                                    lpword_t text) {}
DECLARE_XAM_EXPORT1(XamLrcKeyboardUpdateText, kInput, kStub);

// An edit from a remote keyboard: replace the characters [start, end) of the
// text with the inserted ones. The digest is the SHA-1 of the text the edit
// was made against, which must still be the current text.
struct X_LRC_KEYBOARD_TEXT_CHANGE {
  xe::be<uint32_t> start;         // 0x00
  xe::be<uint32_t> end;           // 0x04
  xe::be<uint32_t> unk_08;        // 0x08
  xe::be<uint32_t> unk_0C;        // 0x0C
  uint8_t text_digest[0x14];      // 0x10
  xe::be<uint32_t> unk_24;        // 0x24
  xe::be<uint32_t> insert_count;  // 0x28
  // Followed by insert_count UTF-16 characters.
};
static_assert_size(X_LRC_KEYBOARD_TEXT_CHANGE, 0x2C);

static const xe::be<uint16_t>* LrcInsertedChars(
    const X_LRC_KEYBOARD_TEXT_CHANGE* change) {
  return reinterpret_cast<const xe::be<uint16_t>*>(change + 1);
}

// Length of a text that must end within its capacity, in characters.
static std::optional<uint32_t> LrcTextLength(const xe::be<uint16_t>* text,
                                             uint32_t capacity) {
  if (!text || !capacity || capacity > 0x7FFFFFFF) {
    return std::nullopt;
  }
  for (uint32_t i = 0; i < capacity; i++) {
    if (!text[i]) {
      return i;
    }
  }
  return std::nullopt;
}

// Checks an edit against the current text: the range must lie in the text,
// the result must fit, the digest must match, and the title's filter must
// accept every inserted character.
dword_result_t XamLrcKeyboardValidateTextChange_entry(
    lpword_t text, dword_t text_capacity,
    pointer_t<X_LRC_KEYBOARD_TEXT_CHANGE> change, dword_t char_filter,
    dword_t filter_context) {
  const xe::be<uint16_t>* text_chars = text;
  const auto length = LrcTextLength(text_chars, text_capacity);
  if (!length || !change || !char_filter || !filter_context) {
    return false;
  }
  const uint32_t start = change->start;
  const uint32_t end = change->end;
  if (end < start || end > *length) {
    return false;
  }
  const uint32_t insert_count = change->insert_count;
  const uint64_t new_length = uint64_t(*length) - (end - start) + insert_count;
  if (new_length + 1 > text_capacity) {
    return false;
  }

  uint8_t digest[0x14];
  xboxkrnl::XeCryptSha(reinterpret_cast<const uint8_t*>(text_chars),
                       *length * sizeof(uint16_t), nullptr, 0, nullptr, 0,
                       digest, sizeof(digest));
  if (std::memcmp(digest, change->text_digest, sizeof(digest))) {
    return false;
  }

  const auto inserted = LrcInsertedChars(change);
  auto thread_state = XThread::GetCurrentThread()->thread_state();
  for (uint32_t i = 0; i < insert_count; i++) {
    uint64_t args[] = {uint16_t(inserted[i]), uint32_t(filter_context)};
    if (!uint32_t(kernel_state()->processor()->Execute(
            thread_state, char_filter, args, std::size(args)))) {
      return false;
    }
  }
  return true;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardValidateTextChange, kInput, kImplemented);

// Writes source_text with a validated edit applied into text.
void XamLrcKeyboardApplyTextChange_entry(
    lpword_t text, dword_t text_capacity, lpword_t source_text,
    pointer_t<X_LRC_KEYBOARD_TEXT_CHANGE> change) {
  xe::be<uint16_t>* dest = text;
  if (!dest || !change || !text_capacity || text_capacity > 0x7FFFFFFF) {
    return;
  }
  // As on the console the text is cleared first, so the source can't be the
  // same buffer.
  std::memset(dest, 0, text_capacity * sizeof(uint16_t));
  uint32_t length = 0;
  if (const xe::be<uint16_t>* source = source_text) {
    while (length + 1 < text_capacity && source[length]) {
      dest[length] = source[length];
      length++;
    }
  }

  // The edit was validated against the source, so this only keeps a bad one
  // inside the buffer.
  const uint32_t start = change->start;
  const uint32_t end = change->end;
  const uint32_t insert_count = change->insert_count;
  if (end < start || end > length ||
      uint64_t(length) - (end - start) + insert_count + 1 > text_capacity) {
    return;
  }
  const uint32_t new_length = length - (end - start) + insert_count;
  // Move the rest of the text with its terminator behind the inserted
  // characters, then insert them.
  std::memmove(&dest[start + insert_count], &dest[end],
               (length - end + 1) * sizeof(uint16_t));
  const auto inserted = LrcInsertedChars(change);
  for (uint32_t i = 0; i < insert_count; i++) {
    dest[start + i] = inserted[i];
  }
  dest[new_length] = 0;
}
DECLARE_XAM_EXPORT1(XamLrcKeyboardApplyTextChange, kInput, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Input);
