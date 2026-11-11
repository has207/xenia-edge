/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/guest_pointers.h"
namespace xe {
class ByteStream;
}  // namespace xe

namespace xe {

class Memory;

enum SystemHeapFlag : uint32_t {
  kSystemHeapVirtual = 1 << 0,
  kSystemHeapPhysical = 1 << 1,

  kSystemHeapDefault = kSystemHeapVirtual,
};

enum class HeapType : uint8_t {
  kGuestVirtual,
  kGuestXex,
  kGuestPhysical,
  kHostPhysical,
};

enum MemoryAllocationFlag : uint32_t {
  kMemoryAllocationReserve = 1 << 0,
  kMemoryAllocationCommit = 1 << 1,
};

enum MemoryProtectFlag : uint32_t {
  kMemoryProtectRead = 1 << 0,
  kMemoryProtectWrite = 1 << 1,
  kMemoryProtectNoCache = 1 << 2,
  kMemoryProtectWriteCombine = 1 << 3,

  kMemoryProtectNoAccess = 0,
};

// Write-combine memory is CPU-writable (for GPU uploads), so treat it
// as writable alongside the regular write flag.
inline bool IsWritableProtect(uint32_t protect) {
  return (protect & kMemoryProtectWrite) ||
         (protect & kMemoryProtectWriteCombine);
}

inline xe::memory::PageAccess ToPageAccess(uint32_t protect) {
  bool is_writable = IsWritableProtect(protect);

  if ((protect & kMemoryProtectRead) && !is_writable) {
    return xe::memory::PageAccess::kReadOnly;
  } else if ((protect & kMemoryProtectRead) && is_writable) {
    return xe::memory::PageAccess::kReadWrite;
  } else {
    return xe::memory::PageAccess::kNoAccess;
  }
}

// Equivalent to the Win32 MEMORY_BASIC_INFORMATION struct.
struct HeapAllocationInfo {
  // A pointer to the base address of the region of pages.
  uint32_t base_address;
  // A pointer to the base address of a range of pages allocated by the
  // VirtualAlloc function. The page pointed to by the BaseAddress member is
  // contained within this allocation range.
  uint32_t allocation_base;
  // The memory protection option when the region was initially allocated.
  uint32_t allocation_protect;
  // The size specified when the region was initially allocated, in bytes.
  uint32_t allocation_size;
  // The size of the region beginning at the base address in which all pages
  // have identical attributes, in bytes.
  uint32_t region_size;
  // The state of the pages in the region (commit/free/reserve).
  uint32_t state;
  // The access protection of the pages in the region.
  uint32_t protect;
};

// Describes a single page in the page table.
union PageEntry {
  uint64_t qword;
  struct {
    // Base address of the allocated region in 4k pages.
    uint32_t base_address : 20;
    // Total number of pages in the allocated region in 4k pages.
    uint32_t region_page_count : 20;
    // Protection bits specified during region allocation.
    // Composed of bits from MemoryProtectFlag.
    uint32_t allocation_protect : 4;
    // Current protection bits as of the last Protect.
    // Composed of bits from MemoryProtectFlag.
    uint32_t current_protect : 4;
    // Allocation state of the page as a MemoryAllocationFlag bit mask.
    uint32_t state : 2;
    // Set once a commit has initialized the page. Decommit and release keep it
    // because the page holds data from then on.
    uint32_t initialized : 1;
    uint32_t reserved : 13;
  };
};

// Heap abstraction for page-based allocation.
class BaseHeap {
 public:
  virtual ~BaseHeap();

  // Offset of the heap in relative to membase, without host_address_offset
  // adjustment.
  uint32_t heap_base() const { return heap_base_; }

  // Length of the heap range.
  uint32_t heap_size() const { return heap_size_; }

  // Size of each page within the heap range in bytes.
  uint32_t page_size() const { return page_size_; }

  // Amount of pages assigned to heap
  uint32_t total_page_count() const { return uint32_t(page_table_.size()); }

  // Sum of unreserved pages in heap
  uint32_t unreserved_page_count() const { return unreserved_page_count_; }

  // Sum of reserved pages in heap
  uint32_t reserved_page_count() const {
    return total_page_count() - unreserved_page_count();
  }

  // Type of specified heap
  HeapType heap_type() const { return heap_type_; }

  // Set only via Memory::SetPhysicalAliasSkipHostProtect.
  bool skip_host_protect() const { return skip_host_protect_; }
  void set_skip_host_protect(bool value) { skip_host_protect_ = value; }

  // Offset added to the virtual addresses to convert them to host addresses
  // (not including membase).
  uint32_t host_address_offset() const { return host_address_offset_; }

  template <typename T = uint8_t*>
  inline T TranslateRelative(size_t relative_address) const {
    return reinterpret_cast<T>(membase_ + heap_base_ + host_address_offset_ +
                               relative_address);
  }

  // Disposes and decommits all memory and clears the page table.
  virtual void Dispose();

  // Dumps information about all allocations within the heap to the log.
  void DumpMap();

  // Allocates pages with the given properties and allocation strategy.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if not enough contiguous pages can be found.
  virtual bool Alloc(uint32_t size, uint32_t alignment,
                     uint32_t allocation_type, uint32_t protect, bool top_down,
                     uint32_t* out_address);

  // Allocates pages at the given address.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if the pages are already allocated.
  virtual bool AllocFixed(uint32_t base_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type,
                          uint32_t protect);

  // Allocates pages at an address within the given address range.
  // This can reserve and commit the pages as well as set protection modes.
  // This will fail if not enough contiguous pages can be found.
  virtual bool AllocRange(uint32_t low_address, uint32_t high_address,
                          uint32_t size, uint32_t alignment,
                          uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address);

  virtual bool AllocSystemHeap(uint32_t size, uint32_t alignment,
                               uint32_t allocation_type, uint32_t protect,
                               bool top_down, uint32_t* out_address);
  // Decommits pages in the given range.
  // Partial overlapping pages will also be decommitted.
  virtual bool Decommit(uint32_t address, uint32_t size);

