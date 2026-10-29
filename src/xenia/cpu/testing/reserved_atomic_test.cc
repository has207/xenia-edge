/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/thread_state.h"

#if XE_ARCH_ARM64
#include "xenia/base/cvar.h"
#include "xenia/base/platform_arm64.h"
DECLARE_int64(a64_extension_mask);
#endif  // XE_ARCH_ARM64

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

namespace {

// Guest memory is big endian, these read and write in host order.
uint32_t GuestRead32(Memory* memory, uint32_t address) {
  return xe::byte_swap(*memory->TranslateVirtual<uint32_t*>(address));
}
void GuestWrite32(Memory* memory, uint32_t address, uint32_t value) {
  *memory->TranslateVirtual<uint32_t*>(address) = xe::byte_swap(value);
}

// A second guest thread on the same processor. Its context has its own
// reservation but shares the granule table, so tests can watch one thread's
// store cancel another's reservation.
class SecondThread {
 public:
  SecondThread(Processor* processor, Memory* memory) : memory_(memory) {
    stack_address_ = memory->SystemHeapAlloc(kStackSize);
    thread_state_ = std::make_unique<ThreadState>(processor, 0x200,
                                                  stack_address_ + kStackSize);
  }
  ~SecondThread() {
    thread_state_.reset();
    memory_->SystemHeapFree(stack_address_);
  }

  PPCContext* context() const { return thread_state_->context(); }

 private:
  static constexpr uint32_t kStackSize = 64 * 1024;
  Memory* memory_;
  uint32_t stack_address_ = 0;
  std::unique_ptr<ThreadState> thread_state_;
};

// 128 byte aligned, so addr and addr + 4 share a granule and addr + 128 does
// not.
uint32_t AllocScratch(Memory* memory) {
  return memory->SystemHeapAlloc(256, 128);
}

void FreeScratch(Memory* memory, PPCContext* ctx) {
  memory->SystemHeapFree(static_cast<uint32_t>(ctx->r[4]));
}

// a64 emits stwcx. two ways: a single casal under LSE, or an ldaxr/stlxr loop
// without it. Every core that runs these tests reports LSE, so the loop is only
// reachable by masking the feature off and rebuilding the emitter's cached
// flags before the backend is constructed.
enum class AtomicsMode { kDetected, kNoLSE };

class ScopedAtomicsMode {
 public:
  explicit ScopedAtomicsMode(AtomicsMode mode) {
#if XE_ARCH_ARM64
    saved_mask_ = cvars::a64_extension_mask;
    if (mode == AtomicsMode::kNoLSE) {
      cvars::a64_extension_mask = 0;
      xe::arm64::InitFeatureFlags();
    }
#endif  // XE_ARCH_ARM64
  }
  ~ScopedAtomicsMode() {
#if XE_ARCH_ARM64
    cvars::a64_extension_mask = saved_mask_;
    xe::arm64::InitFeatureFlags();
#endif  // XE_ARCH_ARM64
  }

  ScopedAtomicsMode(const ScopedAtomicsMode&) = delete;
  ScopedAtomicsMode& operator=(const ScopedAtomicsMode&) = delete;

