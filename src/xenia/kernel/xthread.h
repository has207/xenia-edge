/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTHREAD_H_
#define XENIA_KERNEL_XTHREAD_H_

#include <atomic>
#include <memory>
#include <string>

#include "xenia/base/mutex.h"
#if !XE_PLATFORM_WIN32
#include <condition_variable>
#include <csignal>
#include <mutex>
#endif
#if XE_PLATFORM_WIN32
#include <csetjmp>
#endif
#include "xenia/base/threading.h"
#include "xenia/cpu/thread.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/util/native_list.h"
#include "xenia/kernel/util/xfiletime.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

constexpr fourcc_t kThreadSaveSignature = make_fourcc("THRD");

class XEvent;

enum IRQL_FLAGS : uint8_t {
  IRQL_PASSIVE = 0,
  IRQL_APC = 1,
  IRQL_DISPATCH = 2,
  IRQL_DPC = 3,
  IRQL_AUDIO = 68,   // used a few times in the audio driver
  IRQL_CLOCK = 116,  // irql used by the clock interrupt
  IRQL_HIGHEST = 124
};

// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ke/kthread_state.htm
enum X_KTHREAD_STATE_FLAGS : uint8_t {
  KTHREAD_STATE_INITIALIZED = 0,
  KTHREAD_STATE_READY = 1,
  KTHREAD_STATE_RUNNING = 2,
  KTHREAD_STATE_STANDBY = 3,
  KTHREAD_STATE_TERMINATED = 4,
  KTHREAD_STATE_WAITING = 5,
  KTHREAD_STATE_UNKNOWN = 6,  //"Transition" except that makes no sense here, so
                              // 6 likely has a different meaning on xboxkrnl
};

constexpr uint32_t X_CREATE_SUSPENDED = 0x00000001;

constexpr uint32_t X_TLS_OUT_OF_INDEXES = UINT32_MAX;
struct XDPC {
  xe::be<uint16_t> type;
  uint8_t selected_cpu_number;
  uint8_t desired_cpu_number;
  X_LIST_ENTRY list_entry;
  xe::be<uint32_t> routine;
  xe::be<uint32_t> context;
  xe::be<uint32_t> arg1;
  xe::be<uint32_t> arg2;

  void Initialize(uint32_t guest_func, uint32_t guest_context) {
    type = 19;
    selected_cpu_number = 0;
    desired_cpu_number = 0;
    routine = guest_func;
    context = guest_context;
  }
};

struct XAPC {
  static constexpr uint32_t kSize = 40;
  static constexpr uint32_t kDummyKernelRoutine = 0xF00DFF00;
  static constexpr uint32_t kDummyRundownRoutine = 0xF00DFF01;

  // KAPC is 0x28(40) bytes? (what's passed to ExAllocatePoolWithTag)
  // This is 4b shorter than NT - looks like the reserved dword at +4 is gone.
  // NOTE: stored in guest memory.
  uint16_t type;                     // +0
  uint8_t apc_mode;                  // +2
  uint8_t enqueued;                  // +3
  xe::be<uint32_t> thread_ptr;       // +4
  X_LIST_ENTRY list_entry;           // +8
  xe::be<uint32_t> kernel_routine;   // +16
  xe::be<uint32_t> rundown_routine;  // +20
  xe::be<uint32_t> normal_routine;   // +24
  xe::be<uint32_t> normal_context;   // +28
  xe::be<uint32_t> arg1;             // +32
  xe::be<uint32_t> arg2;             // +36
};

struct X_KSEMAPHORE {
  X_DISPATCH_HEADER header;
  xe::be<uint32_t> limit;
};
static_assert_size(X_KSEMAPHORE, 0x14);

