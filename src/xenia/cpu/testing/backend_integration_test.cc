/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "xenia/base/platform.h"
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_code_cache.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_code_cache.h"
#include "xenia/cpu/backend/a64/a64_stack_layout.h"
#endif  // XE_ARCH

using namespace xe;
using namespace xe::cpu;
using namespace xe::cpu::hir;
using namespace xe::cpu::testing;
using xe::cpu::ppc::PPCContext;

// =============================================================================
// SetGuestRoundingMode (C++ path, not HIR opcode)
// =============================================================================
// This tests that calling SetGuestRoundingMode from C++ (as the kernel
// emulation layer does) actually changes the hardware rounding mode, so that
// subsequent JIT'd FP operations produce correctly rounded results.
TEST_CASE("SET_GUEST_ROUNDING_MODE_CPP_PATH", "[backend]") {
  // The HIR function just does an f32 add and returns the result.
  // The rounding mode is set from C++ in pre_call, NOT via HIR opcode.
  TestFunction test([](HIRBuilder& b) {
    auto a = b.Convert(LoadFPR(b, 4), FLOAT32_TYPE);
    auto c = b.Convert(LoadFPR(b, 5), FLOAT32_TYPE);
    auto sum = b.Add(a, c);
    StoreFPR(b, 3, b.Convert(sum, FLOAT64_TYPE));
    b.Return();
  });

  // Mode 2 = toward +infinity.
  test.Run(
      [&test](PPCContext* ctx) {
        ctx->f[4] = 1.0;
        ctx->f[5] = std::ldexp(1.0, -24);
        // Call the C++ SetGuestRoundingMode path.
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 2);
      },
      [&test](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        float expected = std::nextafterf(1.0f, 2.0f);
        REQUIRE(result == expected);
        // Reset to nearest for subsequent tests.
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 0);
      });

  // Mode 1 = toward zero.
  test.Run(
      [&test](PPCContext* ctx) {
        ctx->f[4] = 1.0;
        ctx->f[5] = std::ldexp(1.0, -24);
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 1);
      },
      [&test](PPCContext* ctx) {
        auto result = static_cast<float>(ctx->f[3]);
        REQUIRE(result == 1.0f);
        test.processors[0]->backend()->SetGuestRoundingMode(ctx, 0);
      });
}

// =============================================================================
// Guest Trampolines
// =============================================================================
// Test that CreateGuestTrampoline creates a callable trampoline that
// transitions from guest JIT code back to a host C++ callback.
static std::atomic<int> trampoline_call_count{0};
static void* trampoline_received_arg1 = nullptr;
static void* trampoline_received_arg2 = nullptr;

static void TrampolineCallback(ppc::PPCContext* ctx, void* userarg1,
                               void* userarg2) {
  trampoline_call_count.fetch_add(1);
  trampoline_received_arg1 = userarg1;
  trampoline_received_arg2 = userarg2;
  // Write a marker value so the test can verify the callback ran.
  ctx->r[3] = 0xCAFEBABE;
}

TEST_CASE("GUEST_TRAMPOLINE_BASIC", "[backend]") {
  // Reset global state.
  trampoline_call_count = 0;
  trampoline_received_arg1 = nullptr;
  trampoline_received_arg2 = nullptr;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  // Create a trampoline with known userdata pointers.
  void* tag1 = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1111));
  void* tag2 = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2222));
  uint32_t trampoline_addr = processor->backend()->CreateGuestTrampoline(
      TrampolineCallback, tag1, tag2, false);

  REQUIRE(trampoline_addr != 0);
  REQUIRE(trampoline_addr >= 0x80000000);
  REQUIRE(trampoline_addr < 0x80040000);

  // Clean up the trampoline.
  processor->backend()->FreeGuestTrampoline(trampoline_addr);
}

// =============================================================================
// Host -> Guest -> Host round-trip via BuiltinFunction (GuestToHostThunk)
// =============================================================================
// Tests the full thunk chain: host C++ calls into JIT'd guest code, which
// calls a builtin (host C++ function) via GuestToHostThunk, then returns.
// This exercises HostToGuestThunk entry, GuestToHostThunk transition, and
// proper return to guest code and back to host.

static std::atomic<int> builtin_call_count{0};
static void BuiltinHandler(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  builtin_call_count.fetch_add(1);
  // Write a known marker so the test can verify the builtin actually ran.
  ctx->r[3] = 0xDEADBEEF;
}

