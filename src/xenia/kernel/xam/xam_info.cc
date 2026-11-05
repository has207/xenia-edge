/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/base/utf8.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/system_manifest.h"
#include "xenia/kernel/xam/title_image_device.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xam_ui.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_modules.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xconfig.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/xbox.h"

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/fmt/include/fmt/xchar.h"

DEFINE_int32(avpack, 8,
             "Video modes\n"
             " 0 = PAL-60 Component (SD)\n"
             " 1 = Unused\n"
             " 2 = PAL-60 SCART\n"
             " 3 = 480p Component (HD)\n"
             " 4 = HDMI+A\n"
             " 5 = PAL-60 Composite/S-Video\n"
             " 6 = VGA\n"
             " 7 = TV PAL-60\n"
             " 8 = HDMI (default)",
             "Video");
DEFINE_bool(staging_mode, 0,
            "Enables preview mode in dashboards to render debug information.",
            "Kernel");

DEFINE_bool(in_process_title_relaunch, true,
            "Handle title-to-title launches in-process via full "
            "Shutdown/Setup cycle instead of spawning a new emulator process.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {

// https://github.com/tpn/winsdk-10/blob/master/Include/10.0.14393.0/km/wdm.h#L15539
typedef enum _MODE { KernelMode, UserMode, MaximumMode } MODE;

dword_result_t XamFeatureEnabled_entry(qword_t feature_bit) {
  const std::bitset<36> feature(0x40ffffffff);
  if (feature.test(feature_bit)) {
    return 1;
  }
  return 0;
}
DECLARE_XAM_EXPORT1(XamFeatureEnabled, kNone, kImplemented);

dword_result_t XamGetStagingMode_entry() { return cvars::staging_mode; }
DECLARE_XAM_EXPORT1(XamGetStagingMode, kNone, kStub);

// Empty stub schema binary.
uint8_t schema_bin[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00,
    0x00, 0x2C, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x2C, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
};

dword_result_t XamGetOnlineSchema_entry() {
  // Kept with the kernel, since a relaunch frees the memory it's in.
  uint32_t& schema_guest = kernel_state()->xam_state()->online_schema_address_;

  if (!schema_guest) {
    schema_guest =
        kernel_state()->memory()->SystemHeapAlloc(8 + sizeof(schema_bin));
    auto schema = kernel_state()->memory()->TranslateVirtual(schema_guest);
    std::memcpy(schema + 8, schema_bin, sizeof(schema_bin));
    xe::store_and_swap<uint32_t>(schema + 0, schema_guest + 8);
    xe::store_and_swap<uint32_t>(schema + 4, sizeof(schema_bin));
  }

  // return pointer to the schema ptr/schema size struct
  return schema_guest;
}
DECLARE_XAM_EXPORT1(XamGetOnlineSchema, kNone, kImplemented);

dword_result_t XamQueryLiveHiveA_entry(
    lpstring_t feature_name, lpstring_t value_ptr, dword_t value_buffer_size,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // See Netplay, create shared function for both XamQueryLiveHiveA and
  // XamGetLiveHiveValueA
  X_RESULT extended_error = 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
  if (!feature_name || !value_ptr || !value_buffer_size) {
    extended_error = X_E_INVALIDARG;
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped_ptr, X_STATUS_SUCCESS, extended_error, 0);
    return X_E_PENDING;
  } else {
    return extended_error;
  }
}
DECLARE_XAM_EXPORT1(XamQueryLiveHiveA, kMisc, kStub);

dword_result_t XamGetLiveHiveValueA_entry(
    lpstring_t feature_name, lpstring_t value_ptr, dword_t value_buffer_size,
    int_t unk, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // See Netplay
  X_RESULT result = 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
  if (!feature_name || !value_ptr || !value_buffer_size) {
    result = X_E_INVALIDARG;
  }

  auto thread = kernel::XThread::GetCurrentThread();
  auto ctx = thread->thread_state()->context();
  auto type = xboxkrnl::xeKeGetCurrentProcessType(ctx);
  if (unk == -1 && kernel_state()->emulator()->title_id() == kDashboardID &&
      type == 2) {
    return XamQueryLiveHiveA_entry(feature_name, value_ptr, value_buffer_size,
                                   overlapped_ptr);
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamGetLiveHiveValueA, kMisc, kStub);

// Telemetry for Microsoft's servers. No report is made and the other calls skip
// a null one.
void XamWebInstrumentationCreateReport_entry(lpstring_t name,
                                             lpdword_t report_ptr) {
  if (report_ptr) {
    *report_ptr = 0;
  }
}
DECLARE_XAM_EXPORT1(XamWebInstrumentationCreateReport, kMisc, kStub);

void XamWebInstrumentationSetUserVar_entry(dword_t report, dword_t var,
                                           lpvoid_t value) {}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSetUserVar, kMisc, kStub);

void XamWebInstrumentationSendReport_entry(dword_t report) {}
DECLARE_XAM_EXPORT1(XamWebInstrumentationSendReport, kMisc, kStub);

void XamWebInstrumentationDestroyReport_entry(lpdword_t report_ptr) {}
DECLARE_XAM_EXPORT1(XamWebInstrumentationDestroyReport, kMisc, kStub);

dword_result_t XamQueryLiveHiveW_entry(
    lpu16string_t feature_name, lpu16string_t value_ptr,
    dword_t value_buffer_size, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // See Netplay, create shared function for both XamQueryLiveHiveW and
  // XamGetLiveHiveValueW
  X_RESULT extended_error = 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
  if (!feature_name || !value_ptr || !value_buffer_size) {
    extended_error = X_E_INVALIDARG;
  } else if (std::string target;
             feature_name.value() == u"GameImageAssetUriRoot" &&
             kernel_state()->file_system()->FindSymbolicLink(
                 kTitleImageSymbolicLink, target)) {
    // Title images come from the TitleImages: device rather than Xbox Live.
    string_util::copy_and_swap_truncating(static_cast<char16_t*>(value_ptr),
                                          kTitleImageUriRoot,
                                          value_buffer_size);
    extended_error = X_E_SUCCESS;
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped_ptr, X_STATUS_SUCCESS, extended_error, 0);
    return X_E_PENDING;
  } else {
    return extended_error;
  }
}
DECLARE_XAM_EXPORT1(XamQueryLiveHiveW, kMisc, kStub);

dword_result_t XamGetLiveHiveValueW_entry(
    lpu16string_t feature_name, lpu16string_t value_ptr,
    dword_t value_buffer_size, int_t unk,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // See Netplay
  X_RESULT result = 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
  if (!feature_name || !value_ptr || !value_buffer_size) {
    result = X_E_INVALIDARG;
  }

  auto thread = kernel::XThread::GetCurrentThread();
  auto ctx = thread->thread_state()->context();
  auto type = xboxkrnl::xeKeGetCurrentProcessType(ctx);
  if (unk == -1 && kernel_state()->emulator()->title_id() == kDashboardID &&
      type == 2) {
    return XamQueryLiveHiveW_entry(feature_name, value_ptr, value_buffer_size,
                                   overlapped_ptr);
  }

  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamGetLiveHiveValueW, kMisc, kStub);

dword_result_t keXamBuildResourceLocator(uint64_t module,
                                         const std::u16string& container,
                                         const std::u16string& resource,
                                         lpvoid_t buffer_ptr,
                                         uint32_t buffer_count) {
  std::u16string path;
  if (!module) {
    path = fmt::format(u"file://media:/{}.xzp#{}", container, resource);
    XELOGD(
        "XamBuildResourceLocator({0}) returning locator to local file {0}.xzp",
        xe::to_utf8(container));
  } else {
    path = fmt::format(u"section://{:X},{}#{}", (uint32_t)module, container,
                       resource);
  }
  xe::string_util::copy_and_swap_truncating(buffer_ptr.as<char16_t*>(), path,
                                            buffer_count);
  return 0;
}

dword_result_t XamBuildResourceLocator_entry(qword_t module,
                                             lpu16string_t container,
                                             lpu16string_t resource,
                                             lpvoid_t buffer_ptr,
                                             dword_t buffer_count) {
  return keXamBuildResourceLocator(module, container.value(), resource.value(),
                                   buffer_ptr, buffer_count);
}
DECLARE_XAM_EXPORT1(XamBuildResourceLocator, kNone, kImplemented);

