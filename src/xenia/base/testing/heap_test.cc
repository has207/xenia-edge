/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

#include "xenia/memory.h"

namespace xe {
namespace test {

// Helper to create a VirtualHeap for testing without a full Memory instance.
// Uses reserve-only allocations to avoid needing real host memory mappings.
class TestHeap {
 public:
  TestHeap(uint32_t heap_base, uint32_t heap_size, uint32_t page_size,
           HeapType heap_type = HeapType::kGuestXex) {
    heap_.Initialize(nullptr, nullptr, heap_type, heap_base, heap_size,
                     page_size);
  }

  ~TestHeap() {
    // Don't call Dispose — it tries to DeallocFixed on nullptr membase.
  }

  VirtualHeap& heap() { return heap_; }

  // Reserve-only allocation (skips host memory commit).
  bool Alloc(uint32_t size, uint32_t alignment, bool top_down,
             uint32_t* out_address) {
    return heap_.AllocRange(heap_.heap_base(),
                            heap_.heap_base() + heap_.heap_size() - 1, size,
                            alignment, kMemoryAllocationReserve,
                            kMemoryProtectRead, top_down, out_address);
  }

  bool AllocRange(uint32_t low, uint32_t high, uint32_t size,
                  uint32_t alignment, bool top_down, uint32_t* out_address) {
    return heap_.AllocRange(low, high, size, alignment,
                            kMemoryAllocationReserve, kMemoryProtectRead,
                            top_down, out_address);
  }

  bool AllocFixed(uint32_t base_address, uint32_t size) {
    return heap_.AllocFixed(base_address, size, heap_.page_size(),
                            kMemoryAllocationReserve, kMemoryProtectRead);
  }

  bool Release(uint32_t address) { return heap_.Release(address); }

  uint32_t unreserved_page_count() const {
    return heap_.unreserved_page_count();
  }

  uint32_t total_page_count() const { return heap_.total_page_count(); }

 private:
  VirtualHeap heap_;
};

// ============================================================================
// Basic allocation and release
// ============================================================================

TEST_CASE("heap_alloc_basic", "[heap]") {
  // 1MB heap, 4KB pages = 256 pages
  TestHeap h(0x80000000, 0x100000, 0x1000);
  REQUIRE(h.total_page_count() == 256);
  REQUIRE(h.unreserved_page_count() == 256);

  uint32_t addr = 0;
  REQUIRE(h.Alloc(0x1000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80000000);
  REQUIRE(h.unreserved_page_count() == 255);

  REQUIRE(h.Alloc(0x2000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80001000);
  REQUIRE(h.unreserved_page_count() == 253);
}

TEST_CASE("heap_alloc_top_down", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  // Top-down treats high_page_number as exclusive, so the top page is
  // never handed out.
  uint32_t addr = 0;
  REQUIRE(h.Alloc(0x1000, 0x1000, true, &addr));
  REQUIRE(addr == 0x800FE000);
  REQUIRE(h.unreserved_page_count() == 255);

  REQUIRE(h.Alloc(0x2000, 0x1000, true, &addr));
  REQUIRE(addr == 0x800FC000);
  REQUIRE(h.unreserved_page_count() == 253);
}

TEST_CASE("heap_alloc_release", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  uint32_t addr1 = 0, addr2 = 0;
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &addr1));
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &addr2));
  REQUIRE(addr1 == 0x80000000);
  REQUIRE(addr2 == 0x80004000);
  REQUIRE(h.unreserved_page_count() == 248);

  REQUIRE(h.Release(addr1));
  REQUIRE(h.unreserved_page_count() == 252);

  REQUIRE(h.Release(addr2));
  REQUIRE(h.unreserved_page_count() == 256);
}

// ============================================================================
// Coalescing
// ============================================================================

TEST_CASE("heap_coalesce_adjacent_releases", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  // Allocate 3 adjacent 4-page blocks.
  uint32_t a1 = 0, a2 = 0, a3 = 0;
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &a1));
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &a2));
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &a3));
  REQUIRE(a1 == 0x80000000);
  REQUIRE(a2 == 0x80004000);
  REQUIRE(a3 == 0x80008000);

  // Release middle block, then adjacent blocks — should coalesce.
  REQUIRE(h.Release(a2));
  REQUIRE(h.Release(a1));
  REQUIRE(h.Release(a3));

  // All freed. Now allocate a 12-page block — should succeed in the
  // coalesced free region.
  uint32_t big = 0;
  REQUIRE(h.Alloc(0xC000, 0x1000, false, &big));
  REQUIRE(big == 0x80000000);
}