 private:
#if XE_ARCH_ARM64
  int64_t saved_mask_ = 0;
#endif  // XE_ARCH_ARM64
};

#if XE_ARCH_ARM64
#define XE_RESERVE_ATOMICS_MODES AtomicsMode::kDetected, AtomicsMode::kNoLSE
#else
#define XE_RESERVE_ATOMICS_MODES AtomicsMode::kDetected
#endif  // XE_ARCH_ARM64

// lwarx only, leaving the reservation live on return.
void EmitGuestReservedLoad(HIRBuilder& b) {
  auto addr = LoadGPR(b, 4);
  StoreGPR(b, 3,
           b.ZeroExtend(b.ByteSwap(b.LoadWithReserve(addr, INT32_TYPE)),
                        INT64_TYPE));
  b.Return();
}

// A full lwarx/stwcx. pair, storing r5 and reporting the result in r6.
void EmitGuestReservedPair(HIRBuilder& b) {
  auto addr = LoadGPR(b, 4);
  StoreGPR(b, 3,
           b.ZeroExtend(b.ByteSwap(b.LoadWithReserve(addr, INT32_TYPE)),
                        INT64_TYPE));
  auto stored = b.StoreWithReserve(
      addr, b.ByteSwap(b.Truncate(LoadGPR(b, 5), INT32_TYPE)), INT32_TYPE);
  StoreGPR(b, 6, b.ZeroExtend(stored, INT64_TYPE));
  b.Return();
}

// stwcx. with no lwarx of its own, so it completes whatever reservation the
// context already holds. Lets a test set the reservation up by hand and drive
// the guest store down one specific failure path.
void EmitGuestReservedStoreOnly(HIRBuilder& b) {
  auto addr = LoadGPR(b, 4);
  auto stored = b.StoreWithReserve(
      addr, b.ByteSwap(b.Truncate(LoadGPR(b, 5), INT32_TYPE)), INT32_TYPE);
  StoreGPR(b, 6, b.ZeroExtend(stored, INT64_TYPE));
  b.Return();
}

// lwarx at r4, stwcx. four bytes up: same granule, but not the reserved
// address.
void EmitGuestReservedPairOffset(HIRBuilder& b) {
  auto addr = LoadGPR(b, 4);
  StoreGPR(b, 3,
           b.ZeroExtend(b.ByteSwap(b.LoadWithReserve(addr, INT32_TYPE)),
                        INT64_TYPE));
  auto stored = b.StoreWithReserve(
      b.Add(addr, b.LoadConstantUint64(4)),
      b.ByteSwap(b.Truncate(LoadGPR(b, 5), INT32_TYPE)), INT32_TYPE);
  StoreGPR(b, 6, b.ZeroExtend(stored, INT64_TYPE));
  b.Return();
}

// A pair, then a second stwcx. that has no reservation left to consume.
void EmitGuestReservedPairTwice(HIRBuilder& b) {
  auto addr = LoadGPR(b, 4);
  StoreGPR(b, 3,
           b.ZeroExtend(b.ByteSwap(b.LoadWithReserve(addr, INT32_TYPE)),
                        INT64_TYPE));
  auto first = b.StoreWithReserve(
      addr, b.ByteSwap(b.Truncate(LoadGPR(b, 5), INT32_TYPE)), INT32_TYPE);
  StoreGPR(b, 6, b.ZeroExtend(first, INT64_TYPE));
  auto second = b.StoreWithReserve(
      addr, b.ByteSwap(b.Truncate(LoadGPR(b, 7), INT32_TYPE)), INT32_TYPE);
  StoreGPR(b, 8, b.ZeroExtend(second, INT64_TYPE));
  b.Return();
}

// The 64-bit ldarx/stdcx. pair.
void EmitGuestReservedPair64(HIRBuilder& b) {
  auto addr = LoadGPR(b, 4);
  StoreGPR(b, 3, b.ByteSwap(b.LoadWithReserve(addr, INT64_TYPE)));
  auto stored = b.StoreWithReserve(addr, b.ByteSwap(LoadGPR(b, 5)), INT64_TYPE);
  StoreGPR(b, 6, b.ZeroExtend(stored, INT64_TYPE));
  b.Return();
}

// One lock-free stack attempt on the head at r4: r7 = 0 pops into r3, r7 = 1
// pushes r5. r6 = 1 if it landed. A pop links the dummy node in r8.
void EmitGuestStackAttempt(HIRBuilder& b) {
  auto head = LoadGPR(b, 4);
  auto push = b.CompareNE(LoadGPR(b, 7), b.LoadConstantUint64(0));
  auto node = b.Select(push, LoadGPR(b, 5), LoadGPR(b, 8));
  auto top = b.LoadWithReserve(head, INT32_TYPE);
  auto top_node = b.ZeroExtend(b.ByteSwap(top), INT64_TYPE);
  StoreGPR(b, 3, top_node);
  b.Store(b.Add(node, b.LoadConstantUint64(4)), top);
  auto next = b.Load(b.Add(top_node, b.LoadConstantUint64(4)), INT32_TYPE);
  auto stored = b.StoreWithReserve(
      head, b.Select(push, b.ByteSwap(b.Truncate(node, INT32_TYPE)), next),
      INT32_TYPE);
  StoreGPR(b, 6, b.ZeroExtend(stored, INT64_TYPE));
  b.Return();
}

// Oversubscribed threads pop and push a few nodes, returns how many pops got a
// node another thread already owned.
template <typename Attempt>
uint32_t StressStack(Processor* processor, Memory* memory, Attempt attempt) {
  const uint32_t thread_count =
      std::max(4u, 2 * std::thread::hardware_concurrency());
  const uint32_t node_count = thread_count + 2;
  constexpr uint32_t kNodeStride = 64;

  uint32_t head = AllocScratch(memory);
  uint32_t nodes = memory->SystemHeapAlloc(node_count * kNodeStride + 128, 128);
  // only a corrupted list can run dry and pop the trap
  uint32_t trap = nodes + node_count * kNodeStride;
  uint32_t dummy = trap + kNodeStride;
  GuestWrite32(memory, trap + 4, trap);
  for (uint32_t i = 0; i < node_count; ++i) {
    uint32_t node = nodes + i * kNodeStride;
    GuestWrite32(memory, node + 4,
                 i + 1 < node_count ? node + kNodeStride : trap);
  }
  GuestWrite32(memory, head, nodes);

  std::vector<std::atomic<uint32_t>> owned(node_count);
  std::atomic<uint32_t> double_pops{0};
  std::atomic<bool> stop{false};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);

