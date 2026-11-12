/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_GUEST_SCHEDULER_H_
#define XENIA_KERNEL_GUEST_SCHEDULER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <vector>

#include "xenia/base/threading.h"

namespace xe {
namespace kernel {

class KernelState;
class XObject;
class XThread;

// Cooperative, in-kernel scheduler for guest threads.
//
// Each of the 360's 6 logical CPUs has its own dispatch host thread. Each
// guest thread runs as a host fiber pinned by its guest current_cpu to one
// dispatch thread. Fibers on different dispatch threads run truly in
// parallel, while fibers sharing one are cooperatively scheduled, switching
// at yield points (waits, delays,
// NtYieldExecution, exit) and at JIT safepoints, which test a per-context
// preempt flag raised by the watchdog on slice expiry, by a priority
// preemption or by a wake. A single lock guards every per-CPU queue and is
// never held across a fiber switch.
class GuestScheduler {
 public:
  explicit GuestScheduler(KernelState* kernel_state);
  ~GuestScheduler();

  // True if the cooperative scheduler is active (gated by the cvar).
  static bool enabled();

  // Starts the per-CPU dispatch threads the first time it is called (no-op
  // after).
  void EnsureStarted();
  void Shutdown();

  // Adds |thread| to its CPU's ready queue by guest affinity and wakes that
  // CPU. Idempotent and safe to call from any host thread. A thread already
  // running, queued, blocked or suspended is left alone.
  void MarkReady(XThread* thread);

  // Returns |thread| to its CPU's ready queue once its guest suspend count has
  // dropped to zero. No-op for a thread suspended while running or queued,
  // which is not parked yet and just gets dispatched again.
  void ResumeThread(XThread* thread);

  // Re-links a ready thread into its correct priority level after a priority
  // change. No-op if not queued.
  void RequeueForPriority(XThread* thread);

  // Moves a ready thread to the CPU its guest affinity now names, which the
  // dispatch loop only does once the old CPU dequeues it.
  void MigrateForAffinity(XThread* thread);

  // Yields from a spin loop. On a fiber this hands the dispatch thread to the
  // next ready fiber, since a co-resident holder can only run if we yield.
  // Off the fiber path it yields (or briefly sleeps) the host thread.
  static void SpinYield(std::chrono::milliseconds host_sleep = {});

  // Pokes every dispatch thread with a blocked waiter so it re-polls now rather
  // than waiting out the backoff, flagging its running fiber to yield at the
  // next safepoint. Call after signaling a host primitive a cooperative wait
  // polls. Coarse, it does not track which object was signaled, and the
  // backoff remains a backstop so a missed wake only adds latency.
  void WakeAll();

  // Targeted WakeAll: pokes only the CPUs hosting a blocked waiter of
  // |object|. Untracked waits (9+ object sets) poll on the backoff cadence,
  // which remains the backstop throughout.
  void WakeForSignal(const XObject* object);

  // Whether an offloaded call may overlap the others. Set from the device, see
  // vfs::Device::supports_concurrent_io.
  enum class BlockingCallClass {
    kSerial,
    kConcurrent,
  };

  // Runs |fn|, a blocking host call such as a disc read, on an I/O worker
  // without waiting. kSerial calls share one worker, kConcurrent calls go to a
  // pool so one slow call cannot hold up the rest. Runs inline once shutdown
  // has begun.
  void PostHostCall(std::function<void()> fn, BlockingCallClass call_class);
  // PostHostCall once host uptime reaches |deadline_ms|, with no worker held
  // until then. Runs inline once shutdown has begun. Shutdown drops one still
  // waiting as it drops queued calls.
  void PostHostCallAt(uint64_t deadline_ms, std::function<void()> fn,
                      BlockingCallClass call_class);

  // True when the calling thread is a scheduler-managed fiber, so a blocking
  // host call would stall other fibers and should be offloaded instead.
  static bool CurrentThreadOffloadsBlockingCalls();

  // True inside a call posted by PostHostCall, so on a shared I/O worker
  // rather than a guest thread.
  static bool CurrentThreadIsBlockingCallWorker();

