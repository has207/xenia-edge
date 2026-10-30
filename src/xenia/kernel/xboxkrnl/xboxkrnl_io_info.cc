/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/info/volume.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/device.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

uint32_t GetQueryFileInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case XFileInternalInformation:
      return sizeof(X_FILE_INTERNAL_INFORMATION);
    case XFilePositionInformation:
      return sizeof(X_FILE_POSITION_INFORMATION);
    case XFileXctdCompressionInformation:
      return sizeof(X_FILE_XCTD_COMPRESSION_INFORMATION);
    case XFileNetworkOpenInformation:
      return sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
    // TODO(gibbed): structures to get the size of.
    case XFileModeInformation:
    case XFileAlignmentInformation:
    case XFileSectorInformation:
    case XFileIoPriorityInformation:
      return 4;
    case XFileNameInformation:
    case XFileAllocationInformation:
      return 8;
    case XFileBasicInformation:
      return 40;
    default:
      return 0;
  }
}

dword_result_t NtQueryInformationFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block_ptr,
    lpvoid_t info_ptr, dword_t info_length, dword_t info_class) {
  uint32_t minimum_length = GetQueryFileInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t out_length;

  switch (info_class) {
    case XFileInternalInformation: {
      // Internal unique file pointer. Not sure why anyone would want this.
      // TODO(benvanik): use pointer to fs::entry?
      auto info = info_ptr.as<X_FILE_INTERNAL_INFORMATION*>();
      info->index_number = xe::memory::hash_combine(0, file->path());
      out_length = sizeof(*info);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.as<X_FILE_POSITION_INFORMATION*>();
      info->current_byte_offset = file->position();
      out_length = sizeof(*info);
      break;
    }
    case XFileAlignmentInformation: {
      // Requested by XMountUtilityDrive XAM-task
      auto info = info_ptr.as<uint32_t*>();
      *info = 0;  // FILE_BYTE_ALIGNMENT?
      out_length = sizeof(*info);
      break;
    }
    case XFileSectorInformation: {
      // SW that uses this seems to use the output as a way of uniquely
      // identifying a file for sorting/lookup so we can just give it an
      // arbitrary 4 byte integer most of the time
      XELOGW("Stub XFileSectorInformation!");
      auto info = info_ptr.as<uint32_t*>();
      size_t fname_hash = xe::memory::hash_combine(82589933LL, file->path());
      *info = static_cast<uint32_t>(fname_hash ^ (fname_hash >> 32));
      out_length = sizeof(uint32_t);
      break;
    }
    case XFileXctdCompressionInformation: {
      XELOGE(
          "NtQueryInformationFile(XFileXctdCompressionInformation) "
          "unimplemented");
      // Files that are XCTD compressed begin with the magic 0x0FF512ED but we
      // shouldn't detect this that way. There's probably a flag somewhere
      // (attributes?) that defines if it's compressed or not.
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    };
    case XFileNetworkOpenInformation: {
      // Make sure we're working with up-to-date information, just in case the
      // file size has changed via something other than NtSetInfoFile
      // (eg. seems NtWriteFile might extend the file in some cases). The host
      // lookup can block on a slow drive.
      kernel_state()->RunBlockingIo(
          [&]() { file->entry()->update(); },
          GuestScheduler::BlockingCallClass::kConcurrent);

      auto info = info_ptr.as<X_FILE_NETWORK_OPEN_INFORMATION*>();
      info->creation_time = file->entry()->create_timestamp();
      info->last_access_time = file->entry()->access_timestamp();
      info->last_write_time = file->entry()->write_timestamp();
      info->change_time = file->entry()->write_timestamp();
      info->allocation_size = file->entry()->allocation_size();
      info->end_of_file = file->entry()->size();
      info->attributes = file->entry()->attributes();
      out_length = sizeof(*info);
      break;
    }
    case XFileBasicInformation:
      // The file system takes it to set but not to query.
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    default: {
      // Unsupported, for now.
      assert_always();
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    }
  }

  // A failed request leaves the status block alone.
  if (io_status_block_ptr && XSUCCEEDED(status)) {
    io_status_block_ptr->status = status;
    io_status_block_ptr->information = out_length;
  }

  return status;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryInformationFile, kFileSystem, kImplemented);