  std::vector<std::thread> threads;
  for (uint32_t t = 0; t < thread_count; ++t) {
    threads.emplace_back([&, t]() {
      SecondThread thread(processor, memory);
      PPCContext* ctx = thread.context();
      ctx->lr = 0xBCBCBCBC;
      ctx->r[4] = head;
      ctx->r[8] = dummy;
      uint32_t ops = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        if (!(++ops & 1023) && std::chrono::steady_clock::now() > deadline) {
          break;
        }
        do {
          attempt(ctx, false);
        } while (!ctx->r[6]);
        uint32_t node = uint32_t(ctx->r[3]);
        if (node == trap) {
          double_pops.fetch_add(1);
          stop = true;
          break;
        }
        uint32_t index = (node - nodes) / kNodeStride;
        if (owned[index].exchange(1)) {
          double_pops.fetch_add(1);
          stop = true;
          break;
        }
        owned[index].store(0);
        ctx->r[5] = node;
        do {
          attempt(ctx, true);
        } while (!ctx->r[6]);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  memory->SystemHeapFree(nodes);
  memory->SystemHeapFree(head);
  return double_pops.load();
}
}  // namespace

// =============================================================================
// Host reservation pair
// =============================================================================
TEST_CASE("RESERVED_HOST_PAIR", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0x11223344u);

        auto* backend = ctx->processor->backend();
        REQUIRE(backend->ReservedLoad32(ctx, addr) == 0x11223344u);
        REQUIRE(backend->ReservedStore32(ctx, addr, 0xAABBCCDDu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0xAABBCCDDu);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

TEST_CASE("RESERVED_HOST_STORE_WITHOUT_LOAD", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0x11223344u);

        auto* backend = ctx->processor->backend();
        REQUIRE_FALSE(backend->ReservedStore32(ctx, addr, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x11223344u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

// A store consumes the reservation, so a second one has nothing to use.
TEST_CASE("RESERVED_HOST_STORE_TWICE", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);

        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(ctx, addr);
        REQUIRE(backend->ReservedStore32(ctx, addr, 0x12345678u));
        REQUIRE_FALSE(backend->ReservedStore32(ctx, addr, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x12345678u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

// Storing to an address the load didn't reserve fails, same granule or not.
TEST_CASE("RESERVED_HOST_ADDRESS_MISMATCH", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);
        GuestWrite32(test.memory.get(), addr + 4, 0u);

        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(ctx, addr);
        REQUIRE_FALSE(backend->ReservedStore32(ctx, addr + 4, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr + 4) == 0u);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

TEST_CASE("RESERVED_HOST_VALUE_CHANGED", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);

        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(ctx, addr);
        GuestWrite32(test.memory.get(), addr, 0x99999999u);
        REQUIRE_FALSE(backend->ReservedStore32(ctx, addr, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x99999999u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

TEST_CASE("RESERVED_HOST_PAIR_64", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        *test.memory->TranslateVirtual<uint64_t*>(addr) =
            xe::byte_swap(uint64_t(0x1122334455667788ull));

        auto* backend = ctx->processor->backend();
        REQUIRE(backend->ReservedLoad64(ctx, addr) == 0x1122334455667788ull);
        REQUIRE(backend->ReservedStore64(ctx, addr, 0xAABBCCDDEEFF0011ull));
        REQUIRE(xe::byte_swap(*test.memory->TranslateVirtual<uint64_t*>(
                    addr)) == 0xAABBCCDDEEFF0011ull);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

// =============================================================================
// Cross-thread invalidation
// =============================================================================
TEST_CASE("RESERVED_HOST_CROSS_THREAD_INVALIDATE", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  SecondThread other(test.processors[0].get(), test.memory.get());
  test.Run(
      [&test, &other](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);

        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(other.context(), addr);
        backend->ReservedLoad32(ctx, addr);
        // bumps the granule, so the other thread's reservation is gone
        REQUIRE(backend->ReservedStore32(ctx, addr, 0x12345678u));
        REQUIRE_FALSE(
            backend->ReservedStore32(other.context(), addr, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x12345678u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

// A store anywhere in the granule cancels the reservation.
TEST_CASE("RESERVED_HOST_GRANULE_INVALIDATE", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  SecondThread other(test.processors[0].get(), test.memory.get());
  test.Run(
      [&test, &other](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);
        GuestWrite32(test.memory.get(), addr + 4, 0u);

        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(other.context(), addr);
        backend->ReservedLoad32(ctx, addr + 4);
        REQUIRE(backend->ReservedStore32(ctx, addr + 4, 0x12345678u));
        REQUIRE_FALSE(
            backend->ReservedStore32(other.context(), addr, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

// A reservation in a different granule survives.
TEST_CASE("RESERVED_HOST_OTHER_GRANULE_SURVIVES", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  SecondThread other(test.processors[0].get(), test.memory.get());
  test.Run(
      [&test, &other](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);
        GuestWrite32(test.memory.get(), addr + 128, 0u);

        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(other.context(), addr + 128);
        backend->ReservedLoad32(ctx, addr);
        REQUIRE(backend->ReservedStore32(ctx, addr, 0x12345678u));
        REQUIRE(
            backend->ReservedStore32(other.context(), addr + 128, 0xAABBCCDDu));
        REQUIRE(GuestRead32(test.memory.get(), addr + 128) == 0xAABBCCDDu);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

// =============================================================================
// Host and JIT sharing one reservation domain
// =============================================================================
// A host store completes a reservation the JIT took.
TEST_CASE("RESERVED_GUEST_LOAD_HOST_STORE", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedLoad);
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0x11223344u);
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[3] == 0x11223344ull);

        auto* backend = ctx->processor->backend();
        REQUIRE(backend->ReservedStore32(ctx, addr, 0xAABBCCDDu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0xAABBCCDDu);
        FreeScratch(test.memory.get(), ctx);
      });
}

// A host store from another thread cancels a reservation the JIT took.
TEST_CASE("RESERVED_HOST_STORE_INVALIDATES_GUEST", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedLoad);
  SecondThread other(test.processors[0].get(), test.memory.get());
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);
      },
      [&test, &other](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(other.context(), addr);
        REQUIRE(backend->ReservedStore32(other.context(), addr, 0x12345678u));
        // the guest's lwarx reservation is stale now
        REQUIRE_FALSE(backend->ReservedStore32(ctx, addr, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x12345678u);
        FreeScratch(test.memory.get(), ctx);
      });
}

// A JIT stwcx. cancels a reservation host code took.
TEST_CASE("RESERVED_GUEST_STORE_INVALIDATES_HOST", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedPair);
  SecondThread other(test.processors[0].get(), test.memory.get());
  test.Run(
      [&test, &other](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0x12345678ull;
        GuestWrite32(test.memory.get(), addr, 0u);
        ctx->processor->backend()->ReservedLoad32(other.context(), addr);
      },
      [&test, &other](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        // the guest's own pair succeeded
        REQUIRE(ctx->r[6] == 1ull);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x12345678u);

        REQUIRE_FALSE(ctx->processor->backend()->ReservedStore32(
            other.context(), addr, 0xDEADBEEFu));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x12345678u);
        FreeScratch(test.memory.get(), ctx);
      });
}

// =============================================================================
// JIT stwcx. paths
// =============================================================================
// Nothing reserved, so the store is refused before it looks at memory.
TEST_CASE("RESERVED_GUEST_STORE_WITHOUT_LOAD", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedStoreOnly);
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0xDEADBEEFull;
        GuestWrite32(test.memory.get(), addr, 0x11223344u);
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[6] == 0ull);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x11223344u);
        FreeScratch(test.memory.get(), ctx);
      });
}

// A reservation host code took is completed by the guest's stwcx.
TEST_CASE("RESERVED_HOST_LOAD_GUEST_STORE", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedStoreOnly);
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0xAABBCCDDull;
        GuestWrite32(test.memory.get(), addr, 0x11223344u);
        ctx->processor->backend()->ReservedLoad32(ctx, addr);
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[6] == 1ull);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0xAABBCCDDu);
        FreeScratch(test.memory.get(), ctx);
      });
}

