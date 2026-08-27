/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#endif  // XE_PLATFORM_MAC

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_ANDROID
#include <dlfcn.h>
#include <linux/ashmem.h>
#include <string.h>
#include <sys/ioctl.h>

#include "xenia/base/main_android.h"
#endif

#if XE_PLATFORM_GNU_LINUX
// Needs Linux 6.3 / glibc 2.38 headers.
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

DEFINE_bool(use_shm_open, false,
            "Back guest memory and the code cache with a /dev/shm file instead "
            "of memfd.\n"
            "Exposes both as named files that other processes can open to "
            "inspect guest memory or JIT output while a title runs.",
            "Linux");
#endif  // XE_PLATFORM_GNU_LINUX

namespace xe {
namespace memory {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ =
          reinterpret_cast<decltype(android_ASharedMemory_create_)>(
              dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  static const size_t page_size_ = static_cast<size_t>(getpagesize());
  return page_size_;
}
size_t allocation_granularity() { return page_size(); }

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

PageAccess ToXeniaProtectFlags(const char* protection) {
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == 'x') {
    return PageAccess::kExecuteReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == 'x') {
    return PageAccess::kExecuteReadOnly;
  }
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == '-') {
    return PageAccess::kReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == '-') {
    return PageAccess::kReadOnly;
  }
  return PageAccess::kNoAccess;
}