TEST_CASE("heap_coalesce_merge_before", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  uint32_t a1 = 0, a2 = 0;
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &a1));
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &a2));

  // Release first, then second — second should merge with first.
  REQUIRE(h.Release(a1));
  REQUIRE(h.Release(a2));

  uint32_t big = 0;
  REQUIRE(h.Alloc(0x8000, 0x1000, false, &big));
  REQUIRE(big == 0x80000000);
}

TEST_CASE("heap_coalesce_merge_after", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  uint32_t a1 = 0, a2 = 0;
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &a1));
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &a2));

  // Release second, then first — first should merge with second.
  REQUIRE(h.Release(a2));
  REQUIRE(h.Release(a1));

  uint32_t big = 0;
  REQUIRE(h.Alloc(0x8000, 0x1000, false, &big));
  REQUIRE(big == 0x80000000);
}

// ============================================================================
// Fragmentation resistance
// ============================================================================

TEST_CASE("heap_fragmentation_reuse", "[heap]") {
  // 80KB heap, 4KB pages = 20 pages
  TestHeap h(0x80000000, 0x14000, 0x1000);

  // Allocate 4 x 4-page blocks (uses 16 of 20 pages).
  uint32_t a[4];
  for (int i = 0; i < 4; ++i) {
    REQUIRE(h.Alloc(0x4000, 0x1000, false, &a[i]));
  }
  REQUIRE(h.unreserved_page_count() == 4);

  // Release alternating blocks to fragment.
  REQUIRE(h.Release(a[0]));  // free pages 0-3
  REQUIRE(h.Release(a[2]));  // free pages 8-11

  // Can't allocate 5 pages (no single contiguous block of 5 in gaps).
  uint32_t fail_addr = 0;
  REQUIRE_FALSE(h.Alloc(0x5000, 0x1000, false, &fail_addr));

  // Can allocate 4 pages (fits in either free gap).
  uint32_t ok_addr = 0;
  REQUIRE(h.Alloc(0x4000, 0x1000, false, &ok_addr));
  REQUIRE(ok_addr == 0x80000000);  // bottom-up, first fit.

  // Release remaining to defragment.
  REQUIRE(h.Release(a[1]));
  REQUIRE(h.Release(a[3]));
  REQUIRE(h.Release(ok_addr));

  // Now 20 pages should be available as one contiguous block.
  REQUIRE(h.unreserved_page_count() == 20);
  uint32_t big = 0;
  REQUIRE(h.Alloc(0xC000, 0x1000, false, &big));
  REQUIRE(big == 0x80000000);
}

// ============================================================================
// Alignment
// ============================================================================

TEST_CASE("heap_alloc_alignment", "[heap]") {
  // 1MB heap, 4KB pages
  TestHeap h(0x80000000, 0x100000, 0x1000);

  // Allocate 1 page to offset the next allocation.
  uint32_t first = 0;
  REQUIRE(h.Alloc(0x1000, 0x1000, false, &first));
  REQUIRE(first == 0x80000000);

  // Allocate with 64KB alignment — should skip to 0x80010000.
  uint32_t aligned = 0;
  REQUIRE(h.Alloc(0x1000, 0x10000, false, &aligned));
  REQUIRE((aligned % 0x10000) == 0);
  REQUIRE(aligned == 0x80010000);
}

TEST_CASE("heap_alloc_alignment_top_down", "[heap]") {
  // 1MB heap, 4KB pages
  TestHeap h(0x80000000, 0x100000, 0x1000);

  // Top-down skips the top page (0x800FF000), so a 1-page allocation
  // lands on page 0xFE.
  uint32_t first = 0;
  REQUIRE(h.Alloc(0x1000, 0x1000, true, &first));
  REQUIRE(first == 0x800FE000);

  // 64KB-aligned top-down aligns only the start, so the highest aligned base
  // below page 0xFE is page 0xF0.
  uint32_t aligned = 0;
  REQUIRE(h.Alloc(0x1000, 0x10000, true, &aligned));
  REQUIRE((aligned % 0x10000) == 0);
  REQUIRE(aligned == 0x800F0000);
}

// ============================================================================
// AllocFixed
// ============================================================================