// A plain store leaves the granule generation alone, so only the compare
// against the cached value can catch it.
TEST_CASE("RESERVED_GUEST_VALUE_CHANGED", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedStoreOnly);
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0xDEADBEEFull;
        GuestWrite32(test.memory.get(), addr, 0u);
        ctx->processor->backend()->ReservedLoad32(ctx, addr);
        GuestWrite32(test.memory.get(), addr, 0x99999999u);
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[6] == 0ull);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x99999999u);
        FreeScratch(test.memory.get(), ctx);
      });
}

// Another thread's store to the same granule bumps the generation, so the
// guest's stwcx. is refused even though the address and value both still match.
TEST_CASE("RESERVED_GUEST_GRANULE_INVALIDATED", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedStoreOnly);
  SecondThread other(test.processors[0].get(), test.memory.get());
  test.Run(
      [&test, &other](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0xDEADBEEFull;
        GuestWrite32(test.memory.get(), addr, 0u);
        GuestWrite32(test.memory.get(), addr + 4, 0u);

        auto* backend = ctx->processor->backend();
        backend->ReservedLoad32(ctx, addr);
        backend->ReservedLoad32(other.context(), addr + 4);
        REQUIRE(
            backend->ReservedStore32(other.context(), addr + 4, 0x12345678u));
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[6] == 0ull);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0u);
        FreeScratch(test.memory.get(), ctx);
      });
}