  // Dispatch thread index a guest CPU maps to, for co-residency checks.
  int DispatchCpuOf(uint8_t guest_cpu) const;

  bool shutting_down() const { return shutting_down_.load(); }

  // Waits on a host Fence. On a fiber it polls and parks instead of blocking
  // the dispatch thread, so an unbounded wait such as a UI dialog does not
  // freeze the guest threads sharing it. The Fence must have one waiter.
  static void WaitOnFence(xe::threading::Fence& fence);

  // Opens a background-scheduling window on the background processors, as the
  // console does from its vblank DPC. Those CPUs prefer the low priority band
  // for its duration. Safe to call off a dispatch thread.
  void EnterBackgroundMode();

  // Yields the running guest fiber back to its CPU's idle fiber. Returns (on
  // the calling fiber) once the dispatcher switches back into it.
  void YieldToScheduler();

  // Cooperative yield: re-queue the current thread on its current CPU, then let
  // that CPU pick the next ready thread. |quantum_end| decays priority, for a
  // safepoint expiry or NtYieldExecution. |to_lower| lets the next dispatch
  // prefer another ready thread even if lower priority, matching real
  // NtYieldExecution, and preemption passes false to keep strict priority.
  // Returns true if another fiber ran on this CPU during the yield, so
  // NtYieldExecution can report NO_YIELD_PERFORMED like NT.
  bool YieldCurrentThread(bool quantum_end, bool to_lower = true);

  // YieldCurrentThread, returning false without a switch if nothing could run.
  bool YieldExecution(bool quantum_end);

  // Parks the running guest fiber on its CPU's blocked list and yields. Returns
  // once the dispatcher re-readies it so the wait can re-poll. A single-object
  // wait on an epoch-bumping type is re-readied only when the epoch moves past
  // |wait_epoch|, |deadline_ms| (absolute host uptime, 0 = none) arrives, a
  // user APC lands on an alertable waiter or a kernel APC on an interruptible
  // one. Anything else re-polls every pass. |interruptible| false keeps a
  // terminate from ending the fiber at a park whose waker holds a pointer into
  // this stack.
  void BlockCurrentThread(uint64_t deadline_ms = 0, uint32_t wait_epoch = 0,
                          bool alertable = false, bool interruptible = true);

  // Marks the running guest fiber finished so its CPU can reclaim it, dropping
  // the final handle once control is back on the idle fiber. Call before the
  // final YieldToScheduler().
  void NotifyThreadExited(XThread* thread);

  // Detaches |thread| from every ready, blocked and suspended queue, and drops
  // any per-CPU pointer to it. Used by a crashed fiber detaching itself so a
  // dispatcher can't reach it again. Returns true if the thread may then be
  // freed, meaning nothing is standing on its fiber stack.
  bool ForgetThread(XThread* thread);

  // Handles a fiber-backed thread terminated from another host thread. Returns
  // true if the caller may reclaim it now, nothing will ever stand on its
  // stack again. Otherwise its dispatcher runs it to a safepoint or wait
  // resume point where it exits and is reclaimed.
  bool TerminateThread(XThread* thread);

  // Ends the calling fiber now if an external Terminate marked it, handing it
  // to the dispatcher for reclaim. Returns otherwise.
  void ExitIfTerminated();

  KernelState* kernel_state() const { return kernel_state_; }

 private:
  // Xbox 360 logical CPU count, also the maximum number of dispatch threads.
  static constexpr int kMaxCpus = 6;
  // Re-poll cadence for waiters that resolve only by polling: ungated waits
  // and the APC check of alertable waiters. Quiet gated waiters do not tick
  // at this rate, they wake on signals, deadlines and the backstop.
  static constexpr uint64_t kPollBackoffMs = 1;
  // At least this often every blocked fiber re-polls regardless of the epoch
  // gate, so a missed epoch bump costs a latency blip, not a hang.
  static constexpr uint64_t kRepollBackstopMs = 64;
  // Floor on the watchdog's wake period. Below this the wakeups cost more than
  // the preemption accuracy is worth on a mobile SoC.
  static constexpr uint64_t kMinWatchdogPeriodUs = 250;

