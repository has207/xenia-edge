/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PROCESSOR_H_
#define XENIA_CPU_PROCESSOR_H_

#include <atomic>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/debug_listener.h"
#include "xenia/cpu/entry_table.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

DECLARE_bool(debug);

namespace xe {
namespace cpu {

constexpr fourcc_t kProcessorSaveSignature = make_fourcc("PROC");

class Breakpoint;
class StackWalker;
class XexModule;

enum class Irql : uint32_t {
  PASSIVE = 0,
  APC = 1,
  DISPATCH = 2,
  DPC = 3,
};

// Describes the current state of the emulator as known to the debugger.
// This determines which state the debugger is in and what operations are
// allowed.
enum class ExecutionState {
  // Target is running; the debugger is not waiting for any events.
  kRunning,
  // Target is running in stepping mode with the debugger waiting for feedback.
  kStepping,
  // Target is paused for debugging.
  kPaused,
  // Target has been stopped and cannot be restarted (crash, etc).
  kEnded,
};

class Processor {
 public:
  Processor(Memory* memory, ExportResolver* export_resolver);
  ~Processor();

  Memory* memory() const { return memory_; }
  StackWalker* stack_walker() const { return stack_walker_.get(); }
  ppc::PPCFrontend* frontend() const { return frontend_.get(); }
  backend::Backend* backend() const { return backend_.get(); }
  ExportResolver* export_resolver() const { return export_resolver_; }

  bool Setup(std::unique_ptr<backend::Backend> backend);

  // Runs any pre-launch logic once the module and thread have been setup.
  void PreLaunch();

  // The current execution state of the emulator.
  ExecutionState execution_state() const { return execution_state_; }

  // True if a debug listener is attached and the debugger is active.
  bool is_debugger_attached() const { return !!debug_listener_; }
  // Gets the active debug listener, if any.
  DebugListener* debug_listener() const { return debug_listener_; }
  // Sets the active debug listener, if any.
  // This can be used to detach the listener.
  void set_debug_listener(DebugListener* debug_listener);
  // Sets a handler that will be called from a random thread when a debugger
  // listener is required (such as on a breakpoint hit/etc).
  // Will only be called if the debug listener has not already been specified
  // with set_debug_listener.
  void set_debug_listener_request_handler(
      std::function<DebugListener*(Processor*)> handler) {
    debug_listener_handler_ = std::move(handler);
  }

  void set_debug_info_flags(uint32_t debug_info_flags) {
    debug_info_flags_ = debug_info_flags;
  }

  bool AddModule(std::unique_ptr<Module> module);
  void RemoveModule(const Module* module);
  Module* GetModule(const std::string_view name);
  std::vector<Module*> GetModules();

  Module* builtin_module() const { return builtin_module_; }
  Function* DefineBuiltin(const std::string_view name,
                          BuiltinFunction::Handler handler, void* arg0,
                          void* arg1);

  // Runs for a guest sc before the default handling, true if it handled it.
  using SyscallHook = bool (*)(ppc::PPCContext* context);
  SyscallHook syscall_hook() const { return syscall_hook_.load(); }
  void set_syscall_hook(SyscallHook hook) { syscall_hook_.store(hook); }

  // Runs for a guest call to an address with no guest code, where the
  // instruction fetch faults. Returns the address the guest moved the call to,
  // or 0 if it didn't move it.
  using CodeFaultHook = uint32_t (*)(ppc::PPCContext* context,
                                     uint32_t address);
  CodeFaultHook code_fault_hook() const { return code_fault_hook_.load(); }
  void set_code_fault_hook(CodeFaultHook hook) { code_fault_hook_.store(hook); }

  // Runs for a guest load or store at |pc| that faults on |address|, on the
  // faulting thread with the guest context as it was at the access. Returns
  // the address a handler continued at, or 0 if none took the fault.
  using DataFaultHook = uint32_t (*)(ppc::PPCContext* context, uint32_t pc,
                                     bool is_store, uint32_t address);
  DataFaultHook data_fault_hook() const { return data_fault_hook_.load(); }
  void set_data_fault_hook(DataFaultHook hook) { data_fault_hook_.store(hook); }