TEST_CASE("RESERVED_GUEST_ADDRESS_MISMATCH", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedPairOffset);
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0xDEADBEEFull;
        GuestWrite32(test.memory.get(), addr, 0x11223344u);
        GuestWrite32(test.memory.get(), addr + 4, 0u);
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[3] == 0x11223344ull);
        REQUIRE(ctx->r[6] == 0ull);
        REQUIRE(GuestRead32(test.memory.get(), addr + 4) == 0u);
        FreeScratch(test.memory.get(), ctx);
      });
}

// The first store consumes the reservation, so the second has nothing to use.
TEST_CASE("RESERVED_GUEST_STORE_TWICE", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedPairTwice);
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0x12345678ull;
        ctx->r[7] = 0xDEADBEEFull;
        GuestWrite32(test.memory.get(), addr, 0u);
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[6] == 1ull);
        REQUIRE(ctx->r[8] == 0ull);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x12345678u);
        FreeScratch(test.memory.get(), ctx);
      });
}

TEST_CASE("RESERVED_GUEST_PAIR_64", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestReservedPair64);
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        ctx->r[5] = 0xAABBCCDDEEFF0011ull;
        *test.memory->TranslateVirtual<uint64_t*>(addr) =
            xe::byte_swap(uint64_t(0x1122334455667788ull));
      },
      [&test](PPCContext* ctx) {
        uint32_t addr = static_cast<uint32_t>(ctx->r[4]);
        REQUIRE(ctx->r[3] == 0x1122334455667788ull);
        REQUIRE(ctx->r[6] == 1ull);
        REQUIRE(xe::byte_swap(*test.memory->TranslateVirtual<uint64_t*>(
                    addr)) == 0xAABBCCDDEEFF0011ull);
        FreeScratch(test.memory.get(), ctx);
      });
}

