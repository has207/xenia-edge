/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_BACKEND_H_
#define XENIA_CPU_BACKEND_A64_A64_BACKEND_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "xenia/base/bit_map.h"
#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/backend/backend.h"

namespace xe {
class Exception;
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class A64CodeCache;

typedef void* (*HostToGuestThunk)(void* target, void* arg0, void* arg1);
typedef void* (*GuestToHostThunk)(void* target, void* arg0, void* arg1);
typedef void (*ResolveFunctionThunk)();
// Host entry to a stwcx. helper, returns 1 if the store landed. The value is
// guest endian.
typedef uint32_t (*ReservedStoreThunk)(uint32_t guest_address,
                                       ppc::PPCContext* context, uint64_t value,
                                       void* host_address);

// Place guest trampolines in an address range that the HV normally occupies.
static constexpr uint32_t GUEST_TRAMPOLINE_BASE = 0x80000000;
static constexpr uint32_t GUEST_TRAMPOLINE_END = 0x80040000;
static constexpr uint32_t GUEST_TRAMPOLINE_MIN_LEN = 8;
static constexpr uint32_t MAX_GUEST_TRAMPOLINES =
    (GUEST_TRAMPOLINE_END - GUEST_TRAMPOLINE_BASE) / GUEST_TRAMPOLINE_MIN_LEN;

// Xenon reservation granule is one 128 byte cache line.
static constexpr uint32_t A64_RESERVE_GRANULE_SHIFT = 7;
// A generation counter per granule, hashed. stwcx. bumps its granule to kill
// other threads' reservations. Colliding granules only cost a spurious failure.
static constexpr uint32_t A64_RESERVE_ENTRY_BITS = 20;
static constexpr uint32_t A64_RESERVE_ENTRY_MASK =
    (1u << A64_RESERVE_ENTRY_BITS) - 1;
static constexpr uint32_t A64_RESERVE_NUM_ENTRIES = A64_RESERVE_ENTRY_MASK + 1;

struct ReserveHelper {
  std::atomic<uint32_t> generations[A64_RESERVE_NUM_ENTRIES];