struct X_KTHREAD;
struct X_KPROCESS;
struct X_KPRCB {
  TypedGuestPointer<X_KTHREAD> current_thread;  // 0x0
  TypedGuestPointer<X_KTHREAD> next_thread;     // 0x4
  TypedGuestPointer<X_KTHREAD> idle_thread;     // 0x8
  uint8_t current_cpu;                          // 0xC
  uint8_t unk_D[3];                             // 0xD
  // should only have 1 bit set, used for ipis
  xe::be<uint32_t> processor_mask;  // 0x10
  // incremented in clock interrupt
  xe::be<uint32_t> dpc_clock;        // 0x14
  xe::be<uint32_t> interrupt_clock;  // 0x18
  xe::be<uint32_t> unk_1C;           // 0x1C
  xe::be<uint32_t> unk_20;           // 0x20
  // various fields used by KeIpiGenericCall
  xe::be<uint32_t> ipi_args[3];  // 0x24
  // looks like the target cpus clear their corresponding bit
  // in this mask to signal completion to the initiator
  xe::be<uint32_t> targeted_ipi_cpus_mask;  // 0x30
  xe::be<uint32_t> ipi_function;            // 0x34
  // used to synchronize?
  TypedGuestPointer<X_KPRCB> ipi_initiator_prcb;  // 0x38
  xe::be<uint32_t> unk_3C;                        // 0x3C
  xe::be<uint32_t> dpc_related_40;                // 0x40
  // must be held to modify any dpc-related fields in the kprcb
  xe::be<uint32_t> dpc_lock;                         // 0x44
  X_LIST_ENTRY queued_dpcs_list_head;                // 0x48
  xe::be<uint32_t> dpc_active;                       // 0x50
  X_KSPINLOCK spin_lock;                             // 0x54
  TypedGuestPointer<X_KTHREAD> running_idle_thread;  // 0x58
  // definitely scheduler related
  X_SINGLE_LIST_ENTRY enqueued_threads_list;      // 0x5C
  xe::be<uint32_t> has_ready_thread_by_priority;  // 0x60
  // i think the following mask has something to do with the array that comes
  // after
  xe::be<uint32_t> unk_mask_64;  // 0x64

  X_LIST_ENTRY unk_68[32];  // 0x68
  // ExTerminateThread tail calls a function that does KeInsertQueueDpc of this
  // dpc
  XDPC thread_exit_dpc;  // 0x168
  // thread_exit_dpc's routine drains this list and frees each threads threadid,
  // kernel stack and dereferences the thread
  X_LIST_ENTRY terminating_threads_list;  // 0x184
  XDPC switch_thread_processor_dpc;       // 0x18C
};
// Processor Control Region
struct X_KPCR {
  xe::be<uint32_t> tls_ptr;   // 0x0
  xe::be<uint32_t> msr_mask;  // 0x4
  union {
    xe::be<uint16_t> software_interrupt_state;  // 0x8
    struct {
      uint8_t generic_software_interrupt;    // 0x8
      uint8_t apc_software_interrupt_state;  // 0x9
    };
  };
  xe::be<uint16_t> unk_0A;           // 0xA
  uint8_t processtype_value_in_dpc;  // 0xC
  uint8_t timeslice_ended;           // 0xD
  uint8_t timer_pending;             // 0xE
  uint8_t unk_0F;                    // 0xF
  // used in KeSaveFloatingPointState / its vmx counterpart
  xe::be<uint32_t> thread_fpu_related;   // 0x10
  xe::be<uint32_t> thread_vmx_related;   // 0x14
  uint8_t current_irql;                  // 0x18
  uint8_t background_scheduling_active;  // 0x19
  uint8_t background_scheduling_1A;      // 0x1A
  uint8_t background_scheduling_1B;      // 0x1B
  xe::be<uint32_t> timer_related;        // 0x1C
  uint8_t unk_20[0x10];                  // 0x20
  xe::be<uint64_t> pcr_ptr;              // 0x30

  // this seems to be just garbage data? we can stash a pointer to context here
  // as a hack for now
  union {
    uint8_t unk_38[8];    // 0x38
    uint64_t host_stash;  // 0x38
  };
  uint8_t unk_40[28];                      // 0x40
  xe::be<uint32_t> unk_stack_5c;           // 0x5C
  uint8_t unk_60[12];                      // 0x60
  xe::be<uint32_t> use_alternative_stack;  // 0x6C
  xe::be<uint32_t> stack_base_ptr;  // 0x70 Stack base address (high addr)
  xe::be<uint32_t> stack_end_ptr;   // 0x74 Stack end (low addr)

