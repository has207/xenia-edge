/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "xenia/base/byte_order.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

// User mode code and the handler it traps into run on a per-thread fiber.
// User mode code runs against its own membase, see EnableUserModeViews.

namespace xe {
namespace kernel {
namespace xboxkrnl {

using namespace xe::literals;
using cpu::ppc::PPCContext;

namespace {

// Points the JIT's guest addressing at the user mode or kernel address space.
void SetUserAddressing(PPCContext* context, bool user) {
  auto memory = kernel_memory();
  if (user) {
    context->virtual_membase = memory->user_virtual_membase();
    context->tlb_read = memory->user_tlb(false);
    context->tlb_write = memory->user_tlb(true);
  } else {
    context->virtual_membase = memory->virtual_membase();
    context->tlb_read = memory->kernel_tlb();
    context->tlb_write = memory->kernel_tlb();
  }
}

// Trap frame layout. RuntimeHost uses the GPRs, lr and iar. XeFu also uses
// ctr, cr and xer and sets ctr before resuming. Where msr goes is unknown.
constexpr uint32_t kKframesSize = XThread::UserMode::kInterruptFrameSize;
constexpr uint32_t kKframesGpr = 0x50;
constexpr uint32_t kKframesMsr = 0x160;
constexpr uint32_t kKframesCtr = 0x1A0;
constexpr uint32_t kKframesLr = 0x1A8;
constexpr uint32_t kKframesIar = 0x1AC;
constexpr uint32_t kKframesCr = XThread::UserMode::kInterruptFrameCr;
constexpr uint32_t kKframesXer = 0x1B8;

// MSR[PR], which a trap frame from user mode carries.
constexpr uint32_t kMsrUserMode = 0x4000;

// The exception record a fault hands the trap handler.
constexpr uint32_t kRecordCode = 0x0;
constexpr uint32_t kRecordAddress = 0xC;
constexpr uint32_t kRecordParameterCount = 0x10;
constexpr uint32_t kRecordInformation = 0x14;
constexpr uint32_t kRecordSize = 0x1C;

constexpr uint32_t kReturnSentinel = 0xBCBCBCBC;
// Keeps the handler clear of the frame that called KeEnterUserMode.
constexpr uint64_t kHandlerStackGap = 0x100;

uint32_t GetXer(const PPCContext* context) {
  return (uint32_t(context->xer_so) << 31) | (uint32_t(context->xer_ov) << 30) |
         (uint32_t(context->xer_ca) << 29);
}

void SetXer(PPCContext* context, uint32_t xer) {
  context->xer_so = (xer >> 31) & 1;
  context->xer_ov = (xer >> 30) & 1;
  context->xer_ca = (xer >> 29) & 1;
}

void SaveKframes(PPCContext* context, uint8_t* kframes, uint32_t iar) {
  std::memset(kframes, 0, kKframesSize);
  for (size_t i = 0; i < 32; ++i) {
    xe::store_and_swap<uint64_t>(kframes + kKframesGpr + i * 8, context->r[i]);
  }
  xe::store_and_swap<uint64_t>(kframes + kKframesCtr, context->ctr);
  xe::store_and_swap<uint32_t>(kframes + kKframesCr, uint32_t(context->cr()));
  xe::store_and_swap<uint32_t>(kframes + kKframesXer, GetXer(context));
  xe::store_and_swap<uint32_t>(kframes + kKframesMsr,
                               uint32_t(context->msr) | kMsrUserMode);
  xe::store_and_swap<uint32_t>(kframes + kKframesLr, uint32_t(context->lr));
  xe::store_and_swap<uint32_t>(kframes + kKframesIar, iar);
}

uint32_t LoadKframes(PPCContext* context, const uint8_t* kframes) {
  for (size_t i = 0; i < 32; ++i) {
    context->r[i] = xe::load_and_swap<uint64_t>(kframes + kKframesGpr + i * 8);
  }
  context->ctr = xe::load_and_swap<uint64_t>(kframes + kKframesCtr);
  context->set_cr(xe::load_and_swap<uint32_t>(kframes + kKframesCr));
  SetXer(context, xe::load_and_swap<uint32_t>(kframes + kKframesXer));
  context->lr = xe::load_and_swap<uint32_t>(kframes + kKframesLr);
  return xe::load_and_swap<uint32_t>(kframes + kKframesIar);
}

// Whether |fiber| runs user code or the trap handler for |user_mode|.
bool IsUserModeFiber(XThread::UserMode* user_mode,
                     xe::threading::Fiber* fiber) {
  if (fiber == user_mode->handler_fiber.get()) {
    return true;
  }
  for (auto& user_fiber : user_mode->fibers) {
    if (fiber == user_fiber->fiber.get()) {
      return true;
    }
  }
  return false;
}

// Runs guest code from a host frame that nothing returns to.
void RunGuest(XThread* thread, cpu::Function* function, uint32_t address) {
  if (!function) {
    XELOGE("User mode: no guest code at {:08X}", address);
    return;
  }
  function->Call(thread->thread_state(), kReturnSentinel);
}

void RunGuest(XThread* thread, uint32_t address) {
  RunGuest(
      thread,
      thread->thread_state()->context()->processor->ResolveFunction(address),
      address);
}

// Exchanges |state| into the context, recorded so leaving user mode undoes it.
void PushStackpointState(PPCContext* context, XThread::UserMode* user_mode,
                         void* state) {
  context->processor->backend()->SwapStackpointState(context, state);
  user_mode->swapped_stackpoint_states.push_back(state);
}

void PopStackpointState(PPCContext* context, XThread::UserMode* user_mode) {
  if (user_mode->swapped_stackpoint_states.empty()) {
    return;
  }
  context->processor->backend()->SwapStackpointState(
      context, user_mode->swapped_stackpoint_states.back());
  user_mode->swapped_stackpoint_states.pop_back();
}

// Resumes KeEnterUserMode on the kernel fiber and never returns.
void LeaveUserMode(XThread* thread, uint32_t value) {
  auto user_mode = thread->user_mode();
  auto context = thread->thread_state()->context();
  user_mode->in_user_code = false;
  user_mode->leave_value = value;
  SetUserAddressing(context, false);
  auto& registers = user_mode->kernel_registers;
  std::memcpy(context->r, registers.r, sizeof(registers.r));
  context->ctr = registers.ctr;
  context->lr = registers.lr;
  context->set_cr(registers.cr);
  SetXer(context, registers.xer);
  while (!user_mode->swapped_stackpoint_states.empty()) {
    PopStackpointState(context, user_mode);
  }
  user_mode->kernel_fiber->SwitchTo();
}

// Runs the trap handler for user code that trapped at |resume_address|.
// |record| is the fault's exception record, or 0 for a system call. Returns
// true if the handler returned; |out_address| is where it resumes, possibly
// moved. Returns false if the handler left user mode and the kernel later
// resumed the trap, having loaded the registers itself.
// TODO(has207): the backend's record of the MXCSR/FPCR mode isn't restored
// after the handler, so it can disagree with the mode the trapped code resumes
// in, and the JIT may then skip a mode switch it needs.
bool TrapIntoHandler(PPCContext* context, XThread* thread, uint32_t record,
                     uint32_t resume_address, uint32_t* out_address) {
  auto user_mode = thread->user_mode();
  auto trapped = user_mode->running;
  user_mode->in_user_code = false;
  thread->SaveInterruptedUserCr();
  SetUserAddressing(context, false);
  auto kframes = context->TranslateVirtual<uint8_t*>(user_mode->kframes);
  SaveKframes(context, kframes, resume_address);
  trapped->resume_address = resume_address;
  trapped->stack_pointer = uint32_t(context->r[1]);
  trapped->handler_returned = false;
  user_mode->parked.push_back(trapped);
  user_mode->trapped = trapped;

  auto& registers = user_mode->kernel_registers;
  context->r[1] = (registers.r[1] - kHandlerStackGap) & ~uint64_t(0xF);
  context->r[2] = registers.r[2];
  context->r[13] = registers.r[13];
  context->r[3] = record;
  context->r[4] = user_mode->kframes;
  context->lr = kReturnSentinel;
  PushStackpointState(context, user_mode, user_mode->handler_stackpoint_state);
  context->processor->backend()->PrepareForReentry(context);
  user_mode->handler_fiber->Restart();
  thread->set_active_fiber(user_mode->handler_fiber.get());
  user_mode->handler_fiber->SwitchTo();

  // Back on the trapped fiber, because the handler returned or because the
  // kernel entered user mode again to resume this trap.
  user_mode->running = trapped;
  if (!trapped->handler_returned) {
    // KeEnterUserMode loaded the resumed registers and stackpoints.
    return false;
  }
  PopStackpointState(context, user_mode);
  auto parked_trap =
      std::find(user_mode->parked.begin(), user_mode->parked.end(), trapped);
  if (parked_trap != user_mode->parked.end()) {
    user_mode->parked.erase(parked_trap);
  }

  *out_address = LoadKframes(context, kframes);
  thread->RestoreInterruptedUserCr();
  SetUserAddressing(context, true);
  user_mode->in_user_code = true;
  return true;
}

bool UserModeSyscall(PPCContext* context) {
  XThread* thread = XThread::GetCurrentThread();
  auto user_mode = thread ? thread->user_mode() : nullptr;
  if (!user_mode || !user_mode->in_user_code) {
    return false;
  }
  const uint32_t resume_address = uint32_t(context->scratch);
  uint32_t address;
  if (!TrapIntoHandler(context, thread, 0, resume_address, &address)) {
    return true;
  }
  if (address != resume_address) {
    // The handler redirected the trap rather than resuming it.
    RunGuest(thread, address);
    XELOGE("User mode: code at {:08X} returned without leaving user mode",
           address);
    LeaveUserMode(thread, 0);
  }
  return true;
}

// Hands the fiber back to KeEnterUserMode, which restarts it at
// restart_address. The faulting access's host frames are abandoned.
void RestartUserModeThunk() {
  XThread* thread = XThread::GetCurrentThread();
  auto user_mode = thread->user_mode();
  user_mode->in_user_code = false;
  user_mode->kernel_fiber->SwitchTo();
}

// Fills the exception record for an access violation at |fault_address| by the
// instruction at |address|.
void FillAccessViolationRecord(XThread::UserMode* user_mode, uint32_t address,
                               bool is_write, uint32_t fault_address) {
  auto record =
      kernel_memory()->TranslateVirtual<uint8_t*>(user_mode->exception_record);
  std::memset(record, 0, kRecordSize);
  xe::store_and_swap<uint32_t>(record + kRecordCode, X_STATUS_ACCESS_VIOLATION);
  xe::store_and_swap<uint32_t>(record + kRecordAddress, address);
  xe::store_and_swap<uint32_t>(record + kRecordParameterCount, 2);
  xe::store_and_swap<uint32_t>(record + kRecordInformation, is_write ? 1 : 0);
  xe::store_and_swap<uint32_t>(record + kRecordInformation + 4, fault_address);
}

// A user mode call to an address with no code faults on the instruction fetch.
// The guest's handler can move the call elsewhere, as XeFu does for a call
// through an empty entry of its translated code table. It fills the entry and
// resumes at the new block.
uint32_t UserModeCodeFault(PPCContext* context, uint32_t address) {
  XThread* thread = XThread::GetCurrentThread();
  auto user_mode = thread ? thread->user_mode() : nullptr;
  if (!user_mode || !user_mode->in_user_code || !user_mode->running ||
      xe::threading::Fiber::GetCurrentFiber() !=
          user_mode->running->fiber.get()) {
    return 0;
  }
  FillAccessViolationRecord(user_mode, address, false, address);
  uint32_t resume_address;
  if (!TrapIntoHandler(context, thread, user_mode->exception_record, address,
                       &resume_address)) {
    // The kernel resumed the trap at the target, which still has no code.
    XELOGE("User mode: the fetch fault at {:08X} was retried by the kernel",
           address);
    return 0;
  }
  return resume_address != address ? resume_address : 0;
}

// Runs the trap handler for a fault on the guest instruction at |address|.
// Returns whether the interrupted access runs again, false if the fiber has to
// restart at the address the handler left instead.
bool RunFaultHandler(PPCContext* context, XThread* thread, uint32_t address) {
  auto user_mode = thread->user_mode();
  uint32_t resume_address;
  const bool handler_returned = TrapIntoHandler(
      context, thread, user_mode->exception_record, address, &resume_address);
  if (handler_returned && resume_address == address) {
    return true;
  }
  user_mode->restart_pending = true;
  user_mode->restart_address = handler_returned ? resume_address : address;
  return false;
}

#if !XE_PLATFORM_WIN32
// Guest code can't run on the signal stack, where the next fault overwrites the
// frame, so a fault diverts to the thunk below, which copies it out first.
struct DivertedFault {
  HostThreadContext context;
  // The guest instruction the fault interrupted.
  uint32_t address;
};
thread_local DivertedFault diverted_fault;

// Runs the trap handler on the faulting fiber's own stack, where guest code is
// safe to run, then resumes the access it interrupted. Never returns.
void UserModeFaultThunk() {
  XThread* thread = XThread::GetCurrentThread();
  const HostThreadContext context = diverted_fault.context;
  const uint32_t address = diverted_fault.address;
#if XE_ARCH_AMD64
  // The signal return put back the MXCSR the fault ran with, which the context
  // doesn't carry and the handler's guest code changes.
  const uint32_t mxcsr = _mm_getcsr();
#endif  // XE_ARCH_AMD64
  if (RunFaultHandler(thread->thread_state()->context(), thread, address)) {
    // The retry runs from the host registers, so changes the handler made to
    // the frame are lost.
    // TODO(has207): load the frame back into the host context instead.
#if XE_ARCH_AMD64
    _mm_setcsr(mxcsr);
#endif  // XE_ARCH_AMD64
    ResumeHostContext(&context);
  }
  RestartUserModeThunk();
}
#endif  // !XE_PLATFORM_WIN32

// A user mode access with no page table entry, or one its entry's protection
// forbids. The guest's handler can fix the entry and return, and the access
// runs again.
Memory::UserFaultResult UserModeFault(uint32_t fault_address, bool is_write,
                                      Exception* ex) {
  XThread* thread = XThread::GetCurrentThread();
  auto user_mode = thread ? thread->user_mode() : nullptr;
  if (!user_mode || !user_mode->in_user_code || !user_mode->running ||
      xe::threading::Fiber::GetCurrentFiber() !=
          user_mode->running->fiber.get()) {
    return Memory::UserFaultResult::kNotTaken;
  }
  PPCContext* context = thread->thread_state()->context();
  auto function =
      context->processor->backend()->code_cache()->LookupFunction(ex->pc());
  if (!function) {
    // Host code faulted on the guest's behalf, so there is no guest
    // instruction to resume.
    return Memory::UserFaultResult::kNotTaken;
  }
  const uint32_t address = function->MapMachineCodeToGuestAddress(ex->pc());
  FillAccessViolationRecord(user_mode, address, is_write, fault_address);

#if XE_PLATFORM_WIN32
  if (RunFaultHandler(context, thread, address)) {
    // The retry runs from the host registers, so changes the handler made to
    // the frame are lost.
    // TODO(has207): load the frame back into the host context instead.
    return Memory::UserFaultResult::kTaken;
  }
  DivertToThunk(ex, &RestartUserModeThunk);
  return Memory::UserFaultResult::kDiverted;
#else
  // The thunk gets the registers the access has to resume from.
  diverted_fault.context = *ex->thread_context();
  diverted_fault.address = address;
  DivertToThunk(ex, &UserModeFaultThunk);
  return Memory::UserFaultResult::kDiverted;
#endif  // XE_PLATFORM_WIN32
}

std::unique_ptr<XThread::UserMode::UserFiber> CreateUserFiber(
    XThread* thread, PPCContext* context) {
  auto user_fiber = std::make_unique<XThread::UserMode::UserFiber>();
  xe::threading::Fiber::CreationParameters fiber_params;
  fiber_params.stack_size = 16_MiB;
  user_fiber->fiber = xe::threading::Fiber::Create(
      fiber_params, [thread, user_fiber = user_fiber.get()]() {
        RunGuest(thread, user_fiber->entry_address);
        XELOGE("User mode: code at {:08X} returned without leaving user mode",
               user_fiber->entry_address);
        LeaveUserMode(thread, 0);
      });
  user_fiber->stackpoint_state =
      context->processor->backend()->CreateStackpointState();
  return user_fiber;
}

XThread::UserMode* CreateUserMode(XThread* thread, PPCContext* context) {
  auto user_mode = std::make_unique<XThread::UserMode>();
  xe::threading::Fiber::CreationParameters fiber_params;
  fiber_params.stack_size = 16_MiB;
  user_mode->handler_fiber =
      xe::threading::Fiber::Create(fiber_params, [thread]() {
        auto user_mode = thread->user_mode();
        RunGuest(thread, user_mode->handler_function, user_mode->handler);
        // The handler returned rather than leaving user mode.
        auto trapped = user_mode->trapped;
        trapped->handler_returned = true;
        thread->set_active_fiber(trapped->fiber.get());
        trapped->fiber->SwitchTo();
      });
  user_mode->handler_stackpoint_state =
      context->processor->backend()->CreateStackpointState();
  user_mode->kframes = kernel_state()->memory()->SystemHeapAlloc(kKframesSize);
  user_mode->exception_record =
      kernel_state()->memory()->SystemHeapAlloc(kRecordSize);
  user_mode->interrupt_frame = kernel_state()->memory()->SystemHeapAlloc(
      XThread::UserMode::kInterruptFrameSize);
  thread->guest_object<X_KTHREAD>()->unk_128 = user_mode->interrupt_frame;
  auto result = user_mode.get();
  thread->set_user_mode(std::move(user_mode));
  return result;
}

}  // namespace

dword_result_t KeCreateUserMode_entry(dword_t unknown, lpvoid_t descriptor,
                                      const ppc_context_t& context) {
  // User code runs on its own host fibers, so the backend has to keep their
  // stackpoint records apart. One that keeps none would unwind a fiber onto
  // another's host stack.
  auto backend = context->processor->backend();
  void* stackpoint_probe = backend->CreateStackpointState();
  if (!stackpoint_probe) {
    XELOGE("KeCreateUserMode: the backend keeps no per-stack records");
    return X_STATUS_NOT_SUPPORTED;
  }
  backend->DestroyStackpointState(stackpoint_probe);
  kernel_memory()->SetUserPageTable(descriptor.guest_address());
  if (!kernel_memory()->EnableUserModeViews()) {
    return X_STATUS_NO_MEMORY;
  }
  context->processor->EnableDynamicCode();
  context->processor->set_syscall_hook(&UserModeSyscall);
  // The handler reads the registers of a fault from the context, which the JIT
  // otherwise leaves stale mid-block.
  // TODO(has207): this slows all user mode code down. Recovering the registers
  // from the host context at the fault would avoid it.
  context->processor->KeepDynamicCodeContextStores();
  kernel_memory()->set_user_fault_hook(&UserModeFault);
  context->processor->set_code_fault_hook(&UserModeCodeFault);
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT2(KeCreateUserMode, kThreading, kImplemented, kSketchy);

// Returns the value KeLeaveUserMode is called with.
dword_result_t KeEnterUserMode_entry(lpvoid_t user_context, dword_t handler,
                                     lpvoid_t nonvolatile_save, dword_t unknown,
                                     const ppc_context_t& context) {
  XThread* thread = XThread::GetCurrentThread();
  PPCContext* guest_context = context.value();
  if (!kernel_memory()->user_virtual_membase()) {
    XELOGE("KeEnterUserMode without a user mode address space");
    return 0;
  }
  auto user_mode = thread->user_mode();
  if (user_mode &&
      IsUserModeFiber(user_mode, xe::threading::Fiber::GetCurrentFiber())) {
    // The kernel would be entering on a fiber it is already running on, which
    // would restart it under itself and overwrite the registers to leave with.
    XELOGE("KeEnterUserMode from user mode is not supported");
    return 0;
  }
  if (!user_mode) {
    user_mode = CreateUserMode(thread, guest_context);
  }

  auto& registers = user_mode->kernel_registers;
  std::memcpy(registers.r, guest_context->r, sizeof(registers.r));
  registers.ctr = guest_context->ctr;
  registers.lr = guest_context->lr;
  registers.cr = uint32_t(guest_context->cr());
  registers.xer = GetXer(guest_context);
  if (!user_mode->handler_function || user_mode->handler != handler) {
    user_mode->handler = handler;
    user_mode->handler_function =
        guest_context->processor->ResolveFunction(handler);
  }

  auto p = user_context.as<const uint8_t*>();
  for (size_t i = 0; i < 32; ++i) {
    guest_context->r[i] = xe::load_and_swap<uint64_t>(p + kContextGpr + i * 8);
  }
  guest_context->ctr = xe::load_and_swap<uint64_t>(p + kContextCtr);
  guest_context->lr = xe::load_and_swap<uint32_t>(p + kContextLr);
  guest_context->set_cr(xe::load_and_swap<uint32_t>(p + kContextCr));
  SetXer(guest_context, xe::load_and_swap<uint32_t>(p + kContextXer));
  const uint32_t entry_address = xe::load_and_swap<uint32_t>(p + kContextIar);
  if (!guest_context->r[1]) {
    // A context with no stack pointer borrows the kernel's. XeFu never sets one
    // and its generated code pushes a frame through r1 on its first call. User
    // code gets the lower half of the stack left so the trap handler and the
    // kernel's servicing of a trap run above it without overwriting its
    // frames.
    const uint32_t kernel_sp = uint32_t(registers.r[1]);
    const uint32_t limit = thread->stack_limit();
    const uint32_t user_sp =
        limit && limit < kernel_sp
            ? (limit + (kernel_sp - limit) / 2) & ~uint32_t(0xF)
            : kernel_sp;
    guest_context->r[1] = user_sp;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
      XELOGW(
          "KeEnterUserMode: the context at {:08X} names no stack. User mode "
          "runs at {:08X} on the kernel stack",
          user_context.guest_address(), user_sp);
    }
  }

  if (!xe::threading::Fiber::GetCurrentFiber()) {
    user_mode->adopted_fiber = xe::threading::Fiber::CreateFromThread();
  }
  user_mode->kernel_fiber = xe::threading::Fiber::GetCurrentFiber();
  SetUserAddressing(guest_context, true);

  // The kernel services a trap by entering again at the instruction after it,
  // with the stack pointer it trapped with, so that fiber continues in place.
  XThread::UserMode::UserFiber* target = nullptr;
  for (size_t i = user_mode->parked.size(); i-- > 0;) {
    auto parked = user_mode->parked[i];
    if (parked->resume_address == entry_address &&
        parked->stack_pointer == uint32_t(guest_context->r[1])) {
      target = parked;
      break;
    }
  }
  if (target) {
    // Traps parked after it belong to calls that already finished.
    while (user_mode->parked.back() != target) {
      user_mode->parked.pop_back();
    }
    user_mode->parked.pop_back();
    PushStackpointState(guest_context, user_mode, target->stackpoint_state);
  } else {
    // Guest stacks grow down, so an entry at or above a parked trap's stack
    // pointer has overwritten its frames.
    while (!user_mode->parked.empty() &&
           user_mode->parked.back()->stack_pointer <=
               uint32_t(guest_context->r[1])) {
      XELOGD(
          "User mode: entry at {:08X} with r1 {:08X} drops the trap parked "
          "at {:08X} with r1 {:08X}",
          entry_address, uint32_t(guest_context->r[1]),
          user_mode->parked.back()->resume_address,
          user_mode->parked.back()->stack_pointer);
      user_mode->parked.pop_back();
    }
    for (auto& user_fiber : user_mode->fibers) {
      if (std::find(user_mode->parked.begin(), user_mode->parked.end(),
                    user_fiber.get()) == user_mode->parked.end()) {
        target = user_fiber.get();
        break;
      }
    }
    if (!target) {
      user_mode->fibers.push_back(CreateUserFiber(thread, guest_context));
      target = user_mode->fibers.back().get();
    }
    target->entry_address = entry_address;
    PushStackpointState(guest_context, user_mode, target->stackpoint_state);
    guest_context->processor->backend()->PrepareForReentry(guest_context);
    target->fiber->Restart();
  }
  user_mode->running = target;
  user_mode->in_user_code = true;
  for (;;) {
    thread->set_active_fiber(target->fiber.get());
    target->fiber->SwitchTo();
    if (!user_mode->restart_pending) {
      break;
    }
    user_mode->restart_pending = false;
    target->entry_address = user_mode->restart_address;
    guest_context->processor->backend()->PrepareForReentry(guest_context);
    target->fiber->Restart();
    user_mode->in_user_code = true;
  }
  thread->set_active_fiber(nullptr);
  return user_mode->leave_value;
}
DECLARE_XBOXKRNL_EXPORT3(KeEnterUserMode, kThreading, kImplemented,
                         kHighFrequency, kSketchy);

void KeLeaveUserMode_entry(dword_t value) {
  XThread* thread = XThread::GetCurrentThread();
  auto user_mode = thread ? thread->user_mode() : nullptr;
  if (!user_mode ||
      xe::threading::Fiber::GetCurrentFiber() == user_mode->kernel_fiber) {
    XELOGE("KeLeaveUserMode({}) outside user mode", uint32_t(value));
    return;
  }
  LeaveUserMode(thread, value);
}
DECLARE_XBOXKRNL_EXPORT3(KeLeaveUserMode, kThreading, kImplemented,
                         kHighFrequency, kSketchy);

void KeContextFromKframes_entry(lpvoid_t kframes, lpvoid_t context) {
  auto k = kframes.as<const uint8_t*>();
  auto p = context.as<uint8_t*>();
  const uint32_t flags = xe::load_and_swap<uint32_t>(p + kContextFlags);
  // The kernel side only resumes user mode from a context that says so.
  xe::store_and_swap<uint32_t>(
      p + kContextUserModeControl,
      (xe::load_and_swap<uint32_t>(k + kKframesMsr) & kMsrUserMode) ? 1 : 0);
  if (flags & kContextControl) {
    xe::store_and_swap<uint32_t>(p + kContextMsr,
                                 xe::load_and_swap<uint32_t>(k + kKframesMsr));
    xe::store_and_swap<uint32_t>(p + kContextIar,
                                 xe::load_and_swap<uint32_t>(k + kKframesIar));
    xe::store_and_swap<uint32_t>(p + kContextLr,
                                 xe::load_and_swap<uint32_t>(k + kKframesLr));
    xe::store_and_swap<uint64_t>(p + kContextCtr,
                                 xe::load_and_swap<uint64_t>(k + kKframesCtr));
  }
  // RuntimeHost asks for control and integer state; the kframes carry no
  // floating point or vector registers, which user mode shares with the
  // kernel.
  if (flags & kContextInteger) {
    std::memcpy(p + kContextGpr, k + kKframesGpr, 32 * 8);
    xe::store_and_swap<uint32_t>(p + kContextCr,
                                 xe::load_and_swap<uint32_t>(k + kKframesCr));
    xe::store_and_swap<uint32_t>(p + kContextXer,
                                 xe::load_and_swap<uint32_t>(k + kKframesXer));
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeContextFromKframes, kThreading, kImplemented,
                         kHighFrequency);

// The inverse, which a continue uses to load the context it resumes with.
void KeContextToKframes_entry(lpvoid_t kframes, lpvoid_t context,
                              dword_t flags) {
  auto k = kframes.as<uint8_t*>();
  auto p = context.as<const uint8_t*>();
  if (flags & kContextControl) {
    xe::store_and_swap<uint32_t>(k + kKframesIar,
                                 xe::load_and_swap<uint32_t>(p + kContextIar));
    xe::store_and_swap<uint32_t>(k + kKframesLr,
                                 xe::load_and_swap<uint32_t>(p + kContextLr));
    xe::store_and_swap<uint64_t>(k + kKframesCtr,
                                 xe::load_and_swap<uint64_t>(p + kContextCtr));
  }
  if (flags & kContextInteger) {
    std::memcpy(k + kKframesGpr, p + kContextGpr, 32 * 8);
    xe::store_and_swap<uint32_t>(k + kKframesCr,
                                 xe::load_and_swap<uint32_t>(p + kContextCr));
    xe::store_and_swap<uint32_t>(k + kKframesXer,
                                 xe::load_and_swap<uint32_t>(p + kContextXer));
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeContextToKframes, kThreading, kImplemented);

// Dropping more than was asked for only costs a fault, so none reads its args.
void KeFlushUserModeCurrentTb_entry() {
  kernel_memory()->FlushChangedUserPages();
}
DECLARE_XBOXKRNL_EXPORT1(KeFlushUserModeCurrentTb, kMemory, kImplemented);

void KeFlushUserModeTb_entry() { kernel_memory()->FlushChangedUserPages(); }
DECLARE_XBOXKRNL_EXPORT1(KeFlushUserModeTb, kMemory, kImplemented);

// TODO(has207): keeping unchanged pages here, as the user mode flushes do,
// bugchecks Crimson Skies under XeFu for an unknown reason.
void KeFlushCurrentEntireTb_entry() { kernel_memory()->FlushUserPageTable(); }
DECLARE_XBOXKRNL_EXPORT1(KeFlushCurrentEntireTb, kMemory, kImplemented);

void KeFlushEntireTb_entry() { kernel_memory()->FlushUserPageTable(); }
DECLARE_XBOXKRNL_EXPORT1(KeFlushEntireTb, kMemory, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(UserMode);