  // Decommits and releases pages in the given range.
  // Partial overlapping pages will also be released.
  virtual bool Release(uint32_t address, uint32_t* out_region_size = nullptr);

  // Releases whole pages inside one region. The pages before and after them
  // become regions of their own.
  bool ReleaseRange(uint32_t address, uint32_t size);

  // The runs of the range whose pages no commit has initialized since the heap
  // was reset. Marks the whole range initialized.
  std::vector<std::pair<uint32_t, uint32_t>> TakeUninitializedRuns(
      uint32_t address, uint32_t size);

  // Modifies the protection mode of pages within the given range.
  virtual bool Protect(uint32_t address, uint32_t size, uint32_t protect,
                       uint32_t* old_protect = nullptr);

  // Queries information about the given region of pages.
  // The region size stops growing once it reaches max_region_size, for a
  // caller only asking about a range within it.
  bool QueryRegionInfo(uint32_t base_address, HeapAllocationInfo* out_info,
                       uint32_t max_region_size = UINT32_MAX);

  // Queries the size of the region containing the given address.
  bool QuerySize(uint32_t address, uint32_t* out_size);

  // Queries the base and size of a region containing the given address.
  bool QueryBaseAndSize(uint32_t* in_out_address, uint32_t* out_size);

  // Queries the current protection mode of the region containing the given
  // address.
  bool QueryProtect(uint32_t address, uint32_t* out_protect);

  // Whether the page holding |address| is committed.
  bool IsPageCommitted(uint32_t address);

  // True when no allocation covers any page in the range.
  virtual bool IsRangeUnallocated(uint32_t address, uint32_t size);

  // The most permissive access of the committed pages covering the
  // heap-relative range. Doesn't take the global lock, for callers holding it.
  xe::memory::PageAccess CommittedRangeAccess(uint32_t relative_address,
                                              uint32_t length) const {
    uint32_t page_count = uint32_t(page_table_.size());
    uint32_t page_first = relative_address >> page_size_shift_;
    if (!length || page_first >= page_count) {
      return xe::memory::PageAccess::kNoAccess;
    }
    uint32_t page_last = (relative_address + (length - 1)) >> page_size_shift_;
    if (page_last >= page_count) {
      page_last = page_count - 1;
    }
    xe::memory::PageAccess access = xe::memory::PageAccess::kNoAccess;
    for (uint32_t i = page_first; i <= page_last; ++i) {
      if (!(page_table_[i].state & kMemoryAllocationCommit)) {
        continue;
      }
      xe::memory::PageAccess page_access =
          ToPageAccess(page_table_[i].current_protect);
      if (page_access == xe::memory::PageAccess::kReadWrite) {
        return xe::memory::PageAccess::kReadWrite;
      }
      if (page_access == xe::memory::PageAccess::kReadOnly) {
        access = xe::memory::PageAccess::kReadOnly;
      }
    }
    return access;
  }

  // Queries access across committed pages in the inclusive address range.
  xe::memory::PageAccess QueryRangeAccess(uint32_t low_address,
                                          uint32_t high_address);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  void Reset();

 protected:
  BaseHeap();

  void Initialize(Memory* memory, uint8_t* membase, HeapType heap_type,
                  uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                  uint32_t host_address_offset = 0);

  // Rebuilds free_blocks_ by scanning page_table_. Used after Restore.
  void RebuildFreeBlocks();

  // Applies `protect` to the host mapping backing the inclusive guest page
  // range. Handles a host page larger than the guest page by protecting whole
  // host pages with the most permissive access any guest page inside one
  // needs, so a neighbour's protection is never tightened. page_table_ is read
  // for pages outside the range, so it must still hold their current state.
  bool ApplyHostProtect(uint32_t start_page_number, uint32_t end_page_number,
                        uint32_t protect, uint32_t* old_protect);

  // Backs the guest page range on the host at commit time. Equivalent to a
  // host commit where the guest page size is host-page-aligned, and falls back
  // to ApplyHostProtect where it is not (4 KB guest pages on a 16 KB host).
  bool CommitHostPages(uint32_t start_page_number, uint32_t page_count,
                       uint32_t protect);

  // Frees the page range in page_table_ and the free block tracker.
  void ReleasePages(uint32_t start_page_number, uint32_t page_count);

  // Removes (or splits) the free block covering the given page range.
  void RemoveFreeBlock(uint32_t start_page, uint32_t page_count);

  // Inserts a free block and coalesces with adjacent free blocks.
  void InsertFreeBlock(uint32_t start_page, uint32_t page_count);

  // Guest protection updates page_table_ but not the host mapping. Set on the
  // physical alias while a GPU import holds a page pin over it.
  bool skip_host_protect_ = false;

  Memory* memory_;
  uint8_t* membase_;
  HeapType heap_type_;
  uint32_t heap_base_;
  uint32_t heap_size_;
  uint32_t page_size_;
  uint32_t page_size_shift_;
  uint32_t host_address_offset_;
  uint32_t unreserved_page_count_;
  xe::global_critical_region global_critical_region_;
  std::vector<PageEntry> page_table_;

  // Auxiliary free block tracker: maps start_page -> count of contiguous free
  // pages. Kept in sync with page_table_ mutations. Not serialized.
  std::map<uint32_t, uint32_t> free_blocks_;
};

// Normal heap allowing allocations from guest virtual address ranges.
class VirtualHeap : public BaseHeap {
 public:
  VirtualHeap();
  ~VirtualHeap() override;

  // Initializes the heap properties and allocates the page table.
  void Initialize(Memory* memory, uint8_t* membase, HeapType heap_type,
                  uint32_t heap_base, uint32_t heap_size, uint32_t page_size);
};

// A heap for ranges of memory that are mapped to physical ranges.
// Physical ranges are used by the audio and graphics subsystems representing
// hardware wired directly to memory in the console.
//
// The physical heap and the behavior of sharing pages with virtual pages is
// implemented by having a 'parent' heap that is used to perform allocation in
// the guest virtual address space 1:1 with the physical address space.
class PhysicalHeap : public BaseHeap {
 public:
  PhysicalHeap();
  ~PhysicalHeap() override;