  // maybe these are the stacks used in apcs?
  // i know they're stacks, RtlGetStackLimits returns them if another var here
  // is set

  xe::be<uint32_t> alt_stack_base_ptr;  // 0x78
  xe::be<uint32_t> alt_stack_end_ptr;   // 0x7C
  // if bit 1 is set in a handler pointer, it actually points to a KINTERRUPT
  // otherwise, it points to a function to execute
  xe::be<uint32_t> interrupt_handlers[32];  // 0x80
  X_KPRCB prcb_data;                        // 0x100
  // pointer to KPCRB?
  TypedGuestPointer<X_KPRCB> prcb;  // 0x2A8
  uint8_t unk_2AC[0x2C];            // 0x2AC
};

struct X_KMUTANT {
  X_DISPATCH_HEADER header;            // 0x0
  X_LIST_ENTRY unk_list;               // 0x10
  TypedGuestPointer<X_KTHREAD> owner;  // 0x18
  bool abandoned;                      // 0x1C
  // these might just be padding
  uint8_t unk_1D;  // 0x1D
  uint8_t unk_1E;  // 0x1E
  uint8_t unk_1F;  // 0x1F
};
static_assert_size(X_KMUTANT, 0x20);

enum X_KWAIT_REASON : uint16_t {
  WaitAll = 0,
  WaitAny = 1,
  WaitUnk3 = 3,
};

// https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/ke_x/kwait_block.htm
//  pretty much the vista KWAIT_BLOCK verbatim, except that sparebyte is gone
//  and WaitType is 2 bytes instead of 1
struct X_KWAIT_BLOCK {
  X_LIST_ENTRY wait_list_entry;  // 0x0
  TypedGuestPointer<X_KTHREAD> thread;
  TypedGuestPointer<X_DISPATCH_HEADER> object;
  TypedGuestPointer<X_KWAIT_BLOCK> next_wait_block;
  // this isnt the official vista name, but i think its better.
  // this value is what will be returned to the waiter if this particular wait
  // is satisfied
  xe::be<uint16_t> wait_result_xstatus;
  // WAIT_ALL or WAIT_ANY
  xe::be<X_KWAIT_REASON> wait_type;
};

static_assert_size(X_KWAIT_BLOCK, 0x18);

struct X_KTIMER {
  X_DISPATCH_HEADER header;         // 0x0
  xe::be<uint64_t> due_time;        // 0x10
  X_LIST_ENTRY table_bucket_entry;  // 0x18
  TypedGuestPointer<XDPC> dpc;      // 0x20
  xe::be<uint32_t> period;          // 0x24
};
static_assert_size(X_KTIMER, 0x28);