TEST_CASE("HOST_GUEST_HOST_ROUNDTRIP", "[backend]") {
  builtin_call_count = 0;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  // Define a builtin function that the guest code will call.
  auto* builtin_fn =
      processor->DefineBuiltin("TestBuiltin", BuiltinHandler, nullptr, nullptr);
  REQUIRE(builtin_fn != nullptr);

  // Create a test module with a guest function that calls the builtin.
  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        // Store a pre-call marker in r[4].
        StoreGPR(b, 4, b.LoadConstantUint64(0x11111111));
        // Call the builtin — this goes through GuestToHostThunk.
        b.CallExtern(builtin_fn);
        // Store a post-call marker in r[5] to prove we returned properly.
        StoreGPR(b, 5, b.LoadConstantUint64(0x22222222));
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;
  ctx->r[3] = 0;
  ctx->r[4] = 0;
  ctx->r[5] = 0;

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  // Verify the builtin ran.
  REQUIRE(builtin_call_count == 1);
  // Verify the builtin wrote its marker.
  REQUIRE(ctx->r[3] == 0xDEADBEEF);
  // Verify pre-call code ran.
  REQUIRE(ctx->r[4] == 0x11111111);
  // Verify post-call code ran (guest code continued after GuestToHostThunk).
  REQUIRE(ctx->r[5] == 0x22222222);

  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// GPR preservation across GuestToHostThunk
// =============================================================================
// Tests that callee-saved GPRs used by the register allocator survive a
// host call via GuestToHostThunk. We load values into several GPRs before
// the call, invoke a builtin, then read them back after.

static void EmptyBuiltin(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  // Intentionally empty — we just want to exercise the thunk transition.
}

TEST_CASE("GPR_PRESERVATION_ACROSS_HOST_CALL", "[backend]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* builtin_fn =
      processor->DefineBuiltin("EmptyBuiltin", EmptyBuiltin, nullptr, nullptr);

  // Load known values into r[10]-r[15] (via context load/store, which the
  // register allocator maps to callee-saved GPRs), call the builtin, then
  // copy them to r[3]-r[8] for verification.
  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        // Load known values from context slots.
        auto v0 = LoadGPR(b, 10);
        auto v1 = LoadGPR(b, 11);
        auto v2 = LoadGPR(b, 12);
        auto v3 = LoadGPR(b, 13);
        auto v4 = LoadGPR(b, 14);
        auto v5 = LoadGPR(b, 15);
        // Call host — this must preserve all the above.
        b.CallExtern(builtin_fn);
        // Store them back to different slots for verification.
        StoreGPR(b, 3, v0);
        StoreGPR(b, 4, v1);
        StoreGPR(b, 5, v2);
        StoreGPR(b, 6, v3);
        StoreGPR(b, 7, v4);
        StoreGPR(b, 8, v5);
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;

  // Set known values in source registers.
  ctx->r[10] = 0xAAAAAAAA00000001ULL;
  ctx->r[11] = 0xBBBBBBBB00000002ULL;
  ctx->r[12] = 0xCCCCCCCC00000003ULL;
  ctx->r[13] = 0xDDDDDDDD00000004ULL;
  ctx->r[14] = 0xEEEEEEEE00000005ULL;
  ctx->r[15] = 0xFFFFFFFF00000006ULL;

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  REQUIRE(ctx->r[3] == 0xAAAAAAAA00000001ULL);
  REQUIRE(ctx->r[4] == 0xBBBBBBBB00000002ULL);
  REQUIRE(ctx->r[5] == 0xCCCCCCCC00000003ULL);
  REQUIRE(ctx->r[6] == 0xDDDDDDDD00000004ULL);
  REQUIRE(ctx->r[7] == 0xEEEEEEEE00000005ULL);
  REQUIRE(ctx->r[8] == 0xFFFFFFFF00000006ULL);

  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// VEC register preservation across GuestToHostThunk
// =============================================================================
// Tests that vector registers allocated by the JIT survive a host call.
// Loads vec128 values into VRs, calls a builtin, then reads them back.

static void NeonClobberBuiltin(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  // This function intentionally does nothing, but the ABI allows it to
  // clobber caller-saved NEON/XMM registers. The compiler might use them
  // for local variables, memcpy, etc. The thunk must save/restore them.
}

TEST_CASE("VEC_PRESERVATION_ACROSS_HOST_CALL", "[backend]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* builtin_fn = processor->DefineBuiltin(
      "NeonClobberBuiltin", NeonClobberBuiltin, nullptr, nullptr);

  // Load vec128 values from v[10]-v[13], call builtin, store to v[3]-v[6].
  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        auto vec0 = LoadVR(b, 10);
        auto vec1 = LoadVR(b, 11);
        auto vec2 = LoadVR(b, 12);
        auto vec3 = LoadVR(b, 13);
        // Call host — thunk must preserve VEC regs.
        b.CallExtern(builtin_fn);
        // Store back for verification.
        StoreVR(b, 3, vec0);
        StoreVR(b, 4, vec1);
        StoreVR(b, 5, vec2);
        StoreVR(b, 6, vec3);
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;

  // Set known vec128 values in source VRs.
  ctx->v[10] = vec128i(0x11111111, 0x22222222, 0x33333333, 0x44444444);
  ctx->v[11] = vec128i(0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD);
  ctx->v[12] = vec128i(0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10);
  ctx->v[13] = vec128i(0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x9ABCDEF0);

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  REQUIRE(ctx->v[3] == vec128i(0x11111111, 0x22222222, 0x33333333, 0x44444444));
  REQUIRE(ctx->v[4] == vec128i(0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC, 0xDDDDDDDD));
  REQUIRE(ctx->v[5] == vec128i(0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10));
  REQUIRE(ctx->v[6] == vec128i(0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x9ABCDEF0));

  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// Basic guest code execution — context load/store round-trip
// =============================================================================
// The simplest possible test: enter guest code, read a context value, write
// it to another slot, return. Exercises HostToGuestThunk and epilog.
TEST_CASE("BASIC_GUEST_EXECUTION", "[backend]") {
  TestFunction test([](HIRBuilder& b) {
    StoreGPR(b, 3, LoadGPR(b, 4));
    b.Return();
  });

  test.Run(
      [](PPCContext* ctx) { ctx->r[4] = 0x123456789ABCDEF0ULL; },
      [](PPCContext* ctx) { REQUIRE(ctx->r[3] == 0x123456789ABCDEF0ULL); });
}

// =============================================================================
// Multiple builtin calls in sequence
// =============================================================================
// Exercises that the GuestToHostThunk properly restores state so that
// multiple host calls from the same guest function work correctly.

static std::atomic<int> multi_call_counter{0};
static void CountingBuiltin(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  multi_call_counter.fetch_add(1);
  ctx->r[3] = ctx->r[3] + 1;
}

TEST_CASE("MULTIPLE_BUILTIN_CALLS", "[backend]") {
  multi_call_counter = 0;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* builtin_fn = processor->DefineBuiltin(
      "CountingBuiltin", CountingBuiltin, nullptr, nullptr);

  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        // Initialize r[3] = 0.
        StoreGPR(b, 3, b.LoadConstantUint64(0));
        // Call builtin three times — each increments r[3].
        b.CallExtern(builtin_fn);
        b.CallExtern(builtin_fn);
        b.CallExtern(builtin_fn);
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;
  ctx->r[3] = 0;

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  REQUIRE(multi_call_counter == 3);
  REQUIRE(ctx->r[3] == 3);

  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// NJM (Non-Java Mode) default initialization
// =============================================================================
// Tests that the backend context initializes with NJM enabled by default,
// matching x64 behavior. NJM controls flush-to-zero for denormals.
static uint32_t observed_njm_flags = 0;
static void ReadBackendFlags(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  // Read the backend flags from the backend context, which lives just
  // before the PPCContext in memory.
#if XE_ARCH_AMD64
  auto* bctx = reinterpret_cast<xe::cpu::backend::x64::X64BackendContext*>(
      reinterpret_cast<intptr_t>(ctx) -
      sizeof(xe::cpu::backend::x64::X64BackendContext));
  observed_njm_flags = bctx->flags;
#elif XE_ARCH_ARM64
  auto* bctx = reinterpret_cast<xe::cpu::backend::a64::A64BackendContext*>(
      reinterpret_cast<intptr_t>(ctx) -
      sizeof(xe::cpu::backend::a64::A64BackendContext));
  observed_njm_flags = bctx->flags;
#endif
}

TEST_CASE("NJM_DEFAULT_ON", "[backend]") {
  observed_njm_flags = 0;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* builtin_fn = processor->DefineBuiltin(
      "ReadBackendFlags", ReadBackendFlags, nullptr, nullptr);

  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        b.CallExtern(builtin_fn);
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  // NJM bit (bit 2) should be set by default.
#if XE_ARCH_AMD64
  REQUIRE((observed_njm_flags &
           (1U << xe::cpu::backend::x64::kX64BackendNJMOn)) != 0);
#elif XE_ARCH_ARM64
  REQUIRE((observed_njm_flags &
           (1U << xe::cpu::backend::a64::kA64BackendNJMOn)) != 0);
#endif

  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// SET_NJM — verify NJM toggle updates backend context correctly
// =============================================================================
// NJM (Non-Java Mode) is a VMX feature (VSCR bit 16) that controls
// flush-to-zero for vector FP operations.  We verify that SET_NJM
// correctly updates the cached VMX FPCR/MXCSR and the NJM flag.
static uint32_t observed_njm_flags_after_set = 0;
static uint32_t observed_vmx_fpcr_after_set = 0;

static void ReadBackendNJMState(ppc::PPCContext* ctx, void* arg0, void* arg1) {
#if XE_ARCH_AMD64
  auto* bctx = reinterpret_cast<xe::cpu::backend::x64::X64BackendContext*>(
      reinterpret_cast<intptr_t>(ctx) -
      sizeof(xe::cpu::backend::x64::X64BackendContext));
  observed_njm_flags_after_set = bctx->flags;
  observed_vmx_fpcr_after_set = bctx->mxcsr_vmx;
#elif XE_ARCH_ARM64
  auto* bctx = reinterpret_cast<xe::cpu::backend::a64::A64BackendContext*>(
      reinterpret_cast<intptr_t>(ctx) -
      sizeof(xe::cpu::backend::a64::A64BackendContext));
  observed_njm_flags_after_set = bctx->flags;
  observed_vmx_fpcr_after_set = bctx->fpcr_vmx;
#endif
}

// Helper to build and run a SET_NJM test function.
static void RunSetNJMTest(int njm_value) {
  observed_njm_flags_after_set = 0;
  observed_vmx_fpcr_after_set = 0;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* builtin_fn = processor->DefineBuiltin(
      "ReadBackendNJMState", ReadBackendNJMState, nullptr, nullptr);

  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn, njm_value](HIRBuilder& b) {
        b.SetNJM(b.LoadConstantInt8(njm_value ? 1 : 0));
        b.CallExtern(builtin_fn);
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  memory->SystemHeapFree(stack_address);
}

TEST_CASE("SET_NJM_ON", "[backend]") {
  RunSetNJMTest(1);
  // NJM flag should be set.
#if XE_ARCH_AMD64
  REQUIRE((observed_njm_flags_after_set &
           (1U << xe::cpu::backend::x64::kX64BackendNJMOn)) != 0);
  // MXCSR should have FZ and DAZ set.
  REQUIRE((observed_vmx_fpcr_after_set & (1 << 15)) != 0);  // FZ
  REQUIRE((observed_vmx_fpcr_after_set & (1 << 6)) != 0);   // DAZ
#elif XE_ARCH_ARM64
  REQUIRE((observed_njm_flags_after_set &
           (1U << xe::cpu::backend::a64::kA64BackendNJMOn)) != 0);
  // FPCR_VMX should have FZ (bit 24) set.
  REQUIRE((observed_vmx_fpcr_after_set & (1 << 24)) != 0);
#endif
}

TEST_CASE("SET_NJM_OFF", "[backend]") {
  RunSetNJMTest(0);
  // NJM flag should be cleared.
#if XE_ARCH_AMD64
  REQUIRE((observed_njm_flags_after_set &
           (1U << xe::cpu::backend::x64::kX64BackendNJMOn)) == 0);
  // MXCSR should NOT have FZ or DAZ.
  REQUIRE((observed_vmx_fpcr_after_set & (1 << 15)) == 0);
  REQUIRE((observed_vmx_fpcr_after_set & (1 << 6)) == 0);
#elif XE_ARCH_ARM64
  REQUIRE((observed_njm_flags_after_set &
           (1U << xe::cpu::backend::a64::kA64BackendNJMOn)) == 0);
  // FPCR_VMX should NOT have FZ (bit 24).
  REQUIRE((observed_vmx_fpcr_after_set & (1 << 24)) == 0);
#endif
}

// =============================================================================
// DOT_PRODUCT_3 — inline NEON dot product of first 3 vector elements
// =============================================================================
TEST_CASE("DOT_PRODUCT_3", "[backend]") {
  TestFunction test([](HIRBuilder& b) {
    auto src1 = LoadVR(b, 10);
    auto src2 = LoadVR(b, 11);
    auto result = b.DotProduct3(src1, src2);
    StoreVR(b, 3, result);
    b.Return();
  });

  // Simple case: (1,2,3,ignored) . (4,5,6,ignored) = 1*4+2*5+3*6 = 32
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[10] = vec128f(1.0f, 2.0f, 3.0f, 99.0f);
        ctx->v[11] = vec128f(4.0f, 5.0f, 6.0f, 99.0f);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].f32[0] == 32.0f);
        REQUIRE(ctx->v[3].f32[1] == 32.0f);
        REQUIRE(ctx->v[3].f32[2] == 32.0f);
        REQUIRE(ctx->v[3].f32[3] == 32.0f);
      });

  // Zero vector.
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[10] = vec128f(0.0f, 0.0f, 0.0f, 0.0f);
        ctx->v[11] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3].f32[0] == 0.0f); });

  // Element 4 should be ignored.
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[10] = vec128f(1.0f, 0.0f, 0.0f, 1000.0f);
        ctx->v[11] = vec128f(1.0f, 0.0f, 0.0f, 1000.0f);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3].f32[0] == 1.0f); });
}