#if XE_PLATFORM_MAC
bool IsWritableExecutableMemorySupported() {
  // macOS allows RWX only on anonymous MAP_JIT regions. Callers that see
  // true must allocate via AllocFixed (which sets MAP_JIT) and toggle
  // pthread_jit_write_protect_np around writes. MAP_JIT requires the
  // com.apple.security.cs.allow-jit entitlement; without it the probe
  // fails and JIT is disabled.
  static const bool supported = []() {
    const size_t test_size = page_size();
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_JIT
    flags |= MAP_JIT;
#endif
    void* test_mapping = mmap(nullptr, test_size,
                              PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (test_mapping == MAP_FAILED) {
      XELOGE("MAP_JIT probe failed: {} ({}); JIT will not work",
             strerror(errno), errno);
      return false;
    }
    munmap(test_mapping, test_size);
    return true;
  }();
  return supported;
}
#else
bool IsWritableExecutableMemorySupported() { return true; }
#endif  // XE_PLATFORM_MAC

struct MappedFileRange {
  uintptr_t region_begin;
  uintptr_t region_end;
};

std::vector<MappedFileRange> mapped_file_ranges;
std::mutex g_mapped_file_ranges_mutex;

// Track shm file names for cleanup on exit
std::vector<std::string> g_shm_file_names;
std::mutex g_shm_file_names_mutex;
static bool g_cleanup_handlers_installed = false;

#if !XE_PLATFORM_ANDROID
static void CleanupAtExit() {
  for (const auto& name : g_shm_file_names) {
    shm_unlink(name.c_str());
  }
}

static void InstallCleanupHandlers() {
  if (g_cleanup_handlers_installed) {
    return;
  }
  g_cleanup_handlers_installed = true;

  std::atexit(CleanupAtExit);
  std::at_quick_exit(CleanupAtExit);
}
#endif  // !XE_PLATFORM_ANDROID

// Lets a Win32-style length-0 release find the reservation's extent.
static std::mutex g_reservations_mutex;
static std::unordered_map<void*, size_t> g_reservations;

static void RememberReservation(void* base_address, size_t length) {
  std::lock_guard guard(g_reservations_mutex);
  g_reservations[base_address] = length;
}

// Erase before munmap: a concurrent AllocFixed may reuse the address.
static size_t TakeReservationLength(void* base_address) {
  std::lock_guard guard(g_reservations_mutex);
  auto it = g_reservations.find(base_address);
  if (it == g_reservations.end()) {
    return 0;
  }
  const size_t length = it->second;
  g_reservations.erase(it);
  return length;
}

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  // mmap does not support reserve / commit, so ignore allocation_type.
  uint32_t prot = ToPosixProtectFlags(access);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

#if XE_PLATFORM_MAC
  if (access == PageAccess::kExecuteReadWrite ||
      access == PageAccess::kExecuteReadOnly) {
    flags |= MAP_JIT;
  }
#endif  // XE_PLATFORM_MAC

  if (base_address != nullptr) {
    if (allocation_type == AllocationType::kCommit) {
#if XE_PLATFORM_MAC
      size_t host_page = page_size();
      uintptr_t aligned_addr =
          reinterpret_cast<uintptr_t>(base_address) & ~(host_page - 1);
      uintptr_t end_addr = reinterpret_cast<uintptr_t>(base_address) + length;
      end_addr = (end_addr + host_page - 1) & ~(host_page - 1);
      if (mprotect(reinterpret_cast<void*>(aligned_addr),
                   end_addr - aligned_addr, prot) == 0) {
        return base_address;
      }
      return nullptr;
#else
      if (Protect(base_address, length, access)) {
        return base_address;
      }
      return nullptr;
#endif  // XE_PLATFORM_MAC
    }
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
  }

  void* result = mmap(base_address, length, prot, flags, -1, 0);

  if (result == MAP_FAILED) {
    return nullptr;
  }

  if (base_address != nullptr && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  RememberReservation(result, length);
  return result;
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  const auto region_begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t region_end =
      reinterpret_cast<uintptr_t>(base_address) + length;

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (const auto& mapped_range : mapped_file_ranges) {
    if (region_begin >= mapped_range.region_begin &&
        region_begin < mapped_range.region_end &&
        region_end <= mapped_range.region_end) {
      switch (deallocation_type) {
        case DeallocationType::kDecommit:
          return Protect(base_address, length, PageAccess::kNoAccess);
        case DeallocationType::kRelease:
          return false;
        default:
          assert_unhandled_case(deallocation_type);
      }
    }
  }

  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      return Protect(base_address, length, PageAccess::kNoAccess);
    case DeallocationType::kRelease: {
      // memory_win.cc passes length 0 for MEM_RELEASE; munmap rejects 0.
      const size_t recorded = TakeReservationLength(base_address);
      const size_t release_length = length ? length : recorded;
      if (!release_length) {
        XELOGE(
            "DeallocFixed: release of {} with length 0, but that address is "
            "not a known reservation; refusing to guess",
            base_address);
        return false;
      }
      if (munmap(base_address, release_length) != 0) {
        if (recorded) {
          RememberReservation(base_address, recorded);
        }
        return false;
      }
      return true;
    }
    default:
      assert_unhandled_case(deallocation_type);
  }
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    size_t length_copy = length;
    if (!QueryProtect(base_address, length_copy, *out_old_access)) {
      // Fail closed: callers restore whatever this reports.
      *out_old_access = PageAccess::kNoAccess;
      XELOGW(
          "Protect: could not read the current protection of {}; reporting "
          "kNoAccess",
          base_address);
    }
  }

  uint32_t prot = ToPosixProtectFlags(access);
  int ret = mprotect(base_address, length, prot);
  if (ret != 0) {
    XELOGE("mprotect({}, 0x{:X}, {}) failed: {} ({})", base_address, length,
           prot, strerror(errno), errno);
  }
  return ret == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;
  length = 0;
#if XE_PLATFORM_MAC
  mach_vm_address_t address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name;

  kern_return_t kr = mach_vm_region(
      mach_task_self(), &address, &region_size, VM_REGION_BASIC_INFO_64,
      reinterpret_cast<vm_region_info_t>(&info), &info_count, &object_name);

  if (kr != KERN_SUCCESS) {
    return false;
  }

  if (address > reinterpret_cast<mach_vm_address_t>(base_address)) {
    return false;
  }

  length =
      static_cast<size_t>((address + region_size) -
                          reinterpret_cast<mach_vm_address_t>(base_address));

  if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
      (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadWrite;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_EXECUTE)) ==
             (VM_PROT_READ | VM_PROT_EXECUTE)) {
    access_out = PageAccess::kExecuteReadOnly;
  } else if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) ==
             (VM_PROT_READ | VM_PROT_WRITE)) {
    access_out = PageAccess::kReadWrite;
  } else if (info.protection & VM_PROT_READ) {
    access_out = PageAccess::kReadOnly;
  } else {
    access_out = PageAccess::kNoAccess;
  }

  return true;
