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
#include <memory>

#include "xenia/base/bit_map.h"
#include "xenia/base/cvar.h"
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

// One record per live guest frame, embedded in that frame's host stack at
// StackLayout::STACKPOINT_PREV and linked through stackpoint_head. The
// record's own address identifies the frame: address - STACKPOINT_PREV is
// the frame's post-alloc SP.
struct A64StackpointNode {
  const A64StackpointNode* prev_;  // older frame's node, null at chain root
  uint32_t guest_stack_;           // guest r1 at function entry
  uint32_t guest_return_address_;  // guest lr at function entry
};
static_assert(sizeof(A64StackpointNode) == 16,
              "the push emission pairs prev_ with the ret-slot zeroing and "
              "the two guest words with one stp");

// Walks the chain looking for a longjmp: more than one live frame skipped
// by |guest_sp|. Returns the node to restore, or null (no repair).
const A64StackpointNode* FindStackpointSyncNode(const A64StackpointNode* head,
                                                uint32_t guest_sp);

enum : uint32_t {
  kA64BackendFPCRModeBit = 0,
  kA64BackendHasReserveBit = 1,
  kA64BackendNJMOn = 2,
  kA64BackendNonIEEEMode = 3,
};

// Located prior to the context register (x20) in memory.
// vexptefp/vlogefp estimate constants, splatted across all four lanes. a64 has
// no memory operands and only v0-v3 are scratch, so these live in the backend
// context and load with a single ldr q rather than being materialized.
enum A64EstConst {
  kEstExp2Poly = 0,                 // 6 entries, 2^f minimax on [0,1)
  kEstLog2Poly = kEstExp2Poly + 6,  // 7 entries, log2(1+u) minimax on [0,1]
  kEstScale = kEstLog2Poly + 7,     // 2048.0f
  kEstUnscale,                      // 1.0f / 2048.0f
  kEstExp2Max,                      // 128.0f
  kEstExp2Min,                      // -126.0f
  kEstOne,                          // 0x3F800000
  kEstInt127,                       // 127
  kEstPosInf,                       // 0x7F800000
  kEstNegInf,                       // 0xFF800000
  kEstQNaN,                         // 0x7FC00000
  kEstMantissaMask,                 // 0x007FFFFF
  kEstQuietBit,                     // 0x00400000
  kEstConstCount,
};

struct A64BackendContext {
  alignas(16) uint32_t est_consts[kEstConstCount][4];
  // Scratch vectors for helper routines.
  // Using uint8_t[16] instead of NEON intrinsic types to avoid including
  // arm_neon.h in the header.
  alignas(16) uint8_t helper_scratch_v128s[4][16];
  union {
    uint64_t helper_scratch_u64s[8];
    uint32_t helper_scratch_u32s[16];
  };
  ReserveHelper* reserve_helper_;
  uint64_t cached_reserve_value_;
  uint64_t* guest_tick_count;
  // Constants for CALL_INDIRECT's encoded indirection mode; a call site
  // loads each with one ldr from x19. Stable after code-cache Initialize.
  uint64_t indirection_table_bias;
  uint64_t code_execute_base;
  uint64_t external_indirection_table;
  // The guest-to-host thunk's address, loaded the same way by CallNativeSafe
  // and the preempt yield. Stable once Initialize emits the thunk.
  uint64_t guest_to_host_thunk_address;
  // Same thunk without the vector save/restore, for transitions where
  // no HIR value can be live in q4-q31. See EmitGuestToHostThunkNoVec.
  uint64_t guest_to_host_thunk_no_vec_address;
  const A64StackpointNode* stackpoint_head;
  // address of the live reservation, and its granule generation when taken
  uint32_t reserve_address;
  uint32_t reserve_generation;
  const A64StackpointNode* pending_stackpoint_sync_node;
  unsigned int fpcr_fpu;
  unsigned int fpcr_vmx;
  // bit 0 = 0 if fpcr is fpu, else it is vmx
  // bit 1 = got reserve
  unsigned int flags;
  unsigned int Ox1000;  // constant 0x1000
  // DEFAULT_VMX_FPCR regardless of NJM, for the ops that always flush
  unsigned int fpcr_vmx_daz;
};