TEST_CASE("heap_alloc_fixed", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  REQUIRE(h.AllocFixed(0x80010000, 0x4000));
  REQUIRE(h.unreserved_page_count() == 252);

  // Allocate bottom-up — should get 0x80000000 (before the fixed alloc).
  uint32_t addr = 0;
  REQUIRE(h.Alloc(0x1000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80000000);

  // Allocating at the same fixed address should fail (already reserved).
  REQUIRE_FALSE(h.AllocFixed(0x80010000, 0x1000));
}

// ============================================================================
// Range allocation
// ============================================================================

TEST_CASE("heap_alloc_range", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  // Allocate in a specific sub-range.
  uint32_t addr = 0;
  REQUIRE(h.AllocRange(0x80080000, 0x800FFFFF, 0x4000, 0x1000, false, &addr));
  REQUIRE(addr >= 0x80080000);
  REQUIRE(addr + 0x4000 <= 0x80100000);
}

TEST_CASE("heap_alloc_range_exhaustion", "[heap]") {
  // 64KB heap, 4KB pages = 16 pages
  TestHeap h(0x80000000, 0x10000, 0x1000);

  // Fill the lower half.
  REQUIRE(h.AllocFixed(0x80000000, 0x8000));

  // Try to allocate in the lower half — should fail.
  // Use page-aligned high address so xe::align doesn't extend the range.
  uint32_t addr = 0;
  REQUIRE_FALSE(
      h.AllocRange(0x80000000, 0x80007000, 0x1000, 0x1000, false, &addr));

  // Allocate in the upper half — should succeed.
  REQUIRE(h.AllocRange(0x80008000, 0x8000F000, 0x1000, 0x1000, false, &addr));
  REQUIRE(addr >= 0x80008000);
}

TEST_CASE("heap_alloc_range_low_inside_a_page", "[heap]") {
  // The page holding the floor may start the block, so 16 pages fit from a
  // floor inside page 0x10 to the end of page 0x1F.
  TestHeap h(0x80000000, 0x100000, 0x1000);
  uint32_t addr = 0;
  SECTION("bottom-up") {
    REQUIRE(
        h.AllocRange(0x80010800, 0x8001FFFF, 0x10000, 0x1000, false, &addr));
    CHECK(addr == 0x80010000);
  }
  SECTION("top-down") {
    REQUIRE(h.AllocRange(0x80010800, 0x8001FFFF, 0x10000, 0x1000, true, &addr));
    CHECK(addr == 0x80010000);
  }
}

TEST_CASE("heap_alloc_range_top_down_aligns_the_start", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);
  uint32_t addr = 0;
  SECTION("the ceiling's page starts a stride") {
    REQUIRE(h.AllocRange(0x80010000, 0x80020FFF, 0x1000, 0x10000, true, &addr));
    CHECK(addr == 0x80020000);
  }
  SECTION("the ceiling ends a stride") {
    REQUIRE(h.AllocRange(0x80010000, 0x8002FFFF, 0x1000, 0x10000, true, &addr));
    CHECK(addr == 0x80020000);
  }
}

TEST_CASE("heap_alloc_range_top_down_stride_not_a_power_of_two", "[heap]") {
  // The start is masked with the stride less one like the console's, so a
  // stride of 3 pages only clears bit 1 of the page number.
  TestHeap h(0x80000000, 0x100000, 0x1000);
  uint32_t addr = 0;
  SECTION("the mask clears bit 1 of the ceiling's page") {
    REQUIRE(h.AllocRange(0x80010000, 0x80012FFF, 0x1000, 0x3000, true, &addr));
    CHECK(addr == 0x80010000);
  }
  SECTION("bit 0 stays") {
    REQUIRE(h.AllocRange(0x80010000, 0x80013FFF, 0x1000, 0x3000, true, &addr));
    CHECK(addr == 0x80011000);
  }
  SECTION("a used page starts the block again below it") {
    REQUIRE(h.AllocFixed(0x80011000, 0x1000));
    REQUIRE(h.AllocRange(0x80000000, 0x80013FFF, 0x2000, 0x3000, true, &addr));
    CHECK(addr == 0x8000D000);
  }
  SECTION("starting again below the floor fails") {
    REQUIRE(h.AllocFixed(0x80011000, 0x1000));
    CHECK_FALSE(
        h.AllocRange(0x80010000, 0x80013FFF, 0x2000, 0x3000, true, &addr));
  }
}

// ============================================================================
// The ranges callers search
// ============================================================================