  // Initializes the heap properties and allocates the page table.
  void Initialize(Memory* memory, uint8_t* membase, HeapType heap_type,
                  uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
                  VirtualHeap* parent_heap);

  bool Alloc(uint32_t size, uint32_t alignment, uint32_t allocation_type,
             uint32_t protect, bool top_down, uint32_t* out_address) override;
  bool AllocFixed(uint32_t base_address, uint32_t size, uint32_t alignment,
                  uint32_t allocation_type, uint32_t protect) override;
  bool AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                  uint32_t alignment, uint32_t allocation_type,
                  uint32_t protect, bool top_down,
                  uint32_t* out_address) override;
  bool AllocSystemHeap(uint32_t size, uint32_t alignment,
                       uint32_t allocation_type, uint32_t protect,
                       bool top_down, uint32_t* out_address) override;
  bool Decommit(uint32_t address, uint32_t size) override;
  bool Release(uint32_t base_address,
               uint32_t* out_region_size = nullptr) override;
  bool Protect(uint32_t address, uint32_t size, uint32_t protect,
               uint32_t* old_protect = nullptr) override;
  // Also false where another view of the physical memory has it allocated.
  bool IsRangeUnallocated(uint32_t address, uint32_t size) override;

  void EnableAccessCallbacks(uint32_t physical_address, uint32_t length,
                             bool enable_invalidation_notifications,
                             bool enable_data_providers);
  template <bool enable_invalidation_notifications, bool enable_data_providers>
  XE_NOINLINE void EnableAccessCallbacksInner(
      const uint32_t system_page_first, const uint32_t system_page_last,
      xe::memory::PageAccess protect_access) XE_RESTRICT;

  // Returns true if any page in the range was watched. With
  // invalidate_unwatched the callbacks are raised even when no watch is armed,
  // for a caller that knows the range is about to change rather than one
  // reacting to a fault.
  // contents_discarded is for a write that makes the old contents irrelevant,
  // like a fresh allocation, rather than one that may change only part of them.
  bool TriggerCallbacks(global_unique_lock_type global_lock_locked_once,
                        uint32_t virtual_address, uint32_t length,
                        bool is_write, bool unwatch_exact_range,
                        bool unprotect = true,
                        bool invalidate_unwatched = false,
                        bool contents_discarded = false);

  // For a fault on a read-watched page, with the global lock held once: calls
  // the read callbacks with it released, so waiting on the GPU there stalls
  // only the faulting thread. The watch stays armed, so TriggerCallbacks, under
  // the lock again, still decides the access.
  void ProvideReadWatchedPage(global_unique_lock_type& global_lock_locked_once,
                              uint32_t virtual_address, bool is_write);
  // Before a host write through the physical view: provides, as for a write
  // fault, the pages the range only partly covers, so the write lands over what
  // they are still waiting for.
  void ProvideReadWatchedEdgePages(uint32_t virtual_address, uint32_t length);

  uint32_t GetPhysicalAddress(uint32_t address) const;

  // The 0-512mb heap every physical allocation is recorded in.
  BaseHeap* parent_heap() const { return parent_heap_; }

  uint32_t SystemPagenumToGuestPagenum(uint32_t num) const {
    uint32_t system_base = num << system_page_shift_;
    uint32_t offset = host_address_offset();
    if (system_base < offset) {
      return 0;
    }
    return (system_base - offset) >> page_size_shift_;
  }

  // The most permissive guest access of the guest pages a system page covers.
  // Protection has system page granularity and BaseHeap::Protect resolves a
  // system page the same way, so anything deciding on protection has to agree
  // with it - the host page can be larger than the guest page. The physical
  // views alias the same memory, so where this view has nothing allocated, the
  // access is that of the allocation made through another view, which the
  // guest reaches here too. Inline, called per page in the arming loop.
  xe::memory::PageAccess SystemPageGuestAccess(
      uint32_t system_page_number) const {
    uint32_t offset = host_address_offset();
    uint32_t system_base = system_page_number << system_page_shift_;
    uint32_t system_last = system_base + (system_page_size_ - 1);
    if (system_last < offset) {
      return xe::memory::PageAccess::kNoAccess;
    }
    uint32_t guest_page_first =
        system_base > offset ? (system_base - offset) >> page_size_shift_ : 0;
    uint32_t guest_page_count = uint32_t(page_table_.size());
    if (guest_page_first >= guest_page_count) {
      return xe::memory::PageAccess::kNoAccess;
    }
    uint32_t guest_page_last = (system_last - offset) >> page_size_shift_;
    if (guest_page_last >= guest_page_count) {
      guest_page_last = guest_page_count - 1;
    }
    xe::memory::PageAccess access = xe::memory::PageAccess::kNoAccess;
    bool any_unallocated = false;
    for (uint32_t i = guest_page_first; i <= guest_page_last; ++i) {
      if (!page_table_[i].state) {
        any_unallocated = true;
        continue;
      }
      xe::memory::PageAccess page_access =
          ToPageAccess(page_table_[i].current_protect);
      if (page_access == xe::memory::PageAccess::kReadWrite) {
        return xe::memory::PageAccess::kReadWrite;
      }
      if (page_access == xe::memory::PageAccess::kReadOnly) {
        access = xe::memory::PageAccess::kReadOnly;
      }
    }
    if (any_unallocated) {
      uint32_t relative_address =
          system_base > offset ? system_base - offset : 0;
      xe::memory::PageAccess parent_access = parent_heap_->CommittedRangeAccess(
          GetPhysicalAddress(heap_base_) + relative_address,
          system_last - offset + 1 - relative_address);
      if (parent_access != xe::memory::PageAccess::kNoAccess) {
        access = parent_access;
      }
    }
    return access;
  }

 protected:
  VirtualHeap* parent_heap_;

  uint32_t system_page_size_;
  uint32_t system_page_count_;
  uint32_t system_page_shift_;
  uint32_t padding1_;