struct X_KTHREAD {
  X_DISPATCH_HEADER header;  // 0x0
  util::X_TYPED_LIST<X_KMUTANT, offsetof(X_KMUTANT, unk_list)>
      mutants_list;                  // 0x10
  X_KTIMER wait_timeout_timer;       // 0x18
  X_KWAIT_BLOCK wait_timeout_block;  // 0x40
  uint8_t unk_58[0x4];               // 0x58
  xe::be<uint32_t> stack_base;       // 0x5C
  xe::be<uint32_t> stack_limit;      // 0x60
  xe::be<uint32_t> stack_kernel;     // 0x64
  xe::be<uint32_t> tls_address;      // 0x68
  // state = is thread running, suspended, etc
  uint8_t thread_state;  // 0x6C
  // 0x70 = priority?
  uint8_t alerted[2];         // 0x6D
  uint8_t alertable;          // 0x6F
  uint8_t priority;           // 0x70
  uint8_t fpu_exceptions_on;  // 0x71
  // these two process types both get set to the same thing, process_type is
  // referenced most frequently, however process_type_dup gets referenced a few
  // times while the process is being created
  uint8_t process_type_dup;
  uint8_t process_type;
  // apc_mode determines which list an apc goes into
  util::X_TYPED_LIST<XAPC, offsetof(XAPC, list_entry)> apc_lists[2];
  TypedGuestPointer<X_KPROCESS> process;  // 0x84
  uint8_t executing_kernel_apc;           // 0x88
  // when context switch happens, this is copied into
  // apc_software_interrupt_state for kpcr
  uint8_t deferred_apc_software_interrupt_state;  // 0x89
  uint8_t user_apc_pending;                       // 0x8A
  uint8_t may_queue_apcs;                         // 0x8B
  X_KSPINLOCK apc_lock;                           // 0x8C
  xe::be<uint32_t> num_context_switches_to;       // 0x90
  X_LIST_ENTRY ready_prcb_entry;                  // 0x94
  xe::be<uint32_t> msr_mask;                      // 0x9C
  xe::be<X_STATUS> wait_result;                   // 0xA0
  uint8_t wait_irql;                              // 0xA4
  uint8_t processor_mode;                         // 0xA5
  uint8_t wait_next;                              // 0xA6
  uint8_t wait_reason;                            // 0xA7
  TypedGuestPointer<X_KWAIT_BLOCK> wait_blocks;   // 0xA8
  uint8_t unk_AC[4];                              // 0xAC
  int32_t apc_disable_count;                      // 0xB0
  xe::be<int32_t> quantum;                        // 0xB4
  uint8_t saturation_increment;                   // 0xB8
  uint8_t base_priority;                          // 0xB9
  uint8_t priority_decrement;                     // 0xBA
  uint8_t boost_disabled;                         // 0xBB
  uint8_t suspend_count;                          // 0xBC
  uint8_t was_preempted;                          // 0xBD
  uint8_t terminated;                             // 0xBE
  uint8_t current_cpu;                            // 0xBF
  // these two pointers point to KPRCBs, but seem to be rarely referenced, if at
  // all
  TypedGuestPointer<X_KPRCB> a_prcb_ptr;        // 0xC0
  TypedGuestPointer<X_KPRCB> another_prcb_ptr;  // 0xC4
  uint8_t process_priority_class;               // 0xC8
  uint8_t base_priority_copy;                   // 0xC9
  uint8_t max_dynamic_priority;                 // 0xCA
  uint8_t unk_CB;                               // 0xCB
  X_KSPINLOCK timer_list_lock;                  // 0xCC
  xe::be<uint32_t> stack_alloc_base;            // 0xD0
  XAPC on_suspend;                              // 0xD4
  X_KSEMAPHORE suspend_sema;                    // 0xFC
  // this is an entry in
  X_LIST_ENTRY process_threads;  // 0x110
  xe::be<uint32_t> unk_118;      // 0x118
  X_LIST_ENTRY queue_related;    // 0x11C
  xe::be<uint32_t> unk_124;      // 0x124
  xe::be<uint32_t> unk_128;      // 0x128
  xe::be<uint32_t> unk_12C;      // 0x12C
  xe::be<uint64_t> create_time;  // 0x130
  xe::be<uint64_t> exit_time;    // 0x138
  xe::be<uint32_t> exit_status;  // 0x140
  // tracks all pending timers that have apcs which target this thread
  X_LIST_ENTRY timer_list;          // 0x144
  xe::be<uint32_t> thread_id;       // 0x14C
  xe::be<uint32_t> start_address;   // 0x150
  X_LIST_ENTRY unk_154;             // 0x154
  uint8_t unk_15C[0x4];             // 0x15C
  xe::be<uint32_t> last_error;      // 0x160
  xe::be<uint32_t> fiber_ptr;       // 0x164
  uint8_t unk_168[0x4];             // 0x168
  xe::be<uint32_t> creation_flags;  // 0x16C

  // we handle context differently from a native kernel, so we can stash extra
  // data here! the first 8 bytes of vscr are unused anyway
  union {
    vec128_t vscr;  // 0x170
    struct {
      void* host_xthread_stash;
      uintptr_t vscr_remainder;
    };
  };