uint32_t GetSetFileInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case XFileRenameInformation:
      return sizeof(X_FILE_RENAME_INFORMATION);
    case XFileDispositionInformation:
      return sizeof(X_FILE_DISPOSITION_INFORMATION);
    case XFilePositionInformation:
      return sizeof(X_FILE_POSITION_INFORMATION);
    case XFileCompletionInformation:
      return sizeof(X_FILE_COMPLETION_INFORMATION);
    case XFileAllocationInformation:
      return sizeof(X_FILE_ALLOCATION_INFORMATION);
    case XFileEndOfFileInformation:
      return sizeof(X_FILE_END_OF_FILE_INFORMATION);
    // TODO(gibbed): structures to get the size of.
    case XFileModeInformation:
    case XFileIoPriorityInformation:
      return 4;
    case XFileMountPartitionInformation:
      return 8;
    case XFileLinkInformation:
      return 16;
    case XFileBasicInformation:
      return 40;
    case XFileMountPartitionsInformation:
      return 152;
    default:
      return 0;
  }
}

dword_result_t NtSetInformationFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    lpvoid_t info_ptr, dword_t info_length, dword_t info_class) {
  uint32_t minimum_length = GetSetFileInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }

  X_STATUS result = X_STATUS_SUCCESS;

  switch (info_class) {
    case XFileBasicInformation: {
      auto info = info_ptr.as<X_FILE_BASIC_INFORMATION*>();
      // Of the attributes only the directory bit is kept, which a file can't
      // take.
      if ((info->attributes & vfs::kFileAttributeDirectory) &&
          !(file->entry()->attributes() & vfs::kFileAttributeDirectory)) {
        result = X_STATUS_INVALID_PARAMETER;
        break;
      }

      // The change time follows the write time.
      if (info->creation_time || info->last_access_time ||
          info->last_write_time) {
        result = file->SetTimes(info->creation_time, info->last_access_time,
                                info->last_write_time);
      }
      break;
    }
    case XFileRenameInformation: {
      auto info = info_ptr.as<X_FILE_RENAME_INFORMATION*>();
      // Compute path, possibly attrs relative.
      auto target_name =
          util::TranslateAnsiPath(kernel_memory(), &info->ansi_string);
      xeObStripDosDevicesPrefix(target_name);
      std::filesystem::path target_path = target_name;

      // Place IsValidPath in path from where it can be accessed everywhere
      if (!IsValidPath(xe::path_to_utf8(target_path), false)) {
        return X_STATUS_OBJECT_NAME_INVALID;
      }

      if (!target_path.has_filename()) {
        return X_STATUS_INVALID_PARAMETER;
      }

      result = file->Rename(target_path, info->replace_existing != 0);
      break;
    }
    case XFileDispositionInformation: {
      auto info = info_ptr.as<X_FILE_DISPOSITION_INFORMATION*>();
      bool delete_on_close = info->delete_file ? true : false;
      file->entry()->SetForDeletion(static_cast<bool>(info->delete_file));
      XELOGW("NtSetInformationFile set deleting flag for {} on close to: {}",
             file->name(), delete_on_close);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.as<X_FILE_POSITION_INFORMATION*>();
      file->set_position(info->current_byte_offset);
      break;
    }
    case XFileAllocationInformation: {
      auto info = info_ptr.as<X_FILE_ALLOCATION_INFORMATION*>();
      if (!(file->file_access() &
            (xe::filesystem::FileAccess::kGenericWrite |
             xe::filesystem::FileAccess::kFileWriteData))) {
        result = X_STATUS_ACCESS_DENIED;
        break;
      }
      // The allocation rounds up to whole allocation units. A larger one
      // leaves the file size alone. A smaller one cuts the file to it.
      auto device = file->device();
      uint64_t allocation_size =
          xe::round_up(uint64_t(info->allocation_size),
                       uint64_t(file->entry()->sectors_per_allocation_unit()) *
                           device->bytes_per_sector(),
                       false);
      uint64_t file_size = 0;
      kernel_state()->RunBlockingIo(
          [&]() {
            file->entry()->update();
            file_size = file->entry()->size();
          },
          GuestScheduler::BlockingCallClass::kConcurrent);
      if (allocation_size < file_size) {
        result = file->SetLength(allocation_size);
        kernel_state()->RunBlockingIo(
            [&]() { file->entry()->update(); },
            GuestScheduler::BlockingCallClass::kConcurrent);
      }
      break;
    }
    case XFileEndOfFileInformation: {
      auto info = info_ptr.as<X_FILE_END_OF_FILE_INFORMATION*>();
      result = file->SetLength(info->end_of_file);

      // Update the files vfs::Entry information
      kernel_state()->RunBlockingIo(
          [&]() { file->entry()->update(); },
          GuestScheduler::BlockingCallClass::kConcurrent);
      break;
    }
    case XFileCompletionInformation: {
      // Info contains IO Completion handle and completion key
      auto info = info_ptr.as<X_FILE_COMPLETION_INFORMATION*>();
      auto handle = uint32_t(info->handle);
      auto key = uint32_t(info->key);
      auto port =
          kernel_state()->object_table()->LookupObject<XIOCompletion>(handle);
      // Only an async handle binds to a port. It binds once.
      if (file->is_synchronous() || file->HasCompletionPort()) {
        result = X_STATUS_INVALID_PARAMETER;
      } else if (!port) {
        result = X_STATUS_INVALID_HANDLE;
      } else if (!file->RegisterIOCompletionPort(key, port)) {
        result = X_STATUS_INVALID_PARAMETER;
      }
      break;
    }
    default:
      // Unsupported, for now.
      assert_always();
      break;
  }

  // A failed request leaves the status block alone.
  if (io_status_block && XSUCCEEDED(result)) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtSetInformationFile, kFileSystem, kImplemented,
                         kHighFrequency);