// =============================================================================
// DOT_PRODUCT_4 — inline NEON dot product of all 4 vector elements
// =============================================================================
TEST_CASE("DOT_PRODUCT_4", "[backend]") {
  TestFunction test([](HIRBuilder& b) {
    auto src1 = LoadVR(b, 10);
    auto src2 = LoadVR(b, 11);
    auto result = b.DotProduct4(src1, src2);
    StoreVR(b, 3, result);
    b.Return();
  });

  // (1,2,3,4) . (5,6,7,8) = 5+12+21+32 = 70
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[10] = vec128f(1.0f, 2.0f, 3.0f, 4.0f);
        ctx->v[11] = vec128f(5.0f, 6.0f, 7.0f, 8.0f);
      },
      [](PPCContext* ctx) {
        REQUIRE(ctx->v[3].f32[0] == 70.0f);
        REQUIRE(ctx->v[3].f32[1] == 70.0f);
        REQUIRE(ctx->v[3].f32[2] == 70.0f);
        REQUIRE(ctx->v[3].f32[3] == 70.0f);
      });

  // Length-squared: (3,4,0,0) . (3,4,0,0) = 25
  test.Run(
      [](PPCContext* ctx) {
        ctx->v[10] = vec128f(3.0f, 4.0f, 0.0f, 0.0f);
        ctx->v[11] = vec128f(3.0f, 4.0f, 0.0f, 0.0f);
      },
      [](PPCContext* ctx) { REQUIRE(ctx->v[3].f32[0] == 25.0f); });
}

