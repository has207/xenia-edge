/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xfile.h"
#include "xenia/vfs/virtual_file_system.h"

#include <algorithm>

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XFile::XFile(KernelState* kernel_state, vfs::File* file, bool synchronous,
             bool alertable, bool buffered)
    : XObject(kernel_state, kObjectType),
      file_(file),
      is_synchronous_(synchronous),
      is_alertable_(alertable),
      is_buffered_(buffered) {
  async_event_ = threading::Event::CreateManualResetEvent(false);
  assert_not_null(async_event_);
}

XFile::XFile() : XObject(kObjectType), completion_port_lock_() {
  async_event_ = threading::Event::CreateManualResetEvent(false);
  assert_not_null(async_event_);
}

XFile::~XFile() {
  // TODO(benvanik): signal that the file is closing?
  async_event_->Set();
  file_->Destroy();
}

GuestScheduler::BlockingCallClass XFile::io_call_class() const {
  return device()->supports_concurrent_io()
             ? GuestScheduler::BlockingCallClass::kConcurrent
             : GuestScheduler::BlockingCallClass::kSerial;
}

void XFile::RunSynchronousIo(const std::function<void()>& fn) {
  kernel_state()->RunBlockingIo(fn, io_call_class(), is_alertable_);
}

uint64_t XFile::position() const { return position_.load(); }

void XFile::set_position(uint64_t value) { position_.store(value); }

X_STATUS XFile::QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info,
                               size_t length, const std::string_view file_name,
                               bool restart) {
  // An I/O worker may already hold file_lock_ for a slow read.
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    result = QueryDirectoryInternal(out_info, length, file_name, restart);
  });
  return result;
}