#else
  // No generic POSIX solution exists. The Linux solution should work on all
  // Linux kernel based OS, including Android.
  std::ifstream memory_maps;
  memory_maps.open("/proc/self/maps", std::ios_base::in);
  std::string maps_entry_string;

  while (std::getline(memory_maps, maps_entry_string)) {
    std::stringstream entry_stream(maps_entry_string);
    uintptr_t map_region_begin, map_region_end;
    char separator;
    char protection[5];  // 4 chars (e.g., "r-xp") + null terminator

    entry_stream >> std::hex >> map_region_begin >> separator >>
        map_region_end >> protection;

    if (map_region_begin <= reinterpret_cast<uintptr_t>(base_address) &&
        map_region_end > reinterpret_cast<uintptr_t>(base_address)) {
      length = map_region_end - reinterpret_cast<uintptr_t>(base_address);

      access_out = ToXeniaProtectFlags(protection);

      // Look at the next consecutive mappings
      while (std::getline(memory_maps, maps_entry_string)) {
        std::stringstream next_entry_stream(maps_entry_string);
        uintptr_t next_map_region_begin, next_map_region_end;
        char next_protection[5];  // 4 chars (e.g., "r-xp") + null terminator

        next_entry_stream >> std::hex >> next_map_region_begin >> separator >>
            next_map_region_end >> next_protection;
        if (map_region_end == next_map_region_begin &&
            access_out == ToXeniaProtectFlags(next_protection)) {
          length =
              next_map_region_end - reinterpret_cast<uintptr_t>(base_address);
          map_region_end = next_map_region_end;
          continue;
        }
        break;
      }

      memory_maps.close();
      return true;
    }
  }

  memory_maps.close();
  return false;
#endif  // XE_PLATFORM_MAC
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
#if XE_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), xe::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return ashmem_fd;
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT | O_EXCL;

#if XE_PLATFORM_MAC
  std::string shm_name = "/" + path.filename().string();
  if (shm_name.size() > 30) {
    std::size_t h = std::hash<std::string>{}(shm_name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    shm_name = hash_buf;
  }
  int ret = shm_open(shm_name.c_str(), oflag, 0600);
  if (ret < 0) {
    XELOGE("shm_open({}) failed: {} ({})", shm_name, strerror(errno), errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", shm_name, length,
           strerror(errno), errno);
    close(ret);
    shm_unlink(shm_name.c_str());
    return kFileMappingHandleInvalid;
  }
  // Track for cleanup on abnormal exit and install cleanup handlers
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    g_shm_file_names.push_back(shm_name);
  }
  InstallCleanupHandlers();
  return ret;
#else
  auto full_path = "/" / path;
#if XE_PLATFORM_GNU_LINUX
  // Prefer memfd: unlike /dev/shm it is unaffected by noexec mounts, LSM
  // policy or a container's --shm-size, and the kernel reclaims it on exit so
  // it needs no crash cleanup.
  if (!cvars::use_shm_open) {
    const bool needs_exec = access == PageAccess::kExecuteReadOnly ||
                            access == PageAccess::kExecuteReadWrite;
    int memfd =
        memfd_create(path.c_str(), MFD_CLOEXEC | (needs_exec ? MFD_EXEC : 0u));
    if (memfd < 0 && needs_exec && errno == EINVAL) {
      // Pre-6.3 kernels reject the unknown flag but map exec anyway.
      memfd = memfd_create(path.c_str(), MFD_CLOEXEC);
    }
    if (memfd >= 0) {
      if (ftruncate(memfd, length) < 0) {
        XELOGE("ftruncate(memfd {}, 0x{:X}) failed: {} ({})", path.string(),
               length, strerror(errno), errno);
        close(memfd);
        return kFileMappingHandleInvalid;
      }
      return memfd;
    }
    XELOGW("memfd_create({}) failed: {} ({}), falling back to shm_open",
           path.string(), strerror(errno), errno);
  }