  struct SystemPageFlagsBlock {
    // Whether writing to each page should result trigger invalidation
    // callbacks.
    uint64_t notify_on_invalidation = 0;
    // Whether the first access of each page triggers read callbacks. These
    // pages are protected no-access. The watch is one-shot, cleared on access.
    uint64_t notify_on_read = 0;
  };
  // Protected by global_critical_region. Flags for each 64 system pages,
  // interleaved as blocks, so bit scan can be used to quickly extract ranges.
  std::vector<SystemPageFlagsBlock> system_page_flags_;
};

// Models the entire guest memory system on the console.
// This exposes interfaces to both virtual and physical memory and a TLB and
// page table for allocation, mapping, and protection.
//
// The memory is backed by a memory mapped file and is placed at a stable
// fixed address in the host address space (like 0x100000000). This allows
// efficient guest<->host address translations as well as easy sharing of the
// memory across various subsystems.
//
// The guest memory address space is split into several ranges that have varying
// properties such as page sizes, caching strategies, protections, and
// overlap with other ranges. Each range is represented by a BaseHeap of either
// VirtualHeap or PhysicalHeap depending on type. Heaps model the page tables
// and can handle reservation and committing of requested pages.
class Memory {
 public:
  Memory();
  ~Memory();

  // Initializes the memory system.
  // This may fail if the host address space could not be reserved or the
  // mapping to the file system fails.
  bool Initialize();

  // Resets all memory to zero and resets all allocations.
  void Reset();

  // Full file name and path of the memory-mapped file backing all memory.
  const std::filesystem::path& file_name() const { return file_name_; }

  // Base address of virtual memory in the host address space.
  // This is often something like 0x100000000.
  inline uint8_t* virtual_membase() const { return virtual_membase_; }

  // Translates a guest virtual address to a host address that can be accessed
  // as a normal pointer.
  // Note that the contents at the specified host address are big-endian.
  template <typename T = uint8_t*>
  inline T TranslateVirtual(uint32_t guest_address) const {
#if XE_PLATFORM_WIN32 == 1
    uint8_t* host_address = virtual_membase_ + guest_address;
    if (guest_address >= 0xE0000000) {
      host_address += 0x1000;
    }
    return reinterpret_cast<T>(host_address);
#else
    uint8_t* host_address = virtual_membase_ + guest_address;
    const auto heap = LookupHeap(guest_address);
    if (heap) {
      host_address += heap->host_address_offset();
    }
    return reinterpret_cast<T>(host_address);

#endif
  }
  template <typename T>
  inline T* TranslateVirtual(TypedGuestPointer<T> guest_address) {
    return TranslateVirtual<T*>(guest_address.m_ptr);
  }
  template <typename T>
  inline xe::be<T>* TranslateVirtualBE(uint32_t guest_address)
      XE_RESTRICT const {
    static_assert(!std::is_pointer_v<T> &&
                  sizeof(T) > 1);  // maybe assert is_integral?
    return TranslateVirtual<xe::be<T>*>(guest_address);
  }

  // Base address of physical memory in the host address space.
  // This is often something like 0x200000000.
  inline uint8_t* physical_membase() const { return physical_membase_; }

  // The file mapping backing all guest memory views. Lets a consumer map its
  // own separate view of guest RAM, e.g. to hand to a GPU heap import, without
  // colliding with the write-watch protection on the managed views.
  inline xe::memory::FileMappingHandle mapping_handle() const {
    return mapping_;
  }