  union {
    // 2048 bytes
    vec128_t vmx_context[128];  // 0x180
    struct {
      // 1536 bytes
      X_KWAIT_BLOCK scratch_waitblock_memory[65];
      // space for some more data!
      uint32_t kernel_aux_stack_base_;
      uint32_t kernel_aux_stack_current_;
      uint32_t kernel_aux_stack_limit_;
    };
  };
  xe::be<double> fpscr;            // 0x980
  xe::be<double> fpu_context[32];  // 0x988

  XAPC unk_A88;  // 0xA88
};
static_assert_size(X_KTHREAD, 0xAB0);

#if !XE_PLATFORM_WIN32
// Exception thrown by XThread::Reenter() to unwind through JIT frames.
// C++ exception unwinding uses DWARF .eh_frame info registered for JIT code,
// ensuring destructors and RAII guards in host C++ frames are properly called.
struct FiberReentryException {
  uint32_t address;
};
#endif

class XThread : public XObject, public cpu::Thread {
 public:
  static const XObject::Type kObjectType = XObject::Type::Thread;

  static constexpr uint32_t kStackAddressRangeBegin = 0x70000000;
  static constexpr uint32_t kStackAddressRangeEnd = 0x7F000000;

  static constexpr uint32_t kThreadKernelStackSize = 0xF0;

  struct CreationParams {
    uint32_t stack_size;
    uint32_t xapi_thread_startup;
    uint32_t start_address;
    uint32_t start_context;
    uint32_t creation_flags;
    uint32_t guest_process;
  };

  XThread(KernelState* kernel_state);
  XThread(KernelState* kernel_state, uint32_t stack_size,
          uint32_t xapi_thread_startup, uint32_t start_address,
          uint32_t start_context, uint32_t creation_flags, bool guest_thread,
          bool main_thread = false, uint32_t guest_process = 0);
  ~XThread() override;

  static bool IsInThread(XThread* other);
  static bool IsInThread();
  static XThread* GetCurrentThread();
  // Returns the currently-running thread iff it is a scheduler-managed fiber
  // (guest code on the dispatch host thread), otherwise nullptr. Use this to
  // decide whether a blocking call yields cooperatively or blocks the host
  // thread.
  static XThread* GetCurrentFiberThread();
  static uint32_t GetCurrentThreadHandle();
  static uint32_t GetCurrentThreadId();

  static uint32_t GetLastError();
  static void SetLastError(uint32_t error_code);

  const CreationParams* creation_params() const { return &creation_params_; }
  uint32_t tls_ptr() const { return tls_static_address_; }
  uint32_t pcr_ptr() const { return pcr_address_; }
  uint32_t stack_base() const { return stack_base_; }
  uint32_t stack_limit() const { return stack_limit_; }
  // True if the thread is created by the guest app.
  bool is_guest_thread() const { return guest_thread_; }
  bool main_thread() const { return main_thread_; }
  bool is_running() const { return running_; }

  // True for threads that run a host C++ routine (XHostThread) rather than
  // guest PPC code. These always use a real host thread, never a cooperative
  // fiber, since they run host loops/blocking and other code dereferences their
  // thread().
  virtual bool is_host_thread() const { return false; }

  uint32_t thread_id() const { return thread_id_; }
  uint32_t last_error();
  void set_last_error(uint32_t error_code);
  void set_name(const std::string_view name);

  X_STATUS Create();
  X_STATUS Exit(int exit_code);
  X_STATUS Terminate(int exit_code);

  virtual void Execute();

  virtual void Reenter(uint32_t address);

  void EnterCriticalRegion();
  void LeaveCriticalRegion();

  void EnqueueApc(uint32_t normal_routine, uint32_t normal_context,
                  uint32_t arg1, uint32_t arg2);

  // True if this thread has a user-mode APC queued (or pending). Used by the
  // cooperative scheduler's alertable waits to return USER_APC, the same way a
  // host alertable wait wakes on a queued APC.
  bool HasPendingUserApc();

  int32_t priority() const { return priority_; }
  int32_t QueryPriority();
  // KeQueryBasePriorityThread: the base priority as a signed increment relative
  // to the priority-class base, NOT the absolute priority.
  int32_t QueryBasePriority();
  void SetPriority(int32_t increment);
  // KeSetBasePriorityThread: |increment| is a signed offset from the priority-
  // class base, not an absolute priority. Returns the previous increment.
  int32_t SetBasePriority(int32_t increment);

