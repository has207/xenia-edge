/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <string>
#include <string_view>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamMediaVerificationCreate_entry(dword_t creation_flags) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationCreate, kNone, kStub);

dword_result_t XamMediaVerificationClose_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(XamMediaVerificationClose, kNone, kStub);

dword_result_t XamMediaVerificationVerify_entry(
    dword_t r3, pointer_t<XAM_OVERLAPPED> overlapped, lpvoid_t callback) {
  if (callback) {
    auto thread = XThread::GetCurrentThread();
    thread->EnqueueApc(static_cast<uint32_t>(callback) & ~1u, 0, 0, 0);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationVerify, kNone, kStub);

struct X_SECURITY_FAILURE_INFORMATION {
  xe::be<uint32_t> size;
  xe::be<uint32_t> failed_reads;
  xe::be<uint32_t> failed_hashes;
  xe::be<uint32_t> blocks_checked;
  xe::be<uint32_t> total_blocks;
  xe::be<uint32_t> complete;
};

dword_result_t XamMediaVerificationFailedBlocks_entry(
    pointer_t<X_SECURITY_FAILURE_INFORMATION> failure_information) {
  if (failure_information) {
    if (failure_information->size != sizeof(X_SECURITY_FAILURE_INFORMATION)) {
      return X_ERROR_NOT_ENOUGH_MEMORY;
    }

    failure_information->failed_reads = 0;
    failure_information->failed_hashes = 0;
    failure_information->blocks_checked = 0;
    failure_information->total_blocks = 0;
    failure_information->complete = true;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationFailedBlocks, kNone, kStub);

dword_result_t XamMediaVerificationInject_entry(dword_t r3, dword_t r4) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamMediaVerificationInject, kNone, kStub);

dword_result_t XamGetClosedCaptioningState_entry() {
  return kernel_state()->xconfig()->ReadSetting<uint8_t>(
      XCONFIG_CONSOLE_CATEGORY, XCONFIG_CONSOLE_CLOSED_CAPTIONING);
}
DECLARE_XAM_EXPORT1(XamGetClosedCaptioningState, kNone, kImplemented);

// Reads the 4-byte caption settings, or writes 0 when the config has none.
dword_result_t XamGetClosedCaptioningSettings_entry(lpdword_t settings_ptr) {
  if (!settings_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto xconfig = kernel_state()->xconfig();
  if (xconfig->GetSettingSize(XCONFIG_CONSOLE_CATEGORY,
                              XCONFIG_CONSOLE_CLOSED_CAPTIONING_SETTINGS) !=
      sizeof(uint32_t)) {
    *settings_ptr = 0;
    return X_ERROR_NOT_FOUND;
  }
  xconfig->ReadSetting(XCONFIG_CONSOLE_CATEGORY,
                       XCONFIG_CONSOLE_CLOSED_CAPTIONING_SETTINGS,
                       settings_ptr);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetClosedCaptioningSettings, kNone, kImplemented);

// Ordinals 0x7DC/0x7DD and 0x7E3/0x7E4 load and unload the two system
// extension modules of extra AV codecs. A load takes a reference to the module
// and hands back its export 1, which npctrl calls for its decoder table. It
// returns 0x10000000 when it loads the module and 0 when it only takes another
// reference. An unload drops the reference. Without the modules in the sysext
// folder npctrl plays only the codecs it carries itself (WMA). A load then
// still succeeds with a null export, which npctrl takes as there being no
// extra codecs. It crashes cleaning up after a failed load.
constexpr X_HRESULT kErrorPointer = 0x80004003;
constexpr X_HRESULT kModuleLoaded = 0x10000000;

static std::string SysExtModulePath(std::string_view name) {
  return fmt::format("\\Device\\Harddisk0\\SystemExtPartition\\{}", name);
}

static dword_result_t LoadExtraAVCodecs(lpdword_t export_ptr,
                                        std::string_view name) {
  if (!export_ptr) {
    return kErrorPointer;
  }
  *export_ptr = 0;
  const std::string path = SysExtModulePath(name);
  const bool was_loaded = kernel_state()->GetModule(path, true) != nullptr;
  if (!xboxkrnl::xeXexLoadImage(path)) {
    XELOGW("{} isn't in the sysext folder: no extra AV codecs", name);
    return X_E_SUCCESS;
  }
  *export_ptr =
      kernel_state()->GetModule(path, true)->GetProcAddressByOrdinal(1);
  return was_loaded ? X_E_SUCCESS : kModuleLoaded;
}

static void UnloadExtraAVCodecs(std::string_view name) {
  if (auto module = kernel_state()->GetModule(SysExtModulePath(name), true)) {
    xboxkrnl::xeXexUnloadImage(module->hmodule_ptr());
  }
}

dword_result_t XamLoadExtraAVCodecs_entry(lpdword_t export_ptr) {
  return LoadExtraAVCodecs(export_ptr, "dash.ExtraAVCodecs.xex");
}
DECLARE_XAM_EXPORT1(XamLoadExtraAVCodecs, kNone, kImplemented);

void XamUnloadExtraAVCodecs_entry() {
  UnloadExtraAVCodecs("dash.ExtraAVCodecs.xex");
}
DECLARE_XAM_EXPORT1(XamUnloadExtraAVCodecs, kNone, kImplemented);

dword_result_t XamLoadExtraAVCodecs2_entry(lpdword_t export_ptr) {
  return LoadExtraAVCodecs(export_ptr, "dash.ExtraAVCodecs2.xex");
}
DECLARE_XAM_EXPORT1(XamLoadExtraAVCodecs2, kNone, kImplemented);

void XamUnloadExtraAVCodecs2_entry() {
  UnloadExtraAVCodecs("dash.ExtraAVCodecs2.xex");
}
DECLARE_XAM_EXPORT1(XamUnloadExtraAVCodecs2, kNone, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Media);