// =============================================================================
// FPCR preservation across GuestToHostThunk
// =============================================================================
// Tests that the guest scalar rounding mode survives a host callback.
// The GuestToHostThunk must restore fpcr_fpu after the host call returns,
// otherwise the host C++ runtime's FPCR state leaks into subsequent guest ops.
// It must also enter host code with the host FPCR/MXCSR, not the guest's.

static float host_sum = 0.0f;
static float host_denormal_product = 0.0f;

static void FpcrClobberingBuiltin(ppc::PPCContext* ctx, void* arg0,
                                  void* arg1) {
  volatile float tiny = std::ldexp(1.0f, -24);
  volatile float denormal = std::ldexp(1.0f, -140);
  host_sum = 1.0f + tiny;
  host_denormal_product = denormal * 2.0f;
  // Deliberately clobber FPCR to round-to-nearest (mode 0).
  // If the thunk doesn't restore, the guest will see this mode.
#if XE_ARCH_ARM64
#if XE_COMPILER_MSVC
  _WriteStatusReg(0x5A20, 0ULL);
#else
  __asm__ volatile("msr fpcr, %0" : : "r"(0ULL));
#endif
#elif XE_ARCH_AMD64
  _mm_setcsr((_mm_getcsr() & ~0x6000) | 0x0000);  // round-to-nearest
#endif
}

TEST_CASE("FPCR_PRESERVED_ACROSS_HOST_CALLBACK", "[backend]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* builtin_fn = processor->DefineBuiltin(
      "FpcrClobber", FpcrClobberingBuiltin, nullptr, nullptr);

  // Set rounding to toward-+inf, call the host callback (which clobbers FPCR
  // to round-to-nearest), then do a scalar add. If the thunk restores FPCR
  // properly, the add uses toward-+inf.
  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        b.SetRoundingMode(b.LoadConstantInt32(2));  // toward +inf
        b.CallExtern(builtin_fn);
        // Scalar add after the host call.
        auto a = b.Convert(LoadFPR(b, 4), FLOAT32_TYPE);
        auto c = b.Convert(LoadFPR(b, 5), FLOAT32_TYPE);
        auto sum = b.Add(a, c);
        StoreFPR(b, 3, b.Convert(sum, FLOAT64_TYPE));
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;
  processor->backend()->SetGuestRoundingMode(ctx, 0);

  ctx->f[4] = 1.0;
  ctx->f[5] = std::ldexp(1.0, -24);

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  auto result = static_cast<float>(ctx->f[3]);
  // Toward-+inf: 1.0 + 2^-24 rounds up.
  float expected = std::nextafterf(1.0f, 2.0f);
  REQUIRE(result == expected);
  // The host code rounded to nearest and kept the denormal. A literal, so the
  // expectation does not depend on the FP mode this code runs in.
  REQUIRE(host_sum == 1.0f);
  REQUIRE(host_denormal_product == 0x1p-139f);

  // Reset rounding mode.
  processor->backend()->SetGuestRoundingMode(ctx, 0);
  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// Unwind info registration for JIT code