  // Called at a guest quantum end. Decays a non-real-time thread by boost + 1
  // levels floored at base, then clears the boost.
  void OnQuantumEnd();
  // Called when a thread wakes from a kernel wait. Applies the Xenon unwait
  // boost of |increment| above base, upward only and clamped to max dynamic.
  void BoostOnWake(int32_t increment);

  // Xbox thread IDs:
  // 0 - core 0, thread 0 - user
  // 1 - core 0, thread 1 - user
  // 2 - core 1, thread 0 - sometimes xcontent
  // 3 - core 1, thread 1 - user
  // 4 - core 2, thread 0 - xaudio
  // 5 - core 2, thread 1 - user
  void SetAffinity(uint32_t affinity);
  uint8_t active_cpu() const;
  void SetActiveCpu(uint8_t cpu_index);

  bool GetTLSValue(uint32_t slot, uint32_t* value_out);
  bool SetTLSValue(uint32_t slot, uint32_t value);

  uint32_t suspend_count();
  X_FILETIME creation_time();
  uint32_t start_address();

  X_STATUS Resume(uint32_t* out_suspend_count = nullptr);
  X_STATUS Suspend(uint32_t* out_suspend_count = nullptr);
  X_STATUS Delay(uint32_t processor_mode, uint32_t alertable,
                 uint64_t interval);

#if !XE_PLATFORM_WIN32
  // Performs self-suspension: increments suspend_count and blocks until
  // another thread calls Resume() and suspend_count reaches 0.
  // Returns the previous suspend_count value.
  uint32_t SelfSuspend();
#endif

  xe::threading::Thread* thread() { return thread_.get(); }

  // The fiber this guest thread runs on when the cooperative scheduler is
  // active (null under the host-thread model). Created in the fiber path of
  // Create().
  xe::threading::Fiber* fiber() const { return fiber_.get(); }

  // Drops the self reference from Create and any surviving handle. The delete
  // point for a fiber thread, so the caller must ensure it is not executing.
  void ReclaimExited();