  ReserveHelper() {
    for (auto& generation : generations) {
      generation.store(0, std::memory_order_relaxed);
    }
  }
};

struct A64BackendStackpoint {
  uint64_t host_stack_;
  unsigned guest_stack_;
  // Guest lr at the prolog, which a dynamic code return matches against.
  unsigned guest_return_address_;
};

// The guest return address host code enters guest code with.
constexpr uint32_t kHostReturnAddress = 0xBCBCBCBC;

// Which lookups take a dynamic call cache entry.
enum A64DynamicCallCacheKind : uint32_t {
  kA64DynamicCallAny = 0,
  // Cached by a direct call, which skips the return site checks a branch to
  // the same address needs.
  kA64DynamicCallDirectOnly = 1,
  // The function entered at a return site. A branch takes it only when no
  // live frame returns there and MayResolveLongjmp rules out a longjmp.
  kA64DynamicCallReturnSite = 2,
};
// A resolved guest address that has no indirection slot.
struct A64DynamicCallCacheEntry {
  uint32_t guest_address;
  uint32_t kind;
  uint64_t host_address;
};
constexpr uint32_t kA64DynamicCallCacheSize = 16384;
// EmitDynamicCallLookup indexes and loads these itself.
static_assert(sizeof(A64DynamicCallCacheEntry) == 16);
static_assert(offsetof(A64DynamicCallCacheEntry, kind) == 4);
static_assert(offsetof(A64DynamicCallCacheEntry, host_address) == 8);
static_assert((kA64DynamicCallCacheSize & (kA64DynamicCallCacheSize - 1)) == 0);

uint32_t FindStackpointSyncDepth(const A64BackendStackpoint* stackpoints,
                                 uint32_t current_depth, uint32_t guest_sp);

enum : uint32_t {
  kA64BackendFPCRModeBit = 0,
  kA64BackendHasReserveBit = 1,
  kA64BackendNJMOn = 2,
};

// Located prior to the context register (x20) in memory.
struct A64BackendContext {
  // Scratch vectors for helper routines.
  // Using uint8_t[16] instead of NEON intrinsic types to avoid including
  // arm_neon.h in the header.
  alignas(16) uint8_t helper_scratch_v128s[4][16];
  // vrefp's 32 segments as slope << 16 | base >> 9, for RECIP_V128
  uint32_t vrefp_table[32];
  union {
    uint64_t helper_scratch_u64s[8];
    uint32_t helper_scratch_u32s[16];
  };
  ReserveHelper* reserve_helper_;
  uint64_t cached_reserve_value_;
  uint64_t* guest_tick_count;
  // Fixed for the life of the process, so generated code loads them with one
  // ldr instead of building each as an immediate.
  uint64_t indirection_table_bias;
  uint64_t code_execute_base;
  uint64_t external_indirection_table;
  uint64_t guest_to_host_thunk_address;
  // Same thunk without the q4-q31 save/restore, for CallExtern.
  uint64_t guest_to_host_thunk_no_vec_address;
  // Helpers Initialize emits into the code cache. The stack-repair entry is
  // null unless --a64_enable_host_guest_stack_synchronization, which also
  // gates every site that reads it.
  uint64_t synchronize_guest_and_host_stack_helper_address;
  uint64_t vrsqrtefp_scalar_helper_address;
  uint64_t vrsqrtefp_vector_helper_address;
  // POW2_V128's and LOG2_V128's tables, 2048 entries each, and RECIP_F64's
  uint64_t vexptefp_table_address;
  uint64_t vlogefp_table_address;
  uint64_t fres_table_address;
  uint64_t frsqrte_helper_address;
  uint64_t reserved_store_32_helper_address;
  uint64_t reserved_store_64_helper_address;
  // The address of the yield handler pointer, not the handler: the scheduler
  // installs the handler after guest threads exist and drops it on shutdown.
  uint64_t preempt_yield_handler_address;
  A64BackendStackpoint* stackpoints;
  // allocated by the first dynamic call resolve on this thread
  A64DynamicCallCacheEntry* dynamic_call_cache;
  // address of the live reservation, and its granule generation when taken
  uint32_t reserve_address;
  uint32_t reserve_generation;
  unsigned int current_stackpoint_depth;
  unsigned int pending_stackpoint_sync_depth;
  unsigned int fpcr_fpu;
  unsigned int fpcr_vmx;
  // bit 0 = 0 if fpcr is fpu, else it is vmx
  // bit 1 = got reserve
  unsigned int flags;
  unsigned int Ox1000;  // constant 0x1000
  // DEFAULT_VMX_FPCR regardless of NJM, for the ops that always flush
  unsigned int fpcr_vmx_daz;
  // Set by a dynamic call resolve that continues inside a function rather than
  // at its entry. Emitted code clears it before a lookup and reads it after.
  unsigned int dynamic_target_in_body;
  // The stackpoint depth, host stack pointer and return address of the frame
  // host code entered, which the return to host helper restores.
  unsigned int host_return_stackpoint_depth;
  uint64_t host_return_stack;
  uint64_t host_return_address;
};

// Default FPCR for FPU mode (round to nearest, no flush to zero).
constexpr unsigned int DEFAULT_FPU_FPCR = 0;
// Default FPCR for VMX mode (flush to zero, preserve NaN payloads).
constexpr unsigned int DEFAULT_VMX_FPCR = (1 << 24);  // FZ
// PPC rounding control (RN) to FPCR RMode (bits 23:22)
inline constexpr uint32_t kGuestFpcrTable[4] = {
    (0b00 << 22),  // PPC 0: nearest
    (0b11 << 22),  // PPC 1: toward zero
    (0b01 << 22),  // PPC 2: toward +inf
    (0b10 << 22),  // PPC 3: toward -inf
};
// The NaN fixups rely on FPCR.DN (bit 25) and FPCR.AH (bit 1) being clear in
// every FPCR the guest runs with. SET_NJM only toggles FZ.
static_assert([] {
  constexpr uint32_t kNaNControl = (1u << 25) | (1u << 1);
  for (uint32_t fpcr : kGuestFpcrTable) {
    if (fpcr & kNaNControl) {
      return false;
    }
  }
  return !(DEFAULT_FPU_FPCR & kNaNControl) && !(DEFAULT_VMX_FPCR & kNaNControl);
}());

class A64Backend : public Backend {
 public:
  static constexpr uint32_t kForceReturnAddress = 0x9FFF0000u;

  explicit A64Backend();
  ~A64Backend() override;