// =============================================================================
// Verify that the backend registers unwind data for JIT'd functions so that
// debuggers, profilers, and exception handlers can walk the stack through
// JIT code.
//
// Windows: RtlLookupFunctionEntry directly queries the registered SEH tables.
// POSIX: we unwind from inside a JIT callback and verify we get enough frames
// to have unwound through the JIT thunks.  This exercises the DWARF .eh_frame
// data registered via __register_frame.
//
// _Unwind_Backtrace is used rather than backtrace(3) because it consults the
// registered FDEs on both glibc and Apple libunwind.  backtrace(3) on Apple
// arm64 walks the x29 frame-pointer chain instead, which JIT'd code does not
// maintain, so it stops at the first JIT frame regardless of unwind data.

#if !XE_PLATFORM_WIN32
#include <unwind.h>
static int jit_backtrace_depth = 0;
static _Unwind_Reason_Code CountJITFrame(struct _Unwind_Context* context,
                                         void* arg) {
  ++jit_backtrace_depth;
  return _URC_NO_REASON;
}
static void CaptureJITBacktrace(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  jit_backtrace_depth = 0;
  _Unwind_Backtrace(CountJITFrame, nullptr);
}
#endif

TEST_CASE("JIT_UNWIND_INFO_REGISTERED", "[backend]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

#if XE_PLATFORM_WIN32
  // Compile a minimal guest function and check that Windows can find its
  // RUNTIME_FUNCTION entry via RtlLookupFunctionEntry.
  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [](HIRBuilder& b) {
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  auto* guest_fn = static_cast<GuestFunction*>(fn);
  void* code = guest_fn->machine_code();
  REQUIRE(code != nullptr);

  DWORD64 image_base = 0;
  auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(code),
                                       &image_base, nullptr);
  REQUIRE(entry != nullptr);
  REQUIRE(image_base != 0);
#else
  // On POSIX, unwind from inside a JIT callback. If the .eh_frame unwind info
  // is correctly registered, the unwinder will walk through:
  //   callback -> GuestToHostThunk -> guest func -> HostToGuestThunk -> Call
  // giving at least 4 frames. Without unwind info it stops at 1-2.
  jit_backtrace_depth = 0;

  auto* builtin_fn = processor->DefineBuiltin(
      "CaptureJITBacktrace", CaptureJITBacktrace, nullptr, nullptr);

  auto module = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) { return address == 0x80000000; },
      [builtin_fn](HIRBuilder& b) {
        b.CallExtern(builtin_fn);
        b.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module));
  processor->backend()->CommitExecutableRange(0x80000000, 0x80010000);

  auto fn = processor->ResolveFunction(0x80000000);
  REQUIRE(fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;

  fn->Call(thread_state.get(), uint32_t(ctx->lr));

  REQUIRE(jit_backtrace_depth >= 4);

  memory->SystemHeapFree(stack_address);
#endif

  memory.reset();
}