  // Translates a guest physical address to a host address that can be accessed
  // as a normal pointer.
  // Note that the contents at the specified host address are big-endian.
  template <typename T = uint8_t*>
  inline T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ +
                               (guest_address & 0x1FFFFFFF));
  }

  // Translates a host address to a guest virtual address.
  // Note that the contents at the returned host address are big-endian.
  uint32_t HostToGuestVirtual(const void* host_address) const;

  // Returns the guest physical address for the guest virtual address, or
  // UINT32_MAX if it can't be obtained.
  uint32_t GetPhysicalAddress(uint32_t address) const;

  // Zeros out a range of memory at the given guest address.
  void Zero(uint32_t address, uint32_t size);

  // Fills a range of guest memory with the given byte value.
  void Fill(uint32_t address, uint32_t size, uint8_t value);

  // Copies a non-overlapping range of guest memory (like a memcpy).
  void Copy(uint32_t dest, uint32_t src, uint32_t size);

  // Searches the given range of guest memory for a run of dword values in
  // big-endian order.
  uint32_t SearchAligned(uint32_t start, uint32_t end, const uint32_t* values,
                         size_t value_count);

  // Defines a memory-mapped IO (MMIO) virtual address range that when accessed
  // will trigger the specified read and write callbacks for dword read/writes.
  bool AddVirtualMappedRange(uint32_t virtual_address, uint32_t mask,
                             uint32_t size, void* context,
                             cpu::MMIOReadCallback read_callback,
                             cpu::MMIOWriteCallback write_callback);

  // Gets the defined MMIO range for the given virtual address, if any.
  cpu::MMIORange* LookupVirtualMappedRange(uint32_t virtual_address);

  // Physical memory access callbacks, two types of them.
  //
  // This is simple per-system-page protection without reference counting or
  // stored ranges. Whenever a watched page is accessed, all callbacks for it
  // are triggered. Also the only way to remove callbacks is to trigger them
  // somehow. Since there are no references from pages to individual callbacks,
  // there's no way to disable only a specific callback for a page. Also
  // callbacks may be triggered spuriously, and handlers should properly ignore
  // pages they don't care about.
  //
  // Once callbacks are triggered for a page, the page is not watched anymore
  // until requested again later. It is, however, unwatched only in one guest
  // view of physical memory (because different views may have different
  // protection for the same memory) - but it's rare when the same memory is
  // used with different guest page sizes, and it's okay to fire a callback more
  // than once.
  //
  // Only accessing the guest virtual memory views of physical memory triggers
  // callbacks - data providers, for instance, must write to the host physical
  // heap directly, otherwise their threads may infinitely await themselves.
  //
  // - Invalidation notifications:
  //
  // Protecting from writing. One-shot callbacks for invalidation of various
  // kinds of physical memory caches (such as the GPU copy of the memory).
  //
  // May be triggered for a single page (in case of a write access violation or
  // when need to synchronize data given by data providers) or for multiple
  // pages (like when memory is released, or explicitly to trigger callbacks
  // when host-side code can't rely on regular access violations, like when
  // accessing a file).
  //
  // Since granularity of callbacks is one single page, an invalidation
  // notification handler must invalidate the all the data stored in the touched
  // pages.
  //
  // Because large ranges (like whole framebuffers) may be written to and
  // exceptions are expensive, it's better to unprotect multiple pages as a
  // result of a write access violation, so the shortest common range returned
  // by all the invalidation callbacks (clamped to a sane range and also not to
  // touch pages with provider callbacks) is unprotected.

  // Returns start and length of the smallest physical memory region surrounding
  // the watched region that can be safely unwatched, if it doesn't matter,
  // return (0, UINT32_MAX).
  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length,
      bool exact_range);
  // Returns a handle for unregistering or for skipping one notification handler
  // while triggering data providers.
  void* RegisterPhysicalMemoryInvalidationCallback(
      PhysicalMemoryInvalidationCallback callback, void* callback_context);
  // Unregisters a physical memory invalidation callback previously added with
  // RegisterPhysicalMemoryInvalidationCallback.
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);

  // How a read-watched page is being accessed.
  enum class PhysicalAccess {
    kRead,
    // May change only part of the contents, so the rest has to be current.
    kWrite,
    // Makes the old contents irrelevant - a fresh allocation, a release, or
    // data already written over the range by the host.
    kDiscard,
  };
  // Called on the first CPU access of a page armed as a read watch (via
  // EnablePhysicalMemoryAccessCallbacks with data providers). The page is
  // downgraded and unwatched right after, so it fires once per arm. A write
  // that drops read watches calls it too, before the write proceeds. A fault
  // calls it twice, first without the global critical region, where waiting
  // stalls only the faulting thread, then under it, where it may wait only for
  // what needs no other thread to progress, like already submitted GPU work.
  typedef void (*PhysicalMemoryReadCallback)(void* context_ptr,
                                             uint32_t physical_address_start,
                                             uint32_t length,
                                             PhysicalAccess access);
  void* RegisterPhysicalMemoryReadCallback(PhysicalMemoryReadCallback callback,
                                           void* callback_context);
  void UnregisterPhysicalMemoryReadCallback(void* callback_handle);

  // Enables physical memory access callbacks for the specified memory range,
  // snapped to system page boundaries.
  void EnablePhysicalMemoryAccessCallbacks(
      uint32_t physical_address, uint32_t length,
      bool enable_invalidation_notifications, bool enable_data_providers);

  // Keeps physical_membase_ writable while a GPU import pins it - any mprotect
  // there fails the next submit. Guest protection still applies to the physical
  // windows, and the alias never triggers access callbacks.
  void SetPhysicalAliasSkipHostProtect(bool skip);

  // Forces triggering of watch callbacks for a virtual address range if pages
  // are watched there and unwatching them. Returns whether any page was
  // watched. Must be called with global critical region locking depth of 1.
  // TODO(Triang3l): Implement data providers - this is why locking depth of 1
  // will be required in the future.
  bool TriggerPhysicalMemoryCallbacks(
      global_unique_lock_type global_lock_locked_once, uint32_t virtual_address,
      uint32_t length, bool is_write, bool unwatch_exact_range,
      bool unprotect = true);

  // Allocates virtual memory from the 'system' heap.
  // System memory is kept separate from game memory but is still accessible
  // using normal guest virtual addresses. Kernel structures and other internal
  // 'system' allocations should come from this heap when possible.
  uint32_t SystemHeapAlloc(uint32_t size, uint32_t alignment = 0x20,
                           uint32_t system_heap_flags = kSystemHeapDefault);

  // Frees memory allocated with SystemHeapAlloc.
  void SystemHeapFree(uint32_t address, uint32_t* out_region_size = nullptr);

  // Records the page table KeCreateUserMode was given, before the views exist.
  void SetUserPageTable(uint32_t descriptor_address);

  // What the page table says about a user mode address.
  enum class UserPageState {
    // Translated; out_physical_address holds the result.
    kMapped,
    // No entry for the page. The guest can fill one in.
    kNoEntry,
    // No table page covering the address. The guest can add one.
    kNoTable,
    // An entry names memory xenia doesn't keep physically, or there is no
    // table at all. Adding an entry won't help.
    kUnusable,
    // 4 KB entries that aren't physically contiguous across the 64 KB a host
    // view shows.
    kScattered,
    // The 64 KB a host view shows doesn't start on a physical 64 KB boundary,
    // as in the skewed segments past 0xE0000000.
    kUnaligned,
  };

  // How a user mode fault was handled.
  enum class UserFaultResult {
    // Not delivered to the guest.
    kNotTaken,
    // The guest's handler returned, so retry the access.
    kTaken,
    // |ex| was diverted, so the fault's own host frames are gone. User mode
    // continues elsewhere, or a thunk runs the handler and resumes the access
    // itself.
    kDiverted,
  };

  // Delivers a user mode access the page table can't satisfy to the guest.
  // |ex| gives the faulting host pc, and the hook may divert it.
  using UserFaultHook = UserFaultResult (*)(uint32_t fault_address,
                                            bool is_write, Exception* ex);
  void set_user_fault_hook(UserFaultHook hook) { user_fault_hook_.store(hook); }

  // Claims the address space user mode code runs in, which starts empty.
  bool EnableUserModeViews();

  // Drops every page mapped through the page table, which flushing the entire
  // TB does.
  void FlushUserPageTable();
  // Drops the pages mapped through the page table whose entries changed since
  // they were mapped, which flushing the user mode TB does.
  void FlushChangedUserPages();

  // Base of the user mode address space, null until it is created.
  inline uint8_t* user_virtual_membase() const {
    return user_virtual_membase_.load(std::memory_order_relaxed);
  }

  // On hosts whose pages hold more than one 4 KB page, JIT code translates
  // guest addresses through tables of host_address - guest_address per 4 KB
  // guest page, indexed by guest_address >> 12. Null on other hosts.
  static constexpr uint32_t kGuestTlbEntryCount = 0x100000;
  const uint64_t* kernel_tlb() const { return guest_tlbs_; }
  const uint64_t* user_tlb(bool write) const {
    return guest_tlbs_ ? guest_tlbs_ + kGuestTlbEntryCount * (write ? 2 : 1)
                       : nullptr;
  }
  // Whether the tables are filled, which happens with the user mode views.
  bool guest_tlbs_active() const {
    return guest_tlbs_ && user_virtual_membase_.load(std::memory_order_acquire);
  }

  // The kernel address with the same contents as a user mode address.
  uint32_t UserModeKernelAddress(uint32_t user_address) {
    uint32_t physical_address;
    if (TranslateUserPage(user_address, &physical_address) ==
        UserPageState::kMapped) {
      if (IsKernelVirtualFrame(physical_address)) {
        return physical_address - kKernelVirtualFrameBias;
      }
      // The 0xA0000000 window shows every physical address the table can name.
      return 0xA0000000 + physical_address;
    }
    if (user_address - kUserAliasBase < kUserAliasSize) {
      return user_address + 0x80000000;
    }
    return user_address;
  }

  // Host memory to read what a kernel address shows. A physical window's view
  // keeps the protection last set through that window, but the physical view
  // keeps the latest set through any of them, so read through that.
  const uint8_t* TranslateForRead(uint32_t kernel_address) {
    auto heap = LookupHeap(kernel_address);
    if (heap && heap->heap_type() == HeapType::kGuestPhysical) {
      return TranslatePhysical<const uint8_t*>(
          static_cast<PhysicalHeap*>(heap)->GetPhysicalAddress(kernel_address));
    }
    return TranslateVirtual<const uint8_t*>(kernel_address);
  }

  // The inverse of UserModeKernelAddress for the alias, which is all it covers.
  static uint32_t KernelModeUserAddress(uint32_t kernel_address) {
    if (kernel_address - 0xA0000000 < kUserAliasSize) {
      return kernel_address - 0x80000000;
    }
    return kernel_address;
  }

  // Gets the heap for the address space containing the given address.
  XE_NOALIAS
  const BaseHeap* LookupHeap(uint32_t address) const;
  XE_NOALIAS
  inline BaseHeap* LookupHeap(uint32_t address) {
    return const_cast<BaseHeap*>(
        const_cast<const Memory*>(this)->LookupHeap(address));
  }

  // Queries access across committed pages in a guest virtual address range.
  // Empty, overflowing, unmapped, or cross-heap ranges return kNoAccess.
  // Returns kReadWrite if all pages are readable and writable, kReadOnly if
  // all are readable, and kNoAccess otherwise.
  xe::memory::PageAccess QueryRangeAccess(uint32_t address, uint32_t length);

  // Gets the heap with the given properties.
  BaseHeap* LookupHeapByType(bool physical, uint32_t page_size);

  // Gets the physical base heap.
  VirtualHeap* GetPhysicalHeap();

  void GetHeapsPageStatsSummary(const BaseHeap* const* provided_heaps,
                                size_t heaps_count, uint32_t& unreserved_pages,
                                uint32_t& reserved_pages, uint32_t& used_pages,
                                uint32_t& reserved_bytes);

  // Dumps a map of all allocated memory to the log.
  void DumpMap();

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  void SetMMIOExceptionRecordingCallback(cpu::MmioAccessRecordCallback callback,
                                         void* context);

 private:
