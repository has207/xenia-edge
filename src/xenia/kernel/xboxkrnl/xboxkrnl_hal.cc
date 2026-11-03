/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/smc.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"

DECLARE_int32(avpack);

namespace xe {
namespace kernel {
namespace xboxkrnl {

constexpr std::array<std::string_view, 9> FirmwareReentryMessage = {
    "hard poweroff",
    "hard reset (video error)",
    "hard reset (used for dumpwritedump/frozen processor)",
    "hard reset",
    "power off (hard)",
    "power off (nice)",
    "Shut off (Lost Settings)",
    "Shut off (Frozen Console)",
    "Shut off",
};

void HalReturnToFirmware_entry(dword_t routine) {
  const std::string exitMessage = fmt::format(
      "Game requested a {} via HalReturnToFirmware",
      static_cast<size_t>(routine) < FirmwareReentryMessage.size()
          ? FirmwareReentryMessage[routine]
          : fmt::format("Reboot (Routine Code: {})", routine.value()));
  XELOGE(exitMessage);
  // 0 and 4 power off through the SMC, 5 and 7 through the power down DPC. The
  // others reset.
  const uint32_t r = routine;
  if (r == 0 || r == 4 || r == 5 || r == 7) {
    kernel_state()->PowerOff();
  } else {
    kernel_state()->ExitToDashboard();
  }
}
DECLARE_XBOXKRNL_EXPORT2(HalReturnToFirmware, kNone, kStub, kImportant);

// Queues the power down DPC HalReturnToFirmware(5) does.
void HalPowerDownToBackgroundMode_entry() { kernel_state()->PowerOff(); }
DECLARE_XBOXKRNL_EXPORT1(HalPowerDownToBackgroundMode, kNone, kSketchy);

dword_result_t HalGetCurrentAVPack_entry() { return cvars::avpack; }
DECLARE_XBOXKRNL_EXPORT1(HalGetCurrentAVPack, kNone, kImplemented);

void HalSendSMCMessage_entry(pointer_t<X_SMC_DATA> smc_message,
                             pointer_t<X_SMC_DATA> smc_response) {
  if (!smc_message) {
    return;
  }

  kernel_state()->smc()->CallCommand(smc_message, smc_response);
}
DECLARE_XBOXKRNL_EXPORT3(HalSendSMCMessage, kNone, kStub, kImportant,
                         kHighFrequency);

void HalOpenCloseODDTray_entry(dword_t open_close) {
  kernel_state()->smc()->MoveTray(open_close);
}
DECLARE_XBOXKRNL_EXPORT1(HalOpenCloseODDTray, kNone, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Hal);