// =============================================================================
// Guest -> Guest call through the JIT indirection dispatch
// =============================================================================
// Exercises a guest CALL between two JIT'd functions — the emitter's
// indirection-table lookup and the backend's call/jmp.
//
// The caller loads several guest GPRs into HIR values *before* the call and
// stores them back afterwards.  The regalloc keeps these values in host
// registers from gpr_reg_map_ (x64: R10-R15; A64: X19+) across the call,
// so any indirection-lookup emit that clobbers an HIR-allocatable register
// for scratch will silently trash them and the canary REQUIREs below will
// fail.
TEST_CASE("GUEST_TO_GUEST_CALL", "[backend]") {
  constexpr uint32_t kCallerAddr = 0x80000000;
  constexpr uint32_t kCalleeAddr = 0x80001000;
  constexpr uint64_t kSentinel = 0xCAFEBEEFD00DF00Dull;

  // Distinct canaries — one per guest GPR we carry across the call.
  // Values chosen so any corruption (including low-32 truncation or
  // zero-extension) is visible.
  constexpr uint64_t kCanary4 = 0x1111111122222222ull;
  constexpr uint64_t kCanary5 = 0x3333333344444444ull;
  constexpr uint64_t kCanary6 = 0x5555555566666666ull;
  constexpr uint64_t kCanary7 = 0x7777777788888888ull;
  constexpr uint64_t kCanary8 = 0x9999999900000000ull;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  // TestModule invokes the single generator once per DeclareFunction, so
  // we resolve the callee first and let the caller's HIR reference it.
  int gen_invocation = 0;
  Function* callee_fn = nullptr;
  auto module_owner = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) {
        return address == kCallerAddr || address == kCalleeAddr;
      },
      [&](HIRBuilder& b) {
        if (gen_invocation++ == 0) {
          // Callee: sentinel into r[3], return.
          StoreGPR(b, 3, b.LoadConstantUint64(kSentinel));
          b.Return();
        } else {
          // Caller: r[4..8] -> HIR values live across the call -> r[20..24].
          REQUIRE(callee_fn != nullptr);
          auto v4 = LoadGPR(b, 4);
          auto v5 = LoadGPR(b, 5);
          auto v6 = LoadGPR(b, 6);
          auto v7 = LoadGPR(b, 7);
          auto v8 = LoadGPR(b, 8);
          b.Call(callee_fn);
          StoreGPR(b, 20, v4);
          StoreGPR(b, 21, v5);
          StoreGPR(b, 22, v6);
          StoreGPR(b, 23, v7);
          StoreGPR(b, 24, v8);
          b.Return();
        }
        return true;
      },
      // HIRBuilder::Call ends its block and no fallthrough edge is added,
      // so ControlFlowSimplificationPass would discard the post-call block
      // as "unreachable" and wipe the StoreContexts.  Real PPC code avoids
      // this because the frontend emits explicit branches.
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module_owner));
  processor->backend()->CommitExecutableRange(kCallerAddr,
                                              kCalleeAddr + 0x1000);

  // Resolve the callee first so its Function* is available when the caller's
  // HIR is generated.
  callee_fn = processor->ResolveFunction(kCalleeAddr);
  REQUIRE(callee_fn != nullptr);

  auto* caller_fn = processor->ResolveFunction(kCallerAddr);
  REQUIRE(caller_fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;
  ctx->r[3] = 0;
  ctx->r[4] = kCanary4;
  ctx->r[5] = kCanary5;
  ctx->r[6] = kCanary6;
  ctx->r[7] = kCanary7;
  ctx->r[8] = kCanary8;

  caller_fn->Call(thread_state.get(), uint32_t(ctx->lr));

  // Callee ran and wrote the sentinel into r[3].
  REQUIRE(ctx->r[3] == kSentinel);

  // Live HIR values threaded through the indirection lookup uncorrupted.
  // Failure => the lookup clobbered an HIR-allocatable host register
  // (x64: R10-R15; A64: X19+).
  REQUIRE(ctx->r[20] == kCanary4);
  REQUIRE(ctx->r[21] == kCanary5);
  REQUIRE(ctx->r[22] == kCanary6);
  REQUIRE(ctx->r[23] == kCanary7);
  REQUIRE(ctx->r[24] == kCanary8);

  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// Inlined PPC GPR/LR save-restore helpers
// =============================================================================
// Guest binaries call __savegprlr_N / __restgprlr_N in their prologue and
// epilogue instead of spilling registers inline.  The backend recognizes those
// callees and emits the helper body directly rather than emitting a call, so
// these assert the resulting guest memory and register state.
//
// The XEX loader tags the helpers by byte-pattern search over the guest image
// (xex_module.cc); SetSaverest is called directly here so no XEX is needed.
//
// Helper layout for first GPR N:
//   std rG, -((33 - N) * 8)(r1)   for G = N..31
//   stw r12, -8(r1)               the caller's LR, a 32-bit slot
namespace {
constexpr uint32_t kSaverestCallerAddr = 0x80000000;
constexpr uint32_t kSaverestHelperAddr = 0x80001000;
constexpr unsigned kSaverestFirstGpr = 14;
constexpr uint32_t kSaverestFirstSlot = (33 - kSaverestFirstGpr) * 8;
constexpr uint32_t kSaverestRetAddr = 0xBCBCBCBC;

// Distinct per-register value; a slot mixup or a 32-bit truncation is visible.
uint64_t SaverestCanary(unsigned guest_reg) {
  return (uint64_t(0xC0DE0000u + guest_reg) << 32) | (0xBEEF0000u + guest_reg);
}

// Guest address of the spill slot for `guest_reg`, given the guest stack ptr.
uint32_t SaverestSlot(uint32_t guest_sp, unsigned guest_reg) {
  return guest_sp - kSaverestFirstSlot + (guest_reg - kSaverestFirstGpr) * 8;
}
}  // namespace

TEST_CASE("PPC_SAVEGPRLR_INLINED", "[backend]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  int gen_invocation = 0;
  Function* helper_fn = nullptr;
  auto module_owner = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) {
        return address == kSaverestCallerAddr || address == kSaverestHelperAddr;
      },
      [&](HIRBuilder& b) {
        if (gen_invocation++ == 0) {
          // Helper body: the backend inlines the call, so this never runs.
          b.Return();
        } else {
          REQUIRE(helper_fn != nullptr);
          b.Call(helper_fn);
          b.Return();
        }
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module_owner));
  processor->backend()->CommitExecutableRange(kSaverestCallerAddr,
                                              kSaverestHelperAddr + 0x1000);

  helper_fn = processor->ResolveFunction(kSaverestHelperAddr);
  REQUIRE(helper_fn != nullptr);
  helper_fn->SetSaverest(SaveRestoreType::GPR, /*is_rest=*/false,
                         kSaverestFirstGpr);

  auto* caller_fn = processor->ResolveFunction(kSaverestCallerAddr);
  REQUIRE(caller_fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  uint32_t frame_size = 1024;
  uint32_t frame_address = memory->SystemHeapAlloc(frame_size);
  // The helper writes below r1, so leave headroom underneath it.
  uint32_t guest_sp = frame_address + frame_size / 2;
  std::memset(memory->TranslateVirtual(frame_address), 0, frame_size);

  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = kSaverestRetAddr;
  ctx->r[1] = guest_sp;
  ctx->r[12] = 0x1234ABCD;
  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    ctx->r[g] = SaverestCanary(g);
  }

  caller_fn->Call(thread_state.get(), kSaverestRetAddr);

  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    INFO("guest r" << g);
    REQUIRE(load_and_swap<uint64_t>(memory->TranslateVirtual(
                SaverestSlot(guest_sp, g))) == SaverestCanary(g));
  }
  REQUIRE(load_and_swap<uint32_t>(memory->TranslateVirtual(guest_sp - 8)) ==
          uint32_t(ctx->r[12]));

  memory->SystemHeapFree(frame_address);
  memory->SystemHeapFree(stack_address);
}