  // The object this thread is registered on as a cooperative waiter, owned by
  // XObject::Enter/LeaveCooperativeWait. Atomic because the waiting fiber
  // clears it just as a terminating thread may be reading it, and either order
  // is fine since a redundant release is a no-op.
  // Why a fiber is parked, for the scheduler's no-progress report.
  enum class CooperativeWaitKind : uint8_t {
    kNone = 0,
    kSingle,
    kMultiAny,
    kMultiAll,
    kDelay,
    kFence,
    kIoOffload,
  };
  // Records the wait shape for diagnostics. Extra handles beyond the array are
  // dropped; the count reported is the real one so truncation stays visible.
  void set_cooperative_wait_shape(CooperativeWaitKind kind,
                                  const uint32_t* handles, uint32_t count,
                                  XObject* const* objects = nullptr) {
    auto& links = scheduler_links_;
    links.wait_kind = static_cast<uint8_t>(kind);
    links.wait_handle_count = static_cast<uint8_t>(count > 255 ? 255 : count);
    uint32_t n = count < 8 ? count : 8;
    for (uint32_t i = 0; i < n; ++i) {
      links.wait_handles[i] = handles ? handles[i] : 0;
    }
    // Gating needs every object, so a set that does not fit is not gated at
    // all rather than gated on a subset, which could park past a signal.
    links.wait_gate_count = 0;
    if (objects && count <= 8) {
      for (uint32_t i = 0; i < count; ++i) {
        links.wait_gate_objects[i] = objects[i];
      }
      links.wait_gate_count = static_cast<uint8_t>(count);
    }
  }
  void clear_cooperative_wait_shape() {
    scheduler_links_.wait_kind =
        static_cast<uint8_t>(CooperativeWaitKind::kNone);
    scheduler_links_.wait_handle_count = 0;
    scheduler_links_.wait_gate_count = 0;
  }
  // Summed signal epoch of a tracked multi-wait set; 0 when untracked. Any
  // signal or pulse to any member moves the sum, since both bump the epoch.
  uint32_t cooperative_wait_set_epoch() const {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < scheduler_links_.wait_gate_count; ++i) {
      sum += scheduler_links_.wait_gate_objects[i]->cooperative_signal_epoch();
    }
    return sum;
  }
  uint8_t cooperative_wait_set_count() const {
    return scheduler_links_.wait_gate_count;
  }

  XObject* cooperative_wait_object() const {
    return cooperative_wait_object_.load(std::memory_order_acquire);
  }
  void set_cooperative_wait_object(XObject* object) {
    cooperative_wait_object_.store(object, std::memory_order_release);
  }

  // Intrusive scheduler links, owned exclusively by GuestScheduler and only
  // touched under its lock. Embedding them here keeps the queue operations
  // allocation-free. A thread is in at most one of the ready or blocked lists.
  struct SchedulerLinks {
    XThread* ready_next = nullptr;  // link for the ready OR blocked list
    int cpu = -1;                   // CPU owning the list we are on
    int queued_prio = 0;     // priority level of the ready list we are on
    bool queued = false;     // in the ready list
    bool blocked = false;    // parked in the blocked (waiting) list
    bool suspended = false;  // parked with a nonzero suspend count
    bool running = false;    // executing on a dispatch thread
    bool preempted = false;  // slice cut short by a higher-priority thread
    bool has_run = false;    // diagnostic: dispatched at least once
    bool forced_preempt_logged =
        false;  // one forced-preempt warning per thread
    // Set by an external Terminate, exits the fiber at its next
    // ExitIfTerminated check.
    std::atomic<bool> terminate_pending{false};
    // Absolute raw-tick end of the granted timeslice, 0 = grant fresh at
    // dispatch. Preemption preserves it so the quantum end still arrives.
    uint64_t quantum_deadline_tick = 0;
    // xe::Profiler::ThreadLogHandle for this thread's fiber, installed by
    // SwitchTo for as long as it runs. Void to keep profiling.h, and the UI
    // headers behind it, out of the kernel headers.
    void* profiler_log = nullptr;
    // Re-poll gating, written by BlockCurrentThread, read by RereadyBlocked.
    bool wait_gated = false;        // skip re-polls until something below fires
    bool wait_alertable = false;    // also re-poll on a pending user APC
    uint32_t wait_epoch = 0;        // object epoch sampled before the last poll
    uint64_t wait_deadline_ms = 0;  // absolute host uptime, 0 = none
    // What this fiber parked in and the guest handles it named. Handles, not
    // XObject pointers: safe to print if the object is released mid-dump, and
    // they key the signal ring. Without it a multi-object wait dumps obj=0x0.
    uint8_t wait_kind = 0;  // CooperativeWaitKind
    uint8_t wait_handle_count = 0;
    uint32_t wait_handles[8] = {};
    // Objects of a multi-wait, for re-poll gating. Only read while the fiber is
    // parked, where the waiting frame keeps them alive - the same lifetime the
    // single-object cooperative_wait_object() already relies on. Zero when the
    // set was too large to track, which just means no gating.
    XObject* wait_gate_objects[8] = {};
    uint8_t wait_gate_count = 0;

    // Consecutive safepoints that declined to preempt because the guest was at
    // IRQL >= 2. Bounds the defer so a guest spinning at DISPATCH_LEVEL on a
    // co-resident holder cannot livelock its dispatch CPU forever.
    uint32_t preempt_defers_irql = 0;
    // Same, for holding the global critical region. Diagnostic only - yielding
    // there would let a co-resident fiber re-enter the recursive lock.
    uint32_t preempt_defers_lock = 0;
  };
  SchedulerLinks& scheduler_links() { return scheduler_links_; }

  // A handle a host-side caller can block on until this thread exits. Resolves
  // to the host thread, or (for a fiber-backed thread) its exit event.
  xe::threading::WaitHandle* wait_handle() { return GetWaitHandle(); }

  virtual bool Save(ByteStream* stream) override;
  static object_ref<XThread> Restore(KernelState* kernel_state,
                                     ByteStream* stream);

  // Internal - do not use.
  void AcquireMutantOnStartup(object_ref<XMutant> mutant) {
    pending_mutant_acquires_.push_back(mutant);
  }
  // Rebinds the per-host-thread TLS (XThread / cpu::Thread / ThreadState) so
  // |thread| becomes the current guest thread on the calling host thread. Used
  // on each fiber switch by the guest scheduler. Pass nullptr to clear.
  static void SetCurrentThread(XThread* thread);
  void OnHostThreadExitCleanup();

 protected:
  bool AllocateStack(uint32_t size);
  void FreeStack();
  void InitializeGuestObject();

  void DeliverAPCs();
  void RundownAPCs();

  // Publishes a new effective priority to the guest KTHREAD field, the host
  // thread and the scheduler's ready queue. Every change goes through here.
  void PublishPriority(int32_t priority);

  xe::threading::WaitHandle* GetWaitHandle() override {
    // Under the cooperative scheduler there is no host thread, so a
    // fiber-backed thread exposes an event signaled on exit for other threads
    // to wait on.
    if (thread_) {
      return thread_.get();
    }
    return fiber_exit_event_.get();
  }

  CreationParams creation_params_ = {0};

  std::vector<object_ref<XMutant>> pending_mutant_acquires_;

  uint32_t thread_id_ = 0;
  uint32_t tls_static_address_ = 0;
  uint32_t tls_dynamic_address_ = 0;
  uint32_t tls_total_size_ = 0;
  uint32_t pcr_address_ = 0;
  uint32_t stack_alloc_base_ = 0;  // Stack alloc base
  uint32_t stack_alloc_size_ = 0;  // Stack alloc size
  uint32_t stack_base_ = 0;        // High address
  uint32_t stack_limit_ = 0;       // Low address
  bool guest_thread_ = false;
  bool main_thread_ = false;  // Entry-point thread
  bool running_ = false;

  int32_t priority_ = 0;       // current effective priority (may be decayed)
  int32_t base_priority_ = 0;  // priority floor — decay never goes below this
  int32_t boost_amount_ = 0;   // accumulated priority boost above base