#if XE_PLATFORM_MAC
  int MapViewsMac(uint8_t* mapping_base);
#endif
  int MapViews(uint8_t* mapping_base);
  void UnmapViews();
  bool ClaimUserWindow(uint8_t* user_membase);

  // The file offset the user mode address space shows at an address.
  uint64_t UserViewFileOffset(uint32_t user_address) const;
  // The file offset the kernel address space shows at an address.
  uint64_t KernelViewFileOffset(uint32_t kernel_address) const;
  // The CPU adds 4 KB to addresses at 0xE0000000 and above when the host maps
  // at a coarser granularity so a window offset there is 4 KB above the
  // address the page table names.
  uint32_t UserWindowSkew(uint32_t window_offset) const {
    return (system_allocation_granularity_ > 0x1000 &&
            window_offset >= 0xE0000000 + 0x1000)
               ? 0x1000
               : 0;
  }
  // The kind byte the descriptor gives the segment of |user_address|.
  uint8_t UserSegmentKind(uint32_t user_address);
  // The physical address the page table translates a user mode address to,
  // the PP and changed bits of its entry, and the host address of the entry.
  UserPageState TranslateUserPage(uint32_t user_address,
                                  uint32_t* out_physical_address,
                                  uint32_t* out_protection = nullptr,
                                  uint8_t** out_entry = nullptr);
  // Whether one view of the 64 KB |page| starting at |physical_address| shows
  // what the 4 KB entries of a small segment name, with one protection.
  bool UserPageBlockIsContiguous(uint32_t page, uint32_t physical_address);
  // Maps the page a fault at |window_offset| falls in, under the lock.
  // Without |allow_fallback|, a page the table doesn't map stays unmapped.
  bool MapUserPage(uint32_t window_offset, bool allow_fallback);
  // Shows user mode page |index| per 4 KB piece, for entries no single view can
  // show. Entries the table doesn't have stay unshown and fault. Under the
  // lock.
  bool SplitUserPage(uint32_t index);
  // Shows one 4 KB piece of a split page, in a view of its own on 4 KB hosts
  // and through its host page on others. Under the lock.
  bool MapUserPiece(uint32_t piece, bool allow_fallback);
  // Unmaps user mode page |index| and forgets what its pieces showed. Under
  // the lock.
  void ReleaseUserPage(uint32_t index);
  // The descriptor and table words the translation of |piece| reads: kind,
  // table frame and entry. Under the lock.
  uint64_t UserPieceTableKey(uint32_t piece);
  // The file offset and access a piece shows, from its entry or, with
  // |allow_fallback|, the kernel address space. Under the lock.
  bool ResolveUserPiece(uint32_t piece, bool allow_fallback,
                        uint64_t* out_file_offset, bool* out_from_entry,
                        xe::memory::PageAccess* out_access);
  // Whether host pages hold more than one piece, so a split page is shown per
  // host page and the rest is emulated.
  bool UserHostPagesCoarse() const {
    return system_page_size_ > kUserSmallPageSize;
  }
  uint32_t UserPiecesPerHostPage() const {
    return system_page_size_ / kUserSmallPageSize;
  }
  // Maps the host page from |first_piece| as one view if its pieces show
  // consecutive file pages. Under the lock.
  void MapUserHostPage(uint32_t first_piece);
  void SetUserHostPagesView(uint32_t first_piece, uint32_t count, bool view);
  // Whether the host page holding |piece| is a view, always false on host pages
  // of one piece.
  bool UserHostPageIsView(uint32_t piece) const {
    if (!UserHostPagesCoarse()) {
      return false;
    }
    const uint32_t host_page = piece / UserPiecesPerHostPage();
    return (user_host_page_view_[host_page / 64] >> (host_page % 64)) & 1;
  }
  // Protects the host pages holding |count| pieces from |first_piece| to the
  // strictest access of their pieces. Under the lock.
  void ProtectUserHostPages(uint32_t first_piece, uint32_t count);
  // Protects the host memory showing |piece| after its access changed. Under
  // the lock.
  void ProtectUserPiece(uint32_t piece);
  // Commits the guest address tables and points every entry at the kernel
  // membase or |user_membase|.
  bool FillGuestTlbs(uint8_t* user_membase);
  // Points the user table entries of |count| pieces from |first_piece| at what
  // the pieces show and allow. Under the lock.
  void UpdateUserTlb(uint32_t first_piece, uint32_t count);
  // Handles a fault at |window_offset| in the user mode views. Under the lock.
  bool UserAccessViolation(global_unique_lock_type& global_lock_locked_once,
                           uint32_t window_offset, bool is_write,
                           Exception* ex);
  // Runs the faulting access to a piece that allows it against the memory the
  // piece shows. Under the lock.
  bool EmulateUserAccess(global_unique_lock_type& global_lock_locked_once,
                         Exception* ex);
  static uint8_t* TranslateEmulatedUserAccess(void* context, uint64_t address,
                                              bool is_write, size_t* span,
                                              bool* natively_readable);
  // Records the physical memory |count| user mode pieces from |first_piece|
  // show. Under the lock.
  void TrackUserPieces(uint32_t first_piece, uint32_t count,
                       uint64_t file_offset);
  // The host access an entry's PP bits give user mode code.
  xe::memory::PageAccess UserEntryAccess(uint32_t protection) const;
  // The host access a user mode piece gets from its entry, including its
  // changed bit, and the write watch on the physical page it shows. Under the
  // lock.
  xe::memory::PageAccess UserPieceAccess(uint32_t piece) const;
  // Protects |count| pieces from |first_piece| to their UserPieceAccess,
  // read-write ones included. Under the lock.
  void ProtectUserPieces(uint32_t first_piece, uint32_t count);
  // Write protects the range's physical pages in every user mode page that
  // shows them. Takes the lock.
  void WatchUserWrites(uint32_t physical_address, uint32_t length);
  // Reports a write fault at |window_offset| on a watched page. Under the lock.
  // Returns whether the page was watched.
  bool TriggerUserWriteWatch(uint32_t window_offset);
  // Sets the changed bit of the guest's entry at |entry|.
  static void SetUserEntryChanged(uint8_t* entry);
  // Records whether |piece|, just mapped from the entry for |user_address|,
  // waits for a write to set the entry's changed bit. Under the lock.
  void TrackUserPieceChange(uint32_t piece, uint32_t user_address);
  // Sets the changed bit of the entry behind a write fault at |window_offset|
  // and lets the pieces it maps be written. Under the lock. Returns whether the
  // piece was waiting for that write.
  bool MarkUserPieceChanged(uint32_t window_offset);
  // Calls |fn(piece)| for each user mode piece showing a physical 4 KB page.
  // Under the lock.
  template <typename Fn>
  void ForEachUserPieceShowing(uint32_t physical_page, Fn fn) {
    for (uint32_t piece :
         user_block_pieces_[physical_page / kUserSmallPagesPerView]) {
      if (user_piece_physical_[piece] == physical_page) {
        fn(piece);
      }
    }
  }

  static constexpr uint32_t kUserAliasBase = 0x20000000;
  static constexpr uint32_t kUserAliasSize = 0x20000000;

  // The host allocation granularity, which is what one view shows.
  static constexpr uint32_t kUserPageSize = 0x10000;
  static constexpr uint32_t kUserPageCount = 0x100000000ull / kUserPageSize;
  static constexpr uint32_t kUserSmallPageSize = 0x1000;
  static constexpr uint32_t kUserSmallPagesPerView =
      kUserPageSize / kUserSmallPageSize;
  static constexpr uint32_t kPhysicalSmallPageCount =
      0x20000000 / kUserSmallPageSize;
  static constexpr uint32_t kUserLargePageSize = 0x1000000;

  // The console kernel's page table maps itself here with one entry per 4 KB
  // page of the address space. Titles read it to build user mode page tables.
  // xenia has no such table so each read builds the entry for its page. Like
  // every MMIO range it only handles 32-bit accesses.
  static constexpr uint32_t kKernelPageTableBase = 0x3FC00000;
  static constexpr uint32_t kKernelPageTableSize = 0x00400000;
  static constexpr uint32_t kKernelPageTableValid = 0x1;
  // xenia keeps virtual memory outside physical memory. The self-map gives a
  // virtual page a frame this far above its address, which TranslateUserPage
  // takes back to the page.
  static constexpr uint32_t kKernelVirtualFrameBias = 0x40000000;
  static bool IsKernelVirtualFrame(uint32_t physical_address) {
    return physical_address - kKernelVirtualFrameBias < 0xA0000000;
  }
  void ReserveKernelPageTable();
  uint32_t KernelPageTableEntry(uint32_t entry_address);
  static uint32_t KernelPageTableReadThunk(void* ppc_context, void* context,
                                           uint32_t address);
  static void KernelPageTableWriteThunk(void* ppc_context, void* context,
                                        uint32_t address, uint32_t value);
  // A PTE is the physical address with the protection in its low bits.
  static constexpr uint32_t kUserEntryProtection = 0x3;
  // The console sets C in the entry on the first user mode write through it.
  // XeFu clears it to find which pages the guest it emulates has written.
  static constexpr uint32_t kUserEntryChanged = 0x80;
  static constexpr uint32_t kUserTableSmall = 0x000;   // u16[512], by >> 23
  static constexpr uint32_t kUserTableLarge = 0x400;   // u32[256], by >> 24
  static constexpr uint32_t kUserTableMedium = 0x800;  // u16[32], by >> 27
  static constexpr uint32_t kUserTableKind = 0x840;    // u8[16], by >> 28
  // The segment kind bits that pick its page size. A kind with neither uses
  // 4 KB pages. What the other bits mean isn't known.
  static constexpr uint8_t kUserSegmentLarge = 0x1;   // 16 MB pages, inline
  static constexpr uint8_t kUserSegmentMedium = 0x2;  // 64 KB pages, a table

  static uint32_t HostToGuestVirtualThunk(const void* context,
                                          const void* host_address);

  bool AccessViolationCallback(global_unique_lock_type global_lock_locked_once,
                               void* host_address, bool is_write,
                               Exception* ex);
  static bool AccessViolationCallbackThunk(
      global_unique_lock_type global_lock_locked_once, void* context,
      void* host_address, bool is_write, Exception* ex);

  std::filesystem::path file_name_;
  uint32_t system_page_size_ = 0;
  uint32_t system_allocation_granularity_ = 0;
  uint8_t* virtual_membase_ = nullptr;
  uint8_t* physical_membase_ = nullptr;

  xe::memory::FileMappingHandle mapping_ =
      xe::memory::kFileMappingHandleInvalid;
  uint8_t* mapping_base_ = nullptr;
  union {
    struct {
      uint8_t* v00000000;
      uint8_t* v40000000;
      uint8_t* v7F000000;
      uint8_t* v80000000;
      uint8_t* v90000000;
      uint8_t* vA0000000;
      uint8_t* vC0000000;
      uint8_t* vE0000000;
      uint8_t* physical;
    };
    uint8_t* all_views[9];
  } views_ = {{0}};
  std::atomic<uint8_t*> user_virtual_membase_{nullptr};
  // Changed under the global lock. UserModeKernelAddress reads it without.
  // TODO(has207): UserModeKernelAddress is also handed kernel addresses, which
  // the user mode table then translates as if they were user ones.
  uint32_t user_page_table_ = 0;
  // One bit per user mode page mapped from the table, under the global lock.
  std::vector<uint64_t> user_page_mapped_;
  // The split pages among them, with a bit per 4 KB piece that is shown.
  std::unordered_map<uint32_t, uint16_t> user_page_split_;
  // The user mode views are one more alias of physical memory and writes
  // through them have to reach the same watches. All under the global lock.
  static constexpr uint32_t kUserNoPage = UINT32_MAX;
  // The physical 4 KB page each 4 KB piece of the user mode space shows.
  std::vector<uint32_t> user_piece_physical_;
  // The PageAccess each piece's entry allows user mode, read-write where no
  // entry applies.
  std::vector<uint8_t> user_piece_access_;
  // The UserPieceTableKey of each piece when it was mapped, or UINT64_MAX to
  // drop it at the next flush.
  std::vector<uint64_t> user_piece_table_key_;
  // The user mode pieces showing a page in each physical 64 KB block.
  std::vector<std::vector<uint32_t>> user_block_pieces_;
  // A bit per physical 4 KB page whose next user mode write is reported.
  std::vector<uint64_t> user_write_watched_;
  // A bit per 4 KB piece whose entry allows writes with its changed bit clear,
  // kept read-only so its first write can set the bit.
  std::vector<uint64_t> user_piece_unchanged_;
  // On host pages bigger than a piece, the file page each piece shows, and a
  // bit per host page mapped as a view. An access to any other host page is
  // emulated through user_file_alias_, which maps the whole file.
  std::vector<uint32_t> user_piece_file_page_;
  std::vector<uint64_t> user_host_page_view_;
  static constexpr uint64_t kUserFileAliasSize = 0x120000000ull;
  uint8_t* user_file_alias_ = nullptr;
  // The kernel, user read and user write tables. A user entry points into
  // user_file_alias_ where its piece allows the access, else at the window.
  // TODO(has207): a thread can use an entry briefly after it changes, missing
  // a new watch or reaching a flushed page.
  static constexpr size_t kGuestTlbsSize =
      size_t(kGuestTlbEntryCount) * 3 * sizeof(uint64_t);
  uint64_t* guest_tlbs_ = nullptr;
  std::atomic<UserFaultHook> user_fault_hook_{nullptr};

  std::unique_ptr<cpu::MMIOHandler> mmio_handler_;

  struct {
    VirtualHeap v00000000;
    VirtualHeap v40000000;
    VirtualHeap v80000000;
    VirtualHeap v90000000;

    VirtualHeap physical;
    PhysicalHeap v7F000000;
    PhysicalHeap vA0000000;
    PhysicalHeap vC0000000;
    PhysicalHeap vE0000000;
  } heaps_;

  friend class BaseHeap;

  friend class PhysicalHeap;
  xe::global_critical_region global_critical_region_;
  std::vector<std::pair<PhysicalMemoryInvalidationCallback, void*>*>
      physical_memory_invalidation_callbacks_;
  std::vector<std::pair<PhysicalMemoryReadCallback, void*>*>
      physical_memory_read_callbacks_;
  // Held shared across read callbacks called without the global lock, so
  // unregistering waits for them to return.
  std::shared_mutex physical_memory_read_callback_calls_mutex_;
};

}  // namespace xe

#endif  // XENIA_MEMORY_H_