  // Lets guest code run from committed memory that no module claims.
  void EnableDynamicCode();
  bool dynamic_code_enabled() const {
    return dynamic_code_enabled_.load(std::memory_order_relaxed);
  }
  // Makes dynamic code compiled after this call keep every context store, so a
  // guest handler sees current registers for a faulting access.
  void KeepDynamicCodeContextStores() {
    keep_dynamic_context_stores_.store(true, std::memory_order_relaxed);
  }
  bool KeepsContextStores(const Module* module) const {
    return keep_dynamic_context_stores_.load(std::memory_order_relaxed) &&
           IsDynamicCode(module);
  }
  // Whether |module| is the code the guest writes at runtime.
  bool IsDynamicCode(const Module* module) const {
    return module && module == dynamic_code_module_.get();
  }

  Function* QueryFunction(uint32_t address);
  std::vector<Function*> FindFunctionsWithAddress(uint32_t address);
  void RemoveFunctionByAddress(uint32_t address);
  // Forgets code compiled from [address, address + length), so the next call
  // into it compiles what the guest has since written there, and records the
  // range as the extent of the code the guest wrote.
  void InvalidateCodeRange(uint32_t address, uint32_t length);
  // Changes whenever compiled code may have been dropped. A function resolved
  // before a change may have been dropped before it could be cached.
  uint64_t code_sweep_count() const { return code_sweep_count_.load(); }

  // The exclusive end of the swept range an address falls in, or 0 for none.
  uint32_t SweptCodeEnd(uint32_t address);

  Function* LookupFunction(uint32_t address);
  Module* LookupModule(uint32_t address);
  Function* LookupFunction(Module* module, uint32_t address);
  Function* ResolveFunction(uint32_t address);
  // |expand| appends more addresses to resolve from each resolved function.
  size_t ResolveFunctionsInParallel(
      std::vector<uint32_t> addresses,
      const std::function<void(Function*, std::vector<uint32_t>&)>& expand =
          {});

  bool Execute(ThreadState* thread_state, uint32_t address);
  bool ExecuteRaw(ThreadState* thread_state, uint32_t address);
  uint64_t Execute(ThreadState* thread_state, uint32_t address, uint64_t args[],
                   size_t arg_count);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  // Returns a list of debugger info for all threads that have ever existed.
  // This is the preferred way to sample thread state vs. attempting to ask
  // the kernel.
  std::vector<ThreadDebugInfo*> QueryThreadDebugInfos();

  // Returns the debugger info for the given thread.
  ThreadDebugInfo* QueryThreadDebugInfo(uint32_t thread_id);

  // Adds a breakpoint to the debugger and activates it (if enabled).
  // The given breakpoint will not be owned by the debugger and must remain
  // allocated so long as it is added.
  void AddBreakpoint(Breakpoint* breakpoint);

  // Removes a breakpoint from the debugger and deactivates it.
  void RemoveBreakpoint(Breakpoint* breakpoint);

  // Finds a breakpoint that may be registered at the given address.
  Breakpoint* FindBreakpoint(uint32_t address);

  // Returns all currently registered breakpoints.
  std::vector<Breakpoint*> breakpoints() const;

  // Shows the debug listener, focusing it if it already exists.
  void ShowDebugger();

  // Pauses target execution by suspending all threads.
  // The debug listener will be requested if it has not been attached.
  void Pause();

  // Continues target execution from wherever it is.
  // This will cancel any active step operations and resume all threads.
  void Continue();

  // Steps the given thread a single x64 host instruction.
  // If the step is over a branch the branch will be followed.
  void StepHostInstruction(uint32_t thread_id);

  // Steps the given thread a single PPC guest instruction.
  // If the step is over a branch the branch will be followed.
  void StepGuestInstruction(uint32_t thread_id);

  // Steps the given thread until the guest address is hit.
  // Returns false if the step could not be completed (invalid target address).
  bool StepToGuestAddress(uint32_t thread_id, uint32_t pc);

  // Steps the given thread to the target of the branch at the specified guest
  // address. The address must specify a branch instruction.
  // Returns the new PC guest address.
  uint32_t StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc);

  // Steps the thread to a point where it's safe to terminate or read its
  // context. Returns the PC after we've finished stepping.
  // Pass true for ignore_host if you've stopped the thread yourself
  // in host code you want to ignore.
  // Returns the new PC guest address.
  uint32_t StepToGuestSafePoint(uint32_t thread_id, bool ignore_host = false);

  uint32_t GuestAtomicIncrement32(ppc::PPCContext* context,
                                  uint32_t guest_address);
  uint32_t GuestAtomicDecrement32(ppc::PPCContext* context,
                                  uint32_t guest_address);
  uint32_t GuestAtomicOr32(ppc::PPCContext* context, uint32_t guest_address,
                           uint32_t mask);
  uint32_t GuestAtomicXor32(ppc::PPCContext* context, uint32_t guest_address,
                            uint32_t mask);
  uint32_t GuestAtomicAnd32(ppc::PPCContext* context, uint32_t guest_address,
                            uint32_t mask);
  bool GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                        uint32_t new_value, uint32_t guest_address);

