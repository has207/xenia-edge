/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
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

// Input from a remote control client (LRC), of which none is ever connected.
// xam reports an empty queue as ERROR_EMPTY and writes neither output.
dword_result_t XamLrcGetInputWithSeek_entry(
    pointer_t<X_INPUT_KEYSTROKE> keystroke, lpqword_t seek_ptr) {
  return X_ERROR_EMPTY;
}
DECLARE_XAM_EXPORT1(XamLrcGetInputWithSeek, kInput, kStub);

// The playback state a title shares with remote control clients. Silverlight's
// playback bar sends it on every command and state change.
dword_result_t XamLrcSetMediaState_entry(lpvoid_t media_state) {
  if (!media_state) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLrcSetMediaState, kInput, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Input);