  A64CodeCache* code_cache() const { return code_cache_.get(); }
  void* return_to_host_helper() const { return return_to_host_helper_; }
  uintptr_t emitter_data() const { return emitter_data_; }

  std::string name() const override { return "a64"; }

  HostToGuestThunk host_to_guest_thunk() const { return host_to_guest_thunk_; }
  ResolveFunctionThunk resolve_function_thunk() const {
    return resolve_function_thunk_;
  }

  bool Initialize(Processor* processor) override;

  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) override;

  std::unique_ptr<Assembler> CreateAssembler() override;

  std::unique_ptr<GuestFunction> CreateGuestFunction(Module* module,
                                                     uint32_t address) override;

  void InitializeBackendContext(void* ctx) override;
  void DeinitializeBackendContext(void* ctx) override;
  void PrepareForReentry(void* ctx) override;
  void InvalidateDynamicCalls(uint32_t start, uint32_t end) override;
  static A64BackendStackpoint* AllocStackpoints();
  void* CreateStackpointState() override;
  void DestroyStackpointState(void* state) override;
  void SwapStackpointState(void* ctx, void* state) override;

  A64BackendContext* BackendContextForGuestContext(void* ctx) {
    return reinterpret_cast<A64BackendContext*>(
        reinterpret_cast<intptr_t>(ctx) - sizeof(A64BackendContext));
  }
  // lwarx found the granule claimed, refreshes the generation it took.
  static uint64_t AwaitReservation(void* raw_context, uint64_t guest_address);

  uint32_t CreateGuestTrampoline(GuestTrampolineProc proc, void* userdata1,
                                 void* userdata2, bool long_term) override;
  void FreeGuestTrampoline(uint32_t trampoline_addr) override;
  void SetGuestRoundingMode(void* ctx, unsigned int mode) override;
  bool PopulatePseudoStacktrace(GuestPseudoStackTrace* st) override;

  uint32_t ReservedLoad32(ppc::PPCContext* context, uint32_t address) override;
  uint64_t ReservedLoad64(ppc::PPCContext* context, uint32_t address) override;
  bool ReservedStore32(ppc::PPCContext* context, uint32_t address,
                       uint32_t value) override;
  bool ReservedStore64(ppc::PPCContext* context, uint32_t address,
                       uint64_t value) override;

  bool trace_instr_available() const override;
  bool trace_data_available() const override;
  bool trace_func_available() const override;
  bool trace_instr_enabled() const override;
  void set_trace_instr_enabled(bool value) override;
  bool trace_data_enabled() const override;
  void set_trace_data_enabled(bool value) override;
  bool trace_func_enabled() const override;
  void set_trace_func_enabled(bool value) override;
  std::string FormatSequenceKey(uint64_t key) const override;

  void RecordMMIOExceptionForGuestInstruction(void* host_address);

 private:
  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);
  static uint64_t ReleaseFaultingReservedStore(void* data, Exception* ex);

  std::unique_ptr<A64CodeCache> code_cache_;
  uintptr_t emitter_data_ = 0;

  HostToGuestThunk host_to_guest_thunk_ = nullptr;
  GuestToHostThunk guest_to_host_thunk_ = nullptr;
  GuestToHostThunk guest_to_host_thunk_no_vec_ = nullptr;
  ResolveFunctionThunk resolve_function_thunk_ = nullptr;
  void* synchronize_guest_and_host_stack_helper_ = nullptr;
  void* return_to_host_helper_ = nullptr;
  void* vrsqrtefp_scalar_helper_ = nullptr;
  void* vrsqrtefp_vector_helper_ = nullptr;
  void* frsqrte_helper_ = nullptr;
  void* reserved_store_32_helper_ = nullptr;
  void* reserved_store_64_helper_ = nullptr;
  ReservedStoreThunk reserved_store_32_thunk_ = nullptr;
  ReservedStoreThunk reserved_store_64_thunk_ = nullptr;
  ReservedStoreFaultSite reserved_store_fault_sites_[2] = {};

  alignas(64) ReserveHelper reserve_helper_;
  // The registered backend contexts, whose dynamic call caches a sweep clears.
  xe::global_critical_region global_critical_region_;
  std::vector<void*> backend_contexts_;
  BitMap guest_trampoline_address_bitmap_;
  uint8_t* guest_trampoline_memory_ = nullptr;
  bool guest_trampolines_sub4gb_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_BACKEND_H_