// The xam a system title gets from \SystemRoot for its resources, as xam or
// $flash_xam.
static object_ref<XModule> GetSystemXamModule() {
  if (auto module = kernel_state()->GetModule("xam", true)) {
    return module;
  }
  return kernel_state()->GetModule("$flash_xam", true);
}

dword_result_t XamBuildGamercardResourceLocator_entry(lpu16string_t filename,
                                                      lpvoid_t buffer_ptr,
                                                      dword_t buffer_count) {
  // On an actual xbox these funcs would return a locator to xam.xex resources,
  // but for Xenia we can return a locator to the resources as local files. (big
  // thanks to MS for letting XamBuildResourceLocator return local file
  // locators!)

  // If you're running an app that'll need them, make sure to extract xam.xex
  // resources with xextool ("xextool -d . xam.xex") and add a .xzp extension.
  const auto xam_module = GetSystemXamModule();

  return keXamBuildResourceLocator(xam_module ? xam_module->hmodule_ptr() : 0,
                                   u"gamercrd", filename.value(), buffer_ptr,
                                   buffer_count);
}
DECLARE_XAM_EXPORT1(XamBuildGamercardResourceLocator, kNone, kImplemented);

dword_result_t XamBuildSharedSystemResourceLocator_entry(lpu16string_t filename,
                                                         lpvoid_t buffer_ptr,
                                                         dword_t buffer_count) {
  const auto xam_module = GetSystemXamModule();

  // see notes inside XamBuildGamercardResourceLocator above
  return keXamBuildResourceLocator(xam_module ? xam_module->hmodule_ptr() : 0,
                                   u"shrdres", filename.value(), buffer_ptr,
                                   buffer_count);
}
DECLARE_XAM_EXPORT1(XamBuildSharedSystemResourceLocator, kNone, kImplemented);

dword_result_t XamBuildLegacySystemResourceLocator_entry(lpu16string_t filename,
                                                         lpvoid_t buffer_ptr,
                                                         dword_t buffer_count) {
  return XamBuildSharedSystemResourceLocator_entry(filename, buffer_ptr,
                                                   buffer_count);
}
DECLARE_XAM_EXPORT1(XamBuildLegacySystemResourceLocator, kNone, kImplemented);

dword_result_t XamBuildXamResourceLocator_entry(lpu16string_t filename,
                                                lpvoid_t buffer_ptr,
                                                dword_t buffer_count) {
  return keXamBuildResourceLocator(0, u"xam", filename.value(), buffer_ptr,
                                   buffer_count);
}
DECLARE_XAM_EXPORT1(XamBuildXamResourceLocator, kNone, kImplemented);

dword_result_t XamGetCachedTitleName_entry(dword_t title_id,
                                           dword_t title_name_address,
                                           lpdword_t title_name_length_ptr) {
  char16_t* title_name_ptr =
      kernel_state()->memory()->TranslateVirtual<char16_t*>(title_name_address);

  if (!title_id) {
    *title_name_ptr = 0;
    *title_name_length_ptr = 1;

    return X_ERROR_INVALID_PARAMETER;
  }

  assert_false(title_id != kernel_state()->title_id());

  std::u16string title_name = xe::to_utf16(
      kernel_state()->emulator()->game_info_database()->GetTitleName());

  xe::string_util::copy_and_swap_truncating(title_name_ptr, title_name,
                                            title_name.size() + 1);

  *title_name_length_ptr = title_name.size() + 1;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetCachedTitleName, kNone, kImplemented);

dword_result_t XamReadString_entry(dword_t title_id, qword_t id,
                                   dword_t user_index, dword_t string_out_ptr,
                                   lpdword_t string_size_ptr,
                                   pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!string_out_ptr || id == 0xFFFF) {
    return X_E_INVALIDARG;
  }

  if (!string_size_ptr) {
    return X_E_INSUFFICIENT_BUFFER;
  }

  auto run = [=](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_STATUS result = X_ERROR_SUCCESS;

    // 584111F7 reads leaderboard strings
    const std::u16string localized_string = xe::to_utf16(
        kernel_state()->emulator()->game_info_database()->GetLocalizedString(
            static_cast<uint32_t>(id)));

    const size_t str_buffer_size = *string_size_ptr;

    char16_t* str_buffer =
        kernel_memory()->TranslateVirtual<char16_t*>(string_out_ptr);

    xe::string_util::copy_and_swap_truncating(str_buffer, localized_string,
                                              str_buffer_size);

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = 0;

    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length = 0;
    return run(extended_error, length);
  }

  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamReadString, kNone, kImplemented);

// A kernel version packed as 4.4.16.8 bits.
static uint32_t PackSystemVersion(const KernelVersion& version) {
  return uint32_t(version.major) << 28 | (uint32_t(version.minor) & 0xF) << 24 |
         uint32_t(version.build) << 8 | (uint32_t(version.qfe) & 0xFF);
}

dword_result_t XamGetSystemVersion_entry() {
  return PackSystemVersion(*kernel_state()->GetKernelVersion());
}
DECLARE_XAM_EXPORT1(XamGetSystemVersion, kNone, kImplemented);

void XCustomRegisterDynamicActions_entry() {
  // ???
}
DECLARE_XAM_EXPORT1(XCustomRegisterDynamicActions, kNone, kStub);

dword_result_t XGetAVPack_entry() {
  // Value from
  // https://github.com/Free60Project/libxenon/blob/920146f/libxenon/drivers/xenos/xenos_videomodes.h
  // DWORD
  // Not sure what the values are for this, but 6 is VGA.
  // Other likely values are 3/4/8 for HDMI or something.
  // Games seem to use this as a PAL check - if the result is not 3/4/6/8
  // they explode with errors if not in PAL mode.
  return (cvars::avpack);
}
DECLARE_XAM_EXPORT1(XGetAVPack, kNone, kStub);

dword_result_t XamGetCurrentTitleId_entry() {
  return kernel_state()->emulator()->title_id();
}
DECLARE_XAM_EXPORT1(XamGetCurrentTitleId, kNone, kImplemented);

dword_result_t XamIsCurrentTitleDash_entry(const ppc_context_t& ctx) {
  return ctx->kernel_state->title_id() == kDashboardID;
}
DECLARE_XAM_EXPORT1(XamIsCurrentTitleDash, kNone, kImplemented);

dword_result_t XamSetDashContext_entry(dword_t value,
                                       const ppc_context_t& ctx) {
  // checks XexCheckExecutablePrivilege(8) but we don't care
  if (value != ctx->kernel_state->dash_context_) {
    ctx->kernel_state->dash_context_ = value;
    kernel_state()->BroadcastNotification(
        kXNotificationSystemDashContextChanged, 0);
  }
  return 0;
}
DECLARE_XAM_EXPORT1(XamSetDashContext, kNone, kImplemented);

dword_result_t XamGetDashContext_entry(const ppc_context_t& ctx) {
  return ctx->kernel_state->dash_context_;
}
DECLARE_XAM_EXPORT1(XamGetDashContext, kNone, kImplemented);

dword_result_t XamGetExecutionId_entry(lpdword_t info_ptr) {
  auto module = kernel_state()->GetExecutableModule();
  assert_not_null(module);

  uint32_t guest_hdr_ptr;
  X_STATUS result =
      module->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &guest_hdr_ptr);

  if (XFAILED(result)) {
    return result;
  }

  *info_ptr = guest_hdr_ptr;
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetExecutionId, kNone, kImplemented);

void XamLoaderRegisterLaunchRequestCallback_entry(dword_t callback) {
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  if (xam) {
    xam->SetLaunchCallback(callback);
  }
}
DECLARE_XAM_EXPORT1(XamLoaderRegisterLaunchRequestCallback, kNone, kStub);

static std::string HexBytes(const std::vector<uint8_t>& data,
                            size_t max_bytes) {
  const size_t shown = std::min(data.size(), max_bytes);
  std::string hex;
  hex.reserve(shown * 2 + 3);
  for (size_t i = 0; i < shown; ++i) {
    hex += fmt::format("{:02X}", data[i]);
  }
  if (shown < data.size()) {
    hex += "...";
  }
  return hex;
}

static constexpr size_t kLaunchDataLogBytes = 64;

static void SetLaunchData(const void* data, uint32_t size) {
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  const uint32_t copy_size = std::min(size, MAX_LAUNCH_DATA_SIZE);
  loader_data.launch_data_present = size ? true : false;
  loader_data.launch_data.resize(copy_size);
  std::memcpy(loader_data.launch_data.data(), data, copy_size);
  XELOGI("XamLoaderSetLaunchData: size={} data={}", size,
         HexBytes(loader_data.launch_data, kLaunchDataLogBytes));
}