X_STATUS XFile::QueryDirectoryInternal(X_FILE_DIRECTORY_INFORMATION* out_info,
                                       size_t length,
                                       const std::string_view file_name,
                                       bool restart) {
  std::lock_guard<std::mutex> lock(file_lock_);
  assert_not_null(out_info);

  vfs::Entry* entry = nullptr;

  if (!file_name.empty()) {
    // Only queries in the current directory are supported for now.
    assert_true(utf8::find_any_of(file_name, "\\") == std::string_view::npos);

    find_engine_.SetRule(file_name);

    // Always restart the search?
    find_index_ = 0;
    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (restart) {
      find_index_ = 0;
    }

    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<uint8_t*>(out_info) + length;
  const auto& entry_name = entry->name();
  if (reinterpret_cast<uint8_t*>(&out_info->file_name[0]) + entry_name.size() >
      end) {
    assert_always("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<uint32_t>(find_index_);
  out_info->creation_time = entry->create_timestamp();
  out_info->last_access_time = entry->access_timestamp();
  out_info->last_write_time = entry->write_timestamp();
  out_info->change_time = entry->write_timestamp();
  out_info->end_of_file = entry->size();
  out_info->allocation_size = entry->allocation_size();
  out_info->attributes = entry->attributes();
  out_info->file_name_length = static_cast<uint32_t>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

X_STATUS XFile::Read(uint32_t buffer_guest_address, uint32_t buffer_length,
                     uint64_t byte_offset, uint32_t* out_bytes_read,
                     uint32_t apc_context, bool notify_completion) {
  // file_lock_ is taken inside the closure, on an I/O worker, so it is never
  // held while the calling fiber waits.
  // Booked before the offload, so requests queue in the order they are issued.
  const uint64_t deadline_ms = ReserveDriveTime(byte_offset, buffer_length);
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    result = ReadInternal(buffer_guest_address, buffer_length, byte_offset,
                          out_bytes_read, apc_context, notify_completion);
  });
  AwaitDriveTime(deadline_ms);
  return result;
}

X_STATUS XFile::ReadInternal(uint32_t buffer_guest_address,
                             uint32_t buffer_length, uint64_t byte_offset,
                             uint32_t* out_bytes_read, uint32_t apc_context,
                             bool notify_completion) {
  if (byte_offset == uint64_t(-1)) {
    // Read from current position.
    byte_offset = position_.load();
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  // Zero length means success for a valid file object according to Windows
  // tests.
  if (buffer_length) {
    if (UINT32_MAX - buffer_guest_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      // Games often read directly to texture/vertex buffer memory - in this
      // case, invalidation notifications must be sent. However, having any
      // memory callbacks in the range will result in STATUS_ACCESS_VIOLATION at
      // least on Windows, without anything being read or any callbacks being
      // triggered. So for physical memory, host protection must be bypassed,
      // and invalidation callbacks must be triggered manually (it's also wrong
      // to trigger invalidation callbacks before reading in this case, because
      // during the read, the guest may still access the data around the buffer
      // that is located in the same host pages as the buffer's start and end,
      // on the GPU - and that must not trigger a race condition).
      uint32_t buffer_guest_high_address =
          buffer_guest_address + buffer_length - 1;
      xe::BaseHeap* buffer_start_heap =
          memory()->LookupHeap(buffer_guest_address);
      const xe::BaseHeap* buffer_end_heap =
          memory()->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->heap_type() == HeapType::kGuestPhysical) !=
              (buffer_end_heap->heap_type() == HeapType::kGuestPhysical) ||
          (buffer_start_heap->heap_type() == HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        xe::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->heap_type() == HeapType::kGuestPhysical
                ? static_cast<xe::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap &&
            buffer_physical_heap->QueryRangeAccess(buffer_guest_address,
                                                   buffer_guest_high_address) !=
                memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          if (buffer_physical_heap) {
            // Resolve output is copied in whole pages, so the pages this read
            // only partly covers get it now, for the read to land over it.
            uint64_t file_size = entry()->size();
            buffer_physical_heap->ProvideReadWatchedEdgePages(
                buffer_guest_address,
                uint32_t(std::min<uint64_t>(
                    buffer_length,
                    byte_offset < file_size ? file_size - byte_offset : 0)));
          }
          result = file_->ReadSync(
              std::span<uint8_t>(
                  buffer_physical_heap
                      ? memory()->TranslatePhysical(
                            buffer_physical_heap->GetPhysicalAddress(
                                buffer_guest_address))
                      : memory()->TranslateVirtual(buffer_guest_address),
                  buffer_length),
              size_t(byte_offset), &bytes_read);
          if (XSUCCEEDED(result)) {
            if (buffer_physical_heap) {
              // The read has already replaced the contents it covers, the rest
              // of the buffer keeps them.
              uint32_t read_length =
                  uint32_t(std::min(size_t(buffer_length), bytes_read));
              if (read_length) {
                buffer_physical_heap->TriggerCallbacks(
                    xe::global_critical_region::AcquireDirect(),
                    buffer_guest_address, read_length, true, true, true, false,
                    true);
              }
              if (read_length < buffer_length) {
                buffer_physical_heap->TriggerCallbacks(
                    xe::global_critical_region::AcquireDirect(),
                    buffer_guest_address + read_length,
                    buffer_length - read_length, true, true);
              }
            }

            position_.store(byte_offset + bytes_read);
          }
        }
      }
    }
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(bytes_read);
  }

  if (notify_completion) {
    NotifyCompletion(result, uint32_t(bytes_read), apc_context);
  }

  return result;
}

void XFile::PostIo(XThread* thread, std::function<void(bool cancelled)> fn) {
  {
    std::lock_guard<std::mutex> lock(posted_lock_);
    posted_.emplace_back(thread, std::move(fn));
    if (posted_running_) {
      return;
    }
    posted_running_ = true;
  }
  // Outside the lock, as a post after shutdown began runs inline.
  kernel_state()->guest_scheduler()->PostHostCall(
      [file = retain_object(this)]() { file->RunPostedIo(); }, io_call_class());
}

void XFile::RunPostedIo() {
  std::unique_lock<std::mutex> lock(posted_lock_);
  while (!posted_.empty()) {
    auto [thread, fn] = std::move(posted_.front());
    posted_.pop_front();
    posted_running_thread_ = thread;
    lock.unlock();
    fn(false);
    lock.lock();
    posted_running_thread_ = nullptr;
  }
  posted_running_ = false;
}

void XFile::CancelPostedIo(XThread* thread) {
  std::vector<std::function<void(bool)>> cancelled;
  {
    std::lock_guard<std::mutex> lock(posted_lock_);
    for (auto it = posted_.begin(); it != posted_.end();) {
      if (it->first == thread) {
        cancelled.push_back(std::move(it->second));
        it = posted_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& fn : cancelled) {
    fn(true);
  }
}

bool XFile::HasPostedIo(XThread* thread) {
  std::lock_guard<std::mutex> lock(posted_lock_);
  if (posted_running_thread_ == thread) {
    return true;
  }
  for (const auto& posted : posted_) {
    if (posted.first == thread) {
      return true;
    }
  }
  return false;
}

uint64_t XFile::ReserveDriveTime(uint64_t byte_offset, uint32_t length) {
  // An async completion runs on a shared I/O worker, which must not block.
  if (GuestScheduler::CurrentThreadIsBlockingCallWorker()) {
    return 0;
  }
  // A caller holding the global lock cannot release it to wait.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return 0;
  }
  // Neither of these reaches the medium, and both are common size probes.
  if (!length) {
    return 0;
  }
  const uint64_t offset =
      byte_offset == uint64_t(-1) ? position_.load() : byte_offset;
  if (offset >= file_->entry()->size()) {
    return 0;
  }
  return device()->drive_timing().Reserve(file_->entry(), offset, length);
}

void XFile::AwaitDriveTime(uint64_t deadline_ms) {
  if (!deadline_ms) {
    return;
  }
  // Null off a fiber, and GetCurrentThread would assert there.
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self) {
    // Without fibers this is the guest thread, so blocking it is faithful.
    const uint64_t now = Clock::QueryHostUptimeMillis();
    if (now < deadline_ms) {
      threading::Sleep(std::chrono::milliseconds(deadline_ms - now));
    }
    return;
  }
  // The deadline is host time, so it must not pass through a guest-duration
  // API like XThread::Delay, which scales by the guest time scalar.
  auto* scheduler = kernel_state()->guest_scheduler();
  self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kDelay,
                                   nullptr, 0);
  while (Clock::QueryHostUptimeMillis() < deadline_ms) {
    // A kernel APC runs during the wait, which then goes on.
    if (self->HasDeliverableKernelApc()) {
      self->clear_cooperative_wait_shape();
      xboxkrnl::xeProcessKernelApcs(self->thread_state()->context());
      self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kDelay,
                                       nullptr, 0);
      continue;
    }
    scheduler->BlockCurrentThread(deadline_ms, 0, false);
  }
  self->clear_cooperative_wait_shape();
}