// Default FPCR for FPU mode (round to nearest, no flush to zero).
constexpr unsigned int DEFAULT_FPU_FPCR = 0;
// Default FPCR for VMX mode (flush to zero, preserve NaN payloads).
constexpr unsigned int DEFAULT_VMX_FPCR = (1 << 24);  // FZ
// Invariant: FPCR.DN is clear in every FPCR image this backend installs
// (these defaults, SET_ROUNDING_MODE's fpcr_table, SET_NJM's FZ toggle).
// With DN clear a NaN operand propagates its payload and an invalid
// operation with no NaN operand produces the default QNaN (0x7FC00000 /
// 0x7FF8000000000000), which is also PPC's default QNaN. The scalar and
// vector NaN fixups in a64_sequences.cc and a64_seq_util.h rely on both.

class A64Backend : public Backend {
 public:
  static constexpr uint32_t kForceReturnAddress = 0x9FFF0000u;

  explicit A64Backend();
  ~A64Backend() override;

  A64CodeCache* code_cache() const { return code_cache_.get(); }
  uintptr_t emitter_data() const { return emitter_data_; }

  std::string name() const override { return "a64"; }

  HostToGuestThunk host_to_guest_thunk() const { return host_to_guest_thunk_; }
  GuestToHostThunk guest_to_host_thunk() const { return guest_to_host_thunk_; }
  GuestToHostThunk guest_to_host_thunk_no_vec() const {
    return guest_to_host_thunk_no_vec_;
  }
  ResolveFunctionThunk resolve_function_thunk() const {
    return resolve_function_thunk_;
  }
  void* synchronize_guest_and_host_stack_helper() const {
    return synchronize_guest_and_host_stack_helper_;
  }
  void* vrsqrtefp_scalar_helper() const { return vrsqrtefp_scalar_helper_; }
  void* vrsqrtefp_vector_helper() const { return vrsqrtefp_vector_helper_; }
  void* frsqrte_helper() const { return frsqrte_helper_; }

  bool Initialize(Processor* processor) override;

  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) override;

  std::unique_ptr<Assembler> CreateAssembler() override;

  std::unique_ptr<GuestFunction> CreateGuestFunction(Module* module,
                                                     uint32_t address) override;

  uint64_t CalculateNextHostInstruction(ThreadDebugInfo* thread_info,
                                        uint64_t current_pc) override;

  void InstallBreakpoint(Breakpoint* breakpoint) override;
  void InstallBreakpoint(Breakpoint* breakpoint, Function* fn) override;
  void UninstallBreakpoint(Breakpoint* breakpoint) override;
  void InitializeBackendContext(void* ctx) override;
  void DeinitializeBackendContext(void* ctx) override;
  void PrepareForReentry(void* ctx) override;

  A64BackendContext* BackendContextForGuestContext(void* ctx) {
    return reinterpret_cast<A64BackendContext*>(
        reinterpret_cast<intptr_t>(ctx) - sizeof(A64BackendContext));
  }

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

  uintptr_t capstone_handle_ = 0;

  std::unique_ptr<A64CodeCache> code_cache_;
  uintptr_t emitter_data_ = 0;

  HostToGuestThunk host_to_guest_thunk_ = nullptr;
  GuestToHostThunk guest_to_host_thunk_ = nullptr;
  GuestToHostThunk guest_to_host_thunk_no_vec_ = nullptr;
  ResolveFunctionThunk resolve_function_thunk_ = nullptr;
  void* synchronize_guest_and_host_stack_helper_ = nullptr;
  void* vrsqrtefp_scalar_helper_ = nullptr;
  void* vrsqrtefp_vector_helper_ = nullptr;
  void* frsqrte_helper_ = nullptr;

  alignas(64) ReserveHelper reserve_helper_;
  BitMap guest_trampoline_address_bitmap_;
  uint8_t* guest_trampoline_memory_ = nullptr;
  bool guest_trampolines_sub4gb_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_BACKEND_H_