dword_result_t XamLoaderSetLaunchData_entry(lpvoid_t data, dword_t size) {
  SetLaunchData(data, size);
  return 0;
}
DECLARE_XAM_EXPORT1(XamLoaderSetLaunchData, kNone, kSketchy);

// Stands in for the dashboard, which names the game in launch data.
static void ChooseIndieGameLaunchData(XamModule::LoaderData& loader_data) {
  std::string file_name;
  uint32_t device_id = 0;
  std::string display_name;
  if (!xeXamChooseIndieGame(&file_name, &device_id, &display_name)) {
    return;
  }
  auto& data = loader_data.launch_data;
  data.assign(0x34, 0);
  xe::store_and_swap<uint32_t>(data.data(), 0xCAFEBABE);
  std::memcpy(data.data() + 4, file_name.data(),
              std::min<size_t>(file_name.size(), 0x2A));
  xe::store_and_swap<uint32_t>(data.data() + 0x30, device_id);
  loader_data.launch_data_present = true;
  kernel_state()->emulator()->SetTitleName(display_name);
}

dword_result_t XamLoaderGetLaunchDataSize_entry(lpdword_t size_ptr) {
  if (!size_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  if (loader_data.launch_data.empty() &&
      kernel_state()->title_id() == kXN_2002) {
    ChooseIndieGameLaunchData(loader_data);
  }
  if (loader_data.launch_data.empty()) {
    *size_ptr = 0;
    XELOGI("XamLoaderGetLaunchDataSize: none");
    return X_ERROR_NOT_FOUND;
  }

  const uint32_t size = uint32_t(loader_data.launch_data.size());
  *size_ptr = size;
  XELOGI("XamLoaderGetLaunchDataSize: size={} data={}", size,
         HexBytes(loader_data.launch_data, kLaunchDataLogBytes));
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLoaderGetLaunchDataSize, kNone, kSketchy);

dword_result_t XamLoaderGetLaunchData_entry(lpvoid_t buffer_ptr,
                                            dword_t buffer_size) {
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  auto& loader_data = xam->loader_data();
  if (!loader_data.launch_data_present) {
    XELOGI("XamLoaderGetLaunchData: none");
    return X_ERROR_NOT_FOUND;
  }

  uint32_t copy_size =
      std::min(uint32_t(loader_data.launch_data.size()), uint32_t(buffer_size));
  std::memcpy(buffer_ptr, loader_data.launch_data.data(), copy_size);
  XELOGI("XamLoaderGetLaunchData: buffer_size={} copied={} data={}",
         uint32_t(buffer_size), copy_size,
         HexBytes(loader_data.launch_data, kLaunchDataLogBytes));
  // Launch data is handed out once so it can't reach the next title.
  loader_data.launch_data_present = false;
  loader_data.launch_data.clear();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamLoaderGetLaunchData, kNone, kSketchy);

// Guest paths are case-insensitive so a title's spelling of a name may not
// match the file on disk. Look the real one up in the directory.
static std::filesystem::path ResolveHostFileName(
    const std::filesystem::path& path) {
  const auto dir = path.parent_path();
  const std::string name = xe::path_to_utf8(path.filename());
  if (dir.empty() || name.empty()) {
    return path;
  }
  const auto found = xe::filesystem::FindChildIgnoringCase(dir, name);
  return found.empty() ? path : found;
}

// Whether |path| names a device rather than a file next to the title.
static bool NamesDevice(std::string_view path) {
  return path.find(':') != std::string_view::npos || path.starts_with('\\');
}

// Points host_path, launch_path and package_path, which start out as the
// title's own, at the executable at |entry| on a device outside the title, or
// returns false when it can't be launched from there.
static bool ResolveLaunchTarget(const vfs::Entry* entry,
                                std::filesystem::path& host_path,
                                std::string& launch_path,
                                std::string& package_path) {
  if (auto host_entry = dynamic_cast<const vfs::HostPathEntry*>(entry)) {
    host_path = host_entry->host_path();
    launch_path.clear();
    package_path.clear();
    return true;
  }
  auto container =
      dynamic_cast<const vfs::XContentContainerDevice*>(entry->device());
  if (!container) {
    return false;
  }
  launch_path = entry->path();
  const vfs::Entry* package = container->package_entry();
  if (!package) {
    host_path = container->GetHostPath();
    package_path.clear();
    return true;
  }
  if (auto host_package = dynamic_cast<const vfs::HostPathEntry*>(package)) {
    host_path = host_package->host_path();
    package_path.clear();
    return true;
  }
  // Otherwise the package has to be on the title's own disc, which the
  // relaunch mounts it from.
  const vfs::Entry* title_root = kernel_state()->file_system()->ResolvePath(
      kernel_state()->title_mount_path_);
  if (!package_path.empty() || !title_root ||
      package->device() != title_root->device()) {
    return false;
  }
  package_path = package->path();
  return true;
}

// Starts the title at |host_path| with |launch_path| in it, or its default
// module when empty. Doesn't return.
static void StartTitle(XamModule::LoaderData& loader_data,
                       const std::filesystem::path& host_path,
                       std::string launch_path, std::string package_path) {
  XELOGI(
      "XamLoaderLaunchTitle: normalized host_path={}, package_path={}, "
      "launch_path={}",
      xe::strip_pii(xe::path_to_utf8(host_path)), package_path, launch_path);

  // Handle title launch in-process via full Shutdown/Setup cycle. Disabled
  // on macOS — pthread_cancel there doesn't run C++ destructors, so
  // force-terminated guest threads leak locks and deadlock teardown; spawn a
  // fresh process instead.
#if !XE_PLATFORM_MAC
  if (cvars::in_process_title_relaunch) {
    auto emulator = kernel_state()->emulator();

    XELOGI("XamLoaderLaunchTitle: in-process relaunch to '{}'",
           xe::strip_pii(xe::path_to_utf8(host_path)));

    auto new_host_path = xe::path_to_utf8(host_path);
    auto new_launch_module = launch_path;
    auto new_flags = loader_data.launch_flags;
    auto new_data = loader_data.launch_data;
    auto current_thread = XThread::GetCurrentThread();

    // Must dispatch from a non-guest thread — RelaunchTitle terminates
    // all guest threads including the caller.
    std::thread([emulator, new_host_path = std::move(new_host_path),
                 new_launch_module = std::move(new_launch_module), new_flags,
                 new_data = std::move(new_data),
                 package_path = std::move(package_path)]() mutable {
      emulator->RelaunchTitle(new_host_path, new_launch_module, new_flags,
                              std::move(new_data), true, package_path);
    }).detach();

    // Stop running guest code; RelaunchTitle terminates us. Suspend can
    // return on POSIX, so park rather than fall through to spawn.
    current_thread->Suspend(nullptr);
    while (true) {
      xe::threading::NanoSleep(int64_t(1'000'000'000));
    }
  }
#endif  // !XE_PLATFORM_MAC

  const std::string launch_data_hex =
      HexBytes(loader_data.launch_data, loader_data.launch_data.size());

  auto on_launch_new_title = kernel_state()->emulator()->on_launch_new_title();
  if (on_launch_new_title) {
    XELOGI("XamLoaderLaunchTitle: spawning new title process");
    // The new process has no drive of its own to find the game in.
    auto* emulator = kernel_state()->emulator();
    on_launch_new_title(xe::path_to_utf8(host_path), launch_path,
                        loader_data.launch_flags, launch_data_hex,
                        emulator->KeepsXboxGame(host_path)
                            ? xe::path_to_utf8(emulator->xbox_disc_path())
                            : std::string(),
                        package_path);
  }

  XELOGI("XamLoaderLaunchTitle: terminating to launch new title");
  kernel_state()->TerminateTitle();  // Does not return.
}

// The console's XamLoaderLaunchTitleEx, which XamLoaderLaunchTitle calls with
// flag 4. The D: drive path is only checked for flag 4.
void LaunchTitle(std::string path, std::string_view d_drive_path,
                 uint32_t flags) {
  // Unowned, this frame never unwinds.
  auto* xam = kernel_state()->GetKernelModule<XamModule>("xam.xex").get();

  auto& loader_data = xam->loader_data();
  loader_data.launch_flags = flags;

  // Flag 4 exits to the dashboard for a path rooted at a device. Flag 2 boots
  // the disc in the tray in place of the path, and exits to the dashboard too
  // when the drive is empty.
  std::filesystem::path disc_path;
  if (flags & 2) {
    if (const auto disc = kernel_state()->emulator()->inserted_disc()) {
      disc_path = disc->path;
    }
  }
  if ((flags & 2) || ((flags & 4) && (path.starts_with('\\') ||
                                      d_drive_path.starts_with('\\')))) {
    path.clear();
  }
  // A path on a device that doesn't resolve can't be launched, like the
  // X:\xbox.xex XeFu exits to.
  const vfs::Entry* entry = nullptr;
  if (NamesDevice(path)) {
    entry = kernel_state()->file_system()->ResolvePath(path);
    if (!entry) {
      XELOGW("XamLoaderLaunchTitle: {} does not exist, exiting to dashboard",
             path);
      // xbox.xex starts the XeFu build it picked for the game from XeFu's
      // folder. XeFu exits to xbox.xex, which a folder can do without.
      auto emulator = kernel_state()->emulator();
      const std::string name = xe::utf8::find_name_from_guest_path(path);
      if (emulator->title_id() == kXeFuTitleId &&
          !xe::utf8::equal_case(name, "xbox.xex")) {
        emulator->ReportMissingXeFuFile(name);
      }
      path.clear();
    }
  }

  // An empty path means exit to dashboard.
  if (!path.empty() || !disc_path.empty()) {
    loader_data.launch_data_present = true;

    std::filesystem::path host_path = loader_data.host_path;
    std::string package_path = loader_data.package_path;
    std::string launch_path = xe::path_to_utf8(path);

    XELOGI(
        "XamLoaderLaunchTitle: original host_path={}, package_path={}, "
        "launch_path={}",
        xe::strip_pii(loader_data.host_path), package_path, launch_path);

    // Remove common guest path prefixes (case-insensitive since Xbox
    // paths are case-insensitive, games may pass e.g. "GAME:\")
    auto remove_prefix = [&launch_path](std::string_view prefix) {
      if (xe::utf8::starts_with_case(launch_path, prefix)) {
        launch_path = launch_path.substr(prefix.length());
      }
    };
    remove_prefix("game:\\");
    remove_prefix("d:\\");

    if (!disc_path.empty()) {
      host_path = disc_path;
      package_path.clear();
    } else if (NamesDevice(launch_path)) {
      // Any other device is outside the title. Only a file a host directory
      // backs or one in a package can be launched from there.
      if ((entry->attributes() & vfs::kFileAttributeDirectory) ||
          !ResolveLaunchTarget(entry, host_path, launch_path, package_path)) {
        XELOGW("XamLoaderLaunchTitle: can't launch {}, exiting to dashboard",
               path);
        kernel_state()->ExitToDashboard();
        return;
      }
    } else if (host_path.extension() == ".xex") {
      host_path.remove_filename();
      host_path = ResolveHostFileName(host_path / launch_path);
      launch_path = "";
    }

    StartTitle(loader_data, host_path, std::move(launch_path),
               std::move(package_path));
  }

  XELOGI("XamLoaderLaunchTitle: game requested exit to dashboard");
  kernel_state()->ExitToDashboard();
}

void LaunchHostTitle(const std::filesystem::path& host_path, uint32_t flags) {
  auto* xam = kernel_state()->GetKernelModule<XamModule>("xam.xex").get();
  auto& loader_data = xam->loader_data();
  loader_data.launch_flags = flags;
  loader_data.launch_data_present = true;
  StartTitle(loader_data, host_path, {}, {});
}

void XamLoaderLaunchTitle_entry(lpstring_t raw_name_ptr, dword_t flags) {
  LaunchTitle(raw_name_ptr ? raw_name_ptr.value() : std::string(), {},
              flags | 4);
}
DECLARE_XAM_EXPORT1(XamLoaderLaunchTitle, kNone, kSketchy);

// Nothing mounts the D: drive path, and the command line isn't passed on.
void XamLoaderLaunchTitleEx_entry(lpstring_t raw_name_ptr,
                                  lpstring_t raw_d_drive_path_ptr,
                                  lpstring_t raw_command_line_ptr,
                                  dword_t flags) {
  const std::string d_drive_path =
      raw_d_drive_path_ptr ? raw_d_drive_path_ptr.value() : std::string();
  if (!d_drive_path.empty() || raw_command_line_ptr) {
    XELOGW(
        "XamLoaderLaunchTitleEx: not mounting D: drive path {} or passing "
        "command line {}",
        d_drive_path,
        raw_command_line_ptr ? raw_command_line_ptr.value() : std::string());
  }
  LaunchTitle(raw_name_ptr ? raw_name_ptr.value() : std::string(), d_drive_path,
              flags);
}
DECLARE_XAM_EXPORT1(XamLoaderLaunchTitleEx, kNone, kSketchy);

// Launches a system extended partition file, which is how the dashboard starts
// the Indie Player with an indie game in the launch data.
X_HRESULT_result_t XdfLaunchNewImageFromCache_entry(lpstring_t name_ptr,
                                                    lpvoid_t data_ptr,
                                                    dword_t data_size,
                                                    dword_t flags) {
  if (!name_ptr || (data_ptr && !data_size)) {
    return X_E_INVALIDARG;
  }
  const std::string name = name_ptr.value();
  const std::string path = FindSystemExtFile(name);
  if (path.empty() || !kernel_state()->file_system()->ResolvePath(path)) {
    XELOGW("XdfLaunchNewImageFromCache: {} isn't in the system files", name);
    return X_HRESULT_FROM_WIN32(X_ERROR_NOT_FOUND);
  }
  SetLaunchData(data_ptr, data_size);
  LaunchTitle(path, {}, flags);
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XdfLaunchNewImageFromCache, kNone, kImplemented);

void XamLoaderTerminateTitle_entry() { kernel_state()->ExitToDashboard(); }
DECLARE_XAM_EXPORT1(XamLoaderTerminateTitle, kNone, kSketchy);

// Navigates to the URI pushed with XamPushBackURI, or with none reboots to the
// dashboard.
void XamNavigateBack_entry() { kernel_state()->ExitToDashboard(); }
DECLARE_XAM_EXPORT1(XamNavigateBack, kNone, kSketchy);

// The console's xam takes URIs of up to 0x3FC bytes with the terminator.
constexpr size_t kMaxUriSize = 0x3FC;

// The console's xam rewrites a URI's scheme with these until it's one of its
// base schemes. An alias rewrites into its template with the rest of the URI
// in place of %s, the second template when the user is signed in to LIVE and
// it has one. Only public aliases can be used first.
struct UriAlias {
  std::string_view scheme;
  bool is_public;
  std::string_view template_offline;
  std::string_view template_live;
};
constexpr UriAlias kUriAliases[] = {
    {"pam", false, "dash:pam:%s", {}},
    {"hub", false, "dash:pam:BuiltIn.Hub.xzp:%s", {}},
    {"epix", false, "hub:root:Epix:%s", {}},
    {"gamedetails", true,
     "hub:root:BuiltIn.ContentApp.xzp:gamedetails:MediaId=%s",
     "hub:root:Dash.MP.ContentExplorer.lex:gamedetails:MediaId=%s"},
    {"appdetails", true, "hub:root:BuiltIn.ContentApp.xzp:appdetails:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:appdetails:%s"},
    {"contentdetails", true,
     "hub:root:BuiltIn.ContentApp.xzp:contentdetails:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:contentdetails:%s"},
    {"contentlist",
     true,
     {},
     "hub:root:Dash.MP.ContentExplorer.lex:contentlist:%s"},
    {"contentlibrary", true,
     "hub:root:BuiltIn.ContentApp.xzp:contentlibrary:%s",
     "hub:root:Dash.MP.ContentExplorer.lex:contentlibrary:%s"},
    {"gameslibrary", true, "contentlibrary:PackageType=1;%s", {}},
    {"applibrary", true, "contentlibrary:PackageType=2;%s", {}},
    {"gamesmp", true, {}, "hub:root:Dash.MP.ContentExplorer.lex:gamesmp:%s"},
    {"appsmp", true, {}, "hub:root:Dash.MP.ContentExplorer.lex:appsmp:%s"},
    {"userpins", true, {}, "hub:root:Dash.MP.ContentExplorer.lex:userpins:%s"},
    {"videosmp", true, {}, "hub:root:Dash.MP.ContentExplorer.lex:videosmp:%s"},
    {"search", true, {}, "pam:Dash.Search.xex:scene=results|%s"},
    {"moviedetails",
     true,
     {},
     "pam:Dash.Search.xex:scene=details|itemtype=Movie|id=%s"},
    {"tvEpisodedetails",
     true,
     {},
     "pam:Dash.Search.xex:scene=details|itemtype=TVEpisode|id=%s"},
    {"tvSeasondetails",
     true,
     {},
     "pam:Dash.Search.xex:scene=details|itemtype=TVSeason|id=%s"},
    {"tvSeriesdetails",
     true,
     {},
     "pam:Dash.Search.xex:scene=details|itemtype=TVSeries|id=%s"},
    {"tvShowdetails",
     true,
     {},
     "pam:Dash.Search.xex:scene=details|itemtype=TVShow|id=%s"},
    {"webvideocollection",
     true,
     {},
     "pam:Dash.Search.xex:scene=details|itemtype=WebVideoCollection|id=%s"},
    {"picturelibrary", false, "dash:library:picture:%s", {}},
    {"videolibrary", false, "dash:library:video:%s", {}},
    {"musiclibrary", false, "dash:library:music:%s", {}},
    {"networkhelp", true, "dash:networktroubleshooter", {}},
    {"mediaoverlay",
     false,
     {},
     "dash:pamoverlay:BuiltIn.Hub.xzp:overlay:Dash.MP.ContentExplorer.lex:"
     "overlay:%s"},
    {"http", true, {}, "app:58480880:http:%s"},
    {"https", true, {}, "app:58480880:https:%s"},
    {"microsoftstore", true, {}, "pam:Dash.MP.MicrosoftStore.lex:%s"},
    {"newlivesignup", false, "app:FFFE07DE:%s", "app:FFFE07DE:%s"},
    {"activityfeed",
     false,
     {},
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityFeed:%s"},
    {"activitydetails",
     false,
     {},
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityDetails:%s"},
    {"activityalerts",
     false,
     {},
     "hub:root:Dash.MP.ContentExplorer.lex:ActivityAlerts:%s"},
};

// Schemes naming a title, which xam rewrites into title: or, for an app, app:
// with the title ID. Only a rewritten URI or a privileged caller can name a
// title other than an app.
struct UriTitle {
  std::string_view scheme;
  uint32_t title_id;
  bool is_app;
};
constexpr UriTitle kUriTitles[] = {
    {"dash", kDashboardID, false},
    {"avatareditor", 0x584D07D1, false},
    {"zune", 0x5848085B, true},
    {"newlivesignup", 0xFFFE07DE, false},
};

// The schemes a rewritten URI ends with.
constexpr std::string_view kUriBaseSchemes[] = {"title", "app", "game", "sap"};

// Rewrites |uri| as the console's xam does before opening it, at most ten
// times. |privileged| becomes set once an alias is used.
static X_HRESULT ResolveUri(std::string& uri, bool& privileged, bool live) {
  for (int i = 0; i < 10; i++) {
    const size_t colon = uri.find(':');
    const std::string scheme = uri.substr(0, colon);
    const std::string rest =
        colon == std::string::npos ? std::string() : uri.substr(colon + 1);
    if (scheme.empty() || scheme.size() >= 0x21) {
      return X_E_INVALIDARG;
    }
    const auto is_scheme = [&](std::string_view s) {
      return xe::utf8::equal_case(scheme, s);
    };
    if (std::ranges::any_of(kUriBaseSchemes, is_scheme)) {
      return X_E_SUCCESS;
    }
    std::string rewritten;
    if (const auto title = std::ranges::find_if(
            kUriTitles, [&](const UriTitle& t) { return is_scheme(t.scheme); });
        title != std::end(kUriTitles)) {
      if (!title->is_app && !privileged) {
        return X_E_ACCESS_DENIED;
      }
      rewritten = fmt::format("{}:{:08X}:{}", title->is_app ? "app" : "title",
                              title->title_id, rest);
    } else {
      const auto alias = std::ranges::find_if(
          kUriAliases, [&](const UriAlias& a) { return is_scheme(a.scheme); });
      if (alias == std::end(kUriAliases)) {
        return X_E_INVALIDARG;
      }
      if (!alias->is_public && !privileged) {
        return X_E_ACCESS_DENIED;
      }
      privileged = true;
      std::string_view format = alias->template_offline;
      if (live && !alias->template_live.empty()) {
        format = alias->template_live;
      }
      if (format.empty()) {
        return X_E_INVALIDARG;
      }
      rewritten = std::string(format);
      if (const size_t at = rewritten.find("%s"); at != std::string::npos) {
        rewritten.replace(at, 2, rest);
      }
    }
    if (rewritten.size() >= kMaxUriSize) {
      return X_E_INSUFFICIENT_BUFFER;
    }
    uri = std::move(rewritten);
  }
  return X_E_INVALIDARG;
}

// The dashboard goes to the URI in its launch data on this.
constexpr uint32_t kXNotificationDashboardNavigate = 0x80040021;
// The kind of launch data that holds a URI for the dashboard to go to.
constexpr uint32_t kNavigateLaunchDataKind = 0xC;

static X_HRESULT LaunchUri(uint32_t user_index, const std::string& uri_in,
                           uint32_t flags) {
  // The console picks LIVE's templates when LIVE's logon succeeded.
  const X_RESULT logon =
      kernel_state()->app_manager()->DispatchMessageSync(0xFC, 0x58003, 0, 0);
  const bool live = int32_t(logon) >= 0 && logon != 0x001510F1;
  std::string uri = uri_in;
  bool privileged = flags & 0x80000000;
  if (const X_HRESULT result = ResolveUri(uri, privileged, live);
      XFAILED(result)) {
    XELOGW("XamLaunchURI: can't resolve {}: {:08X}", uri_in, result);
    return result;
  }
  XELOGI("XamLaunchURI: {} resolves to {}", uri_in, uri);
  // A title: URI goes to the handler the console's xam has for the title,
  // with the rest of the URI. Only the dashboard's is here.
  const std::string prefix = fmt::format("title:{:08X}:", kDashboardID);
  if (!xe::utf8::starts_with_case(uri, prefix)) {
    XELOGW("XamLaunchURI: not opening {}", uri);
    return X_E_NOT_SUPPORTED;
  }
  // Only an alias or a privileged caller reaches the dashboard's handler.
  if (!privileged) {
    return X_E_ACCESS_DENIED;
  }
  // The dashboard's handler gives the running dashboard the URI as launch
  // data with a notification when asked to with flag 4. Otherwise it launches
  // the dashboard with that launch data.
  if (kernel_state()->title_id() != kDashboardID || !(flags & 4)) {
    XELOGW("XamLaunchURI: not launching the dashboard for {}", uri);
    return X_E_NOT_SUPPORTED;
  }
  const std::string rest = uri.substr(prefix.size());
  std::vector<uint8_t> data(kMaxUriSize, 0);
  xe::store_and_swap<uint32_t>(data.data() + 4, kNavigateLaunchDataKind);
  xe::store_and_swap<uint32_t>(data.data() + 8, user_index);
  std::memcpy(data.data() + 0xC, rest.data(),
              std::min(rest.size(), kMaxUriSize - 0xC - 1));
  auto& loader_data =
      kernel_state()->GetKernelModule<XamModule>("xam.xex")->loader_data();
  loader_data.launch_data_present = true;
  loader_data.launch_data = std::move(data);
  kernel_state()->BroadcastNotification(
      static_cast<XNotificationID>(kXNotificationDashboardNavigate),
      (user_index << 16) | kNavigateLaunchDataKind);
  return X_E_SUCCESS;
}

// The console's xam passes the URI to its own app, which works out the title
// it's for through its alias table and opens it there.
dword_result_t XamLaunchURI_entry(dword_t user_index, lpstring_t uri_ptr,
                                  dword_t flags,
                                  pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!uri_ptr) {
    return X_E_INVALIDARG;
  }
  const std::string uri = uri_ptr.value();
  if (uri.empty() || uri.size() + 1 >= kMaxUriSize) {
    return X_E_INVALIDARG;
  }
  const X_HRESULT result = LaunchUri(user_index, uri, flags);
  if (overlapped_ptr) {
    // The HRESULT is the extended error, as XMsg requests complete.
    kernel_state()->CompleteOverlappedImmediateEx(
        overlapped_ptr,
        XFAILED(result) ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS, result, 0);
    // E_PENDING, as the console's xam returns once the request is sent.
    return 0x8000000A;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamLaunchURI, kNone, kSketchy);

// The URI to go back to, which the console keeps with whether a system title
// pushed it.
static std::string back_uri_;
static bool back_uri_from_system_title_ = false;

dword_result_t XamPushBackURI_entry(lpstring_t uri_ptr) {
  if (!uri_ptr) {
    back_uri_.clear();
    back_uri_from_system_title_ = false;
    return X_E_SUCCESS;
  }
  const std::string uri = uri_ptr.value();
  if (uri.size() >= kMaxUriSize) {
    back_uri_.clear();
    back_uri_from_system_title_ = false;
    return X_E_INSUFFICIENT_BUFFER;
  }
  back_uri_ = uri;
  back_uri_from_system_title_ = IsSystemTitle(kernel_state()->title_id());
  return X_E_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamPushBackURI, kNone, kImplemented);

// The console returns the two words of the demand XamBeginDemand started, and
// whether the second is set, which is whether there is one. None is started
// here. No demand reads as 0xFE and 0. The dashboard goes home after a sign-in
// when there's none.
dword_result_t XamGetCurrentDemand_entry(lpdword_t first_ptr,
                                         lpdword_t second_ptr) {
  if (first_ptr) {
    *first_ptr = 0xFE;
  }
  if (second_ptr) {
    *second_ptr = 0;
  }
  return 0;
}
DECLARE_XAM_EXPORT1(XamGetCurrentDemand, kNone, kImplemented);

// The console returns the title that ran before the running one. That's not
// kept here, so it's none, as for the first title started.
dword_result_t XamLoaderGetPriorTitleId_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamLoaderGetPriorTitleId, kNone, kSketchy);

uint32_t XamAllocImpl(uint32_t flags, uint32_t size,
                      xe::be<uint32_t>* out_ptr) {
  if (flags & 0x00100000) {  // HEAP_ZERO_memory used unless this flag
    // do nothing!
    // maybe we ought to fill it with nonzero garbage, but otherwise this is a
    // flag we can safely ignore
  }

  // Allocate from the heap. Not sure why XAM does this specially, perhaps
  // it keeps stuff in a separate heap?
  // chrispy: there is a set of different heaps it uses, an array of them. the
  // top 4 bits of the 32 bit flags seems to select the heap
  uint32_t ptr = kernel_state()->memory()->SystemHeapAlloc(size);
  *out_ptr = ptr;

  return X_ERROR_SUCCESS;
}

dword_result_t XamAlloc_entry(dword_t flags, dword_t size, lpdword_t out_ptr) {
  return XamAllocImpl(flags, size, out_ptr);
}
DECLARE_XAM_EXPORT1(XamAlloc, kMemory, kImplemented);

static const unsigned short XamPhysicalProtTable[4] = {
    X_PAGE_READONLY, X_PAGE_READWRITE | X_PAGE_NOCACHE, X_PAGE_READWRITE,
    X_PAGE_WRITECOMBINE | X_PAGE_READWRITE};

dword_result_t XamAllocEx_entry(dword_t phys_flags, dword_t flags, dword_t size,
                                lpdword_t out_ptr, const ppc_context_t& ctx) {
  if ((flags & 0x40000000) == 0) {
    return XamAllocImpl(flags, size, out_ptr);
  }

  uint32_t flags_remapped = phys_flags;
  if ((phys_flags & 0xF000000) == 0) {
    // setting default alignment
    flags_remapped = 0xC000000 | phys_flags & 0xF0FFFFFF;
  }

  uint32_t result = xboxkrnl::xeMmAllocatePhysicalMemoryEx(
      2, size, XamPhysicalProtTable[(flags_remapped >> 28) & 0b11], 0,
      0xFFFFFFFF, 1 << ((flags_remapped >> 24) & 0xF));

  if (result && (flags_remapped & 0x40000000) != 0) {
    memset(ctx->TranslateVirtual<uint8_t*>(result), 0, size);
  }

  *out_ptr = result;
  return result ? 0 : 0x8007000E;
}
DECLARE_XAM_EXPORT1(XamAllocEx, kMemory, kImplemented);

dword_result_t XamFree_entry(lpdword_t ptr) {
  kernel_state()->memory()->SystemHeapFree(ptr.guest_address());

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamFree, kMemory, kImplemented);

// http://www.noxa.org/blog/2011/02/28/building-an-xbox-360-emulator-part-3-feasibilityos/
// http://www.noxa.org/blog/2011/08/13/building-an-xbox-360-emulator-part-5-xex-files/
dword_result_t RtlSleep_entry(dword_t dwMilliseconds, dword_t bAlertable) {
  uint64_t delay{};

  // Convert the delay time to 100-nanosecond intervals
  delay = dwMilliseconds == -1 ? LLONG_MAX
                               : static_cast<int64_t>(-10000) * dwMilliseconds;

  X_STATUS result = xboxkrnl::KeDelayExecutionThread(MODE::UserMode, bAlertable,
                                                     &delay, nullptr);

  // If the delay was interrupted by an APC, keep delaying the thread
  while (bAlertable && result == X_STATUS_ALERTED) {
    result = xboxkrnl::KeDelayExecutionThread(MODE::UserMode, bAlertable,
                                              &delay, nullptr);
  }

  return result == X_STATUS_SUCCESS ? X_STATUS_SUCCESS : X_STATUS_USER_APC;
}
DECLARE_XAM_EXPORT1(RtlSleep, kNone, kImplemented);

dword_result_t SleepEx_entry(dword_t dwMilliseconds, dword_t bAlertable) {
  return RtlSleep_entry(dwMilliseconds, bAlertable);
}
DECLARE_XAM_EXPORT1(SleepEx, kNone, kImplemented);

// https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep
void Sleep_entry(dword_t dwMilliseconds) {
  RtlSleep_entry(dwMilliseconds, false);
}
DECLARE_XAM_EXPORT1(Sleep, kNone, kImplemented);

// https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount
dword_result_t GetTickCount_entry() { return Clock::QueryGuestUptimeMillis(); }
DECLARE_XAM_EXPORT1(GetTickCount, kNone, kImplemented);

dword_result_t RtlSetLastNTError_entry(dword_t error_code) {
  const uint32_t result =
      xe::kernel::xboxkrnl::xeRtlNtStatusToDosError(error_code);
  XThread::SetLastError(result);

  return result;
}
DECLARE_XAM_EXPORT1(RtlSetLastNTError, kNone, kImplemented);

dword_result_t RtlGetLastError_entry() { return XThread::GetLastError(); }
DECLARE_XAM_EXPORT1(RtlGetLastError, kNone, kImplemented);

dword_result_t XGetOverlappedExtendedError_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!overlapped_ptr) {
    return XThread::GetLastError();
  }
  if (overlapped_ptr->result == X_ERROR_IO_PENDING) {
    return X_ERROR_IO_INCOMPLETE;
  }
  return static_cast<uint32_t>(overlapped_ptr->extended_error);
}
DECLARE_XAM_EXPORT1(XGetOverlappedExtendedError, kNone, kImplemented);

dword_result_t GetLastError_entry() { return RtlGetLastError_entry(); }
DECLARE_XAM_EXPORT1(GetLastError, kNone, kImplemented);

dword_result_t GetModuleHandleA_entry(lpstring_t module_name) {
  xe::be<uint32_t> module_ptr = 0;
  const X_STATUS error_code = xe::kernel::xboxkrnl::XexGetModuleHandle(
      module_name ? module_name.value() : "", &module_ptr);

  if (XFAILED(error_code)) {
    RtlSetLastNTError_entry(error_code);

    return NULL;
  }

  return (uint32_t)module_ptr;
}
DECLARE_XAM_EXPORT1(GetModuleHandleA, kNone, kImplemented);

dword_result_t XapipCreateThread_entry(
    lpdword_t thread_attributes, dword_t stack_size, lpvoid_t start_address,
    lpvoid_t parameter, dword_t creation_flags, dword_t thread_processor,
    lpdword_t thread_id) {
  uint32_t flags = (creation_flags >> 2) & 1;

  if (thread_processor != -1) {
    flags |= 1 << thread_processor << 24;
  }

  xe::be<uint32_t> result = 0;

  const X_STATUS error_code = xe::kernel::xboxkrnl::ExCreateThread(
      &result, stack_size, thread_id, start_address, parameter, 0, flags);

  if (XFAILED(error_code)) {
    RtlSetLastNTError_entry(error_code);

    return NULL;
  }

  return (uint32_t)result;
}
DECLARE_XAM_EXPORT1(XapipCreateThread, kNone, kImplemented);

dword_result_t CreateThread_entry(lpdword_t lpThreadAttributes,
                                  dword_t dwStackSize, lpvoid_t lpStartAddress,
                                  lpvoid_t lpParameter, dword_t dwCreationFlags,
                                  lpdword_t lpThreadId) {
  return XapipCreateThread_entry(lpThreadAttributes, dwStackSize,
                                 lpStartAddress, lpParameter, dwCreationFlags,
                                 -1, lpThreadId);
}
DECLARE_XAM_EXPORT1(CreateThread, kNone, kImplemented);

dword_result_t CloseHandle_entry(dword_t hObject) {
  const X_STATUS error_code = xe::kernel::xboxkrnl::NtClose(hObject);

  if (XFAILED(error_code)) {
    RtlSetLastNTError_entry(error_code);

    return false;
  }

  return true;
}
DECLARE_XAM_EXPORT1(CloseHandle, kNone, kImplemented);

dword_result_t ResumeThread_entry(dword_t hThread) {
  uint32_t suspend_count;
  const X_STATUS error_code =
      xe::kernel::xboxkrnl::NtResumeThread(hThread, &suspend_count);

  if (XFAILED(error_code)) {
    RtlSetLastNTError_entry(error_code);

    return -1;
  }

  return suspend_count;
}
DECLARE_XAM_EXPORT1(ResumeThread, kNone, kImplemented);

void ExitThread_entry(dword_t exit_code) {
  xe::kernel::xboxkrnl::ExTerminateThread(exit_code);
}
DECLARE_XAM_EXPORT1(ExitThread, kNone, kImplemented);

dword_result_t GetCurrentThreadId_entry() {
  return XThread::GetCurrentThread()->GetCurrentThreadId();
}
DECLARE_XAM_EXPORT1(GetCurrentThreadId, kNone, kImplemented);

// Returns timeout, or null for INFINITE.
static uint64_t* XapiFormatTimeOut(uint64_t* timeout, uint32_t milliseconds) {
  if (milliseconds == UINT32_MAX) {
    return nullptr;
  }
  *timeout = static_cast<int64_t>(-10000) * milliseconds;
  return timeout;
}

// Returns r3 as the caller passed it, upper half included.
qword_result_t XapiFormatTimeOut_entry(lpqword_t result, dword_t dwMilliseconds,
                                       const ppc_context_t& ctx) {
  uint64_t timeout;
  if (!XapiFormatTimeOut(&timeout, dwMilliseconds)) {
    return 0;
  }
  *result = timeout;
  return ctx->r[3];
}
DECLARE_XAM_EXPORT1(XapiFormatTimeOut, kNone, kImplemented);

dword_result_t WaitForSingleObjectEx_entry(dword_t hHandle,
                                           dword_t dwMilliseconds,
                                           dword_t bAlertable) {
  uint64_t timeout;
  uint64_t* timeout_ptr = XapiFormatTimeOut(&timeout, dwMilliseconds);
  // The kernel takes alertable as a BOOLEAN.
  const uint8_t alertable = uint8_t(bAlertable);

  X_STATUS result =
      xboxkrnl::NtWaitForSingleObjectEx(hHandle, 1, alertable, timeout_ptr);

  while (bAlertable && result == X_STATUS_ALERTED) {
    result =
        xboxkrnl::NtWaitForSingleObjectEx(hHandle, 1, alertable, timeout_ptr);
  }

  if (int32_t(result) < 0) {
    RtlSetLastNTError_entry(result);
    return -1;
  }
  return result;
}
DECLARE_XAM_EXPORT1(WaitForSingleObjectEx, kNone, kImplemented);

dword_result_t WaitForSingleObject_entry(dword_t hHandle,
                                         dword_t dwMilliseconds) {
  return WaitForSingleObjectEx_entry(hHandle, dwMilliseconds, 0);
}
DECLARE_XAM_EXPORT1(WaitForSingleObject, kNone, kImplemented);

dword_result_t lstrlenW_entry(lpu16string_t string) {
  // wcslen?
  if (string) {
    return (uint32_t)string.value().length();
  }

  return NULL;
}
DECLARE_XAM_EXPORT1(lstrlenW, kNone, kImplemented);

dword_result_t XGetAudioFlags_entry() {
  if (cvars::avpack == 2) {
    return 2;
  }

  const auto audio_flags = kernel_state()->xconfig()->ReadSetting<uint32_t>(
      XCONFIG_USER_CATEGORY,
      XCONFIG_USER_CATEGORY_ENTRIES::XCONFIG_USER_AUDIO_FLAGS);

  return audio_flags ? audio_flags : 0x10000 | 0x1;
}
DECLARE_XAM_EXPORT1(XGetAudioFlags, kNone, kImplemented);

/*
        todo: this table should instead be pointed to by a member of kernel
   state and initialized along with the process
*/
static int32_t XamRtlRandomTable[128] = {
    1284227242, 1275210071, 573735546,  790525478,  2139871995, 1547161642,
    179605362,  789336058,  688789844,  1801674531, 1563985344, 1957994488,
    1364589140, 1645522239, 287218729,  606747145,  1972579041, 1085031214,
    1425521274, 1482476501, 1844823847, 57989841,   1897051121, 1935655697,
    1078145449, 1960408961, 1682526488, 842925246,  1500820517, 1214440339,
    1647877149, 682003330,  261478967,  2052111302, 162531612,  583907252,
    1336601894, 1715567821, 413027322,  484763869,  1383384898, 1004336348,
    764733703,  854245398,  651377827,  1614895754, 838170752,  1757981266,
    602609370,  1644491937, 926492609,  220523388,  115176313,  725345543,
    261903793,  746137067,  920026266,  1123561945, 1580818891, 1708537768,
    616249376,  1292428093, 562591055,  343818398,  1788223648, 1659004503,
    2077806209, 299502267,  1604221776, 602162358,  630328440,  1606980848,
    1580436667, 1078081533, 492894223,  839522115,  1979792683, 117609710,
    1767777339, 1454471165, 1965331169, 1844237615, 308236825,  329068152,
    412668190,  796845957,  1303643608, 436374069,  1677128483, 527237240,
    813497703,  1060284298, 1770027372, 1177238915, 884357618,  1409082233,
    1958367476, 448539723,  1592454029, 861567501,  963894560,  73586283,
    362288127,  507921405,  113007714,  823518204,  152049171,  1202660629,
    1326574676, 2025429265, 1035525444, 515967899,  1532298954, 2000478354,
    1450960922, 1417001333, 2049760794, 1229272821, 879983540,  1993962763,
    706699826,  776561741,  2111687655, 1343024091, 1637723038, 1220945662,
    484061587,  1390067357};

/*
        Follows xam exactly, the updates to the random table are probably racy.
*/
dword_result_t RtlRandom_entry(lpdword_t seed_out) {
  int32_t table_seed_new = (0x7FFFFFED * *seed_out + 0x7FFFFFC3) % 0x7FFFFFFF;
  *seed_out = table_seed_new;
  uint32_t param_seed_new =
      (0x7FFFFFED * table_seed_new + 0x7FFFFFC3) % 0x7FFFFFFFu;
  *seed_out = param_seed_new;

  int32_t* update_table_position = &XamRtlRandomTable[param_seed_new & 0x7F];

  int32_t result = *update_table_position;
  *update_table_position = table_seed_new;
  return result;
}

DECLARE_XAM_EXPORT1(RtlRandom, kNone, kImplemented);

dword_result_t Refresh_entry() { return X_ERROR_SUCCESS; }
DECLARE_XAM_EXPORT1(Refresh, kNone, kStub);

dword_result_t XamIsSystemExperienceTitleId_entry(dword_t title_id) {
  return IsSystemExperienceTitle(title_id);
}
DECLARE_XAM_EXPORT1(XamIsSystemExperienceTitleId, kNone, kImplemented);

dword_result_t XamIsSystemTitleId_entry(dword_t title_id) {
  return IsSystemTitle(title_id);
}
DECLARE_XAM_EXPORT1(XamIsSystemTitleId, kNone, kImplemented);

dword_result_t XamIsXbox1TitleId_entry(dword_t title_id) {
  return IsOriginalXboxTitle(title_id);
}
DECLARE_XAM_EXPORT1(XamIsXbox1TitleId, kNone, kImplemented);

dword_result_t XamIsChildAccountSignedIn_entry() {
  for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
    const auto& profile = kernel_state()->xam_state()->GetUserProfile(i);
    if (profile && profile->IsParentalControlled()) {
      return true;
    }
  }

  return false;
}
DECLARE_XAM_EXPORT1(XamIsChildAccountSignedIn, kNone, kImplemented);

void XamSetActiveDashAppInfo_entry(pointer_t<X_DASH_APP_INFO> dash_app) {
  if (!dash_app) {
    kernel_state()->xam_state()->dash_app_info_ = {};
    return;
  }
  std::memcpy(&kernel_state()->xam_state()->dash_app_info_, dash_app,
              sizeof(X_DASH_APP_INFO));
}
DECLARE_XAM_EXPORT1(XamSetActiveDashAppInfo, kNone, kImplemented);

void XamGetActiveDashAppInfo_entry(pointer_t<X_DASH_APP_INFO> dash_app) {
  if (!dash_app) {
    return;
  }
  std::memcpy(dash_app, &kernel_state()->xam_state()->dash_app_info_,
              sizeof(X_DASH_APP_INFO));
}
DECLARE_XAM_EXPORT1(XamGetActiveDashAppInfo, kNone, kImplemented);

void XampWebInstrumentationSetProfileCounts_entry(dword_t live_profiles,
                                                  dword_t local_profiles,
                                                  dword_t adult_profiles,
                                                  dword_t child_profiles) {}
DECLARE_XAM_EXPORT1(XampWebInstrumentationSetProfileCounts, kUserProfiles,
                    kStub);

dword_result_t XamDoesOmniNeedConfiguration_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamDoesOmniNeedConfiguration, kNone, kStub);