  // Per-CPU dispatch state, each driven by its own host thread. Ready and
  // blocked fibers are intrusive FIFOs linked through
  // XThread::scheduler_links().ready_next (a thread is in at most one list).
  struct Cpu {
    // Ready fibers by priority level, ready_summary flagging the non-empty
    // levels so the highest ready priority is one bit scan away.
    XThread* ready_head[32] = {};
    XThread* ready_tail[32] = {};
    std::atomic<uint32_t> ready_summary{0};
    // The fiber currently running on this CPU, for the preemption check.
    XThread* current_thread = nullptr;
    // Set under lock_ by a voluntary yield, so the next DequeueReady prefers
    // any other ready thread over the yielder, even a lower-priority one.
    XThread* yield_to_other = nullptr;
    XThread* blocked_head = nullptr;
    XThread* blocked_tail = nullptr;
    XThread* suspended_head = nullptr;
    XThread* suspended_tail = nullptr;
    XThread* exited_thread = nullptr;
    std::unique_ptr<xe::threading::Thread> host_thread;
    std::unique_ptr<xe::threading::Fiber> idle_fiber;
    std::unique_ptr<xe::threading::Event> ready_event;
    // Hint read lock-free by WakeAll so it only wakes CPUs with a parked
    // waiter. A stale value costs a spurious wake or one backoff interval.
    std::atomic<bool> has_blocked{false};
    // Raw-tick end of the running fiber's slice, stamped by SwitchTo and
    // compared by the watchdog. Guarded by lock_.
    uint64_t quantum_deadline_tick = 0;
    // Makes RunLoop re-poll blocked waiters now instead of on the backoff
    // timer. Set by WakeAll, consumed lock-free by RunLoop.
    std::atomic<bool> repoll_now{false};
    // True while RunLoop sleeps in a ready_event wait. Wakers skip the
    // SetEvent syscall otherwise, a busy dispatch thread re-checks its queues
    // anyway. RunLoop re-checks them after setting this, so a wake that only
    // saw it false is never slept through.
    std::atomic<bool> parked{false};
    // Counts fiber dispatches on this CPU, so a yielder can tell whether
    // anything else ran before it resumed.
    std::atomic<uint64_t> switch_seq{0};
    // Raw-tick end of an open background-scheduling window, 0 if none. While
    // set, DequeueReady prefers the low priority band. Guarded by lock_.
    uint64_t background_until_tick = 0;
    // Earliest raw tick a new window may open, so the duty cycle stays put
    // however often the vblank hook fires. Guarded by lock_.
    uint64_t background_next_tick = 0;
    // Absolute host ms of the next forced full re-poll. Guarded by lock_.
    uint64_t next_force_repoll_ms = 0;
    // Absolute host ms of the next timed re-poll: the earliest gated
    // deadline, the poll cadence while ungated or alertable waiters are
    // parked, or the backstop. Written only by this CPU's dispatch thread,
    // which also reads it to size the idle sleep. 0 = due now.
    uint64_t next_timed_repoll_ms = 0;
    // Highest blocked-fiber priority, so WakeAll only preempts a runner a
    // waiter could outrank. Raised on block, recomputed by RereadyBlocked.
    // Guarded by lock_.
    int max_blocked_prio = -1;
  };

  // The dispatch thread a thread is pinned to by its guest current_cpu.
  int CpuOf(XThread* thread) const;