// The restore helper is always tail-called: it reloads the GPRs and LR, then
// returns to the reloaded LR.  With the spilled LR equal to the caller's own
// return address the backend takes its epilogue, so the call returns normally
// here and the restored context can be inspected.
TEST_CASE("PPC_RESTGPRLR_INLINED", "[backend]") {
  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  int gen_invocation = 0;
  Function* helper_fn = nullptr;
  auto module_owner = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) {
        return address == kSaverestCallerAddr || address == kSaverestHelperAddr;
      },
      [&](HIRBuilder& b) {
        if (gen_invocation++ == 0) {
          b.Return();
        } else {
          REQUIRE(helper_fn != nullptr);
          b.Call(helper_fn, CALL_TAIL);
          b.Return();
        }
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module_owner));
  processor->backend()->CommitExecutableRange(kSaverestCallerAddr,
                                              kSaverestHelperAddr + 0x1000);

  helper_fn = processor->ResolveFunction(kSaverestHelperAddr);
  REQUIRE(helper_fn != nullptr);
  helper_fn->SetSaverest(SaveRestoreType::GPR, /*is_rest=*/true,
                         kSaverestFirstGpr);

  auto* caller_fn = processor->ResolveFunction(kSaverestCallerAddr);
  REQUIRE(caller_fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  uint32_t frame_size = 1024;
  uint32_t frame_address = memory->SystemHeapAlloc(frame_size);
  uint32_t guest_sp = frame_address + frame_size / 2;
  std::memset(memory->TranslateVirtual(frame_address), 0, frame_size);

  // Seed the spill slots as a matching __savegprlr_14 would have left them.
  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    store_and_swap<uint64_t>(
        memory->TranslateVirtual(SaverestSlot(guest_sp, g)), SaverestCanary(g));
  }
  store_and_swap<uint32_t>(memory->TranslateVirtual(guest_sp - 8),
                           kSaverestRetAddr);

  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0;
  ctx->r[1] = guest_sp;
  ctx->r[12] = 0;
  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    ctx->r[g] = 0;
  }

  caller_fn->Call(thread_state.get(), kSaverestRetAddr);

  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    INFO("guest r" << g);
    REQUIRE(ctx->r[g] == SaverestCanary(g));
  }
  // The reloaded LR lands in both r12 and lr, zero-extended from 32 bits.
  REQUIRE(ctx->r[12] == kSaverestRetAddr);
  REQUIRE(ctx->lr == kSaverestRetAddr);

  memory->SystemHeapFree(frame_address);
  memory->SystemHeapFree(stack_address);
}

// The other half of the restore's branch: when the reloaded LR is *not* the
// caller's own return address the backend must dispatch to it as a tail call
// rather than taking the epilogue.  The spilled LR here points at a third
// guest function, which writes a sentinel so the landing is observable.
TEST_CASE("PPC_RESTGPRLR_INLINED_TAIL_DISPATCH", "[backend]") {
  constexpr uint32_t kTailTargetAddr = 0x80002000;
  constexpr uint64_t kTailSentinel = 0x5A5A1234BEEF0001ull;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  int gen_invocation = 0;
  Function* helper_fn = nullptr;
  auto module_owner = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) {
        return address == kSaverestCallerAddr ||
               address == kSaverestHelperAddr || address == kTailTargetAddr;
      },
      [&](HIRBuilder& b) {
        switch (gen_invocation++) {
          case 0:
            // Helper body; the backend inlines the call so this never runs.
            b.Return();
            break;
          case 1:
            // Tail-call target: proves the dispatch landed here.
            StoreGPR(b, 3, b.LoadConstantUint64(kTailSentinel));
            b.Return();
            break;
          default:
            REQUIRE(helper_fn != nullptr);
            b.Call(helper_fn, CALL_TAIL);
            b.Return();
            break;
        }
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module_owner));
  processor->backend()->CommitExecutableRange(kSaverestCallerAddr,
                                              kTailTargetAddr + 0x1000);

  helper_fn = processor->ResolveFunction(kSaverestHelperAddr);
  REQUIRE(helper_fn != nullptr);
  helper_fn->SetSaverest(SaveRestoreType::GPR, /*is_rest=*/true,
                         kSaverestFirstGpr);

  // Resolve the tail target before the caller so it is in the indirection
  // table by the time the restore dispatches to it.
  auto* target_fn = processor->ResolveFunction(kTailTargetAddr);
  REQUIRE(target_fn != nullptr);

  auto* caller_fn = processor->ResolveFunction(kSaverestCallerAddr);
  REQUIRE(caller_fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  uint32_t frame_size = 1024;
  uint32_t frame_address = memory->SystemHeapAlloc(frame_size);
  uint32_t guest_sp = frame_address + frame_size / 2;
  std::memset(memory->TranslateVirtual(frame_address), 0, frame_size);

  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    store_and_swap<uint64_t>(
        memory->TranslateVirtual(SaverestSlot(guest_sp, g)), SaverestCanary(g));
  }
  // Spilled LR differs from the return address passed to Call below, so the
  // epilogue comparison must fail and the tail dispatch must run.
  store_and_swap<uint32_t>(memory->TranslateVirtual(guest_sp - 8),
                           kTailTargetAddr);

  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0;
  ctx->r[1] = guest_sp;
  ctx->r[3] = 0;
  ctx->r[12] = 0;
  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    ctx->r[g] = 0;
  }

  caller_fn->Call(thread_state.get(), kSaverestRetAddr);

  // The dispatch landed in the target rather than returning early.
  REQUIRE(ctx->r[3] == kTailSentinel);
  for (unsigned g = kSaverestFirstGpr; g <= 31; ++g) {
    INFO("guest r" << g);
    REQUIRE(ctx->r[g] == SaverestCanary(g));
  }
  REQUIRE(ctx->r[12] == kTailTargetAddr);
  REQUIRE(ctx->lr == kTailTargetAddr);

  memory->SystemHeapFree(frame_address);
  memory->SystemHeapFree(stack_address);
}