dword_result_t XamFirstRunExperienceShouldRun_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamFirstRunExperienceShouldRun, kNone, kStub);

dword_result_t QueryPerformanceFrequency_entry(lpqword_t query) {
  uint64_t result = Clock::guest_tick_frequency();
  *query = static_cast<uint32_t>(result);
  return 1;
}
DECLARE_XAM_EXPORT1(QueryPerformanceFrequency, kNone, kImplemented);

void GetSystemTimeAsFileTime_entry(lpqword_t time_ptr,
                                   const ppc_context_t& ctx) {
  if (time_ptr) {
    uint32_t ts_bundle = ctx->kernel_state->GetKeTimestampBundle();
    uint64_t time = Clock::QueryGuestSystemTime();
    ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle)->system_time =
        xe::byte_swap(time);
    *time_ptr = time;
  }
}
DECLARE_XAM_EXPORT1(GetSystemTimeAsFileTime, kNone, kImplemented);

dword_result_t XamIsIptvEnabled_entry() {
  const bool iptv_enabled =
      kernel_state()->xconfig()->ReadSetting<uint32_t>(
          X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS) &
      X_RETAIL_FLAGS::IPTVEnabled;

  return !iptv_enabled ? X_E_FAIL : X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamIsIptvEnabled, kNone, kImplemented);