  // |yield_to_other| marks the thread as that CPU's yielder as it is linked.
  void EnqueueReady(XThread* thread, int cpu_index,
                    bool yield_to_other = false);
  XThread* DequeueReady(int cpu_index);
  // Highest-priority ready thread on |cpu| other than |except|, or null if
  // |except| is the only ready thread. Used to honor a voluntary yield.
  static XThread* HighestReadyExcept(const Cpu& cpu, XThread* except);
  void SwitchTo(XThread* next);
  void RereadyBlocked(int cpu_index);
  // Parks a thread whose suspend count is nonzero until Resume drops it.
  // Returns false if the count already reached zero, meaning run it instead.
  bool ParkSuspended(XThread* thread, int cpu_index);
  void RunLoop(int cpu_index);
  // Raises preempt_requested on any CPU whose running fiber outlived its
  // slice, since a dispatch thread cannot tick while it runs a fiber.
  void WatchdogLoop();
  struct BlockingCall;
  // False once shutdown has begun.
  bool EnqueueBlockingCall(BlockingCall* call, BlockingCallClass call_class);
  // Lazily starts the serial I/O worker on the first kSerial offload.
  void EnsureIoWorker();
  void IoWorkerLoop();
  // Queues a kConcurrent call, growing the pool when every worker is busy.
  bool EnqueuePoolCall(BlockingCall* call);
  // Caller holds io_pool_lock_.
  void StartPoolWorkerLocked();
  void IoPoolWorkerLoop();
  // Lazily starts the thread that holds PostHostCallAt calls until they are
  // due.
  void EnsureIoTimer();
  void IoTimerLoop();
  // Runs one queued call, accumulates its counters and frees it.
  void RunBlockingCall(BlockingCall* call);
  // Unlinks |thread| from a singly-linked list (ready_next), fixing up tail.
  static void UnlinkLocked(XThread*& head, XThread*& tail, XThread* thread);
  // Appends to a singly-linked list (ready_next), fixing up tail.
  static void LinkTailLocked(XThread*& head, XThread*& tail, XThread* thread);
  // Prepends to a singly-linked list (ready_next), fixing up tail.
  static void LinkHeadLocked(XThread*& head, XThread*& tail, XThread* thread);
  // Links a thread into a CPU's ready list at its priority level, sets the
  // summary bit, and preempts the CPU's running fiber if outranked. Caller
  // holds lock_ and has set links.queued and links.cpu.
  void LinkReadyLocked(Cpu& cpu, XThread* thread, bool at_head);
  // Out-of-line so the yield fast path stays a single relaxed bool load.
  void ReportGlobalLockHazard();
  // Counts and reports one starvation promotion on |cpu_index|.
  void NoteStarvation(int cpu_index, XThread* runner, XThread* victim,
                      uint32_t spun);
  // Microseconds |thread| has spent in a ready list, 0 if never stamped.
  uint64_t ready_wait_us(XThread* thread) const;

  KernelState* kernel_state_;

  // Preemption timeslice in raw host ticks, calibrated once in EnsureStarted.
  uint64_t quantum_ticks_ = 0;
  // Length of a background-scheduling window, in raw host ticks.
  uint64_t background_ticks_ = 0;
  // Minimum spacing between windows, in raw host ticks.
  uint64_t background_period_ticks_ = 0;
  std::unique_ptr<xe::threading::Thread> watchdog_thread_;
  std::unique_ptr<xe::threading::Event> watchdog_event_;
  // No-progress detection. The stall detector above only catches a CPU that
  // stops switching fibers; the other wedge mode has every CPU rotating
  // healthily while the guest presents no frames, because the fibers are all
  // waiting on something none of them will produce. Triggered off the present
  // counter, it dumps what every CPU is running or waiting on.
  static constexpr uint32_t kNoProgressReportTicks = 2000;  // ~2 s
  uint32_t last_frame_number_ = 0;
  uint32_t no_progress_ticks_ = 0;
  bool no_progress_reported_ = false;
  void ReportNoProgress();

  // Watchdog-only stall detection state, touched under lock_.
  static constexpr uint32_t kStallReportTicks = 2000;  // ~2 s at a 1 ms period
  uint64_t stall_last_seq_[kMaxCpus] = {};
  uint32_t stall_ticks_[kMaxCpus] = {};
  bool stall_reported_[kMaxCpus] = {};

  // Guards every CPU's ready and blocked lists. Never held across a fiber
  // switch.
  std::mutex lock_;
  Cpu cpus_[kMaxCpus];

  // A fiber yielding while it holds the recursive global lock lets a
  // co-resident fiber re-enter it, silently breaking mutual exclusion. Each
  // distinct yield stack is logged once, then the check goes cold.
  std::atomic<bool> global_lock_hazard_saturated_{false};
  std::mutex global_lock_hazard_mutex_;
  std::unordered_set<uint64_t> global_lock_hazard_stacks_;