#if !XE_PLATFORM_WIN32
  // Condition variable for thread self-suspension.
  std::mutex suspend_mutex_;
  std::condition_variable suspend_cv_;
#endif

  // Reentry mechanism for fiber-based stack switching.
  // On Linux, C++ exceptions are used instead of setjmp/longjmp so that
  // destructors and RAII guards in host C++ frames are properly unwound.
  // JIT code has DWARF .eh_frame unwind info registered via __register_frame.
#if XE_PLATFORM_WIN32
  std::jmp_buf reentry_jmp_buf_;
  uint32_t reentry_address_ = 0;
#endif

  // When the cooperative scheduler is active, the guest thread runs on this
  // fiber instead of its own host thread (cpu::Thread::thread_).
  std::unique_ptr<xe::threading::Fiber> fiber_;
  SchedulerLinks scheduler_links_;
  // Set by the first ReclaimExited so both terminal paths reclaim once.
  std::atomic<bool> self_reference_dropped_{false};
  // Owning thread only; no atomics.
  uint32_t zero_delay_spins_ = 0;
  uint64_t zero_delay_last_tick_ = 0;
  // Owned by XObject::Enter/LeaveCooperativeWait.
  std::atomic<XObject*> cooperative_wait_object_{nullptr};
  // Signaled when a fiber-backed thread exits, so waits on the thread object
  // resolve (the host thread handle that normally serves this role is absent).
  std::unique_ptr<xe::threading::Event> fiber_exit_event_;

  std::mutex thread_lock_;
};

class XHostThread : public XThread {
 public:
  XHostThread(KernelState* kernel_state, uint32_t stack_size,
              uint32_t creation_flags, std::function<int()> host_fn,
              uint32_t guest_process = 0);

  bool is_host_thread() const override { return true; }

  void Execute() override;

 private:
  std::function<int()> host_fn_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTHREAD_H_