// =============================================================================
// Stackpoints describe the frame they were recorded in
// =============================================================================
// A stackpoint is recorded once the prolog has allocated the frame, so
// host_stack_ is that frame's own stack pointer. The sync helper restores it as
// it stands and ResolveDynamicReturn reads the host return address at a fixed
// offset from it, neither of which a test can reach through a longjmp.

#if XE_ARCH_AMD64
using ProbeBackend = xe::cpu::backend::x64::X64Backend;
#elif XE_ARCH_ARM64
using ProbeBackend = xe::cpu::backend::a64::A64Backend;
#endif  // XE_ARCH

namespace {
std::atomic<int> stackpoint_probe_calls{0};
uint32_t probe_depth = 0;
uint64_t probe_host_stacks[2] = {0, 0};
uint32_t probe_guest_stacks[2] = {0, 0};
uint64_t probe_host_return = 0;

// Runs with the caller and callee guest frames both live.
void StackpointProbeHandler(ppc::PPCContext* ctx, void* arg0, void* arg1) {
  stackpoint_probe_calls.fetch_add(1);
  auto backend = static_cast<ProbeBackend*>(ctx->processor->backend());
  auto bctx = backend->BackendContextForGuestContext(ctx);
  probe_depth = bctx->current_stackpoint_depth;
  if (!bctx->stackpoints || probe_depth < 2) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    probe_host_stacks[i] = bctx->stackpoints[i].host_stack_;
    probe_guest_stacks[i] = bctx->stackpoints[i].guest_stack_;
  }
#if XE_ARCH_AMD64
  // Read it the way ResolveDynamicReturn does, below the caller's record.
  probe_host_return = *reinterpret_cast<const uint64_t*>(
      bctx->stackpoints[0].host_stack_ - sizeof(uint64_t));
#elif XE_ARCH_ARM64
  // Read it the way ResolveDynamicReturn does, inside the callee's frame.
  probe_host_return = *reinterpret_cast<const uint64_t*>(
      bctx->stackpoints[1].host_stack_ +
      xe::cpu::backend::a64::StackLayout::HOST_RET_ADDR);
#endif  // XE_ARCH
}
}  // namespace

TEST_CASE("STACKPOINT_DESCRIBES_ITS_OWN_FRAME", "[backend]") {
  constexpr uint32_t kProbeCallerAddr = 0x80000000;
  constexpr uint32_t kProbeCalleeAddr = 0x80001000;

  stackpoint_probe_calls = 0;
  probe_depth = 0;
  probe_host_return = 0;

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  auto backend = CreateBackend();
  REQUIRE(backend);

  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));

  auto* probe_fn = processor->DefineBuiltin(
      "StackpointProbe", StackpointProbeHandler, nullptr, nullptr);
  REQUIRE(probe_fn != nullptr);

  int gen_invocation = 0;
  Function* callee_fn = nullptr;
  auto module_owner = std::make_unique<TestModule>(
      processor.get(), "Test",
      [](uint32_t address) {
        return address == kProbeCallerAddr || address == kProbeCalleeAddr;
      },
      [&](HIRBuilder& b) {
        if (gen_invocation++ == 0) {
          b.CallExtern(probe_fn);
          b.Return();
        } else {
          REQUIRE(callee_fn != nullptr);
          b.Call(callee_fn);
          b.Return();
        }
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(module_owner));
  processor->backend()->CommitExecutableRange(kProbeCallerAddr,
                                              kProbeCalleeAddr + 0x1000);

  callee_fn = processor->ResolveFunction(kProbeCalleeAddr);
  REQUIRE(callee_fn != nullptr);
  auto* caller_fn = processor->ResolveFunction(kProbeCallerAddr);
  REQUIRE(caller_fn != nullptr);

  uint32_t stack_size = 64 * 1024;
  uint32_t stack_address = memory->SystemHeapAlloc(stack_size);
  auto thread_state = std::make_unique<ThreadState>(processor.get(), 0x100,
                                                    stack_address + stack_size);
  auto ctx = thread_state->context();
  ctx->lr = 0xBCBCBCBC;
  const uint32_t entry_guest_stack = uint32_t(ctx->r[1]);

  caller_fn->Call(thread_state.get(), uint32_t(ctx->lr));

  REQUIRE(stackpoint_probe_calls == 1);
  // Both guest frames were recorded, the caller at 0 and the callee at 1.
  REQUIRE(probe_depth == 2);
  // A frame's own stack pointer is 16 byte aligned. A record taken before the
  // prolog allocated would hold the entry stack pointer, which on x64 still
  // carries the pushed return address and is 8 off.
  REQUIRE(probe_host_stacks[0] % 16 == 0);
  REQUIRE(probe_host_stacks[1] % 16 == 0);
  REQUIRE(probe_host_stacks[1] < probe_host_stacks[0]);
  // Neither function touches r1.
  REQUIRE(probe_guest_stacks[0] == entry_guest_stack);
  REQUIRE(probe_guest_stacks[1] == entry_guest_stack);
  // The host return address ResolveDynamicReturn reads lands in the caller.
  REQUIRE(probe_host_return != 0);
  auto* code_cache =
      static_cast<ProbeBackend*>(processor->backend())->code_cache();
  REQUIRE(code_cache->LookupFunction(probe_host_return) == caller_fn);

  memory->SystemHeapFree(stack_address);
}