  std::atomic<bool> started_{false};
  std::atomic<bool> shutting_down_{false};
  // Set once Shutdown has joined the dispatch threads and reclaimed every
  // leftover fiber. TerminateThread then frees threads directly.
  std::atomic<bool> stopped_{false};
  // Until something has been dispatched, idle CPUs poll slowly instead of
  // sleeping so a title that never starts can be reported.
  std::atomic<bool> dispatched_any_{false};
  std::atomic<bool> never_dispatched_warned_{false};

  // Heap-owned, deleted by the worker after running.
  struct BlockingCall {
    std::function<void()> fn;
    // Raw host ticks when queued, for the I/O wait-time counter.
    uint64_t queued_ns = 0;
  };
  // Cheap counters for the costs this scheduler adds on a mobile SoC: how
  // often parked waiters force a dispatch CPU awake, and how long offloaded
  // blocking calls queue behind an I/O worker. Reported by
  // ReportStatsIfDue when guest_scheduler_stats is set.
  struct Stats {
    std::atomic<uint64_t> repolls{0};         // RereadyBlocked passes
    std::atomic<uint64_t> rereadied{0};       // waiters actually re-readied
    std::atomic<uint64_t> idle_wakes{0};      // timed wakes of a parked CPU
    std::atomic<uint64_t> switches{0};        // fiber dispatches
    std::atomic<uint64_t> skipped_yields{0};  // yields with nothing to run
    std::atomic<uint64_t> yield_downs{0};     // yields that ran a lower prio
    // Of those, the ones the starvation escape hatch forced.
    std::atomic<uint64_t> starvation_yields{0};
    std::atomic<uint64_t> background_windows{0};  // vblanks that opened one
    std::atomic<uint64_t> background_picks{0};    // dispatches the mask steered
    // Ready-list wait before dispatch, the direct measure of priority
    // inversion. Only accumulated while guest_scheduler_stats is set.
    std::atomic<uint64_t> ready_wait_ticks{0};
    std::atomic<uint64_t> ready_wait_count{0};
    std::atomic<uint64_t> ready_wait_max_ticks{0};
    std::atomic<uint64_t> io_calls{0};
    std::atomic<uint64_t> io_queue_ns{0};  // time queued before the worker ran
    std::atomic<uint64_t> io_run_ns{0};    // time inside the blocking call
    std::atomic<uint64_t> io_queue_max_ns{0};
    // Most pool workers inside a call at once. Queue wait cannot show this.
    // An idle worker takes a call immediately, so it stays low either way.
    std::atomic<uint64_t> io_peak_inflight{0};
  };
  Stats stats_;
  uint64_t stats_last_report_ms_ = 0;
  // Calibrated in EnsureStarted, used to render the raw-tick I/O counters.
  double ticks_per_us_ = 0.0;
  void ReportStatsIfDue();

  std::once_flag io_once_;
  std::atomic<bool> io_started_{false};
  std::mutex io_lock_;
  std::queue<BlockingCall*> io_queue_;
  std::unique_ptr<xe::threading::Thread> io_thread_;
  std::unique_ptr<xe::threading::Event> io_event_;

  // Pool for the calls a device lets overlap, grown on demand. The workers
  // sit blocked in a host read, so the bound is not a host core count.
  static constexpr size_t kMaxIoPoolThreads = 4;
  std::mutex io_pool_lock_;
  std::condition_variable io_pool_cv_;
  std::queue<BlockingCall*> io_pool_queue_;
  std::vector<std::unique_ptr<xe::threading::Thread>> io_pool_threads_;
  // Pool workers currently inside a call, under io_pool_lock_.
  size_t io_pool_busy_ = 0;
  // Mirrors io_pool_threads_.size() so the stats report skips io_pool_lock_.
  std::atomic<size_t> io_pool_size_{0};

  // PostHostCallAt calls by deadline, each queued for its class once due.
  struct TimedCall {
    BlockingCall* call;
    BlockingCallClass call_class;
  };
  std::once_flag io_timer_once_;
  std::mutex io_timer_lock_;
  std::condition_variable io_timer_cv_;
  std::multimap<uint64_t, TimedCall> io_timed_;
  std::unique_ptr<xe::threading::Thread> io_timer_thread_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_GUEST_SCHEDULER_H_