X_STATUS XFile::ReadScatter(uint32_t segments_guest_address, uint32_t length,
                            uint64_t byte_offset, uint32_t* out_bytes_read,
                            uint32_t apc_context, bool notify_completion) {
  // The whole loop as one request, so the fiber waits once.
  const uint64_t deadline_ms = ReserveDriveTime(byte_offset, length);
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    result =
        ReadScatterInternal(segments_guest_address, length, byte_offset,
                            out_bytes_read, apc_context, notify_completion);
  });
  AwaitDriveTime(deadline_ms);
  return result;
}

X_STATUS XFile::ReadScatterInternal(uint32_t segments_guest_address,
                                    uint32_t length, uint64_t byte_offset,
                                    uint32_t* out_bytes_read,
                                    uint32_t apc_context,
                                    bool notify_completion) {
  std::lock_guard<std::mutex> lock(file_lock_);
  X_STATUS result = X_STATUS_SUCCESS;

  // segments points to an array of buffer pointers of type
  // "FILE_SEGMENT_ELEMENT", but they can just be treated as normal pointers
  xe::be<uint32_t>* segments = reinterpret_cast<xe::be<uint32_t>*>(
      memory()->TranslateVirtual(segments_guest_address));

  // TODO: not sure if this is meant to change depending on buffer address?
  // (only game seen using this always seems to use 4096-byte buffers)
  uint32_t page_size = 4096;

  uint32_t read_total = 0;
  uint32_t read_remain = length;
  while (read_remain) {
    uint32_t read_length = read_remain;
    uint32_t read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    uint32_t bytes_read = 0;
    result = ReadInternal(
        read_buffer, read_length,
        byte_offset != uint64_t(-1) ? byte_offset + read_total : byte_offset,
        &bytes_read, apc_context, false);

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += bytes_read;
    read_remain -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(read_total);
  }

  if (notify_completion) {
    NotifyCompletion(result, read_total, apc_context);
  }

  return result;
}

X_STATUS XFile::Write(uint32_t buffer_guest_address, uint32_t buffer_length,
                      uint64_t byte_offset, uint32_t* out_bytes_written,
                      uint32_t apc_context, bool notify_completion) {
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    result = WriteInternal(buffer_guest_address, buffer_length, byte_offset,
                           out_bytes_written, apc_context, notify_completion);
  });
  return result;
}

X_STATUS XFile::WriteInternal(uint32_t buffer_guest_address,
                              uint32_t buffer_length, uint64_t byte_offset,
                              uint32_t* out_bytes_written, uint32_t apc_context,
                              bool notify_completion) {
  // Physical memory is read through the unprotected physical view, as a host
  // write from a watched page would fail rather than fault into its callbacks.
  // Those are triggered first as for a guest read, which brings in resolve
  // output guest RAM doesn't have yet. Outside the file lock, as this takes the
  // global one.
  uint8_t* buffer = memory()->TranslateVirtual(buffer_guest_address);
  if (buffer_length && UINT32_MAX - buffer_guest_address >= buffer_length) {
    xe::BaseHeap* buffer_heap = memory()->LookupHeap(buffer_guest_address);
    if (buffer_heap && buffer_heap->heap_type() == HeapType::kGuestPhysical &&
        memory()->LookupHeap(buffer_guest_address + buffer_length - 1) ==
            buffer_heap) {
      auto buffer_physical_heap = static_cast<xe::PhysicalHeap*>(buffer_heap);
      buffer_physical_heap->TriggerCallbacks(
          xe::global_critical_region::AcquireDirect(), buffer_guest_address,
          buffer_length, false, true);
      buffer = memory()->TranslatePhysical(
          buffer_physical_heap->GetPhysicalAddress(buffer_guest_address));
    }
  }

  std::lock_guard<std::mutex> lock(file_lock_);
  if (byte_offset == uint64_t(-1)) {
    // Write from current position.
    byte_offset = position_.load();
  } else if (byte_offset == kEndOfFile) {
    // The entry's size is only refreshed on request.
    entry()->update();
    byte_offset = entry()->size();
  }

  size_t bytes_written = 0;
  X_STATUS result = file_->WriteSync(std::span<uint8_t>(buffer, buffer_length),
                                     size_t(byte_offset), &bytes_written);
  // A zero-length write leaves the position alone.
  if (XSUCCEEDED(result) && buffer_length) {
    position_.store(byte_offset + bytes_written);
  }

  if (out_bytes_written) {
    *out_bytes_written = uint32_t(bytes_written);
  }

  if (notify_completion) {
    NotifyCompletion(result, uint32_t(bytes_written), apc_context);
  }
  return result;
}

