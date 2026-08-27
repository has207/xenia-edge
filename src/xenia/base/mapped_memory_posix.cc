/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/mapped_memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>

#include "xenia/base/filesystem.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"

namespace xe {

class PosixMappedMemory : public MappedMemory {
 public:
  PosixMappedMemory(void* data, size_t size, int file_descriptor,
                    void* map_base, size_t map_length)
      : MappedMemory(data, size),
        file_descriptor_(file_descriptor),
        map_base_(map_base),
        map_length_(map_length) {}

  ~PosixMappedMemory() override {
    if (map_base_) {
      munmap(map_base_, map_length_);
    }
    if (file_descriptor_ >= 0) {
      close(file_descriptor_);
    }
  }

  static std::unique_ptr<PosixMappedMemory> WrapFileDescriptor(
      int file_descriptor, Mode mode, size_t offset = 0, size_t length = 0) {
    int protection = 0;
    switch (mode) {
      case Mode::kRead:
        protection |= PROT_READ;
        break;
      case Mode::kReadWrite:
        protection |= PROT_READ | PROT_WRITE;
        break;
    }

    struct stat file_stat;
    if (fstat(file_descriptor, &file_stat)) {
      close(file_descriptor);
      return nullptr;
    }
    uint64_t file_size = uint64_t(file_stat.st_size);

    size_t map_length = length;
    if (!length) {
      map_length = size_t(file_size);
    }

    if (!map_length) {
      close(file_descriptor);
      return nullptr;
    }

    // mmap past EOF faults with SIGBUS; grow the file as CreateFileMapping
    // does.
    if (mode == Mode::kReadWrite && offset + map_length > file_size) {
      if (ftruncate(file_descriptor, off_t(offset + map_length))) {
        close(file_descriptor);
        return nullptr;
      }
    }

    const size_t page = xe::memory::page_size();
    const size_t aligned_offset = offset & ~(page - 1);
    const size_t delta = offset - aligned_offset;
    const size_t map_span = map_length + delta;

    void* map_base = mmap(0, map_span, protection, MAP_SHARED, file_descriptor,
                          off_t(aligned_offset));
    if (map_base == MAP_FAILED) {
      close(file_descriptor);
      return nullptr;
    }

    void* data = static_cast<uint8_t*>(map_base) + delta;
    return std::make_unique<PosixMappedMemory>(
        data, map_length, file_descriptor, map_base, map_span);
  }

  void Close(uint64_t truncate_size) override {
    if (map_base_) {
      munmap(map_base_, map_length_);
      map_base_ = nullptr;
      data_ = nullptr;
    }
    if (file_descriptor_ >= 0) {
      if (truncate_size) {
        ftruncate(file_descriptor_, truncate_size);
      }
      close(file_descriptor_);
      file_descriptor_ = -1;
    }
  }

  void Flush() override { msync(map_base_, map_length_, MS_ASYNC); }

 private:
  int file_descriptor_;
  void* map_base_ = nullptr;
  size_t map_length_ = 0;
};

std::unique_ptr<MappedMemory> MappedMemory::Open(
    const std::filesystem::path& path, Mode mode, size_t offset,
    size_t length) {
  int open_flags = 0;
  switch (mode) {
    case Mode::kRead:
      open_flags |= O_RDONLY;
      break;
    case Mode::kReadWrite:
      open_flags |= O_RDWR;
      break;
  }
  int file_descriptor = open(path.c_str(), open_flags);
  if (file_descriptor < 0) {
    return nullptr;
  }
  return PosixMappedMemory::WrapFileDescriptor(file_descriptor, mode, offset,
                                               length);
}

#if XE_PLATFORM_ANDROID
std::unique_ptr<MappedMemory> MappedMemory::OpenForAndroidContentUri(
    const std::string_view uri, Mode mode, size_t offset, size_t length) {
  const char* open_mode = nullptr;
  switch (mode) {
    case Mode::kRead:
      open_mode = "r";
      break;
    case Mode::kReadWrite:
      open_mode = "rw";
      break;
  }
  int file_descriptor =
      xe::filesystem::OpenAndroidContentFileDescriptor(uri, open_mode);
  if (file_descriptor < 0) {
    return nullptr;
  }
  return PosixMappedMemory::WrapFileDescriptor(file_descriptor, mode, offset,
                                               length);
}
#endif  // XE_PLATFORM_ANDROID

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size,
    bool low_address_space) {
  // TODO(DrChat)
  return nullptr;
}

}  // namespace xe