// NtAllocateVirtualMemory with no base: a region starts on 64KB inside the
// title's space for its page size.
TEST_CASE("heap_alloc_range_title_regions_4k_pages", "[heap]") {
  TestHeap h(0x00000000, 0x40000000, 0x1000);
  uint32_t addr = 0;
  SECTION("bottom-up") {
    REQUIRE(
        h.AllocRange(0x00010000, 0x2FFEFFFF, 0x1000, 0x10000, false, &addr));
    CHECK(addr == 0x00010000);
    REQUIRE(
        h.AllocRange(0x00010000, 0x2FFEFFFF, 0x3000, 0x10000, false, &addr));
    CHECK(addr == 0x00020000);
  }
  SECTION("top-down") {
    REQUIRE(h.AllocRange(0x00010000, 0x2FFEFFFF, 0x1000, 0x10000, true, &addr));
    CHECK(addr == 0x2FFE0000);
    REQUIRE(h.AllocRange(0x00010000, 0x2FFEFFFF, 0x3000, 0x10000, true, &addr));
    CHECK(addr == 0x2FFD0000);
  }
}

TEST_CASE("heap_alloc_range_title_regions_64k_pages", "[heap]") {
  TestHeap h(0x40000000, 0x3F000000, 0x10000);
  uint32_t addr = 0;
  SECTION("bottom-up") {
    REQUIRE(
        h.AllocRange(0x40000000, 0x6FFEFFFF, 0x10000, 0x10000, false, &addr));
    CHECK(addr == 0x40000000);
  }
  SECTION("top-down") {
    REQUIRE(
        h.AllocRange(0x40000000, 0x6FFEFFFF, 0x10000, 0x10000, true, &addr));
    CHECK(addr == 0x6FFE0000);
    REQUIRE(
        h.AllocRange(0x40000000, 0x6FFEFFFF, 0x30000, 0x10000, true, &addr));
    CHECK(addr == 0x6FFB0000);
  }
}

// MmCreateKernelStack: a guard page then the stack, bottom-up in the title's
// stack space.
TEST_CASE("heap_alloc_range_kernel_stacks", "[heap]") {
  uint32_t addr = 0;
  SECTION("4KB pages") {
    TestHeap h(0x00000000, 0x40000000, 0x1000);
    REQUIRE(h.AllocRange(0x30000000, 0x37FFFFFF, 0x5000, 0x1000, false, &addr));
    CHECK(addr == 0x30000000);
    REQUIRE(h.AllocRange(0x30000000, 0x37FFFFFF, 0x5000, 0x1000, false, &addr));
    CHECK(addr == 0x30005000);
  }
  SECTION("64KB pages") {
    TestHeap h(0x40000000, 0x3F000000, 0x10000);
    REQUIRE(
        h.AllocRange(0x70000000, 0x77FFFFFF, 0x20000, 0x10000, false, &addr));
    CHECK(addr == 0x70000000);
    REQUIRE(
        h.AllocRange(0x70000000, 0x77FFFFFF, 0x20000, 0x10000, false, &addr));
    CHECK(addr == 0x70020000);
  }
}

// SystemHeapAlloc: bottom-up in the top 256MB of the 4KB virtual heap.
TEST_CASE("heap_alloc_system_heap_virtual", "[heap]") {
  TestHeap h(0x00000000, 0x40000000, 0x1000, HeapType::kGuestVirtual);
  uint32_t addr = 0;
  REQUIRE(h.heap().AllocSystemHeap(0x20, 0x20, kMemoryAllocationReserve,
                                   kMemoryProtectRead, false, &addr));
  CHECK(addr == 0x30000000);
  REQUIRE(h.heap().AllocSystemHeap(0x2000, 0x20, kMemoryAllocationReserve,
                                   kMemoryProtectRead, false, &addr));
  CHECK(addr == 0x30001000);
}

// The kernel module's trampolines: bottom-up where the kernel image would
// sit, on the 64KB xex heap.
TEST_CASE("heap_alloc_range_kernel_trampolines", "[heap]") {
  TestHeap h(0x80000000, 0x10000000, 0x10000);
  uint32_t addr = 0;
  REQUIRE(h.AllocRange(0x80040000, 0x801C0000, 400 * 8, 16, false, &addr));
  CHECK(addr == 0x80040000);
}

// ============================================================================
// Reset
// ============================================================================

TEST_CASE("heap_reset", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);

  // Fill most of the heap.
  uint32_t addr = 0;
  while (h.Alloc(0x1000, 0x1000, false, &addr)) {
  }

  // Reset should restore all pages.
  h.heap().Reset();
  REQUIRE(h.unreserved_page_count() == 256);

  // Should be able to allocate a large block again.
  REQUIRE(h.Alloc(0xF0000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80000000);
}