dword_result_t XamIptvGetServiceName_entry(lpdword_t service_name_ptr) {
  auto address = kernel_state()->xam_state()->GetIptvNameAddress();
  auto buffer = kernel_state()->memory()->TranslateVirtual(address);
  char16_t* data_ptr = reinterpret_cast<char16_t*>(buffer);
  kernel_state()->xconfig()->ReadSetting(
      X_CONFIG_CATEGORY::XCONFIG_IPTV_CATEGORY,
      XCONFIG_IPTV_SERVICE_PROVIDER_NAME, data_ptr);
  if (*data_ptr == u'\0') {
    xe::string_util::copy_and_swap_truncating(data_ptr, u"Xenia TV", 9);
  }
  *service_name_ptr = address;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamIptvGetServiceName, kNone, kImplemented);

dword_result_t XamGetDvrStorage_entry(lpdword_t dvr_storage,
                                      lpdword_t used_dvr_storage,
                                      lpdword_t hdd_unused_space) {
  *dvr_storage = 0;
  *used_dvr_storage = 0;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamGetDvrStorage, kNone, kStub);

dword_result_t XamSetDvrStorage_entry(
    dword_t dvr_storage_size, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSetDvrStorage, kNone, kStub);

dword_result_t XamLookupCommonStringByIndex_entry(dword_t string_index) {
  static std::mutex lock;
  std::lock_guard<std::mutex> guard(lock);
  auto& table = kernel_state()->xam_state()->common_string_table_;
  if (!table) {
    // They're in the xam section of the system UI skin.
    constexpr std::string_view kSkinPath = "\\SystemRoot\\huduiskin.xex";
    uint32_t loaded = 0;
    object_ref<XModule> skin;
    if (xboxkrnl::xeXexLoadImage(kSkinPath)) {
      skin = kernel_state()->GetModule(kSkinPath, true);
    }
    if (!skin ||
        XFAILED(LoadXuiStringTable(*skin, "xam", "xamstrings.xus", &loaded))) {
      XELOGW("XamLookupCommonStringByIndex: no xam string table to read");
    }
    table = loaded;
  }
  return *table ? LookupXuiStringTable(*table, string_index) : 0;
}
DECLARE_XAM_EXPORT1(XamLookupCommonStringByIndex, kNone, kImplemented);

