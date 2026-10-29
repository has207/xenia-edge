/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <fstream>
#include <thread>
#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/base/utf8.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/smc.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/devices/stfs_xbox.h"
#include "xenia/vfs/devices/xcontent_container_device.h"
#include "xenia/xbox.h"

DECLARE_bool(in_process_title_relaunch);

DEFINE_int32(
    license_mask, 0,
    "Set license mask for activated content.\n"
    " 0 = No licenses enabled.\n"
    " 1 = First license enabled. Generally the full version license in\n"
    "     Xbox Live Arcade titles.\n"
    " -1 or 0xFFFFFFFF = All possible licenses enabled. Generally a\n"
    "                    bad idea, could lead to undefined behavior.",
    "Content");

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XamContentGetLicenseMask_entry(lpdword_t mask_ptr,
                                              lpvoid_t overlapped_ptr) {
  if (!mask_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto run = [mask_ptr](uint32_t& extended_error, uint32_t& length) {
    X_RESULT result = X_ERROR_FUNCTION_FAILED;

    // Remark: This cannot be reflected as on console. Xenia can boot games
    // directly and XBLA games can be repacked to ZAR. For these titles we must
    // provide some license. Normally it should fail for OpticalDisc type.
    if (kernel_state()->deployment_type_ != XDeploymentType::kOther) {
      // Each bit in the mask represents a granted license. Available licenses
      // seems to vary from game to game, but most appear to use bit 0 to
      // indicate if the game is purchased or not.
      *mask_ptr = static_cast<uint32_t>(cvars::license_mask);
      result = X_ERROR_SUCCESS;
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = 0;
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT2(XamContentGetLicenseMask, kContent, kStub, kHighFrequency);

dword_result_t xeXamContentResolve(
    dword_t user_index, lpvoid_t content_data_ptr, dword_t content_data_size,
    lpstring_t path_ptr, dword_t path_size, dword_t create_directory,
    lpstring_t root_name_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  auto run = [user_index, content_data_ptr, content_data_size, path_ptr,
              path_size, create_directory, root_name_ptr](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    XCONTENT_AGGREGATE_DATA content_data;
    if (content_data_size == sizeof(XCONTENT_DATA)) {
      content_data = *content_data_ptr.as<XCONTENT_DATA*>();
    } else if (content_data_size == sizeof(XCONTENT_DATA_INTERNAL)) {
      // Due to the current implementation of content data we can't use
      // XCONTENT_DATA_INTERNAL
      content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
    } else {
      assert_always();
      return X_ERROR_INVALID_PARAMETER;
    }
    uint64_t xuid = 0;
    if (user_index < XUserMaxUserCount) {
      const auto profile =
          kernel_state()->xam_state()->profile_manager()->GetProfile(
              static_cast<uint8_t>(user_index));
      if (profile && content_data.content_type == XContentType::kSavedGame) {
        xuid = profile->xuid();
      }
    }

    std::string root_device_path = "";

    if (root_name_ptr) {
      // Check if root_name is valid.
      // root_device_path = std::string(root_name_ptr);
      // Unsupported for now.
      return X_ERROR_INVALID_PARAMETER;
    } else {
      if (content_data.device_id == static_cast<uint32_t>(DummyDeviceId::HDD)) {
        root_device_path = "\\Device\\Harddisk0\\Partition1\\Content\\";
      } else if (content_data.device_id ==
                 static_cast<uint32_t>(DummyDeviceId::ODD)) {
        root_device_path = kernel_state()->title_mount_path_ + "\\content\\";
      } else {
        return X_ERROR_INVALID_PARAMETER;
      }
    }

    const std::string relative_dir =
        fmt::format("{:016X}\\{:08X}\\{:08X}", xuid, kernel_state()->title_id(),
                    static_cast<uint32_t>(content_data.content_type.get()));
    const std::string content_dir = root_device_path + relative_dir;

    // The caller creates the package file itself, so the directory must exist.
    if (create_directory && !kernel_state()->file_system()->CreatePath(
                                content_dir, vfs::kFileAttributeDirectory)) {
      XELOGW("XamContentResolve: Cannot create content directory {}",
             content_dir);
    }

    string_util::copy_truncating(
        path_ptr, content_dir + "\\" + content_data.file_name(), path_size);

    // Check if it exists and try to mount that package
    // Result of buffer_ptr is sent to RtlInitAnsiString.
    // buffer_size is usually 260 (max path).
    return X_ERROR_SUCCESS;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}

dword_result_t XamContentResolve_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpstring_t path_ptr,
    dword_t path_size, dword_t create_directory, lpstring_t root_name_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamContentResolve(user_index, content_data_ptr,
                             sizeof(XCONTENT_DATA), path_ptr, path_size,
                             create_directory, root_name_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentResolve, kContent, kSketchy);

dword_result_t XamContentResolveInternal_entry(
    lpvoid_t content_data_ptr, lpstring_t path_ptr, dword_t path_size,
    dword_t create_directory, lpstring_t root_name_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamContentResolve(
      XUserIndexNone, content_data_ptr, sizeof(XCONTENT_DATA_INTERNAL),
      path_ptr, path_size, create_directory, root_name_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentResolveInternal, kContent, kSketchy);

// https://github.com/MrColdbird/gameservice/blob/master/ContentManager.cpp
dword_result_t XamContentCreateEnumeratorInternal_entry(
    qword_t xuid, dword_t device_id, dword_t content_type, dword_t title_id,
    dword_t content_flags, dword_t items_per_enumerate,
    lpdword_t buffer_size_ptr, lpdword_t handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }

    // TODO(benvanik): memset 0 the data?
    return X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(XCONTENT_DATA) * items_per_enumerate;
  }

  auto e = object_ref<ContentEnumerator>(
      new ContentEnumerator(kernel_state(), items_per_enumerate));

  auto result =
      e->Initialize(XUserIndexAny, 0xFE, 0x20005, 0x20007, 0, 0x98, nullptr);
  if (XFAILED(result)) {
    return result;
  }

  uint32_t title = title_id;
  if (!title) {
    title = kernel_state()->title_id();
  }

  std::vector<XCONTENT_AGGREGATE_DATA> enumerated_content =
      {};  // XCONTENT_DATA_INTERNAL

  if (!device_info || device_info->device_id == DummyDeviceId::HDD) {
    auto content_manager = kernel_state()->content_manager();
    const auto type = static_cast<XContentType>(content_type.value());
    if (xuid) {
      enumerated_content = content_manager->ListContent(
          static_cast<uint32_t>(DummyDeviceId::HDD), xuid, title, type);
    }

    if (!xuid || !(content_flags & vfs::XContentFlag::kExcludeCommon)) {
      // A user gets common content under its file name with the shared bit
      // set, unless one of the user's own has that name. DLC is common
      // whatever its name so it keeps the name it's stored under.
      const size_t own_count = enumerated_content.size();
      for (auto& data : content_manager->ListContent(
               static_cast<uint32_t>(DummyDeviceId::HDD), 0, title, type)) {
        if (xuid && type != XContentType::kMarketplaceContent) {
          if (std::any_of(enumerated_content.begin(),
                          enumerated_content.begin() + own_count,
                          [&](const XCONTENT_AGGREGATE_DATA& own) {
                            return own.file_name() == data.file_name();
                          })) {
            continue;
          }
          data.file_name_raw[0] =
              char(uint8_t(data.file_name_raw[0]) | kSharedContentFileNameBit);
        }
        enumerated_content.push_back(data);
      }
    }
  }

  if (!device_info || device_info->device_id == DummyDeviceId::ODD) {
    auto disc_enumerated_data =
        kernel_state()->content_manager()->ListContentODD(
            static_cast<uint32_t>(DummyDeviceId::ODD), 0, title,
            XContentType(uint32_t(content_type)));

    enumerated_content.insert(enumerated_content.end(),
                              disc_enumerated_data.cbegin(),
                              disc_enumerated_data.cend());
  }

  for (const auto& content_data : enumerated_content) {
    e->AppendItem(content_data);
    XELOGI("{}: Adding: {} (Filename: {}) to enumerator result", __func__,
           xe::to_utf8(content_data.display_name()), content_data.file_name());
  }

  XELOGD("XamContentCreateEnumeratorInternal: added {} items to enumerator",
         e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentCreateEnumeratorInternal, kContent, kImplemented);

dword_result_t XamContentCreateEnumerator_entry(
    dword_t user_index, dword_t device_id, dword_t content_type,
    dword_t content_flags, dword_t items_per_enumerate,
    lpdword_t buffer_size_ptr, lpdword_t handle_out) {
  uint64_t xuid = 0;
  if (user_index < XUserMaxUserCount) {
    const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);

    if (!user) {
      return X_ERROR_NO_SUCH_USER;
    }

    xuid = user->xuid();
  }

  return XamContentCreateEnumeratorInternal_entry(
      xuid, device_id, content_type, 0, content_flags, items_per_enumerate,
      buffer_size_ptr, handle_out);
}
DECLARE_XAM_EXPORT1(XamContentCreateEnumerator, kContent, kImplemented);

enum class kDispositionState : uint32_t { Unknown = 0, Create = 1, Open = 2 };

// xam refuses root names with these characters after an optional
// \??\ or \System??\ prefix in any case.
static bool ValidRootName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (std::string_view prefix : {"\\??\\", "\\System??\\"}) {
    if (xe::utf8::starts_with_case(name, prefix)) {
      name.remove_prefix(prefix.size());
      break;
    }
  }
  return name.find_first_of(":;<=>?@!\"#$%&'()*+,-./[\\]") ==
         std::string_view::npos;
}

// xam takes an ASCII file name but lets its first byte have the top bit set.
static bool ValidFileName(const XCONTENT_DATA& content_data) {
  const auto& name = content_data.file_name_raw;
  if (!name[0]) {
    return false;
  }
  for (size_t i = (uint8_t(name[0]) & kSharedContentFileNameBit) ? 1 : 0;
       i < xe::countof(name) && name[i]; ++i) {
    if (uint8_t(name[i]) & 0x80) {
      return false;
    }
  }
  return true;
}

// Content requests look the user up as they run and use the user's XUID for the
// four user indices and XUID 0 for any other.
static X_RESULT ContentXuid(uint32_t user_index, uint64_t& xuid) {
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_SUCCESS;
  }
  const auto& user = kernel_state()->xam_state()->GetUserProfile(user_index);
  if (!user) {
    return X_ERROR_NO_SUCH_USER;
  }
  xuid = user->xuid();
  return X_ERROR_SUCCESS;
}

// A request with an overlapped completes it with ERROR_FUNCTION_FAILED and its
// error as an HRESULT, or with success.
static X_RESULT CompleteRequest(pointer_t<XAM_OVERLAPPED> overlapped_ptr,
                                X_RESULT result) {
  if (!overlapped_ptr) {
    return result;
  }
  kernel_state()->CompleteOverlappedImmediateEx(
      overlapped_ptr, result ? X_ERROR_FUNCTION_FAILED : X_ERROR_SUCCESS,
      X_HRESULT_FROM_WIN32(result), 0);
  return X_ERROR_IO_PENDING;
}

dword_result_t xeXamContentCreate(dword_t user_index, lpstring_t root_name,
                                  lpvoid_t content_data_ptr,
                                  dword_t content_data_size, dword_t flags,
                                  lpdword_t disposition_ptr,
                                  lpdword_t license_mask_ptr,
                                  dword_t cache_size, qword_t content_size,
                                  pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (cache_size > 0xEF000) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!root_name || !ValidRootName(root_name.value())) {
    return X_ERROR_INVALID_NAME;
  }
  // xam clears them before looking anything up.
  if (disposition_ptr) {
    *disposition_ptr = 0;
  }
  if (license_mask_ptr) {
    *license_mask_ptr = 0;
  }

  XCONTENT_AGGREGATE_DATA content_data;
  if (content_data_size == sizeof(XCONTENT_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }

  auto content_manager = kernel_state()->content_manager();

  auto run = [content_manager, user_index, root_name = root_name.value(), flags,
              content_data, disposition_ptr, license_mask_ptr, overlapped_ptr](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    uint64_t xuid = 0;
    X_RESULT result = ContentXuid(user_index, xuid);
    if (content_data.content_type == XContentType::kMarketplaceContent) {
      xuid = 0;
    }
    if (!result) {
      result = content_manager->CheckPackageName(xuid, content_data);
    }
    // The root is mounted after the content is chosen and made. A create on a
    // root in use still makes the package and fails with ERROR_DUP_NAME, while
    // an open fails with ERROR_ALREADY_EXISTS.
    const bool root_in_use =
        kernel_state()->file_system()->IsSymbolicLinkRegistered(root_name +
                                                                ':');

    kDispositionState disposition = kDispositionState::Unknown;
    if (!result) {
      switch (flags & 0xF) {
        case vfs::XContentFlag::kCreateNew:
          // Fail if exists.
          if (content_manager->ContentExists(xuid, content_data)) {
            result = X_ERROR_ALREADY_EXISTS;
          } else {
            disposition = kDispositionState::Create;
          }
          break;
        case vfs::XContentFlag::kCreateAlways:
          // Overwrite existing unless the root is in use, failing if the
          // delete does.
          if (!root_in_use &&
              content_manager->ContentExists(xuid, content_data)) {
            result = content_manager->DeleteContent(xuid, content_data);
          }
          if (!result) {
            disposition = kDispositionState::Create;
          }
          break;
        case vfs::XContentFlag::kOpenExisting:
          // Open only if exists.
          if (!content_manager->ContentExists(xuid, content_data)) {
            result = X_ERROR_PATH_NOT_FOUND;
          } else {
            disposition = kDispositionState::Open;
          }
          break;
        case vfs::XContentFlag::kOpenAlways:
          // Create if needed.
          if (!content_manager->ContentExists(xuid, content_data)) {
            disposition = kDispositionState::Create;
          } else {
            disposition = kDispositionState::Open;
          }
          break;
        case vfs::XContentFlag::kTruncateExisting:
          // Fail if doesn't exist, if does exist empty and open it.
          if (!content_manager->ContentExists(xuid, content_data)) {
            result = X_ERROR_PATH_NOT_FOUND;
          } else if (root_in_use) {
            // It's opened before it's emptied.
            result = X_ERROR_ALREADY_EXISTS;
          } else {
            result = content_manager->TruncateContent(xuid, content_data);
            if (!result) {
              disposition = kDispositionState::Open;
            }
          }
          break;
        default:
          assert_unhandled_case(flags & 0xF);
          result = X_ERROR_INVALID_PARAMETER;
          break;
      }
    }
    if (root_in_use && disposition != kDispositionState::Unknown) {
      if (disposition == kDispositionState::Create) {
        if (!content_manager->ContentExists(xuid, content_data) &&
            !content_manager->CreatePackage(xuid, content_data)) {
          content_manager->WriteContentHeaderFile(xuid, content_data);
        }
        result = X_ERROR_DUP_NAME;
      } else {
        result = X_ERROR_ALREADY_EXISTS;
      }
      disposition = kDispositionState::Unknown;
    }

    uint32_t content_license = 0;
    if (disposition == kDispositionState::Create) {
      result = content_manager->CreateContent(root_name, xuid, content_data);
      if (!result) {
        content_manager->WriteContentHeaderFile(xuid, content_data);
      }
    } else if (disposition == kDispositionState::Open) {
      result = content_manager->OpenContent(root_name, xuid, content_data,
                                            content_license);
    }
    // A request that failed reports no disposition.
    if (result) {
      disposition = kDispositionState::Unknown;
    }

    if (license_mask_ptr && !result) {
      *license_mask_ptr = content_license;
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);

    if (disposition_ptr) {
      *disposition_ptr = static_cast<uint32_t>(disposition);
    }

    if (result && overlapped_ptr) {
      result = X_ERROR_FUNCTION_FAILED;
    }
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}

dword_result_t XamContentCreateEx_entry(
    dword_t user_index, lpstring_t root_name, lpvoid_t content_data_ptr,
    dword_t flags, lpdword_t disposition_ptr, lpdword_t license_mask_ptr,
    dword_t cache_size, qword_t content_size,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!ValidFileName(*content_data_ptr.as<XCONTENT_DATA*>())) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return xeXamContentCreate(user_index, root_name, content_data_ptr,
                            sizeof(XCONTENT_DATA), flags, disposition_ptr,
                            license_mask_ptr, cache_size, content_size,
                            overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreateEx, kContent, kImplemented);

dword_result_t XamContentCreate_entry(
    dword_t user_index, lpstring_t root_name, lpvoid_t content_data_ptr,
    dword_t flags, lpdword_t disposition_ptr, lpdword_t license_mask_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return XamContentCreateEx_entry(user_index, root_name, content_data_ptr,
                                  flags, disposition_ptr, license_mask_ptr, 0,
                                  0, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreate, kContent, kImplemented);

dword_result_t XamContentCreateInternal_entry(
    lpstring_t root_name, lpvoid_t content_data_ptr, dword_t flags,
    lpdword_t disposition_ptr, lpdword_t license_mask_ptr, dword_t cache_size,
    qword_t content_size, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!ValidFileName(*content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>())) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return xeXamContentCreate(XUserIndexNone, root_name, content_data_ptr,
                            sizeof(XCONTENT_AGGREGATE_DATA), flags,
                            disposition_ptr, license_mask_ptr, cache_size,
                            content_size, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentCreateInternal, kContent, kImplemented);

// Packages can only be mounted from host files, so one inside a container
// such as a Games on Demand package is copied out to the cache first. A copy
// of the same size from an earlier run is reused.
static std::filesystem::path GetContentFileHostPath(vfs::Entry* entry) {
  if (auto host_entry = dynamic_cast<vfs::HostPathEntry*>(entry)) {
    return host_entry->host_path();
  }
  const std::filesystem::path cache_path =
      kernel_state()->emulator()->cache_root() / "content_files" /
      fmt::format("{:08X}", kernel_state()->title_id()) /
      xe::to_path(entry->name());
  std::error_code error;
  if (std::filesystem::is_regular_file(cache_path, error) &&
      std::filesystem::file_size(cache_path, error) == entry->size()) {
    return cache_path;
  }

  vfs::File* file = nullptr;
  if (entry->Open(vfs::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS ||
      !file) {
    return {};
  }
  std::vector<uint8_t> data(entry->size());
  size_t bytes_read = 0;
  const X_STATUS status = file->ReadSync(data, 0, &bytes_read);
  file->Destroy();
  if (status != X_STATUS_SUCCESS || bytes_read != data.size()) {
    return {};
  }
  std::filesystem::create_directories(cache_path.parent_path(), error);
  std::ofstream out(cache_path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), data.size());
  if (!out) {
    return {};
  }
  return cache_path;
}

dword_result_t XamContentOpenFile_entry(
    dword_t user_index, lpstring_t root_name, lpstring_t path, dword_t flags,
    lpdword_t disposition_ptr, lpdword_t license_mask_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // Mounts the content package stored in a file, such as a Kinect speech
  // database (GAME:\nuisp1033p), under root_name.
  // TODO(gibbed): arguments assumed based on XamContentCreate.
  auto run = [path = path.value(), root_name = root_name.value(),
              disposition_ptr, license_mask_ptr](uint32_t& extended_error,
                                                 uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_FILE_NOT_FOUND;
    if (auto entry = kernel_state()->file_system()->ResolvePath(path)) {
      const std::filesystem::path host_path = GetContentFileHostPath(entry);
      if (!host_path.empty()) {
        result = kernel_state()->content_manager()->OpenContentFile(root_name,
                                                                    host_path);
      }
    }
    auto disposition = XSUCCEEDED(result) ? kDispositionState::Open
                                          : kDispositionState::Unknown;
    if (disposition_ptr) {
      *disposition_ptr = static_cast<uint32_t>(disposition);
    }
    if (license_mask_ptr && XSUCCEEDED(result)) {
      *license_mask_ptr = 0;
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  }
  kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
  return X_ERROR_IO_PENDING;
}
DECLARE_XAM_EXPORT1(XamContentOpenFile, kContent, kSketchy);

dword_result_t XamContentFlush_entry(lpstring_t root_name,
                                     pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}
DECLARE_XAM_EXPORT1(XamContentFlush, kContent, kStub);

dword_result_t XamContentClose_entry(lpstring_t root_name,
                                     pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // Closes a previously opened root from XamContentCreate*.
  return CompleteRequest(
      overlapped_ptr,
      kernel_state()->content_manager()->CloseContent(root_name.value()));
}
DECLARE_XAM_EXPORT1(XamContentClose, kContent, kImplemented);

dword_result_t XamContentGetCreator_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpdword_t is_creator_ptr,
    lpqword_t creator_xuid_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!is_creator_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // xam clears it before anything can fail.
  *is_creator_ptr = 0;

  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  auto run = [content_data, user_index, is_creator_ptr, creator_xuid_ptr,
              overlapped_ptr](uint32_t& extended_error,
                              uint32_t& length) -> X_RESULT {
    uint64_t xuid = 0;
    X_RESULT result = ContentXuid(user_index, xuid);
    if (!result) {
      uint64_t creator = 0;
      auto content_manager = kernel_state()->content_manager();
      result = content_manager->CheckPackageName(xuid, content_data);
      if (!result && !content_manager->ContentExists(xuid, content_data)) {
        result = X_ERROR_PATH_NOT_FOUND;
      }
      if (!result) {
        // User always creates saves and shared content is XUID 0's.
        if (content_data.content_type == XContentType::kSavedGame &&
            !(uint8_t(content_data.file_name_raw[0]) &
              kSharedContentFileNameBit)) {
          creator = xuid;
        }
        if (creator_xuid_ptr) {
          *creator_xuid_ptr = creator;
        }
      }
      // xam compares even when the content is missing.
      *is_creator_ptr = creator == xuid;
    }

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = 0;

    if (result && overlapped_ptr) {
      result = X_ERROR_FUNCTION_FAILED;
    }

    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    kernel_state()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  }
}
DECLARE_XAM_EXPORT1(XamContentGetCreator, kContent, kImplemented);

dword_result_t XamContentGetThumbnail_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpvoid_t buffer_ptr,
    lpdword_t buffer_size_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  assert_not_null(buffer_size_ptr);
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  uint64_t xuid = 0;
  X_RESULT result = ContentXuid(user_index, xuid);
  std::vector<uint8_t> buffer;
  if (!result) {
    result = kernel_state()->content_manager()->GetContentThumbnail(
        xuid, content_data, &buffer);
  }
  if (!result) {
    const uint32_t buffer_size = *buffer_size_ptr;
    if (!buffer_ptr) {
      // Only a query for the size gets it back.
      *buffer_size_ptr = uint32_t(buffer.size());
    } else if (buffer_size < buffer.size()) {
      result = X_ERROR_INSUFFICIENT_BUFFER;
    } else {
      std::memcpy((uint8_t*)buffer_ptr, buffer.data(), buffer.size());
    }
  }
  return CompleteRequest(overlapped_ptr, result);
}
DECLARE_XAM_EXPORT1(XamContentGetThumbnail, kContent, kImplemented);

dword_result_t XamContentSetThumbnail_entry(
    dword_t user_index, lpvoid_t content_data_ptr, lpvoid_t buffer_ptr,
    dword_t buffer_size, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  uint64_t xuid = 0;
  X_RESULT result = ContentXuid(user_index, xuid);
  if (!result) {
    // Buffer is PNG data.
    auto buffer = std::vector<uint8_t>((uint8_t*)buffer_ptr,
                                       (uint8_t*)buffer_ptr + buffer_size);
    result = kernel_state()->content_manager()->SetContentThumbnail(
        xuid, content_data, std::move(buffer));
  }
  return CompleteRequest(overlapped_ptr, result);
}
DECLARE_XAM_EXPORT1(XamContentSetThumbnail, kContent, kImplemented);

dword_result_t xeXamContentDelete(dword_t user_index, lpvoid_t content_data_ptr,
                                  dword_t content_data_size,
                                  pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  if (content_data_size == sizeof(XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
  }

  uint64_t xuid = 0;
  X_RESULT result = ContentXuid(user_index, xuid);
  if (!result) {
    result =
        kernel_state()->content_manager()->DeleteContent(xuid, content_data);
  }
  return CompleteRequest(overlapped_ptr, result);
}

dword_result_t XamContentDelete_entry(
    dword_t user_index, lpvoid_t content_data_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return xeXamContentDelete(user_index, content_data_ptr, sizeof(XCONTENT_DATA),
                            overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentDelete, kContent, kImplemented);

dword_result_t XamContentDeleteInternal_entry(
    lpvoid_t content_data_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // INFO: Analysis of xam.xex shows that "internal" functions are wrappers with
  // 0xFE as user_index.
  // In XAM content size is set to 0x200.
  return xeXamContentDelete(XUserIndexNone, content_data_ptr,
                            sizeof(XCONTENT_AGGREGATE_DATA), overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamContentDeleteInternal, kContent, kImplemented);

typedef struct {
  xe::be<uint32_t> stringTitlePtr;
  xe::be<uint32_t> stringTextPtr;
  xe::be<uint32_t> stringBtnMsgPtr;
} X_SWAPDISC_ERROR_MESSAGE;
static_assert_size(X_SWAPDISC_ERROR_MESSAGE, 12);

dword_result_t XamSwapDisc_entry(
    dword_t disc_number, pointer_t<X_KEVENT> completion_handle,
    pointer_t<X_SWAPDISC_ERROR_MESSAGE> error_message) {
  xex2_opt_execution_info* info = nullptr;
  kernel_state()->GetExecutableModule()->GetOptHeader(XEX_HEADER_EXECUTION_INFO,
                                                      &info);

  // Validate the requested disc number
  if (disc_number < 1 || disc_number > info->disc_count) {
    XELOGE("XamSwapDisc: Invalid disc number {} (valid range: 1-{})",
           uint32_t(disc_number), uint8_t(info->disc_count));
    return X_ERROR_INVALID_PARAMETER;
  }

  auto completion_event = [completion_handle]() -> void {
    auto kevent = xboxkrnl::xeKeSetEvent(completion_handle, 1, 0);

    // Release the completion handle
    auto object = XObject::GetNativeObject<XEvent>(
        kernel_state(), completion_handle, completion_handle->header.type);
    if (object) {
      object->Retain();
    }
  };

  if (kernel_state()->emulator()->current_disc_number() == disc_number) {
    completion_event();
    return X_ERROR_SUCCESS;
  }

  auto filesystem = kernel_state()->file_system();
  // Mount to Cdrom0 so the game: symlink points to the new disc
  auto mount_path = "\\Device\\Cdrom0";

  // Unmount the current disc
  if (filesystem->ResolvePath(mount_path) != NULL) {
    filesystem->UnregisterDevice(mount_path);
  }

  std::u16string text_message = xe::load_and_swap<std::u16string>(
      kernel_state()->memory()->TranslateVirtual(error_message->stringTextPtr));

  std::string error_dialog_message;

  // Loop until user provides the correct disc
  while (true) {
    const std::filesystem::path new_disc_path =
        kernel_state()->emulator()->GetNewDiscPath(
            xe::to_utf8(text_message) + "\n\n" + error_dialog_message);
    XELOGI("XamSwapDisc: GetNewDiscPath returned path {}.",
           xe::strip_pii(xe::path_to_utf8(new_disc_path)));

    // Clear the error message for next iteration
    error_dialog_message.clear();

    // Check if user cancelled the disc selection - don't proceed with wrong
    // disc
    if (new_disc_path.empty()) {
      XELOGI("XamSwapDisc: User cancelled disc selection, prompting again...");
      error_dialog_message =
          "ERROR: Disc swap cancelled.\n\n"
          "The game requires the correct disc to continue.\n"
          "Please select the requested disc.";
      continue;  // Ask again
    }

    // Mount the new disc
    auto mount_result =
        kernel_state()->emulator()->MountPath(new_disc_path, mount_path);
    if (mount_result != X_ERROR_SUCCESS) {
      XELOGE("XamSwapDisc: Failed to mount disc at path: {}",
             xe::strip_pii(xe::path_to_utf8(new_disc_path)));
      error_dialog_message =
          "ERROR: Failed to mount the selected disc image.\n"
          "Please select a valid disc image file.";
      continue;  // Ask again
    }

    // Validate the mounted disc
    auto* device = filesystem->GetDevice(mount_path);
    if (!device) {
      XELOGE("XamSwapDisc: Failed to get mounted device");
      filesystem->UnregisterDevice(mount_path);
      error_dialog_message =
          "ERROR: Failed to access the mounted disc.\n"
          "Please try again with a different file.";
      continue;  // Ask again
    }

    // Try to cast to XContentContainerDevice to access disc metadata
    auto* container_device =
        dynamic_cast<vfs::XContentContainerDevice*>(device);
    if (container_device) {
      const auto* header = container_device->GetContainerHeader();
      if (header) {
        const auto& exec_info = header->content_metadata.execution_info;

        // Validate disc number matches what was requested
        if (exec_info.disc_number != disc_number) {
          XELOGE(
              "XamSwapDisc: Disc number mismatch! Requested disc {}, but "
              "mounted disc is disc {}.",
              uint32_t(disc_number), uint8_t(exec_info.disc_number));
          filesystem->UnregisterDevice(mount_path);
          error_dialog_message = fmt::format(
              "ERROR: Wrong Disc!\n\n"
              "Requested: Disc {}\n"
              "Inserted: Disc {}\n\n"
              "Please insert the correct disc.",
              uint32_t(disc_number), uint8_t(exec_info.disc_number));
          continue;  // Ask again
        }

        // Validate title ID matches
        if (exec_info.title_id != info->title_id) {
          XELOGE(
              "XamSwapDisc: Title ID mismatch! Expected {:08X}, but mounted "
              "disc has {:08X}.",
              uint32_t(info->title_id), uint32_t(exec_info.title_id));
          filesystem->UnregisterDevice(mount_path);
          error_dialog_message = fmt::format(
              "ERROR: Wrong Game!\n\n"
              "This disc does not belong to the current game.\n\n"
              "Expected Title ID: {:08X}\n"
              "Inserted Title ID: {:08X}\n\n"
              "Please insert the correct disc.",
              uint32_t(info->title_id), uint32_t(exec_info.title_id));
          continue;  // Ask again
        }

        // Validate disc count matches
        if (exec_info.disc_count != info->disc_count) {
          XELOGW(
              "XamSwapDisc: Disc count mismatch! Expected {} discs, but "
              "mounted disc indicates {} discs",
              uint8_t(info->disc_count), uint8_t(exec_info.disc_count));
        }

        XELOGI(
            "XamSwapDisc: Successfully validated and mounted disc {} of {} "
            "(Title ID: {:08X}, Media ID: {:08X})",
            uint8_t(exec_info.disc_number), uint8_t(exec_info.disc_count),
            uint32_t(exec_info.title_id), uint32_t(exec_info.media_id));

        kernel_state()->emulator()->set_current_disc_number(
            exec_info.disc_number);

        kernel_state()->emulator()->RecordDisc(info->title_id, new_disc_path);

        // Update the host_path in loader_data so title restarts use the new
        // disc
        auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
        if (xam) {
          xam->loader_data().host_path = xe::path_to_utf8(new_disc_path);
        }

        // Notify UI of disc swap for title bar update
        auto on_disc_swap = kernel_state()->emulator()->on_disc_swap();
        if (on_disc_swap) {
          on_disc_swap(exec_info.disc_number);
        }

        // Success - break out of the loop
        break;
      }
    } else {
      XELOGW(
          "XamSwapDisc: Mounted device is not an XContentContainerDevice, "
          "skipping validation");

      // Update the host_path in loader_data so title restarts use the new disc
      auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
      if (xam) {
        xam->loader_data().host_path = xe::path_to_utf8(new_disc_path);
      }

      kernel_state()->emulator()->set_current_disc_number(
          static_cast<uint8_t>(disc_number));

      // For non-container devices, accept them (backward compatibility)
      break;
    }
  }

  completion_event();
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamSwapDisc, kContent, kSketchy);

dword_result_t XamLoaderGetDvdTrayState_entry() {
  return static_cast<uint8_t>(kernel_state()->smc()->GetTrayState());
}
DECLARE_XAM_EXPORT1(XamLoaderGetDvdTrayState, kNone, kImplemented);

void XamLoaderGetMediaInfoEx_entry(lpdword_t media_type, lpdword_t title_id,
                                   lpdword_t unk3) {
  if (media_type) {
    *media_type = X_DVD_DISC_STATE::XBOX_360_GAME_DISC;
  }
  if (title_id) {
    *title_id = 0;
  }
  if (unk3) {
    *unk3 = 0;
  }
}
DECLARE_XAM_EXPORT1(XamLoaderGetMediaInfoEx, kNone, kStub);

void XamLoaderGetMediaInfo_entry(lpdword_t media_type, lpdword_t title_id) {
  XamLoaderGetMediaInfoEx_entry(media_type, title_id, 0);
}
DECLARE_XAM_EXPORT1(XamLoaderGetMediaInfo, kNone, kStub);

dword_result_t XamContentLaunchImageFromFileInternal_entry(
    lpstring_t image_location, lpstring_t xex_name, dword_t unk) {
  const std::string image_path = static_cast<std::string>(image_location);
  const std::string xex_name_ = static_cast<std::string>(xex_name);

  vfs::Entry* entry = kernel_state()->file_system()->ResolvePath(image_path);

  if (!entry) {
    return X_STATUS_NO_SUCH_FILE;
  }

  const std::filesystem::path host_path =
      kernel_state()->emulator()->content_root() / entry->name();
  if (!std::filesystem::exists(host_path)) {
    std::atomic<uint64_t> progress{0};

    vfs::VirtualFileSystem::ExtractContentFile(
        entry, kernel_state()->emulator()->content_root(), progress, true);
  }

  // In-process relaunch is disabled on macOS — see XamLoaderLaunchTitle.
#if !XE_PLATFORM_MAC
  if (cvars::in_process_title_relaunch) {
    auto* emulator = kernel_state()->emulator();
    auto host_path_utf8 = xe::path_to_utf8(host_path);
    std::thread([emulator, host_path_utf8, module = xex_name_]() mutable {
      emulator->RelaunchTitle(host_path_utf8, module, 0, {});
    }).detach();
    // Park off guest code until RelaunchTitle terminates us (Suspend can return
    // on POSIX).
    XThread::GetCurrentThread()->Suspend(nullptr);
    while (true) {
      xe::threading::NanoSleep(int64_t(1'000'000'000));
    }
  }
#endif  // !XE_PLATFORM_MAC

  auto on_launch_new_title = kernel_state()->emulator()->on_launch_new_title();
  if (on_launch_new_title) {
    XELOGI("XamContentLaunchImageFromFileInternal: spawning new title process");
    on_launch_new_title(xe::path_to_utf8(host_path), xex_name_, 0, "", "");
  }

  kernel_state()->TerminateTitle();
  return X_ERROR_SUCCESS;
}

DECLARE_XAM_EXPORT1(XamContentLaunchImageFromFileInternal, kContent, kStub);

dword_result_t XamContentLaunchImageInternal_entry(lpvoid_t content_data_ptr,
                                                   lpstring_t xex_path) {
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // title_id is written into first 8 characters of filename
  const uint32_t title_id = xe::string_util::from_string<uint32_t>(
      content_data.file_name().substr(0, 8), true);

  // This should be done via content_manager, however as it isn't capable of
  // such action we need to improvise.
  const std::string package_path =
      fmt::format("{}/Content/0000000000000000/{:08X}/{:08X}/{}",
                  kernel_state()->title_mount_path_, title_id,
                  static_cast<uint32_t>(content_data.content_type.get()),
                  content_data.file_name());

  auto entry = kernel_state()->file_system()->ResolvePath(package_path);

  if (!entry) {
    return X_STATUS_NO_SUCH_FILE;
  }

  const std::filesystem::path host_path =
      kernel_state()->emulator()->content_root() / entry->name();

  if (!std::filesystem::exists(host_path)) {
    std::atomic<uint64_t> progress{0};
    kernel_state()->file_system()->ExtractContentFile(
        entry, kernel_state()->emulator()->content_root(), progress, true);
  }

  // In-process relaunch is disabled on macOS — see XamLoaderLaunchTitle.
#if !XE_PLATFORM_MAC
  if (cvars::in_process_title_relaunch) {
    auto* emulator = kernel_state()->emulator();
    auto host_path_utf8 = xe::path_to_utf8(host_path);
    std::thread([emulator, host_path_utf8,
                 module = xex_path.value()]() mutable {
      emulator->RelaunchTitle(host_path_utf8, module, 0, {});
    }).detach();
    // Park off guest code until RelaunchTitle terminates us (Suspend can return
    // on POSIX).
    XThread::GetCurrentThread()->Suspend(nullptr);
    while (true) {
      xe::threading::NanoSleep(int64_t(1'000'000'000));
    }
  }
#endif  // !XE_PLATFORM_MAC

  auto on_launch_new_title = kernel_state()->emulator()->on_launch_new_title();
  if (on_launch_new_title) {
    XELOGI("XamContentLaunchImageInternal: spawning new title process");
    on_launch_new_title(xe::path_to_utf8(host_path), xex_path.value(), 0, "",
                        "");
  }

  kernel_state()->TerminateTitle();
  return X_ERROR_SUCCESS;
}

DECLARE_XAM_EXPORT1(XamContentLaunchImageInternal, kContent, kStub);

void XamContentRegisterChangeCallback_entry(dword_t callback) {
  kernel_state()->xam_state()->SetContentRegisterCallback(callback);
}
DECLARE_XAM_EXPORT1(XamContentRegisterChangeCallback, kContent, kImplemented);

dword_result_t XamContentGetDeviceVolumePath_entry(dword_t device_id,
                                                   lpvoid_t path_ptr,
                                                   dword_t path_size,
                                                   dword_t append_backslash) {
  std::string volume_path = "hdd0\\";
  if (device_id != static_cast<uint32_t>(DummyDeviceId::HDD)) {
    return X_ERROR_FUNCTION_FAILED;
  }

  char* path =
      kernel_memory()->TranslateVirtual<char*>(path_ptr.guest_address());

  string_util::copy_truncating(path, volume_path, path_size);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamContentGetDeviceVolumePath, kContent, kStub);

dword_result_t XamContentCopyInternal_entry(
    lpqword_t xuid_ptr, lpdword_t device_id_ptr, lpvoid_t content_data_ptr,
    dword_t unk1, dword_t unk2, dword_t unk3,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  // xam has the dashboard copy the package to another owner or device. There
  // is no dashboard to do it, so this fails the way xam does when its request
  // to the dashboard fails, before touching the overlapped.
  if (!xuid_ptr && !device_id_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (!content_data_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamContentCopyInternal, kContent, kStub);

dword_result_t XamContentExistsOnDeviceInternal_entry(
    pointer_t<XCONTENT_AGGREGATE_DATA> content_data_ptr, dword_t device_id) {
  if (!content_data_ptr) {
    return 0;
  }
  // Looks for the package on device_id instead of the one it names.
  const XCONTENT_AGGREGATE_DATA* source = content_data_ptr;
  XCONTENT_AGGREGATE_DATA content_data = *source;
  content_data.device_id = device_id.value();
  if (!GetDummyDeviceInfo(device_id)) {
    return 0;
  }
  return kernel_state()->content_manager()->ContentExists(content_data.xuid,
                                                          content_data);
}
DECLARE_XAM_EXPORT1(XamContentExistsOnDeviceInternal, kContent, kImplemented);

// XamCache* keep files on the system cache partition, each described by a
// 0x248 byte block: flags at 0x0 (bit 0 creates), the cache type at 0x4, an id
// at 0x8, a 64-bit key at 0x10 and the open handle at 0x24. Nothing in xenia
// stores into this cache, so no file can be found in it.
constexpr uint32_t kCacheFileFlagCreate = 0x1;
constexpr uint32_t kCacheFileHandleOffset = 0x24;
constexpr uint32_t kCacheFileNameOffset = 0x28;

dword_result_t XamCacheOpenFile_entry(lpvoid_t cache_file_ptr) {
  if (!cache_file_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto cache_file = cache_file_ptr.as<uint8_t*>();
  // xam resets the handle and name before it looks for the file.
  xe::store_and_swap<uint32_t>(cache_file + kCacheFileHandleOffset, 0xFFFFFFFF);
  cache_file[kCacheFileNameOffset] = 0;
  uint32_t flags = xe::load_and_swap<uint32_t>(cache_file);
  if (flags & kCacheFileFlagCreate) {
    XELOGW("XamCacheOpenFile: creating cache type {:08X} files is unsupported",
           xe::load_and_swap<uint32_t>(cache_file + 4));
    return X_ERROR_NOT_SUPPORTED;
  }
  return X_ERROR_FILE_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamCacheOpenFile, kContent, kStub);

dword_result_t XamCacheCloseFile_entry(lpvoid_t cache_file_ptr,
                                       dword_t delete_file) {
  if (!cache_file_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  // XamCacheOpenFile never opens one, so there is nothing to close.
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheCloseFile, kContent, kStub);

dword_result_t XamCacheReset_entry(dword_t cache_type) {
  // The cache is always empty.
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamCacheReset, kContent, kStub);

// XamBackgroundDownload* drive the dashboard's download queue. Its items are
// 0x380 bytes. There is no dashboard or marketplace, so the queue stays empty
// and requests that would change it fail.
constexpr uint32_t kBackgroundDownloadItemSize = 0x380;
static std::atomic<uint32_t> background_download_mode_ = 0;

static X_HRESULT BackgroundDownloadQueueRequest(
    lpvoid_t item_ptr, pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  if (!item_ptr) {
    return X_E_INVALIDARG;
  }
  // xam fails before it touches the overlapped when the dashboard can't take
  // the request.
  return X_E_FAIL;
}

dword_result_t XamBackgroundDownloadSetMode_entry(dword_t mode) {
  background_download_mode_ = mode;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadSetMode, kContent, kImplemented);

dword_result_t XamBackgroundDownloadGetMode_entry() {
  return background_download_mode_.load();
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadGetMode, kContent, kImplemented);

dword_result_t XamBackgroundDownloadGetActiveItem_entry(lpdword_t active_ptr,
                                                        lpvoid_t item_ptr) {
  // Nothing is downloading.
  if (active_ptr && item_ptr) {
    *active_ptr = 0;
    item_ptr.Zero(kBackgroundDownloadItemSize);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadGetActiveItem, kContent, kImplemented);

dword_result_t XamBackgroundDownloadItemAdd_entry(
    dword_t user_index, lpvoid_t item_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return BackgroundDownloadQueueRequest(item_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemAdd, kContent, kStub);

dword_result_t XamBackgroundDownloadItemMakeFirst_entry(
    dword_t user_index, lpvoid_t item_ptr, dword_t flags,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return BackgroundDownloadQueueRequest(item_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemMakeFirst, kContent, kStub);

dword_result_t XamBackgroundDownloadBindItems_entry(
    dword_t user_index, lpvoid_t item_ptr, dword_t flags,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  return BackgroundDownloadQueueRequest(item_ptr, overlapped_ptr);
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadBindItems, kContent, kStub);

dword_result_t XamBackgroundDownloadItemGetHistoryStatus_entry(
    dword_t user_index, lpvoid_t item_ptr, lpdword_t status_ptr) {
  // The history is empty.
  return X_ERROR_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemGetHistoryStatus, kContent,
                    kImplemented);

dword_result_t XamBackgroundDownloadItemGetHistoryStatusEx_entry(
    dword_t user_index, lpvoid_t item_key_ptr, lpdword_t status_ptr) {
  return X_ERROR_NOT_FOUND;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemGetHistoryStatusEx, kContent,
                    kImplemented);

dword_result_t XamBackgroundDownloadItemGetStatus_entry(
    dword_t user_index, lpvoid_t item_ptr, dword_t check_content,
    dword_t check_active, lpdword_t is_active_ptr, lpdword_t is_queued_ptr,
    lpdword_t in_history_ptr) {
  // The item is neither downloading, queued nor in the history.
  if (is_active_ptr) {
    *is_active_ptr = 0;
  }
  if (is_queued_ptr) {
    *is_queued_ptr = 0;
  }
  if (in_history_ptr) {
    *in_history_ptr = 0;
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemGetStatus, kContent, kSketchy);

dword_result_t XamBackgroundDownloadSelectDevice_entry(
    dword_t device_id, qword_t required_bytes, lpdword_t device_id_ptr) {
  if (!device_id_ptr) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Prefers the requested device, else takes the first with enough room.
  const uint64_t required = required_bytes;
  const uint32_t preferred = device_id;
  uint32_t selected = 0;
  for (const auto* device : ListStorageDevices()) {
    if (device->free_bytes < required) {
      continue;
    }
    uint32_t id = static_cast<uint32_t>(device->device_id);
    if (!selected || id == preferred) {
      selected = id;
    }
  }
  if (!selected) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  *device_id_ptr = selected;
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadSelectDevice, kContent, kSketchy);

dword_result_t XamBackgroundDownloadItemToContentData_entry(
    lpvoid_t item_ptr, dword_t use_device, lpvoid_t content_data_ptr) {
  // Reading an offer item into package data needs the item layout, which is
  // unknown. Report that it couldn't be converted.
  return 0;
}
DECLARE_XAM_EXPORT1(XamBackgroundDownloadItemToContentData, kContent, kStub);

// XamXlfs* run the queue that uploads files to Xbox LIVE file storage. There is
// no LIVE storage to upload to, so the queue can't be started.
dword_result_t XamXlfsInitializeUploadQueue_entry(lpvoid_t params_ptr,
                                                  lpdword_t queue_handle_ptr) {
  if (queue_handle_ptr) {
    *queue_handle_ptr = 0;
  }
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamXlfsInitializeUploadQueue, kContent, kStub);

dword_result_t XamXlfsUninitializeUploadQueue_entry(dword_t queue_handle,
                                                    lpvoid_t params_ptr) {
  return X_HRESULT_FROM_WIN32(X_ERROR_INVALID_HANDLE);
}
DECLARE_XAM_EXPORT1(XamXlfsUninitializeUploadQueue, kContent, kStub);

dword_result_t XamXlfsMountUploadQueueInstance_entry(dword_t queue_handle,
                                                     lpvoid_t params_ptr,
                                                     lpvoid_t mount_name_ptr,
                                                     lpvoid_t unk_ptr) {
  return X_HRESULT_FROM_WIN32(X_ERROR_INVALID_HANDLE);
}
DECLARE_XAM_EXPORT1(XamXlfsMountUploadQueueInstance, kContent, kStub);

dword_result_t XamXlfsUnmountUploadQueueInstance_entry(dword_t queue_handle,
                                                       lpvoid_t mount_name_ptr,
                                                       lpvoid_t params_ptr) {
  return X_HRESULT_FROM_WIN32(X_ERROR_INVALID_HANDLE);
}
DECLARE_XAM_EXPORT1(XamXlfsUnmountUploadQueueInstance, kContent, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Content);