// ============================================================================
// Stress: many alloc/release cycles
// ============================================================================

TEST_CASE("heap_stress_alloc_release", "[heap]") {
  // 272KB heap, 4KB pages = 68 pages (extra pages avoid off-by-one in range
  // check for full-heap-sized allocations).
  TestHeap h(0x80000000, 0x44000, 0x1000);

  // Allocate 16 x 4-page blocks (uses 64 of 68 pages).
  uint32_t addrs[16];
  for (int i = 0; i < 16; ++i) {
    REQUIRE(h.Alloc(0x4000, 0x1000, false, &addrs[i]));
  }
  REQUIRE(h.unreserved_page_count() == 4);

  // Release all odd-indexed blocks.
  for (int i = 1; i < 16; i += 2) {
    REQUIRE(h.Release(addrs[i]));
  }
  REQUIRE(h.unreserved_page_count() == 36);

  // Re-allocate 4-page blocks into the gaps.
  for (int i = 1; i < 16; i += 2) {
    REQUIRE(h.Alloc(0x4000, 0x1000, false, &addrs[i]));
  }
  REQUIRE(h.unreserved_page_count() == 4);

  // Release everything.
  for (int i = 0; i < 16; ++i) {
    REQUIRE(h.Release(addrs[i]));
  }
  REQUIRE(h.unreserved_page_count() == 68);

  // 64-page allocation should succeed after full release (coalesced).
  uint32_t full = 0;
  REQUIRE(h.Alloc(0x40000, 0x1000, false, &full));
  REQUIRE(full == 0x80000000);
}

// ============================================================================
// 64KB page heap (like v40000000)
// ============================================================================

TEST_CASE("heap_64k_pages", "[heap]") {
  // 4MB heap, 64KB pages = 64 pages
  TestHeap h(0x40000000, 0x400000, 0x10000);
  REQUIRE(h.total_page_count() == 64);

  uint32_t addr = 0;
  REQUIRE(h.Alloc(0x10000, 0x10000, false, &addr));
  REQUIRE(addr == 0x40000000);
  REQUIRE(h.unreserved_page_count() == 63);

  REQUIRE(h.Alloc(0x20000, 0x10000, false, &addr));
  REQUIRE(addr == 0x40010000);
  REQUIRE(h.unreserved_page_count() == 61);

  REQUIRE(h.Release(0x40000000));
  REQUIRE(h.Release(0x40010000));
  REQUIRE(h.unreserved_page_count() == 64);
}

// ============================================================================
// Releasing free pages and part of a region
// ============================================================================

// The region holding an address as {base, size}, {0, 0} for a free page.
static std::pair<uint32_t, uint32_t> RegionOf(BaseHeap& heap,
                                              uint32_t address) {
  HeapAllocationInfo info = {};
  REQUIRE(heap.QueryRegionInfo(address, &info));
  if (!info.state) {
    return {0, 0};
  }
  return {info.allocation_base, info.allocation_size};
}

