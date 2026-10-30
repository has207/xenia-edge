/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <charconv>
#include <functional>

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/utf8.h"
#include "xenia/emulator.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xiocompletion.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/vfs/device.h"
#include "xenia/xbox.h"

DECLARE_bool(apply_title_update);

namespace xe {
namespace kernel {
namespace xboxkrnl {

// Bit 0 only picks the APC mode, see QueueIoApc.
static bool QueuesApc(uint32_t apc_routine, uint32_t apc_context) {
  return (apc_routine & ~1u) && apc_context;
}

// The console queues an I/O completion routine as a kernel APC, which runs
// before the service returns on an inline completion. Bit 0 asks for a user APC
// instead, as XAPI's ReadFileEx completion trampoline does.
static void QueueIoApc(XThread* thread, uint32_t apc_routine,
                       uint32_t apc_context, uint32_t status_block_address) {
  thread->EnqueueApc(apc_routine & ~1u, apc_context, status_block_address, 0,
                     apc_routine & 1);
}

// Status last, so a caller polling it for completion reads a valid count.
static void WriteIoStatus(X_IO_STATUS_BLOCK* status_block, X_STATUS status,
                          uint32_t information) {
  status_block->information = information;
  std::atomic_thread_fence(std::memory_order_release);
  status_block->status = status;
}

// A handle bound to a completion port takes no APC routine. A synchronous
// handle uses its file position, marked -1, without an offset or with
// FILE_USE_FILE_POINTER_POSITION (-2). An async handle needs an offset. A write
// given FILE_WRITE_TO_END_OF_FILE (-1) appends at XFile::kEndOfFile. Any other
// negative offset is invalid.
static X_STATUS ValidateRequest(XFile* file, uint32_t apc_routine,
                                lpqword_t byte_offset_ptr, bool is_write,
                                uint64_t* out_byte_offset) {
  *out_byte_offset = uint64_t(-1);
  if (apc_routine && file->HasCompletionPort()) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!byte_offset_ptr) {
    return file->is_synchronous() ? X_STATUS_SUCCESS
                                  : X_STATUS_INVALID_PARAMETER;
  }
  int64_t byte_offset = int64_t(uint64_t(*byte_offset_ptr));
  if (byte_offset == -2 && file->is_synchronous()) {
    return X_STATUS_SUCCESS;
  }
  if (byte_offset == -1 && is_write) {
    *out_byte_offset = XFile::kEndOfFile;
    return X_STATUS_SUCCESS;
  }
  if (byte_offset < 0) {
    return X_STATUS_INVALID_PARAMETER;
  }
  *out_byte_offset = uint64_t(byte_offset);
  return X_STATUS_SUCCESS;
}

// Without a file system that queues every request, a zero-length transfer, a
// read at or past EOF, a write that extends the file and a buffered transfer
// complete inline.
static bool CompletesAsync(XFile* file, uint32_t length, uint64_t byte_offset,
                           bool is_write) {
  if (file->is_synchronous()) {
    return false;
  }
  if (file->device()->queues_all_requests()) {
    return true;
  }
  if (file->is_buffered() || !length) {
    return false;
  }
  uint64_t file_size = file->entry()->size();
  return is_write ? byte_offset + length <= file_size : byte_offset < file_size;
}

// Runs |transfer|, which returns a status and fills in the bytes moved, then
// reports the completion. A request that pends runs on an I/O worker.
static X_STATUS SubmitRequest(const object_ref<XFile>& file,
                              const object_ref<XEvent>& ev,
                              uint32_t apc_routine, uint32_t apc_context,
                              X_IO_STATUS_BLOCK* status_block,
                              uint32_t status_block_address, bool pends,
                              std::function<X_STATUS(uint32_t*)> transfer) {
  auto thread = retain_object(XThread::GetCurrentThread());
  // Cleared at issue, so a wait cannot return on an earlier completion.
  if (ev) {
    ev->Reset();
  }
  file->ResetWaitState();
  auto complete = [file, ev, thread, apc_routine, apc_context, status_block,
                   status_block_address, transfer = std::move(transfer)](
                      bool posted, bool cancelled) {
    uint32_t bytes = 0;
    X_STATUS status = cancelled ? X_STATUS_CANCELLED : transfer(&bytes);
    // A failure the caller was not told PENDING for completes nothing.
    if (!posted && XFAILED(status)) {
      return status;
    }
    if (status_block) {
      WriteIoStatus(status_block, status, bytes);
    }
    if (QueuesApc(apc_routine, apc_context)) {
      QueueIoApc(thread.get(), apc_routine, apc_context, status_block_address);
    }
    file->NotifyCompletion(status, bytes, apc_context);
    if (ev) {
      ev->Set(KernelState::kIoDiskIncrement, false);
    }
    return status;
  };
  if (pends) {
    if (status_block) {
      status_block->status = X_STATUS_PENDING;
      status_block->information = 0;
    }
    file->PostIo(thread.get(),
                 [complete = std::move(complete)](bool cancelled) {
                   complete(true, cancelled);
                 });
    return X_STATUS_PENDING;
  }
  X_STATUS status = complete(false, false);
  // A kernel APC the completion queued runs before the service returns.
  xeProcessKernelApcs(thread->thread_state()->context());
  return status;
}

struct CreateOptions {
  // https://processhacker.sourceforge.io/doc/ntioapi_8h.html
  static constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;
  // Optimization - files access will be sequential, not random.
  static constexpr uint32_t FILE_SEQUENTIAL_ONLY = 0x00000004;
  static constexpr uint32_t FILE_NO_INTERMEDIATE_BUFFERING = 0x00000008;
  static constexpr uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
  static constexpr uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
  static constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
  // Optimization - file access will be random, not sequential.
  static constexpr uint32_t FILE_RANDOM_ACCESS = 0x00000800;
};

// XeFu, the original Xbox emulator, keeps each Xbox title's data, its T: drive,
// in Xbox1\TDATA\<title id> in the HDD's Compatibility directory and always
// passes full paths. Splits a lowercase path in there into the title id and
// the rest.
static bool SplitXeFuTitleDataPath(std::string_view path, uint32_t* title_id,
                                   std::string_view* rest) {
  constexpr std::string_view kTitleData = "\\xbox1\\tdata\\";
  const size_t found = path.rfind(kTitleData);
  if (found == std::string_view::npos) {
    return false;
  }
  const std::string_view id = path.substr(found + kTitleData.size(), 8);
  if (id.size() != 8 ||
      std::from_chars(id.data(), id.data() + id.size(), *title_id, 16).ptr !=
          id.data() + id.size()) {
    return false;
  }
  *rest = path.substr(found + kTitleData.size() + id.size());
  return true;
}

// An original Xbox title applies its own title update: it runs the executable
// in T:\$u when $u\ContentMeta.xbx lists the running one's version. The status
// that hides $u from it, success for any other path. $u still lists.
static X_STATUS HideXeFuTitleUpdate(std::string_view path) {
  const std::string lower = xe::utf8::lower_ascii(path);
  uint32_t title_id;
  std::string_view rest;
  constexpr std::string_view kTitleUpdate = "\\$u";
  if (!SplitXeFuTitleDataPath(lower, &title_id, &rest) ||
      !rest.starts_with(kTitleUpdate)) {
    return X_STATUS_SUCCESS;
  }
  rest.remove_prefix(kTitleUpdate.size());
  // A trailing separator still names $u.
  if (rest.find_first_not_of('\\') == std::string_view::npos) {
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  return rest.front() == '\\' ? X_STATUS_OBJECT_PATH_NOT_FOUND
                              : X_STATUS_SUCCESS;
}

// XeFu opens the running Xbox title's data directory as the title starts. The
// certificate of the XBE it loaded, at 0x10000 in user mode, names the title
// and its version.
static void NoteXeFuTitle(std::string_view path) {
  const std::string lower = xe::utf8::lower_ascii(path);
  uint32_t title_id = 0;
  std::string_view rest;
  if (!SplitXeFuTitleDataPath(lower, &title_id, &rest) || !rest.empty()) {
    return;
  }
  // XeFu's own system data, such as the soundtrack database it opens at boot.
  if ((title_id >> 16) == 0xFFFE) {
    return;
  }
  auto memory = kernel_memory();
  // Little-endian like everything the x86 side keeps. Each field is translated
  // on its own, as consecutive user pages need not map to consecutive kernel
  // addresses.
  auto read_user = [memory](uint32_t address, auto* out) {
    const uint32_t kernel_address = memory->UserModeKernelAddress(address);
    if (kernel_address == address) {
      return false;
    }
    *out = xe::load<std::remove_pointer_t<decltype(out)>>(
        memory->TranslateForRead(kernel_address));
    return true;
  };
  std::string name;
  // Its version tells a title update apart from the disc's executable.
  std::optional<vfs::XbeMetadata> running;
  uint32_t magic = 0, certificate = 0, certificate_title_id = 0;
  // XBEH.
  if (read_user(0x10000, &magic) && magic == 0x48454258 &&
      read_user(0x10118, &certificate) &&
      read_user(certificate + 0x8, &certificate_title_id)) {
    // Another title's data, such as an alternate title the game mounts.
    if (certificate_title_id != title_id) {
      return;
    }
    std::u16string title_name;
    for (uint32_t i = 0; i < 40; ++i) {
      uint16_t c = 0;
      if (!read_user(certificate + 0xC + i * 2, &c) || !c) {
        break;
      }
      title_name.push_back(char16_t(c));
    }
    name = xe::to_utf8_lossy(title_name);
    vfs::XbeMetadata xbe;
    if (read_user(certificate + 0xAC, &xbe.version)) {
      xbe.title_id = title_id;
      running = std::move(xbe);
    }
  }
  if (name.empty()) {
    name = fmt::format("{:08X}", title_id);
  }
  // On the UI thread, which reads both.
  auto emulator = kernel_state()->emulator();
  auto set_title = [emulator, name, running]() {
    if (running) {
      emulator->SetRunningXbe(*running);
    }
    if (emulator->title_name() != name) {
      emulator->SetTitleName(name);
    }
  };
  if (auto window = emulator->display_window()) {
    window->app_context().CallInUIThread(set_title);
  } else {
    set_title();
  }
}

// IoCreateFile option that opens the directory holding the path's last
// component.
constexpr uint32_t kIoOpenTargetDirectory = 0x4;

dword_result_t IoCreateFile_entry(lpdword_t handle_out, dword_t desired_access,
                                  pointer_t<X_OBJECT_ATTRIBUTES> object_attrs,
                                  pointer_t<X_IO_STATUS_BLOCK> io_status_block,
                                  lpqword_t allocation_size_ptr,
                                  dword_t file_attributes, dword_t share_access,
                                  dword_t creation_disposition,
                                  dword_t create_options, dword_t options) {
  uint64_t allocation_size = 0;  // is this correct???
  if (allocation_size_ptr) {
    allocation_size = *allocation_size_ptr;
  }

  if (!object_attrs) {
    // ..? Some games do this. This parameter is not optional.
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  // A null name opens what the root directory handle refers to. That's how a
  // file is reopened from a handle.
  auto object_name =
      util::TranslateAnsiStringPointer(kernel_memory(), object_attrs->name_ptr);

  vfs::Entry* root_entry = nullptr;

  const bool has_root_handle =
      object_attrs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      object_attrs->root_directory != 0;

  // Compute path, possibly attrs relative.
  auto target_path = util::TranslateAnsiPath(kernel_memory(), object_name);
  // A name relative to a directory handle is never under \??\.
  if (!has_root_handle) {
    xeObStripDosDevicesPrefix(target_path);
  }

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    XELOGFS("NtCreateFile({}) = OBJECT_NAME_INVALID", target_path);
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (has_root_handle) {
    auto root_file = kernel_state()->object_table()->LookupObject<XFile>(
        object_attrs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);

    root_entry = root_file->entry();
  }

  X_STATUS result = X_STATUS_SUCCESS;
  if (!cvars::apply_title_update &&
      kernel_state()->title_id() == kXeFuTitleId) {
    result = HideXeFuTitleUpdate(target_path);
  }

  // Attempt open (or create). The host open can block on a slow drive.
  vfs::File* vfs_file;
  vfs::FileAction file_action;
  if (XSUCCEEDED(result)) {
    kernel_state()->RunBlockingIo(
        [&]() {
          if (options & kIoOpenTargetDirectory) {
            result = kernel_state()->file_system()->OpenFile(
                root_entry, xe::utf8::find_base_guest_path(target_path),
                vfs::FileDisposition::kOpen, desired_access, true, false,
                &vfs_file, &file_action);
            if (result == X_STATUS_OBJECT_NAME_NOT_FOUND) {
              result = X_STATUS_OBJECT_PATH_NOT_FOUND;
            }
            if (XSUCCEEDED(result)) {
              file_action =
                  vfs_file->entry()->GetChild(
                      xe::utf8::find_name_from_guest_path(target_path))
                      ? vfs::FileAction::kExists
                      : vfs::FileAction::kDoesNotExist;
            }
            return;
          }
          result = kernel_state()->file_system()->OpenFile(
              root_entry, target_path,
              vfs::FileDisposition((uint32_t)creation_disposition),
              desired_access,
              (create_options & CreateOptions::FILE_DIRECTORY_FILE) != 0,
              (create_options & CreateOptions::FILE_NON_DIRECTORY_FILE) != 0,
              &vfs_file, &file_action);
        },
        GuestScheduler::BlockingCallClass::kConcurrent);
  }
  object_ref<XFile> file = nullptr;

  // A failed open leaves the handle and the information 0.
  X_HANDLE handle = 0;
  if (XSUCCEEDED(result)) {
    // If true, desired_access SYNCHRONIZE flag must be set.
    bool alertable =
        (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_ALERT) != 0;
    bool synchronous =
        alertable ||
        (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_NONALERT);
    bool buffered =
        !(create_options & CreateOptions::FILE_NO_INTERMEDIATE_BUFFERING);
    file = object_ref<XFile>(
        new XFile(kernel_state(), vfs_file, synchronous, alertable, buffered));

    // Handle ref is incremented, so return that.
    handle = file->handle();
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information =
        XSUCCEEDED(result) ? uint32_t(file_action) : 0;
  }

  *handle_out = handle;

  XELOGFS("NtCreateFile({}) = {:08X}, handle={:08X}", target_path, result,
          handle);
  if (XSUCCEEDED(result) && kernel_state()->title_id() == kXeFuTitleId) {
    NoteXeFuTitle(target_path);
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(IoCreateFile, kFileSystem, kImplemented);

dword_result_t NtCreateFile_entry(lpdword_t handle_out, dword_t desired_access,
                                  pointer_t<X_OBJECT_ATTRIBUTES> object_attrs,
                                  pointer_t<X_IO_STATUS_BLOCK> io_status_block,
                                  lpqword_t allocation_size_ptr,
                                  dword_t file_attributes, dword_t share_access,
                                  dword_t creation_disposition,
                                  dword_t create_options) {
  return IoCreateFile_entry(handle_out, desired_access, object_attrs,
                            io_status_block, allocation_size_ptr,
                            file_attributes, share_access, creation_disposition,
                            create_options, 0);
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateFile, kFileSystem, kImplemented);

dword_result_t NtOpenFile_entry(
    lpdword_t handle_out, dword_t desired_access,
    pointer_t<X_OBJECT_ATTRIBUTES> object_attributes,
    pointer_t<X_IO_STATUS_BLOCK> io_status_block, dword_t share_access,
    dword_t open_options) {
  return NtCreateFile_entry(
      handle_out, desired_access, object_attributes, io_status_block, nullptr,
      0, share_access, static_cast<uint32_t>(xe::vfs::FileDisposition::kOpen),
      open_options);
}
DECLARE_XBOXKRNL_EXPORT1(NtOpenFile, kFileSystem, kImplemented);

dword_result_t NtReadFile_entry(dword_t file_handle, dword_t event_handle,
                                lpvoid_t apc_routine_ptr, lpvoid_t apc_context,
                                pointer_t<X_IO_STATUS_BLOCK> io_status_block,
                                lpvoid_t buffer, dword_t buffer_length,
                                lpqword_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  uint64_t byte_offset = 0;
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  } else if (XSUCCEEDED(result)) {
    result = ValidateRequest(file.get(), static_cast<uint32_t>(apc_routine_ptr),
                             byte_offset_ptr, false, &byte_offset);
  }

  if (XSUCCEEDED(result)) {
    uint32_t buffer_address = buffer.guest_address();
    uint32_t length = buffer_length;
    uint32_t apc_context_address = apc_context.guest_address();
    result = SubmitRequest(
        file, ev, static_cast<uint32_t>(apc_routine_ptr), apc_context_address,
        io_status_block, io_status_block.guest_address(),
        CompletesAsync(file.get(), length, byte_offset, false),
        [file, buffer_address, length, byte_offset,
         apc_context_address](uint32_t* bytes_read) {
          X_STATUS status = file->Read(buffer_address, length, byte_offset,
                                       bytes_read, apc_context_address, false);
          if (XSUCCEEDED(status)) {
            if (auto patch = kernel_state()->xmp_volume_patch()) {
              auto host_buf = kernel_memory()->TranslateVirtual(buffer_address);
              patch->OnFileRead(file->entry()->name(), host_buf, length,
                                buffer_address);
            }
          }
          return status;
        });
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtReadFile, kFileSystem, kImplemented, kHighFrequency);

dword_result_t NtReadFileScatter_entry(
    dword_t file_handle, dword_t event_handle, lpvoid_t apc_routine_ptr,
    lpvoid_t apc_context, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    lpdword_t segment_array, dword_t length, lpqword_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  uint64_t byte_offset = 0;
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  } else if (XSUCCEEDED(result) &&
             (file->is_buffered() || file->device()->name() == "STFS")) {
    // Scatter reads need an unbuffered handle. STFS takes none.
    result = X_STATUS_INVALID_PARAMETER;
  } else if (XSUCCEEDED(result)) {
    result = ValidateRequest(file.get(), static_cast<uint32_t>(apc_routine_ptr),
                             byte_offset_ptr, false, &byte_offset);
  }

  if (XSUCCEEDED(result)) {
    uint32_t segments_address = segment_array.guest_address();
    uint32_t read_length = length;
    uint32_t apc_context_address = apc_context.guest_address();
    result = SubmitRequest(
        file, ev, static_cast<uint32_t>(apc_routine_ptr), apc_context_address,
        io_status_block, io_status_block.guest_address(),
        CompletesAsync(file.get(), read_length, byte_offset, false),
        [file, segments_address, read_length, byte_offset,
         apc_context_address](uint32_t* bytes_read) {
          return file->ReadScatter(segments_address, read_length, byte_offset,
                                   bytes_read, apc_context_address, false);
        });
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtReadFileScatter, kFileSystem, kImplemented);

dword_result_t NtWriteFile_entry(dword_t file_handle, dword_t event_handle,
                                 function_t apc_routine, lpvoid_t apc_context,
                                 pointer_t<X_IO_STATUS_BLOCK> io_status_block,
                                 lpvoid_t buffer, dword_t buffer_length,
                                 lpqword_t byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  uint64_t byte_offset = 0;
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  } else if (XSUCCEEDED(result)) {
    result = ValidateRequest(file.get(), apc_routine, byte_offset_ptr, true,
                             &byte_offset);
  }

  if (XSUCCEEDED(result)) {
    uint32_t buffer_address = buffer.guest_address();
    uint32_t length = buffer_length;
    uint32_t apc_context_address = apc_context.guest_address();
    result = SubmitRequest(
        file, ev, apc_routine, apc_context_address, io_status_block,
        io_status_block.guest_address(),
        CompletesAsync(file.get(), length, byte_offset, true),
        [file, buffer_address, length, byte_offset,
         apc_context_address](uint32_t* bytes_written) {
          X_STATUS status =
              file->Write(buffer_address, length, byte_offset, bytes_written,
                          apc_context_address, false);
          if (XSUCCEEDED(status)) {
            if (auto patch = kernel_state()->xmp_volume_patch()) {
              auto host_buf = kernel_memory()->TranslateVirtual(buffer_address);
              patch->OnFileWrite(file->entry()->name(), host_buf, length,
                                 buffer_address);
            }
          }
          return status;
        });
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtWriteFile, kFileSystem, kImplemented);

dword_result_t NtCreateIoCompletion_entry(
    lpdword_t out_handle, dword_t desired_access,
    pointer_t<X_OBJECT_ATTRIBUTES> object_attribs,
    dword_t num_concurrent_threads) {
  auto completion =
      object_ref<XIOCompletion>(new XIOCompletion(kernel_state()));
  if (out_handle) {
    *out_handle = completion->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateIoCompletion, kFileSystem, kImplemented);

dword_result_t NtSetIoCompletion_entry(dword_t handle, dword_t key_context,
                                       dword_t apc_context,
                                       dword_t completion_status,
                                       dword_t num_bytes) {
  auto port =
      kernel_state()->object_table()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    return X_STATUS_INVALID_HANDLE;
  }

  XIOCompletion::IONotification notification;
  notification.key_context = key_context;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = completion_status;

  port->QueueNotification(notification);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(NtSetIoCompletion, kFileSystem, kImplemented,
                         kHighFrequency);

// Dequeues a packet from the completion port.
dword_result_t NtRemoveIoCompletion_entry(
    dword_t handle, lpdword_t key_context, lpdword_t apc_context,
    pointer_t<X_IO_STATUS_BLOCK> io_status_block, lpqword_t timeout) {
  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t info = 0;

  auto port =
      kernel_state()->object_table()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    status = X_STATUS_INVALID_HANDLE;
  }

  uint64_t timeout_ticks =
      timeout ? static_cast<uint32_t>(*timeout)
              : static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
  XIOCompletion::IONotification notification;
  if (port->WaitForNotification(timeout_ticks, &notification)) {
    if (key_context) {
      *key_context = notification.key_context;
    }
    if (apc_context) {
      *apc_context = notification.apc_context;
    }

    if (io_status_block) {
      io_status_block->status = notification.status;
      io_status_block->information = notification.num_bytes;
    }
  } else {
    status = X_STATUS_TIMEOUT;
  }

  return status;
}
DECLARE_XBOXKRNL_EXPORT2(NtRemoveIoCompletion, kFileSystem, kImplemented,
                         kHighFrequency);

dword_result_t NtCancelIoFile_entry(dword_t handle,
                                    const ppc_context_t& context) {
  auto file = kernel_state()->object_table()->LookupObject<XFile>(handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }
  // The calling thread's requests on the file that have yet to start complete
  // as cancelled. The call returns once the one running has completed,
  // polling every 10 ms.
  XThread* thread = XThread::GetCurrentThread();
  file->CancelPostedIo(thread);
  while (file->HasPostedIo(thread)) {
    uint64_t interval = uint64_t(-100000);
    KeDelayExecutionThread(0, false, &interval, context);
  }
  // A kernel APC the completions queued runs before the service returns.
  xeProcessKernelApcs(context);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCancelIoFile, kFileSystem, kImplemented);

dword_result_t NtQueryFullAttributesFile_entry(
    pointer_t<X_OBJECT_ATTRIBUTES> obj_attribs,
    pointer_t<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
  auto object_name =
      util::TranslateAnsiStringPointer(kernel_memory(), obj_attribs->name_ptr);

  object_ref<XFile> root_file;
  if (obj_attribs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      obj_attribs->root_directory != 0) {
    root_file = kernel_state()->object_table()->LookupObject<XFile>(
        obj_attribs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);
    assert_always();
  }

  auto target_path = util::TranslateAnsiPath(kernel_memory(), object_name);
  // A name relative to a directory handle is never under \??\.
  if (!root_file) {
    xeObStripDosDevicesPrefix(target_path);
  }

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // Resolve the file using the virtual file system. The host lookup can block
  // on a slow drive.
  vfs::Entry* entry = nullptr;
  kernel_state()->RunBlockingIo(
      [&]() {
        entry = kernel_state()->file_system()->ResolvePath(target_path);
        if (entry) {
          entry->update();
        }
      },
      GuestScheduler::BlockingCallClass::kConcurrent);
  if (entry) {
    // Found.
    file_info->creation_time = entry->create_timestamp();
    file_info->last_access_time = entry->access_timestamp();
    file_info->last_write_time = entry->write_timestamp();
    file_info->change_time = entry->write_timestamp();
    file_info->allocation_size = entry->allocation_size();
    file_info->end_of_file = entry->size();
    file_info->attributes = entry->attributes();

    return X_STATUS_SUCCESS;
  }

  return X_STATUS_NO_SUCH_FILE;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryFullAttributesFile, kFileSystem, kImplemented);

dword_result_t NtQueryDirectoryFile_entry(
    dword_t file_handle, dword_t event_handle, function_t apc_routine,
    lpvoid_t apc_context, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    pointer_t<X_FILE_DIRECTORY_INFORMATION> file_info_ptr, dword_t length,
    pointer_t<X_ANSI_STRING> file_name, dword_t restart_scan) {
  if (length < 72) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  uint32_t info = 0;

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  auto name = util::TranslateAnsiPath(kernel_memory(), file_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(name, true)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (file) {
    X_FILE_DIRECTORY_INFORMATION dir_info = {0};
    result =
        file->QueryDirectory(file_info_ptr, length, name, restart_scan != 0);
    if (XSUCCEEDED(result)) {
      info = length;
    }
  } else {
    result = X_STATUS_NO_SUCH_FILE;
  }

  if (XFAILED(result)) {
    info = 0;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = info;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryDirectoryFile, kFileSystem, kImplemented);

dword_result_t NtFlushBuffersFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block_ptr) {
  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }
  // Writes reach the host as they run. On an async handle the flush queues
  // behind the handle's pending requests, so it completes after them.
  return SubmitRequest(file, nullptr, 0, 0, io_status_block_ptr,
                       io_status_block_ptr.guest_address(),
                       !file->is_synchronous(),
                       [](uint32_t*) { return X_STATUS_SUCCESS; });
}
DECLARE_XBOXKRNL_EXPORT1(NtFlushBuffersFile, kFileSystem, kImplemented);

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntopensymboliclinkobject
dword_result_t NtOpenSymbolicLinkObject_entry(
    lpdword_t handle_out, pointer_t<X_OBJECT_ATTRIBUTES> object_attrs) {
  if (!object_attrs) {
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  assert_true(object_attrs->attributes == 64);  // case insensitive

  auto object_name =
      util::TranslateAnsiStringPointer(kernel_memory(), object_attrs->name_ptr);

  auto target_path = xeObSymbolicLinkName(
      util::TranslateAnsiPath(kernel_memory(), object_name));

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0) {
    assert_always();
  }

  std::string link_path;
  if (!kernel_state()->file_system()->FindSymbolicLink(target_path,
                                                       link_path)) {
    return X_STATUS_NO_SUCH_FILE;
  }

  object_ref<XSymbolicLink> symlink(new XSymbolicLink(kernel_state()));
  symlink->Initialize(target_path, link_path);

  *handle_out = symlink->handle();

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtOpenSymbolicLinkObject, kFileSystem, kImplemented);

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntquerysymboliclinkobject
dword_result_t NtQuerySymbolicLinkObject_entry(
    dword_t handle, pointer_t<X_ANSI_STRING> target) {
  auto symlink =
      kernel_state()->object_table()->LookupObject<XSymbolicLink>(handle);
  if (!symlink) {
    return X_STATUS_NO_SUCH_FILE;
  }
  auto length = std::min(static_cast<size_t>(target->maximum_length),
                         symlink->target().size());
  if (length > 0) {
    auto target_buf = kernel_memory()->TranslateVirtual(target->pointer);
    std::memcpy(target_buf, symlink->target().c_str(), length);
  }
  target->length = static_cast<uint16_t>(length);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtQuerySymbolicLinkObject, kFileSystem, kImplemented);

dword_result_t FscGetCacheElementCount_entry(dword_t r3) { return 0; }
DECLARE_XBOXKRNL_EXPORT1(FscGetCacheElementCount, kFileSystem, kStub);

dword_result_t FscSetCacheElementCount_entry(dword_t unk_0, dword_t unk_1) {
  // unk_0 = 0
  // unk_1 looks like a count? in what units? 256 is a common value
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(FscSetCacheElementCount, kFileSystem, kStub);

struct X_DRIVE_GEOMETRY {
  xe::be<uint32_t> sector_count;
  xe::be<uint32_t> sector_size;
};
static_assert_size(X_DRIVE_GEOMETRY, 0x8);

struct X_PARTITION_INFO {
  xe::be<uint64_t> unk;
  xe::be<uint64_t> total_size;
};
static_assert_size(X_PARTITION_INFO, 0x10);

// todo: this should fill in the io status block and queue the apc
dword_result_t NtDeviceIoControlFile_entry(
    dword_t handle, dword_t event_handle, dword_t apc_routine,
    dword_t apc_context, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    dword_t io_control_code, lpvoid_t input_buffer, dword_t input_buffer_len,
    lpvoid_t output_buffer, dword_t output_buffer_len) {
  // Called by XMountUtilityDrive cache-mounting code
  // (checks if the returned values look valid, values below seem to pass the
  // checks)
  constexpr uint32_t cache_size = 0xFF000;

  if (io_control_code == X_IOCTL_DISK_GET_DRIVE_GEOMETRY) {
    if (output_buffer_len < sizeof(X_DRIVE_GEOMETRY)) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    auto buffer = output_buffer.as<X_DRIVE_GEOMETRY*>();
    buffer->sector_count = cache_size / 0x200;
    buffer->sector_size = 0x200;  // 0x200, 0x1000, 0x4000
  } else if (io_control_code == X_IOCTL_DISK_GET_PARTITION_INFO) {
    if (output_buffer_len < sizeof(X_PARTITION_INFO)) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    auto buffer = output_buffer.as<X_PARTITION_INFO*>();
    buffer->unk = 0;
    buffer->total_size = cache_size;
  } else if (io_control_code == X_IOCTL_CDROM_AUTHENTICATE_DISC) {
    // The game in the drive is always an authentic disc.
  } else {
    XELOGD("NtDeviceIoControlFile(0x{:X}) - unhandled IOCTL!",
           uint32_t(io_control_code));
    assert_always();
    return X_STATUS_INVALID_PARAMETER;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtDeviceIoControlFile, kFileSystem, kStub);

// cache: is a host folder mounted for every title, so no STFS volume is made
// over a cache partition and the caller's device object stays as it was.
dword_result_t StfsCreateDevice_entry(lpvoid_t params, dword_t params_size) {
  if (params_size != 0x58) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(StfsCreateDevice, kFileSystem, kStub);

dword_result_t StfsControlDevice_entry(lpvoid_t device, dword_t control_code,
                                       lpvoid_t buffer) {
  if (control_code > 4) {
    return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(StfsControlDevice, kFileSystem, kStub);
// device_extension_size = additional bytes of data (aligned up to 8 byte
// granularity) that will be allocated at the tail of the resulting device
// object. although it is allocated at the tail, it is accessed through a
// pointer at offset 0x18 so in theory a guest could be unaware that its a
// single allocation device_name is optional, extra_device_object_attributes
// gets assigned to the attributes field of the OBJECT_ATTRIBUTES used for
// ObCreateObject

// todo: need device guest object struct + host object for device
struct X_DRIVER_OBJECT {
  xe::be<uint32_t> driver_start_io_ptr;
  xe::be<uint32_t> driver_delete_device_ptr;
  xe::be<uint32_t> driver_dismount_volume_ptr;
  xe::be<uint32_t> major_function_ptr[11];
};
static_assert_size(X_DRIVER_OBJECT, 0x38);

struct X_KDEVICE_QUEUE {
  xe::be<uint16_t> type;                  // 0x0 sz:0x2
  xe::be<uint8_t> padding;                // 0x2 sz:0x1
  xe::be<uint8_t> busy;                   // 0x3 sz:0x1
  xe::be<uint32_t> lock;                  // 0x4 sz:0x4
  xe::be<X_LIST_ENTRY> device_list_head;  // 0x8 sz:0x8
};
static_assert_size(X_KDEVICE_QUEUE, 0x10);

struct X_KDEVICE_QUEUE_ENTRY {
  X_LIST_ENTRY device_list_entry;  // 0x0 sz:0x2
  xe::be<uint32_t> sort_key;       // 0x8 sz:0x4
  xe::be<uint8_t> inserted;        // 0xC sz:0x1
};
static_assert_size(X_KDEVICE_QUEUE_ENTRY, 0x10);

struct X_IRP_ASYNC_PARAM {
  xe::be<uint32_t> user_apc_routine_ptr;  // 0x0 sz:0x4
  xe::be<uint32_t> user_apc_context_ptr;  // 0x4 sz:0x4
};
static_assert_size(X_IRP_ASYNC_PARAM, 0x8);

union X_UNION_IRP_OVERLAY {
  X_IRP_ASYNC_PARAM asynchronous_parameters;
  xe::be<int64_t> allocation_size;
};

struct X_IRP_OVERLAY {
  union {
    X_KDEVICE_QUEUE_ENTRY device_queue_entry;  // 0x0 sz:0x10
    X_LIST_ENTRY device_list_entry;            // 0x0 sz:0x8
    xe::be<uint32_t> driver_context_ptr[4];    // 0x0 sz:0x10
  };
  xe::be<uint32_t> locked_buffer_length;    // 0x10 sz:0x4
  TypedGuestPointer<X_KTHREAD> thread_ptr;  // 0x14 sz:0x4
  X_LIST_ENTRY list_entry;                  // 0x18 sz:0x8
  union {
    xe::be<uint32_t>
        current_stack_location_ptr;  // 0x20 sz:0x4, X_IO_STACK_LOCATION -> 0x24
    xe::be<uint32_t> packet_type;    // 0x20 sz:0x4
  };
  xe::be<uint32_t>
      original_file_object_ptr;  // 0x24 sz:0x4, X_FILE_OBJECT -> 0x68
};
static_assert_size(X_IRP_OVERLAY, 0x28);

union X_IRP_TAIL {
  X_IRP_OVERLAY overlay;                // 0x0 sz:0x28
  xe::be<XAPC> apc;                     // 0x0 sz:0x28
  xe::be<uint32_t> completion_key_ptr;  // 0x0 sz:0x4
};

struct X_IRP {
  xe::be<uint16_t> type;                               // 0x0 sz:0x2
  xe::be<uint16_t> size;                               // 0x2 sz:0x2
  xe::be<uint32_t> flags;                              // 0x4 sz:0x4
  X_LIST_ENTRY thread_list_entry;                      // 0x8 sz:0x8
  X_IO_STATUS_BLOCK io_status;                         // 0x10 sz:0x8
  xe::be<uint8_t> stack_count;                         // 0x18 sz:0x1
  xe::be<uint8_t> current_location;                    // 0x19 sz:0x1
  xe::be<uint8_t> pending_returned;                    // 0x1A sz:0x1
  xe::be<uint8_t> cancel;                              // 0x1B sz:0x1
  xe::be<uint32_t> user_buffer_ptr;                    // 0x1C sz:0x4
  TypedGuestPointer<X_IO_STATUS_BLOCK> user_iosb_ptr;  // 0x20 sz:0x4
  TypedGuestPointer<X_KEVENT> user_event_ptr;          // 0x24 sz:0x4
  X_UNION_IRP_OVERLAY overlay;                         // 0x28 sz:0x8
  X_IRP_TAIL tail;                                     // 0x30 sz:0x28
  xe::be<uint32_t> cancel_routine_ptr;                 // 0x58 sz:0x4
};
static_assert_size(X_IRP, 0x60);

struct X_DEVICE_OBJECT {
  xe::be<uint16_t> type;                                      // 0x0 sz:0x2
  xe::be<uint16_t> device_extension_size;                     // 0x2 sz:0x2
  xe::be<uint32_t> reference_count;                           // 0x4 sz:0x4
  TypedGuestPointer<X_DRIVER_OBJECT> drive_object_ptr;        // 0x8 sz:0x4
  TypedGuestPointer<X_DEVICE_OBJECT> mounted_or_self_device;  // 0xC sz:0x4
  TypedGuestPointer<X_IRP> current_irp_ptr;                   // 0x10 sz:0x4
  xe::be<uint32_t> flags;                                     // 0x14 sz:0x4
  xe::be<uint32_t> device_extension_ptr;                      // 0x18 sz:0x4
  xe::be<uint8_t> device_type;                                // 0x1C sz:0x1
  xe::be<uint8_t> start_io_flags;                             // 0x1D sz:0x1
  xe::be<uint8_t> stack_size;                                 // 0x1E sz:0x1
  xe::be<uint8_t> delete_pending;                             // 0x1F sz:0x1
  xe::be<uint32_t> sector_size;  // 0x20 sz:0x4, set by XamRamDriveCreate
  xe::be<uint32_t>
      alignment;  // 0x24 sz:0x4, NtQueryInformationFile called to verify
  xe::be<X_KDEVICE_QUEUE> device_queue;  // 0x28 sz:0x10
  xe::be<X_KEVENT> device_lock;          // 0x38 sz:0x10
  xe::be<uint32_t> start_io_count;       // 0x48 sz:0x4
  xe::be<uint32_t> start_io_key;         // 0x4C sz:0x4
};
static_assert_size(X_DEVICE_OBJECT, 0x50);

dword_result_t IoCreateDevice_entry(pointer_t<X_DRIVER_OBJECT> driver_object,
                                    dword_t device_extension_size,
                                    pointer_t<X_ANSI_STRING> device_name,
                                    dword_t device_type,
                                    dword_t extra_device_object_attributes,
                                    lpdword_t device_object,
                                    const ppc_context_t& ctx) {
  // Called from XMountUtilityDrive XAM-task code
  // We'll alloc some scratch space for it so it doesn't cause any exceptions
  auto kernel_mem = ctx->kernel_state->memory();

  uint32_t required_size =
      sizeof(X_DEVICE_OBJECT) + xe::align<uint32_t>(device_extension_size, 8);

  auto out_guest = kernel_mem->SystemHeapAlloc(required_size);

  auto out = kernel_mem->TranslateVirtual<X_DEVICE_OBJECT*>(out_guest);

  memset(out, 0, required_size);

  out->type = 3;  // maybe device object's Ob type?

  // this stores the total object size, without alignment!
  out->device_extension_size = device_extension_size + sizeof(X_DEVICE_OBJECT);

  // from 17559
  if (device_type == 7 || device_type == 58 || device_type == 62 ||
      device_type == 45 || device_type == 2 || device_type == 60 ||
      device_type == 61 || device_type == 36 || device_type == 64 ||
      device_type == 65 || device_type == 66 || device_type == 67 ||
      device_type == 68 || device_type == 69 || device_type == 70 ||
      device_type == 72 || device_type == 73) {
    out->mounted_or_self_device = 0;
  } else {
    out->mounted_or_self_device = static_cast<uint32_t>(out_guest);
  }
  out->device_type = static_cast<uint8_t>(device_type);

  uint32_t flags_field_value = 16;
  if (device_name) {
    flags_field_value |= 8;
  }
  out->stack_size = 1;
  out->flags = flags_field_value;
  if (device_extension_size != 0) {
    // pointer to device specific data
    // XMountUtilityDrive writes some kind of header here
    out->device_extension_ptr = out_guest + 80;
  }

  out->drive_object_ptr = static_cast<uint32_t>(driver_object);

  *device_object = static_cast<uint32_t>(out_guest);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(IoCreateDevice, kFileSystem, kStub);

// supposed to invoke a callback on the driver object! its some sort of
// destructor function intended to be called for all devices created from the
// driver
void IoDeleteDevice_entry(pointer_t<X_DEVICE_OBJECT> device_ptr,
                          const ppc_context_t& ctx) {
  if (device_ptr) {
    auto kernel_mem = ctx->kernel_state->memory();
    kernel_mem->SystemHeapFree(device_ptr);
  }
}

DECLARE_XBOXKRNL_EXPORT1(IoDeleteDevice, kFileSystem, kStub);

// A handle xenia opened sits on a host backed file system with no guest
// volume to dismount, so a valid one is all that is checked.
dword_result_t IoDismountVolumeByFileHandle_entry(dword_t file_handle) {
  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(IoDismountVolumeByFileHandle, kFileSystem, kStub);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Io);