#endif  // XE_PLATFORM_GNU_LINUX
  int ret = shm_open(full_path.c_str(), oflag, 0600);
  if (ret < 0) {
    XELOGE("shm_open({}) failed: {} ({})", full_path.string(), strerror(errno),
           errno);
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    XELOGE("ftruncate({}, 0x{:X}) failed: {} ({})", full_path.string(), length,
           strerror(errno), errno);
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  // Track for cleanup on abnormal exit and install cleanup handlers
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    g_shm_file_names.push_back(full_path.string());
  }
  InstallCleanupHandlers();
  return ret;
#endif  // XE_PLATFORM_MAC
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  close(handle);
#if !XE_PLATFORM_ANDROID
#if XE_PLATFORM_MAC
  std::string shm_name = "/" + path.filename().string();
  if (shm_name.size() > 30) {
    std::size_t h = std::hash<std::string>{}(shm_name);
    char hash_buf[24];
    std::snprintf(hash_buf, sizeof(hash_buf), "/%016zx", h);
    shm_name = hash_buf;
  }
  shm_unlink(shm_name.c_str());
  // Remove from tracking
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    auto it = std::ranges::find(g_shm_file_names, shm_name);
    if (it != g_shm_file_names.end()) {
      g_shm_file_names.erase(it);
    }
  }
#else
  auto full_path = "/" / path;
  // Only shm_open mappings have a name to unlink; a memfd dies with the close
  // above and was never tracked.
  bool tracked = false;
  {
    std::lock_guard guard(g_shm_file_names_mutex);
    auto it = std::ranges::find(g_shm_file_names, full_path.string());
    if (it != g_shm_file_names.end()) {
      g_shm_file_names.erase(it);
      tracked = true;
    }
  }
  if (tracked) {
    shm_unlink(full_path.c_str());
  }
#endif  // XE_PLATFORM_MAC
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  uint32_t prot = ToPosixProtectFlags(access);

  int flags = MAP_SHARED;
  if (base_address != nullptr) {
#ifdef MAP_FIXED_NOREPLACE
    flags |= MAP_FIXED_NOREPLACE;
#endif
  }

  void* result = mmap(base_address, length, prot, flags, handle, file_offset);

  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Without MAP_FIXED_NOREPLACE (e.g. macOS), a non-null base_address is just
  // a hint. Enforce the caller's contract by failing on address mismatch so
  // callers can retry at a different base, matching AllocFixed's behavior.
  if (base_address != nullptr && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  mapped_file_ranges.push_back({reinterpret_cast<uintptr_t>(result),
                                reinterpret_cast<uintptr_t>(result) + length});
  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  std::lock_guard guard(g_mapped_file_ranges_mutex);

#if XE_PLATFORM_MAC
  uintptr_t unmap_begin = reinterpret_cast<uintptr_t>(base_address);
  uintptr_t unmap_end = unmap_begin + length;

  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end(); ++mapped_range) {
    if (unmap_begin >= mapped_range->region_begin &&
        unmap_end <= mapped_range->region_end) {
      uintptr_t orig_begin = mapped_range->region_begin;
      uintptr_t orig_end = mapped_range->region_end;
      mapped_file_ranges.erase(mapped_range);

      if (orig_begin < unmap_begin) {
        mapped_file_ranges.push_back({orig_begin, unmap_begin});
      }
      if (unmap_end < orig_end) {
        mapped_file_ranges.push_back({unmap_end, orig_end});
      }

      return munmap(base_address, length) == 0;
    }
  }

  return munmap(base_address, length) == 0;
#else
  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end();) {
    if (mapped_range->region_begin ==
            reinterpret_cast<uintptr_t>(base_address) &&
        mapped_range->region_end ==
            reinterpret_cast<uintptr_t>(base_address) + length) {
      mapped_file_ranges.erase(mapped_range);
      return munmap(base_address, length) == 0;
    }
    ++mapped_range;
  }
  // TODO: Implement partial file unmapping.
  assert_always("Error: Partial unmapping of files not yet supported.");
  return munmap(base_address, length) == 0;
#endif  // XE_PLATFORM_MAC
}

}  // namespace memory
}  // namespace xe