TEST_CASE("heap_release_free_page", "[heap]") {
  // A free first page reads as a region of no pages starting there.
  TestHeap h(0x80000000, 0x100000, 0x1000);
  REQUIRE_FALSE(h.Release(0x80000000));
  REQUIRE(h.unreserved_page_count() == 256);

  uint32_t addr = 0;
  REQUIRE(h.Alloc(0x1000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80000000);
  REQUIRE(h.Release(addr));
  REQUIRE_FALSE(h.Release(addr));
  REQUIRE(h.unreserved_page_count() == 256);
}

TEST_CASE("heap_release_range", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);
  uint32_t addr = 0;
  REQUIRE(h.Alloc(0x3000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80000000);

  SECTION("the middle page leaves a region either side") {
    REQUIRE(h.heap().ReleaseRange(0x80001000, 0x1000));
    CHECK(RegionOf(h.heap(), 0x80000000) ==
          std::pair<uint32_t, uint32_t>(0x80000000, 0x1000));
    CHECK(RegionOf(h.heap(), 0x80001000) == std::pair<uint32_t, uint32_t>());
    CHECK(RegionOf(h.heap(), 0x80002000) ==
          std::pair<uint32_t, uint32_t>(0x80002000, 0x1000));
    CHECK(h.unreserved_page_count() == 254);
    REQUIRE(h.Release(0x80000000));
    REQUIRE(h.Release(0x80002000));
    CHECK(h.unreserved_page_count() == 256);
    // The three pages coalesced again.
    REQUIRE(h.Alloc(0x3000, 0x1000, false, &addr));
    CHECK(addr == 0x80000000);
  }

  SECTION("the first page leaves the rest as a region") {
    REQUIRE(h.heap().ReleaseRange(0x80000000, 0x1000));
    CHECK(RegionOf(h.heap(), 0x80000000) == std::pair<uint32_t, uint32_t>());
    CHECK(RegionOf(h.heap(), 0x80001000) ==
          std::pair<uint32_t, uint32_t>(0x80001000, 0x2000));
    CHECK_FALSE(h.Release(0x80000000));
    REQUIRE(h.Release(0x80001000));
    CHECK(h.unreserved_page_count() == 256);
  }

  SECTION("the last page shrinks the region") {
    REQUIRE(h.heap().ReleaseRange(0x80002000, 0x1000));
    CHECK(RegionOf(h.heap(), 0x80000000) ==
          std::pair<uint32_t, uint32_t>(0x80000000, 0x2000));
    CHECK(RegionOf(h.heap(), 0x80002000) == std::pair<uint32_t, uint32_t>());
    uint32_t size = 0;
    REQUIRE(h.heap().Release(0x80000000, &size));
    CHECK(size == 0x2000);
    CHECK(h.unreserved_page_count() == 256);
  }

  SECTION("all of it frees the region") {
    REQUIRE(h.heap().ReleaseRange(0x80000000, 0x3000));
    CHECK(RegionOf(h.heap(), 0x80000000) == std::pair<uint32_t, uint32_t>());
    CHECK(h.unreserved_page_count() == 256);
  }

  SECTION("a range past the region or on free pages changes nothing") {
    CHECK_FALSE(h.heap().ReleaseRange(0x80002000, 0x2000));
    CHECK_FALSE(h.heap().ReleaseRange(0x80003000, 0x1000));
    CHECK(RegionOf(h.heap(), 0x80000000) ==
          std::pair<uint32_t, uint32_t>(0x80000000, 0x3000));
    CHECK(h.unreserved_page_count() == 253);
  }
}

TEST_CASE("heap_uninitialized_runs", "[heap]") {
  using Runs = std::vector<std::pair<uint32_t, uint32_t>>;
  TestHeap h(0x80000000, 0x100000, 0x1000);
  uint32_t addr = 0;
  REQUIRE(h.Alloc(0x3000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80000000);
  CHECK(h.heap().TakeUninitializedRuns(0x80001000, 0x1000) ==
        Runs{{0x80001000, 0x1000}});

  // A page stays initialized through a release so a later commit of it holds
  // that data.
  REQUIRE(h.Release(addr));
  REQUIRE(h.Alloc(0x3000, 0x1000, false, &addr));
  REQUIRE(addr == 0x80000000);
  CHECK(h.heap().TakeUninitializedRuns(0x80000000, 0x3000) ==
        Runs{{0x80000000, 0x1000}, {0x80002000, 0x1000}});
  CHECK(h.heap().TakeUninitializedRuns(0x80000000, 0x3000).empty());
}

TEST_CASE("heap_unreserved_page_count_in_range", "[heap]") {
  TestHeap h(0x80000000, 0x100000, 0x1000);
  REQUIRE(h.AllocFixed(0x80010000, 0x4000));
  REQUIRE(h.AllocFixed(0x800F0000, 0x10000));

  CHECK(h.heap().UnreservedPageCountInRange(0x80000000, 0x800FFFFF) == 236);
  // Only the free pages on each side of a block count.
  CHECK(h.heap().UnreservedPageCountInRange(0x8000F000, 0x80014FFF) == 2);
  // The bounds are the pages holding them.
  CHECK(h.heap().UnreservedPageCountInRange(0x8000FFFF, 0x80014001) == 2);
  CHECK(h.heap().UnreservedPageCountInRange(0x80010000, 0x80013FFF) == 0);
  CHECK(h.heap().UnreservedPageCountInRange(0x80020000, 0x80020000) == 1);
  // Clamped to the heap.
  CHECK(h.heap().UnreservedPageCountInRange(0x70000000, 0x8000FFFF) == 16);
  CHECK(h.heap().UnreservedPageCountInRange(0x800E0000, 0xFFFFFFFF) == 16);
  CHECK(h.heap().UnreservedPageCountInRange(0x80020000, 0x8001FFFF) == 0);
}

}  // namespace test
}  // namespace xe