dword_result_t XamLogLocalizationEtx_entry(dword_t error_code, dword_t unk) {
  if (error_code == 0x80300034) {
    // uses second unk for some function
    return X_ERROR_SUCCESS;
  } else if (error_code == 0x80300035) {
    // uses second unk for some function
    return X_ERROR_SUCCESS;
  }
  return X_E_NOT_IMPLEMENTED;
}
DECLARE_XAM_EXPORT1(XamLogLocalizationEtx, kNone, kStub);

dword_result_t XamUpdateGetBaseSystemVersion_entry() {
  return PackSystemVersion(KernelVersion(kBaseKernelBuildVersion));
}
DECLARE_XAM_EXPORT1(XamUpdateGetBaseSystemVersion, kNone, kImplemented);

dword_result_t XamUpdateGetCurrentSystemVersion_entry() {
  return PackSystemVersion(*kernel_state()->GetKernelVersion());
}
DECLARE_XAM_EXPORT1(XamUpdateGetCurrentSystemVersion, kNone, kImplemented);

// The newest version an installed optional media update gives for
// module_name. Its module_name.xex or module_nameLatest.xex returns the
// version from export 1. 0 without an update, and the name is only read once
// there is one.
static uint32_t GetOptionalMediaUpdateVersion(lpstring_t module_name) {
  constexpr uint32_t kOptionalMediaUpdateTitleId = 0xFFFE07DF;
  constexpr std::string_view kRoot = "CodecPackLoaderRoot";
  auto content_manager = kernel_state()->content_manager();
  auto thread_state = XThread::GetCurrentThread()->thread_state();
  uint32_t newest = 0;
  for (const auto& data : content_manager->ListContent(
           static_cast<uint32_t>(DummyDeviceId::HDD), 0,
           kOptionalMediaUpdateTitleId, XContentType::kMarketplaceContent)) {
    uint32_t license = 0;
    if (content_manager->OpenContent(kRoot, 0, data, license)) {
      continue;
    }
    for (std::string_view suffix : {"", "Latest"}) {
      const std::string path =
          fmt::format("{}:\\{}{}.xex", kRoot, module_name.value(), suffix);
      if (!kernel_state()->file_system()->ResolvePath(path)) {
        continue;
      }
      const uint32_t hmodule = xboxkrnl::xeXexLoadImage(path);
      if (!hmodule) {
        continue;
      }
      auto module = kernel_state()->GetModule(path, true);
      if (const uint32_t get_version =
              module ? module->GetProcAddressByOrdinal(1) : 0) {
        newest = std::max(newest, uint32_t(kernel_state()->processor()->Execute(
                                      thread_state, get_version, nullptr, 0)));
      }
      xboxkrnl::xeXexUnloadImage(hmodule);
    }
    content_manager->CloseContent(kRoot);
  }
  return newest;
}

// Whether the installed optional media update gives module_name at least
// min_version.
dword_result_t XamIsOptionalMediaUpdateInstalled_entry(lpstring_t module_name,
                                                       dword_t min_version) {
  const uint32_t version =
      module_name ? GetOptionalMediaUpdateVersion(module_name) : 0;
  return version >= min_version;
}
DECLARE_XAM_EXPORT1(XamIsOptionalMediaUpdateInstalled, kNone, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Info);