uint32_t GetQueryVolumeInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case XFileFsVolumeInformation:
      return sizeof(X_FILE_FS_VOLUME_INFORMATION);
    case XFileFsSizeInformation:
      return sizeof(X_FILE_FS_SIZE_INFORMATION);
    case XFileFsDeviceInformation:
      return sizeof(X_FILE_FS_DEVICE_INFORMATION);
    case XFileFsAttributeInformation:
      return sizeof(X_FILE_FS_ATTRIBUTE_INFORMATION);
    // TODO(gibbed): structures to get the size of.
    default:
      XELOGW("Unimplemented Info Class: 0x{:08x}", info_class);
      return 0;
  }
}

dword_result_t NtQueryVolumeInformationFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block_ptr,
    lpvoid_t info_ptr, dword_t info_length, dword_t info_class) {
  uint32_t minimum_length = GetQueryVolumeInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t out_length;

  switch (info_class) {
    case XFileFsVolumeInformation: {
      auto info = info_ptr.as<X_FILE_FS_VOLUME_INFORMATION*>();
      info->creation_time = 0;
      info->serial_number = 0;  // set for FATX, but we don't do that currently
      info->supports_objects = 0;
      info->label_length = 0;
      out_length = offsetof(X_FILE_FS_VOLUME_INFORMATION, label);
      break;
    }
    case XFileFsSizeInformation: {
      auto device = file->device();
      auto info = info_ptr.as<X_FILE_FS_SIZE_INFORMATION*>();
      info->total_allocation_units = device->total_allocation_units();
      info->available_allocation_units = device->available_allocation_units();
      info->sectors_per_allocation_unit =
          file->entry()->sectors_per_allocation_unit();
      info->bytes_per_sector = device->bytes_per_sector();
      // TODO(gibbed): sanity check, XCTD userland code seems to require this.
      assert_true(info->bytes_per_sector == 0x200);
      out_length = sizeof(*info);
      break;
    }
    case XFileFsAttributeInformation: {
      auto device = file->device();
      const auto& name = device->name();
      auto info = info_ptr.as<X_FILE_FS_ATTRIBUTE_INFORMATION*>();
      info->attributes = device->attributes();
      info->component_name_max_length = device->component_name_max_length();
      info->name_length = uint32_t(name.size());
      if (info_length >= 12 + name.size()) {
        std::memcpy(info->name, name.data(), name.size());
        out_length =
            offsetof(X_FILE_FS_ATTRIBUTE_INFORMATION, name) + info->name_length;
      } else {
        status = X_STATUS_BUFFER_OVERFLOW;
        out_length = offsetof(X_FILE_FS_ATTRIBUTE_INFORMATION, name);
      }
      break;
    }
    case XFileFsDeviceInformation: {
      auto info = info_ptr.as<X_FILE_FS_DEVICE_INFORMATION*>();
      auto file_device = file->device();
      XELOGW("Stub XFileFsDeviceInformation!");
      info->device_type =
          FILE_DEVICE_UNKNOWN;  // 415608D8 checks for FILE_DEVICE_EHSTOR;
      // 58480880 deletes a content package it reopens unless its volume
      // reports exactly 2. What the value means isn't known.
      const bool is_content_package = xe::utf8::starts_with(
          file_device->mount_path(), "\\Device\\Content\\");
      info->characteristics = is_content_package ? 2 : 0;
      out_length = sizeof(X_FILE_FS_DEVICE_INFORMATION);
      break;
    }
    default: {
      assert_always();
      out_length = 0;
      break;
    }
  }

  if (io_status_block_ptr) {
    io_status_block_ptr->status = status;
    io_status_block_ptr->information = out_length;
  }

  return status;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryVolumeInformationFile, kFileSystem,
                         kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(IoInfo);