 public:
  // TODO(benvanik): hide.
  void OnThreadCreated(uint32_t handle, ThreadState* thread_state,
                       Thread* thread);
  void OnThreadExit(uint32_t thread_id);
  void OnThreadDestroyed(uint32_t thread_id);
  void OnThreadEnteringWait(uint32_t thread_id);
  void OnThreadLeavingWait(uint32_t thread_id);

  bool OnUnhandledException(Exception* ex);
  bool OnThreadBreakpointHit(Exception* ex);

  // Instruction coverage counters live in a per-thread arena so the JIT can
  // increment them unsynchronized. Every arena shares one layout: a function
  // reserves an offset here, and each thread holds a private copy at that
  // offset. Returns GuestFunction::kInvalidCoverageOffset if the arena is
  // full.
  size_t AllocateTraceCountsOffset(uint32_t start_address,
                                   uint32_t instruction_count);
  uint8_t* AcquireTraceCounts(uint32_t thread_id);
  void ReleaseTraceCounts(uint8_t* arena);
  // Starts a fresh capture window. Racing threads may land a count on either
  // side of this, which does not matter at the scale being measured.
  void ResetTraceCounts();
  // Call once a title's config is applied and before its code is translated.
  void RefreshTraceCountsEnabled();
  // Handed over by the backend once a function is emitted, so the sequences it
  // selected can be weighted by how often each guest instruction runs.
  void RecordSequenceSamples(uint32_t start_address,
                             std::vector<backend::SequenceSample> samples);
  bool trace_counts_enabled() const { return trace_counts_enabled_; }

 private:
  // Write the guestcoverage, guestcoveragethreads and guestsequences tables
  // appended to the profiler's CSV dump. Both require trace_counts_mutex_.
  void DumpTraceCounts(FILE* f);
  void DumpSequences(FILE* f);

  // All require trace_counts_mutex_ held.
  uint8_t* ReserveTraceCountsArenaLocked(uint32_t thread_id);
  void SetTraceCountsArenaThreadLocked(uint8_t* arena, uint32_t thread_id);
  bool EnsureTraceCountsFallbackLocked();
  bool CommitTraceCountsLocked(size_t required);
  void FoldTraceCountsLocked(uint8_t* arena);

  // Synchronously demands a debug listener.
  void DemandDebugListener();

  // Suspends all known threads (except the caller).
  bool SuspendAllThreads();
  // Resumes the given thread.
  bool ResumeThread(uint32_t thread_id);
  // Resumes all known threads (except the caller).
  bool ResumeAllThreads();
  // Updates all cached thread execution info (state, call stacks, etc).
  // The given override thread handle and context will be used in place of
  // sampled values for that thread.
  void UpdateThreadExecutionStates(
      uint32_t override_handle = 0,
      HostThreadContext* override_context = nullptr);

  // Suspends all breakpoints, uninstalling them as required.
  // No breakpoints will be triggered until they are resumed.
  void SuspendAllBreakpoints();
  // Resumes all breakpoints, re-installing them if required.
  void ResumeAllBreakpoints();