// =============================================================================
// Stalled stores
// =============================================================================
TEST_CASE("RESERVED_HOST_STACK_STRESS", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  auto* processor = test.processors[0].get();
  auto* memory = test.memory.get();
  auto* backend = processor->backend();
  uint32_t double_pops =
      StressStack(processor, memory, [&](PPCContext* ctx, bool push) {
        uint32_t head = uint32_t(ctx->r[4]);
        uint32_t top = backend->ReservedLoad32(ctx, head);
        if (push) {
          GuestWrite32(memory, uint32_t(ctx->r[5]) + 4, top);
          ctx->r[6] = backend->ReservedStore32(ctx, head, uint32_t(ctx->r[5]));
        } else {
          ctx->r[3] = top;
          ctx->r[6] =
              backend->ReservedStore32(ctx, head, GuestRead32(memory, top + 4));
        }
      });
  REQUIRE(double_pops == 0);
}

TEST_CASE("RESERVED_GUEST_STACK_STRESS", "[reserve]") {
  ScopedAtomicsMode atomics_mode(GENERATE(XE_RESERVE_ATOMICS_MODES));
  TestFunction test(EmitGuestStackAttempt);
  auto* processor = test.processors[0].get();
  auto* fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn);
  uint32_t double_pops = StressStack(
      processor, test.memory.get(), [&](PPCContext* ctx, bool push) {
        ctx->r[7] = push ? 1 : 0;
        fn->Call(ctx->thread_state, uint32_t(ctx->lr));
      });
  REQUIRE(double_pops == 0);
}

// =============================================================================
// Processor::GuestAtomic wrappers
// =============================================================================
TEST_CASE("GUEST_ATOMIC_INCREMENT32", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0x11223344u);

        // returns the value from before the update
        REQUIRE(ctx->processor->GuestAtomicIncrement32(ctx, addr) ==
                0x11223344u);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x11223345u);
        REQUIRE(ctx->processor->GuestAtomicDecrement32(ctx, addr) ==
                0x11223345u);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x11223344u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

TEST_CASE("GUEST_ATOMIC_BITWISE32", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0x0000FF00u);

        REQUIRE(ctx->processor->GuestAtomicOr32(ctx, addr, 0x000000FFu) ==
                0x0000FF00u);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x0000FFFFu);
        REQUIRE(ctx->processor->GuestAtomicXor32(ctx, addr, 0x0000FFFFu) ==
                0x0000FFFFu);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0u);
        GuestWrite32(test.memory.get(), addr, 0x0000FFFFu);
        REQUIRE(ctx->processor->GuestAtomicAnd32(ctx, addr, 0x0000FF00u) ==
                0x0000FFFFu);
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x0000FF00u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}

TEST_CASE("GUEST_ATOMIC_CAS32", "[reserve]") {
  TestFunction test([](HIRBuilder& b) { b.Return(); });
  test.Run(
      [&test](PPCContext* ctx) {
        uint32_t addr = AllocScratch(test.memory.get());
        ctx->r[4] = addr;
        GuestWrite32(test.memory.get(), addr, 0u);

        REQUIRE_FALSE(
            ctx->processor->GuestAtomicCAS32(ctx, 1u, 0x12345678u, addr));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0u);
        REQUIRE(ctx->processor->GuestAtomicCAS32(ctx, 0u, 0x12345678u, addr));
        REQUIRE(GuestRead32(test.memory.get(), addr) == 0x12345678u);
      },
      [&test](PPCContext* ctx) { FreeScratch(test.memory.get(), ctx); });
}