X_STATUS XFile::SetLength(size_t length) {
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    result = file_->SetLength(length);
  });
  return result;
}
X_STATUS XFile::SetTimes(uint64_t create_timestamp, uint64_t access_timestamp,
                         uint64_t write_timestamp) {
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    result =
        file_->SetTimes(create_timestamp, access_timestamp, write_timestamp);
  });
  return result;
}
X_STATUS XFile::Rename(const std::filesystem::path file_path,
                       bool replace_existing) {
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    result = kernel_state()->file_system()->RenameEntry(
        entry(), xe::path_to_utf8(file_path), replace_existing);
  });
  return result;
}

bool XFile::RegisterIOCompletionPort(uint32_t key,
                                     object_ref<XIOCompletion> port) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);
  if (!completion_ports_.empty()) {
    return false;
  }
  completion_ports_.push_back({key, port});
  return true;
}

bool XFile::HasCompletionPort() {
  std::lock_guard<std::mutex> lock(completion_port_lock_);
  return !completion_ports_.empty();
}

void XFile::RemoveIOCompletionPort(uint32_t key) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto it = completion_ports_.begin(); it != completion_ports_.end();
       it++) {
    if (it->first == key) {
      completion_ports_.erase(it);
      break;
    }
  }
}

bool XFile::Save(ByteStream* stream) {
  // XELOGD("XFile {:08X} ({})", handle(),
  //        file_->entry()->absolute_path().c_str());

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(std::string_view(file_->entry()->absolute_path()));
  stream->Write<uint64_t>(position_);
  stream->Write(file_access());
  stream->Write<bool>(
      (file_->entry()->attributes() & vfs::kFileAttributeDirectory) != 0);
  stream->Write<bool>(is_synchronous_);
  stream->Write<bool>(is_buffered_);

  return true;
}

object_ref<XFile> XFile::Restore(KernelState* kernel_state,
                                 ByteStream* stream) {
  auto file = new XFile();
  file->kernel_state_ = kernel_state;
  if (!file->RestoreObject(stream)) {
    delete file;
    return nullptr;
  }

  auto abs_path = stream->Read<std::string>();
  uint64_t position = stream->Read<uint64_t>();
  auto access = stream->Read<uint32_t>();
  auto is_directory = stream->Read<bool>();
  auto is_synchronous = stream->Read<bool>();
  auto is_buffered = stream->Read<bool>();

  // XELOGD("XFile {:08X} ({})", file->handle(), abs_path);

  vfs::File* vfs_file = nullptr;
  vfs::FileAction action;
  auto res = kernel_state->file_system()->OpenFile(
      nullptr, abs_path, vfs::FileDisposition::kOpen, access, is_directory,
      false, &vfs_file, &action);
  if (XFAILED(res)) {
    // XELOGE("Failed to open XFile: error {:08X}", res);
    return object_ref<XFile>(file);
  }

  file->file_ = vfs_file;
  file->position_ = position;
  file->is_synchronous_ = is_synchronous;
  file->is_buffered_ = is_buffered;

  return object_ref<XFile>(file);
}

void XFile::NotifyCompletion(X_STATUS status, uint32_t num_bytes,
                             uint32_t apc_context) {
  // A request without an APC context posts no packet.
  if (apc_context) {
    XIOCompletion::IONotification notify;
    notify.apc_context = apc_context;
    notify.num_bytes = num_bytes;
    notify.status = status;
    NotifyIOCompletionPorts(notify);
  }
  async_event_->Set();
}

void XFile::NotifyIOCompletionPorts(
    XIOCompletion::IONotification& notification) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto port : completion_ports_) {
    notification.key_context = port.first;
    port.second->QueueNotification(notification);
  }
}

}  // namespace kernel
}  // namespace xe