  void OnFunctionDefined(Function* function);

  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);
  void OnStepCompleted(ThreadDebugInfo* thread_info);
  void OnBreakpointHit(ThreadDebugInfo* thread_info, Breakpoint* breakpoint);

  // Calculates the next guest instruction based on the current thread state and
  // current PC. This will look for branches and other control flow
  // instructions.
  uint32_t CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                         uint32_t current_pc);

  bool DemandFunction(Function* function);

  Memory* memory_ = nullptr;
  std::unique_ptr<StackWalker> stack_walker_;

  std::function<DebugListener*(Processor*)> debug_listener_handler_;
  DebugListener* debug_listener_ = nullptr;

  // Which debug features are enabled in generated code.
  uint32_t debug_info_flags_ = 0;
  struct TraceCountsRegion {
    uint32_t start_address;
    size_t offset;
    size_t count;
    // Counts from threads that have exited, folded in as they go.
    std::unique_ptr<uint64_t[]> retired;
    // What the backend emitted for this function, empty until it is compiled.
    std::vector<backend::SequenceSample> samples;
  };
  struct TraceCountsArena {
    uint8_t* base;
    // Zero once the owning thread exits, or for the shared fallback.
    uint32_t thread_id;
  };
  uintptr_t trace_counts_dump_section_ = 0;
  std::mutex trace_counts_mutex_;
  std::vector<TraceCountsRegion> trace_counts_regions_;
  // Every arena ever reserved, and the subset free for a new thread to claim.
  std::vector<TraceCountsArena> trace_counts_arenas_;
  std::vector<uint8_t*> trace_counts_free_;
  // Shared by any thread that could not get a private arena.
  uint8_t* trace_counts_fallback_ = nullptr;
  bool trace_counts_failed_ = false;
  bool trace_counts_enabled_ = false;
  size_t trace_counts_next_offset_ = 0;
  size_t trace_counts_committed_ = 0;

  std::unique_ptr<ppc::PPCFrontend> frontend_;
  std::unique_ptr<backend::Backend> backend_;
  ExportResolver* export_resolver_ = nullptr;

  EntryTable entry_table_;
  xe::global_critical_region global_critical_region_;
  ExecutionState execution_state_ = ExecutionState::kPaused;
  std::vector<std::unique_ptr<Module>> modules_;
  Module* builtin_module_ = nullptr;
  uint32_t next_builtin_address_ = 0xFFFF0000u;
  // Consulted after modules_, so a loaded module always takes precedence.
  std::unique_ptr<Module> dynamic_code_module_;
  std::atomic<bool> dynamic_code_enabled_{false};
  std::atomic<bool> keep_dynamic_context_stores_{false};
  std::atomic<SyscallHook> syscall_hook_{nullptr};
  std::atomic<CodeFaultHook> code_fault_hook_{nullptr};
  std::atomic<DataFaultHook> data_fault_hook_{nullptr};

  // Code can be written through one address and run through another, so swept
  // ranges are keyed by physical address where there is one. This bit keeps
  // those keys apart from plain virtual addresses.
  static constexpr uint64_t kPhysicalCodeKey = 0x100000000ull;
  void RecordSweptCode(uint32_t address, uint32_t length);
  uint64_t CodeRangeKey(uint32_t address);
  // Swept code ranges by start key, holding the exclusive end of each.
  // Guarded with the global lock.
  std::map<uint64_t, uint64_t> swept_code_ranges_;
  // Dynamic code can be written through one name and compiled under another
  // and a sweep finds what was compiled from its memory by key. Each piece lies
  // in one 4 KB page, since the next page can be anywhere physically.
  struct DynamicCodePiece {
    uint64_t end_key;
    uint32_t function_address;
  };
  void RecordDynamicCode(const Function* function);
  // Dynamic code pieces by start key. Guarded with the global lock.
  std::multimap<uint64_t, DynamicCodePiece> dynamic_code_pieces_;
  // Calls |callback(key, end_key)| for the part of [start, end) in each 4 KB
  // page, or of the code |function| was compiled from.
  template <typename Callback>
  void ForEachCodePiece(uint64_t start, uint64_t end, Callback&& callback);
  template <typename Callback>
  void ForEachCodePiece(const Function* function, Callback&& callback);

  // Bumped by each sweep before it drops anything, and by a compile dropped as
  // it publishes.
  std::atomic<uint64_t> code_sweep_count_{0};
  // A compile reads the code outside the lock, and a sweep in the meantime
  // can't find it to drop. So the sweep count at the start of each compile of
  // dynamic code in flight is kept, and the key ranges swept since the oldest
  // of them, which the compile checks its own code against before it
  // publishes. Guarded with the global lock.
  struct CompileSweep {
    uint64_t count;
    uint64_t start_key;
    uint64_t end_key;
  };
  std::multiset<uint64_t> compile_sweep_counts_;
  std::deque<CompileSweep> compile_sweeps_;
  // Start and end a compile, returning the count it starts at. The caller
  // holds the global lock.
  uint64_t BeginCompile();
  void EndCompile(uint64_t sweep_count);
  // Whether a sweep after |sweep_count| covered the code |function| was
  // compiled from.
  bool SweptSince(const Function* function, uint64_t sweep_count);

  // Maps thread ID to state. Updated on thread create, and threads are never
  // removed. Must be guarded with the global lock.
  std::map<uint32_t, std::unique_ptr<ThreadDebugInfo>> thread_debug_infos_;

  // TODO(benvanik): cleanup/change structures.
  std::vector<Breakpoint*> breakpoints_;

  Irql irql_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PROCESSOR_H_
