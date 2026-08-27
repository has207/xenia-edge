/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_scheduler.h"

#include <algorithm>
#include <string>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/mutex.h"
#include "xenia/base/profiling.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

DEFINE_bool(
    guest_scheduler_stats, false,
    "Log guest scheduler counters once a second: blocked-waiter re-poll rate, "
    "fiber switches, forced preemptions, and how long offloaded blocking calls "
    "queue behind the single I/O worker.",
    "Kernel");

namespace xe {
namespace kernel {

// Logical CPU index of the host thread currently executing, or -1 on any
// non-dispatch thread. Set by each CPU's RunLoop.
static thread_local int t_current_cpu = -1;

// Off a dispatch thread there is no CPU to index, so the caller must bail.
static bool OnDispatchThread(const char* what) {
  if (t_current_cpu >= 0) {
    return true;
  }
  XELOGW("GuestScheduler: {} called off a dispatch thread, ignoring", what);
  return false;
}

// Clamps a thread's priority to the ready-queue index range [0, 31].
static int ClampPriority(int32_t priority) {
  return priority < 0 ? 0 : (priority > 31 ? 31 : priority);
}

// Safepoints that may decline to preempt before one is forced through anyway.
// A guest spinning at DISPATCH_LEVEL passes safepoints at roughly the loop
// rate, so this is a short wait in wall-clock terms, and the alternative is an
// unbounded livelock when the holder it spins on is co-resident.
static constexpr uint32_t kMaxIrqlPreemptDefers = 4096;
// Reporting threshold for the lock case, which is never forced.
static constexpr uint32_t kLockPreemptDeferReport = 65536;

// Sleeping a fiber would stall its dispatch thread.
static void ReleaseSpinWait(void* /*raw_context*/, uint64_t sleep_ns) {
  if (XThread::GetCurrentFiberThread()) {
    GuestScheduler::SpinYield();
  } else {
    xe::threading::NanoSleep(int64_t(sleep_ns));
  }
}

// JIT safepoint handler. The cold path cleared the flag, so the deferred
// cases re-set it to retry at the next safepoint.
static void PreemptCurrentFiber(void* /*raw_context*/) {
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self) {
    return;
  }
  auto* context = self->thread_state()->context();
  auto& links = self->scheduler_links();
  // A co-resident fiber would re-enter the recursive lock on this host thread,
  // silently breaking mutual exclusion, so this one is never forced. Report a
  // fiber stuck here instead - it means guest code is spinning under the global
  // lock, which the lock's own holder has to resolve.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    context->preempt_requested = 1;
    if (++links.preempt_defers_lock == kLockPreemptDeferReport) {
      XELOGW(
          "GuestScheduler: fiber tid={:08X} '{}' has declined {} preemptions "
          "holding the global critical region; co-resident fibers cannot run",
          self->thread_id(), self->thread_name(), kLockPreemptDeferReport);
    }
    return;
  }
  links.preempt_defers_lock = 0;
  // At DISPATCH_LEVEL the console masks the decrementer, but it also runs the
  // lock holder on another core. Here the holder may be a fiber queued behind
  // this one, so honoring the mask indefinitely livelocks. Defer a bounded
  // number of times, then switch anyway - IRQL still orders guest APCs.
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  bool forced_at_irql = false;
  if (kpcr->current_irql >= 2) {
    if (++links.preempt_defers_irql < kMaxIrqlPreemptDefers) {
      context->preempt_requested = 1;
      return;
    }
    forced_at_irql = true;
    self->kernel_state()->guest_scheduler()->NoteForcedPreempt();
    if (!links.forced_preempt_logged) {
      links.forced_preempt_logged = true;
      XELOGW(
          "GuestScheduler: forcing preemption of tid={:08X} '{}' at IRQL {} "
          "after {} declined safepoints (first time for this thread)",
          self->thread_id(), self->thread_name(), uint32_t(kpcr->current_irql),
          links.preempt_defers_irql);
    }
  }
  links.preempt_defers_irql = 0;
  // Involuntary quantum end, so no yield to a lower-priority thread - except
  // on the forced path, where the whole point is to reach a holder the strict
  // priority order would keep queued behind us.
  self->kernel_state()->guest_scheduler()->YieldCurrentThread(true,
                                                              forced_at_irql);
}

// Raw host ticks per us for the watchdog's deadline math, 0 if unusable.
static double CalibrateTicksPerUs() {
  // Median of three short samples. A single busy-spin sample can be stretched
  // by a migration between big and little cores or a DVFS transition mid-loop,
  // and the accept range below is far too wide to catch a merely skewed one.
  double samples[3] = {};
  for (double& sample : samples) {
    uint64_t qpc_freq = Clock::host_tick_frequency_platform();
    uint64_t qpc0 = Clock::host_tick_count_platform();
    uint64_t tsc0 = Clock::host_tick_count_raw();
    uint64_t qpc_end = qpc0 + qpc_freq / 2000;  // ~0.5 ms
    while (Clock::host_tick_count_platform() < qpc_end) {
    }
    uint64_t qpc1 = Clock::host_tick_count_platform();
    uint64_t tsc1 = Clock::host_tick_count_raw();
    double secs = qpc1 > qpc0 ? double(qpc1 - qpc0) / double(qpc_freq) : 0.0;
    sample = secs > 0.0 ? double(tsc1 - tsc0) / (secs * 1e6) : 0.0;
  }
  if (samples[0] > samples[1]) {
    std::swap(samples[0], samples[1]);
  }
  if (samples[1] > samples[2]) {
    std::swap(samples[1], samples[2]);
  }
  if (samples[0] > samples[1]) {
    std::swap(samples[0], samples[1]);
  }
  double per_us = samples[1];
  // Spans an x86 TSC at 1-6 GHz and an ARM64 generic timer at 1-100 MHz.
  if (per_us < 0.5 || per_us > 100000.0) {
    return 0.0;
  }
  return per_us;
}

GuestScheduler::GuestScheduler(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

GuestScheduler::~GuestScheduler() { Shutdown(); }

bool GuestScheduler::enabled() { return cvars::guest_scheduler; }

int GuestScheduler::DispatchCpuOf(uint8_t guest_cpu) const {
  // Wrap rather than fold to 0: an out-of-range guest CPU is already unusual,
  // and folding every one of them onto CPU 0 stacks them on the dispatch
  // thread the main thread already uses.
  return guest_cpu % kMaxCpus;
}

int GuestScheduler::CpuOf(XThread* thread) const {
  return DispatchCpuOf(thread->guest_object<X_KTHREAD>()->current_cpu);
}

void GuestScheduler::EnsureStarted() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  xe::cpu::backend::preempt_yield_handler = &PreemptCurrentFiber;
  xe::cpu::backend::spin_wait_release_handler = &ReleaseSpinWait;
  // Not in the ctor, which runs before per-title cvar overrides are applied.
  double ticks_per_us = CalibrateTicksPerUs();
  ticks_per_us_ = ticks_per_us;
  quantum_ticks_ =
      static_cast<uint64_t>(ticks_per_us * cvars::guest_scheduler_quantum_us);
  if (quantum_ticks_) {
    XELOGI("GuestScheduler: preemption slice = {} us ({} ticks)",
           uint32_t(cvars::guest_scheduler_quantum_us), quantum_ticks_);
  } else {
    // Priority and wake preemption still work, they raise the flag directly.
    XELOGW(
        "GuestScheduler: no timeslice preemption ({}), a fiber that never "
        "yields or waits can hog its CPU",
        ticks_per_us > 0.0 ? "guest_scheduler_quantum_us is 0"
                           : "host tick counter did not calibrate");
  }

  for (int i = 0; i < kMaxCpus; ++i) {
    cpus_[i].ready_event = xe::threading::Event::CreateAutoResetEvent(false);
  }
  if (quantum_ticks_) {
    watchdog_event_ = xe::threading::Event::CreateAutoResetEvent(false);
    xe::threading::Thread::CreationParameters params;
    watchdog_thread_ =
        xe::threading::Thread::Create(params, [this]() { WatchdogLoop(); });
    watchdog_thread_->set_name("Guest Scheduler Watchdog");
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    xe::threading::Thread::CreationParameters params;
    cpus_[i].host_thread =
        xe::threading::Thread::Create(params, [this, i]() { RunLoop(i); });
    cpus_[i].host_thread->set_name(std::string("Guest CPU ") +
                                   std::to_string(i));
  }
}

void GuestScheduler::Shutdown() {
  if (!started_.load() && !io_started_.load()) {
    return;
  }
  if (stopped_.load()) {
    return;
  }
  shutting_down_.store(true);
  for (Cpu& cpu : cpus_) {
    if (cpu.ready_event) {
      cpu.ready_event->Set();
    }
  }
  if (io_event_) {
    io_event_->Set();
  }
  if (watchdog_event_) {
    watchdog_event_->Set();
  }
  for (Cpu& cpu : cpus_) {
    if (!cpu.host_thread) {
      continue;
    }
    // Join before reset(), which only closes the handle. A spinning fiber
    // only leaves via the preempt flag, so keep raising it until the loop
    // drains.
    int waited_ms = 0;
    while (xe::threading::Wait(cpu.host_thread.get(), false,
                               std::chrono::milliseconds(50)) ==
           xe::threading::WaitResult::kTimeout) {
      {
        std::lock_guard<std::mutex> lock(lock_);
        for (int i = 0; i < kMaxCpus; ++i) {
          if (XThread* running = cpus_[i].current_thread) {
            running->thread_state()->context()->preempt_requested = 1;
          }
        }
      }
      for (int i = 0; i < kMaxCpus; ++i) {
        if (cpus_[i].ready_event) {
          cpus_[i].ready_event->Set();
        }
      }
      waited_ms += 50;
      if (waited_ms % 2000 == 0) {
        XELOGW(
            "GuestScheduler: shutdown has waited {} ms for a dispatch thread, "
            "its fiber is not reaching a safepoint",
            waited_ms);
      }
    }
    cpu.host_thread.reset();
  }
  if (watchdog_thread_) {
    xe::threading::Wait(watchdog_thread_.get(), false);
    watchdog_thread_.reset();
  }
  // After the dispatch threads, so no fiber is still watching a BlockingCall.
  if (io_thread_) {
    xe::threading::Wait(io_thread_.get(), false);
    io_thread_.reset();
  }
  // Everything still linked is unreachable now that the dispatch threads are
  // gone. Reclaim each thread so a relaunch does not leak it and its stack.
  std::vector<XThread*> leftovers;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto drain = [&leftovers](XThread*& head, XThread*& tail) {
      for (XThread* t = head; t;) {
        auto& links = t->scheduler_links();
        XThread* next = links.ready_next;
        links.queued = false;
        links.blocked = false;
        links.suspended = false;
        links.ready_next = nullptr;
        leftovers.push_back(t);
        t = next;
      }
      head = nullptr;
      tail = nullptr;
    };
    for (Cpu& cpu : cpus_) {
      for (int prio = 0; prio < 32; ++prio) {
        drain(cpu.ready_head[prio], cpu.ready_tail[prio]);
      }
      cpu.ready_summary = 0;
      drain(cpu.blocked_head, cpu.blocked_tail);
      drain(cpu.suspended_head, cpu.suspended_tail);
      if (cpu.exited_thread) {
        leftovers.push_back(cpu.exited_thread);
        cpu.exited_thread = nullptr;
      }
      cpu.yield_to_other = nullptr;
      cpu.current_thread = nullptr;
      cpu.has_blocked.store(false, std::memory_order_relaxed);
    }
  }
  if (!leftovers.empty()) {
    XELOGI("GuestScheduler: reclaiming {} parked fibers on shutdown",
           leftovers.size());
  }
  for (XThread* t : leftovers) {
    // A parked waiter's registration would otherwise dangle on the object.
    XObject::AbandonCooperativeWait(t);
    t->ReclaimExited();
  }
  stopped_.store(true);
}

void GuestScheduler::EnqueueReady(XThread* thread, int cpu_index,
                                  bool yield_to_other) {
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    // The single gate for every "make it runnable" request, so a state that
    // already owns its wake-up is a silent no-op. Blocked and suspended move
    // via RereadyBlocked and ResumeThread, all three lists sharing ready_next.
    if (links.blocked || links.suspended) {
      return;
    }
    // A running fiber's context is not saved until it yields, so only the
    // dispatch thread that owns it, links.cpu, may re-queue it.
    if (links.running && links.cpu != t_current_cpu) {
      return;
    }
    if (links.queued) {
      return;
    }
    links.queued = true;
    links.cpu = cpu_index;
    bool at_head = links.preempted;
    links.preempted = false;
    LinkReadyLocked(cpus_[cpu_index], thread, at_head);
    if (yield_to_other) {
      cpus_[cpu_index].yield_to_other = thread;
    }
  }
  // Only a parked dispatch thread needs the syscall.
  if (cpus_[cpu_index].parked.load() && cpus_[cpu_index].ready_event) {
    cpus_[cpu_index].ready_event->Set();
  }
}

void GuestScheduler::MarkReady(XThread* thread) {
  assert_not_null(thread);
  // Don't re-enqueue a terminated thread, or a stray Resume could revive a
  // zombie.
  if (thread->guest_object<X_KTHREAD>()->thread_state ==
      KTHREAD_STATE_TERMINATED) {
    return;
  }
  EnqueueReady(thread, CpuOf(thread));
}

void GuestScheduler::ResumeThread(XThread* thread) {
  assert_not_null(thread);
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    if (links.suspended) {
      Cpu& cpu = cpus_[links.cpu];
      UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
      links.suspended = false;
      links.ready_next = nullptr;
    }
  }
  // Only enqueues if it was never queued, e.g. created suspended.
  MarkReady(thread);
}

bool GuestScheduler::ParkSuspended(XThread* thread, int cpu_index) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  // Re-read under the lock, a Resume racing the dispatcher's check would have
  // found us not yet parked and parking anyway would strand the thread.
  // Termination overrides suspension, run it so it can exit.
  if (thread->suspend_count() == 0 ||
      links.terminate_pending.load(std::memory_order_relaxed)) {
    return false;
  }
  // Clearing running last, so it is never both unowned and unlisted.
  links.suspended = true;
  links.cpu = cpu_index;
  links.ready_next = nullptr;
  links.quantum_deadline_tick = 0;
  Cpu& cpu = cpus_[cpu_index];
  LinkTailLocked(cpu.suspended_head, cpu.suspended_tail, thread);
  links.running = false;
  return true;
}

XThread* GuestScheduler::HighestReadyExcept(const Cpu& cpu, XThread* except) {
  uint32_t summary = cpu.ready_summary;
  while (summary) {
    int level = 31 - xe::lzcnt(summary);
    summary &= ~(uint32_t(1) << level);
    XThread* head = cpu.ready_head[level];
    if (head != except) {
      return head;
    }
    // Its successor outranks anything on a lower level.
    if (except->scheduler_links().ready_next) {
      return except->scheduler_links().ready_next;
    }
  }
  return nullptr;
}

XThread* GuestScheduler::DequeueReady(int cpu_index) {
  std::lock_guard<std::mutex> lock(lock_);
  Cpu& cpu = cpus_[cpu_index];
  if (cpu.ready_summary == 0) {
    return nullptr;
  }
  // Strict priority alone lets a high-priority yield-spinner deadlock on the
  // lower-priority co-resident it depends on, so a voluntary yield opts out.
  XThread* yielder = cpu.yield_to_other;
  cpu.yield_to_other = nullptr;

  // Highest set bit = highest ready priority.
  int level = 31 - xe::lzcnt(cpu.ready_summary);
  XThread* thread = cpu.ready_head[level];
  if (yielder && thread == yielder) {
    if (XThread* other = HighestReadyExcept(cpu, yielder)) {
      // |other| may sit mid-list, so unlink it generally rather than as a head.
      int other_level = other->scheduler_links().queued_prio;
      UnlinkLocked(cpu.ready_head[other_level], cpu.ready_tail[other_level],
                   other);
      if (!cpu.ready_head[other_level]) {
        cpu.ready_summary &= ~(uint32_t(1) << other_level);
      }
      auto& other_links = other->scheduler_links();
      other_links.ready_next = nullptr;
      other_links.queued = false;
      other_links.running = true;
      return other;
    }
  }

  auto& links = thread->scheduler_links();
  cpu.ready_head[level] = links.ready_next;
  if (!cpu.ready_head[level]) {
    cpu.ready_tail[level] = nullptr;
    cpu.ready_summary &= ~(uint32_t(1) << level);
  }
  links.ready_next = nullptr;
  links.queued = false;
  // Owned from here, not from SwitchTo, because in between it is in no list and
  // a concurrent MarkReady would queue it onto another CPU.
  links.running = true;
  return thread;
}

void GuestScheduler::LinkTailLocked(XThread*& head, XThread*& tail,
                                    XThread* thread) {
  if (tail) {
    tail->scheduler_links().ready_next = thread;
  } else {
    head = thread;
  }
  tail = thread;
}

void GuestScheduler::LinkHeadLocked(XThread*& head, XThread*& tail,
                                    XThread* thread) {
  thread->scheduler_links().ready_next = head;
  head = thread;
  if (!tail) {
    tail = thread;
  }
}

void GuestScheduler::LinkReadyLocked(Cpu& cpu, XThread* thread, bool at_head) {
  auto& links = thread->scheduler_links();
  int prio = ClampPriority(thread->priority());
  links.queued_prio = prio;
  links.ready_next = nullptr;
  if (at_head) {
    LinkHeadLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
  } else {
    LinkTailLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
  }
  cpu.ready_summary |= uint32_t(1) << prio;
  // Outranking the running fiber flags it, so its next JIT safepoint yields
  // and the dispatcher picks us.
  XThread* running = cpu.current_thread;
  if (running && running != thread &&
      prio > ClampPriority(running->priority())) {
    running->scheduler_links().preempted = true;
    running->thread_state()->context()->preempt_requested = 1;
  }
}

void GuestScheduler::UnlinkLocked(XThread*& head, XThread*& tail,
                                  XThread* thread) {
  XThread** link = &head;
  XThread* prev = nullptr;
  while (*link) {
    if (*link == thread) {
      *link = thread->scheduler_links().ready_next;
      if (tail == thread) {
        tail = prev;
      }
      return;
    }
    prev = *link;
    link = &(*link)->scheduler_links().ready_next;
  }
}

void GuestScheduler::RequeueForPriority(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  if (!links.queued || links.cpu < 0) {
    return;
  }
  Cpu& cpu = cpus_[links.cpu];
  int old = links.queued_prio;
  UnlinkLocked(cpu.ready_head[old], cpu.ready_tail[old], thread);
  if (!cpu.ready_head[old]) {
    cpu.ready_summary &= ~(uint32_t(1) << old);
  }
  LinkReadyLocked(cpu, thread, false);
}

bool GuestScheduler::ForgetThread(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  auto& links = thread->scheduler_links();
  // A thread that ever ran has a live fiber stack and one a dispatch thread
  // owns is about to be switched to, so neither may be freed.
  const bool reclaimable = !links.has_run && !links.running;
  if (links.cpu >= 0) {
    Cpu& cpu = cpus_[links.cpu];
    if (links.queued) {
      int prio = links.queued_prio;
      UnlinkLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
      if (!cpu.ready_head[prio]) {
        cpu.ready_summary &= ~(uint32_t(1) << prio);
      }
    } else if (links.blocked) {
      UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
    } else if (links.suspended) {
      UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
    }
  }
  links.queued = false;
  links.blocked = false;
  links.suspended = false;
  links.ready_next = nullptr;
  // Drop every raw pointer a CPU may still hold to it. A fiber detaching itself
  // keeps current_thread, which SwitchTo clears on the way out.
  for (Cpu& cpu : cpus_) {
    if (cpu.yield_to_other == thread) {
      cpu.yield_to_other = nullptr;
    }
    if (cpu.exited_thread == thread) {
      cpu.exited_thread = nullptr;
    }
    if (cpu.current_thread == thread && !links.running) {
      cpu.current_thread = nullptr;
    }
  }
  return reclaimable;
}

bool GuestScheduler::TerminateThread(XThread* thread) {
  int wake_cpu = -1;
  {
    std::lock_guard<std::mutex> lock(lock_);
    auto& links = thread->scheduler_links();
    links.terminate_pending.store(true, std::memory_order_relaxed);
    if (stopped_.load() || !started_.load()) {
      // No dispatcher will ever run it again, detach it and let the caller
      // free the stack, parked frames and all.
      if (links.cpu >= 0) {
        Cpu& cpu = cpus_[links.cpu];
        if (links.queued) {
          int prio = links.queued_prio;
          UnlinkLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
          if (!cpu.ready_head[prio]) {
            cpu.ready_summary &= ~(uint32_t(1) << prio);
          }
        } else if (links.blocked) {
          UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
        } else if (links.suspended) {
          UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
        }
      }
      links.queued = false;
      links.blocked = false;
      links.suspended = false;
      links.ready_next = nullptr;
      assert_false(links.running);
      return !links.running;
    }
    if (links.running) {
      // Force it to a safepoint, where ExitIfTerminated ends it.
      thread->thread_state()->context()->preempt_requested = 1;
      return false;
    }
    if (links.blocked || links.suspended) {
      // Termination overrides a wait or suspend. Dispatch it so it exits on
      // its own stack and the idle loop reclaims it.
      Cpu& cpu = cpus_[links.cpu];
      if (links.blocked) {
        UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
      } else {
        UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
      }
      links.blocked = false;
      links.suspended = false;
      links.ready_next = nullptr;
      links.queued = true;
      LinkReadyLocked(cpus_[links.cpu], thread, true);
      wake_cpu = links.cpu;
    } else if (!links.queued && !links.has_run) {
      // Created suspended and never queued, nothing is on its stack.
      return true;
    }
    // A queued thread diverts at its resume point, and one that already
    // exited or crashed is the dispatcher's to reclaim.
  }
  if (wake_cpu >= 0 && cpus_[wake_cpu].parked.load() &&
      cpus_[wake_cpu].ready_event) {
    cpus_[wake_cpu].ready_event->Set();
  }
  return false;
}

void GuestScheduler::SwitchTo(XThread* next) {
  assert_not_null(next);
  assert_not_null(next->fiber());
  auto& links = next->scheduler_links();
  if (!links.has_run) {
    links.has_run = true;
    dispatched_any_.store(true);
    XELOGI("GuestScheduler: first run tid={:08X} '{}'", next->thread_id(),
           next->thread_name());
  }
  {
    std::lock_guard<std::mutex> lock(lock_);
    assert_true(links.running);
    cpus_[t_current_cpu].switch_seq.fetch_add(1, std::memory_order_relaxed);
    cpus_[t_current_cpu].current_thread = next;
    // Grant a fresh slice only if the previous one was consumed. A preempted
    // thread resumes with its remainder, so its quantum end still arrives.
    if (!links.quantum_deadline_tick) {
      links.quantum_deadline_tick =
          Clock::host_tick_count_raw() + quantum_ticks_;
    }
    cpus_[t_current_cpu].quantum_deadline_tick = links.quantum_deadline_tick;
  }
  stats_.switches.fetch_add(1, std::memory_order_relaxed);
  XThread::SetCurrentThread(next);
  next->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_RUNNING;
  // A flag raised while this fiber was off-CPU is stale, the dispatcher
  // already served it. A raise racing this clear is restored by the watchdog.
  next->thread_state()->context()->preempt_requested = 0;
  // The profiler keys its scope stack by host thread, so without this every
  // fiber dispatched here would nest its scopes inside whichever one ran
  // before it. A yield resumes this line, so the restore below pairs with it.
  // With no log of its own the fiber shares this dispatch thread's, which
  // misattributes but still records.
  void* fiber_log = links.profiler_log;
  void* dispatch_log = fiber_log ? Profiler::SwapThreadLog(fiber_log) : nullptr;
  next->fiber()->SwitchTo();
  if (fiber_log) {
    Profiler::SwapThreadLog(dispatch_log);
  }
  // Back on the idle fiber.
  {
    std::lock_guard<std::mutex> lock(lock_);
    links.running = false;
    cpus_[t_current_cpu].current_thread = nullptr;
  }
  XThread::SetCurrentThread(nullptr);
}

void GuestScheduler::ReportGlobalLockHazard() {
  static constexpr size_t kMaxReports = 32;
  static constexpr size_t kMaxFrames = 32;

  XThread* self = XThread::GetCurrentThread();
  uint32_t tid = self ? self->thread_id() : 0;
  const char* name = self ? self->thread_name().c_str() : "?";

  cpu::StackWalker* stack_walker =
      kernel_state_->processor() ? kernel_state_->processor()->stack_walker()
                                 : nullptr;
  if (!stack_walker) {
    if (!global_lock_hazard_saturated_.exchange(true)) {
      XELOGW(
          "GuestScheduler: fiber tid={:08X} '{}' yielded while holding the "
          "global critical region (no stack walker to name the shim).",
          tid, name);
    }
    return;
  }

  uint64_t frame_pcs[kMaxFrames] = {};
  uint64_t stack_hash = 0;
  size_t frame_count =
      stack_walker->CaptureStackTrace(frame_pcs, 0, kMaxFrames, &stack_hash);
  if (!frame_count) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(global_lock_hazard_mutex_);
    if (!global_lock_hazard_stacks_.insert(stack_hash).second) {
      return;  // Already reported.
    }
    if (global_lock_hazard_stacks_.size() >= kMaxReports) {
      global_lock_hazard_saturated_.store(true, std::memory_order_relaxed);
    }
  }

  cpu::StackFrame frames[kMaxFrames] = {};
  stack_walker->ResolveStack(frame_pcs, frames, frame_count);

  uint32_t guest_lr = self ? uint32_t(self->thread_state()->context()->lr) : 0;
  // The region is a scoped lock, so the acquiring shim is an ancestor frame.
  XELOGW(
      "GuestScheduler: fiber tid={:08X} '{}' yielded while holding the global "
      "critical region (guest lr={:08X}). A co-resident fiber can now re-enter "
      "the recursive lock. Yield path host stack:",
      tid, name, guest_lr);
  for (size_t i = 0; i < frame_count; ++i) {
    cpu::StackFrame& frame = frames[i];
    if (frame.type == cpu::StackFrame::Type::kHost) {
      XELOGW("  #{:02} host  {:016X} {}", i, frame.host_pc,
             frame.host_symbol.name[0] ? frame.host_symbol.name : "?");
    } else {
      XELOGW("  #{:02} guest {:016X} pc={:08X}", i, frame.host_pc,
             frame.guest_pc);
    }
  }
}

void GuestScheduler::YieldToScheduler() {
  if (!OnDispatchThread("YieldToScheduler")) {
    return;
  }
  if (!global_lock_hazard_saturated_.load(std::memory_order_relaxed) &&
      xe::global_critical_region::is_held_by_current_thread()) {
    ReportGlobalLockHazard();
  }
  cpus_[t_current_cpu].idle_fiber->SwitchTo();
}

void GuestScheduler::ExitIfTerminated() {
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self || !self->scheduler_links().terminate_pending.load(
                   std::memory_order_relaxed)) {
    return;
  }
  // The wait registration may be newer than the one Terminate abandoned.
  XObject::AbandonCooperativeWait(self);
  // A park or dispatch since the terminate may have overwritten this.
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_TERMINATED;
  NotifyThreadExited(self);
  YieldToScheduler();  // never returns
}

bool GuestScheduler::YieldCurrentThread(bool quantum_end, bool to_lower) {
  if (!OnDispatchThread("YieldCurrentThread")) {
    return false;
  }
  // An externally terminated thread stops here.
  ExitIfTerminated();
  XThread* self = XThread::GetCurrentThread();
  auto& links = self->scheduler_links();
  // A slice cut short by a higher-priority thread is not a quantum end, that
  // thread re-runs at the head instead.
  if (quantum_end && !links.preempted) {
    self->OnQuantumEnd();
  }
  // Only a preemption keeps the remaining slice, anything else consumed it.
  if (!links.preempted) {
    links.quantum_deadline_tick = 0;
  }
  int cpu_index = t_current_cpu;
  uint64_t seq_before =
      cpus_[cpu_index].switch_seq.load(std::memory_order_relaxed);
  // Re-queue on the current CPU, not the affinity CPU, because our context is
  // not saved until the yield below and another CPU must not grab it yet.
  EnqueueReady(self, t_current_cpu, to_lower);
  YieldToScheduler();
  // Terminated while queued.
  ExitIfTerminated();
  // One dispatch is our own resume, more means another fiber ran in between.
  // A migration to another CPU counts as scheduling activity outright.
  return t_current_cpu != cpu_index ||
         cpus_[cpu_index].switch_seq.load(std::memory_order_relaxed) -
                 seq_before >
             1;
}

void GuestScheduler::SpinYield(std::chrono::milliseconds host_sleep) {
  XThread* self = XThread::GetCurrentFiberThread();
  if (self) {
    // The holder we spin on may be a fiber queued behind us on this same
    // dispatch thread, so yielding the host thread would never let it run.
    auto* scheduler = self->kernel_state()->guest_scheduler();
    if (host_sleep.count()) {
      // Parking rather than re-queueing, so a lone fiber idles instead of
      // spinning its dispatch thread at full speed.
      scheduler->BlockCurrentThread();
    } else {
      scheduler->YieldCurrentThread(false);
    }
    return;
  }
  if (host_sleep.count()) {
    xe::threading::Sleep(host_sleep);
  } else {
    xe::threading::MaybeYield();
  }
}

void GuestScheduler::EnsureIoWorker() {
  std::call_once(io_once_, [this]() {
    io_event_ = xe::threading::Event::CreateAutoResetEvent(false);
    xe::threading::Thread::CreationParameters params;
    io_thread_ =
        xe::threading::Thread::Create(params, [this]() { IoWorkerLoop(); });
    io_thread_->set_name("Guest I/O");
    io_started_.store(true);
  });
}

bool GuestScheduler::CurrentThreadOffloadsBlockingCalls() {
  if (!enabled() || !XThread::GetCurrentFiberThread()) {
    return false;
  }
  // The offloaded call can need the global critical region itself, and only
  // this fiber can release it, so holding it means running inline.
  return !xe::global_critical_region::is_held_by_current_thread();
}

void GuestScheduler::WaitOnFence(xe::threading::Fence& fence) {
  XThread* self = enabled() ? XThread::GetCurrentFiberThread() : nullptr;
  if (!self) {
    fence.Wait();
    return;
  }
  auto* scheduler = self->kernel_state()->guest_scheduler();
  self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kFence,
                                   nullptr, 0);
  while (!fence.TryWait()) {
    // The signaler touches the fence on this stack, terminate must not free
    // it.
    scheduler->BlockCurrentThread(0, 0, false, false);
  }
  self->clear_cooperative_wait_shape();
}

void GuestScheduler::RunBlockingHostCallOffloaded(
    const std::function<void()>& fn) {
  EnsureIoWorker();
  BlockingCall call;
  call.fn = &fn;
  call.queued_ns = Clock::host_tick_count_raw();
  {
    std::lock_guard<std::mutex> lock(io_lock_);
    io_queue_.push(&call);
  }
  io_event_->Set();
  XThread* self = XThread::GetCurrentFiberThread();
  if (self) {
    self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kIoOffload,
                                     nullptr, 0);
  }
  while (!call.done.load(std::memory_order_acquire)) {
    // The worker writes |call| on this stack, terminate must not free it.
    BlockCurrentThread(0, 0, false, false);
  }
  if (self) {
    self->clear_cooperative_wait_shape();
  }
}

void GuestScheduler::IoWorkerLoop() {
  Profiler::ThreadEnter("GuestScheduler IO");
  while (!shutting_down_.load()) {
    BlockingCall* call = nullptr;
    {
      std::lock_guard<std::mutex> lock(io_lock_);
      if (!io_queue_.empty()) {
        call = io_queue_.front();
        io_queue_.pop();
      }
    }
    if (!call) {
      xe::threading::Wait(io_event_.get(), false);
      continue;
    }
    uint64_t started = Clock::host_tick_count_raw();
    (*call->fn)();
    uint64_t finished = Clock::host_tick_count_raw();
    // Raw ticks, converted only at report time.
    uint64_t queued_for = started - call->queued_ns;
    stats_.io_calls.fetch_add(1, std::memory_order_relaxed);
    stats_.io_queue_ns.fetch_add(queued_for, std::memory_order_relaxed);
    stats_.io_run_ns.fetch_add(finished - started, std::memory_order_relaxed);
    uint64_t prev_max = stats_.io_queue_max_ns.load(std::memory_order_relaxed);
    while (queued_for > prev_max &&
           !stats_.io_queue_max_ns.compare_exchange_weak(
               prev_max, queued_for, std::memory_order_relaxed)) {
    }
    call->done.store(true, std::memory_order_release);
    // Wake the parked caller instead of leaving it to the backoff timer.
    WakeAll();
  }
}

void GuestScheduler::WakeAll() {
  if (!started_.load()) {
    return;
  }
  // Skip the lock when no CPU has a blocked waiter. A stale hint costs at
  // most one backoff interval.
  bool any_blocked = false;
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load()) {
      any_blocked = true;
      break;
    }
  }
  if (!any_blocked) {
    return;
  }
  // Ask each CPU with a blocked waiter to re-poll, preempting its runner only
  // when a waiter outranks it. An equal-priority preempt would head-requeue
  // the runner past ready threads on every signal and starve them.
  {
    std::lock_guard<std::mutex> lock(lock_);
    for (int i = 0; i < kMaxCpus; ++i) {
      Cpu& cpu = cpus_[i];
      if (!cpu.blocked_head) {
        continue;
      }
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      XThread* running = cpu.current_thread;
      if (running &&
          cpu.max_blocked_prio > ClampPriority(running->priority())) {
        running->scheduler_links().preempted = true;
        running->thread_state()->context()->preempt_requested = 1;
      }
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load(std::memory_order_relaxed) &&
        cpus_[i].parked.load() && cpus_[i].ready_event) {
      cpus_[i].ready_event->Set();
    }
  }
}

void GuestScheduler::WakeForSignal(const XObject* object) {
  if (!started_.load()) {
    return;
  }
  bool any_blocked = false;
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load()) {
      any_blocked = true;
      break;
    }
  }
  if (!any_blocked) {
    return;
  }
  bool wake[kMaxCpus] = {};
  {
    std::lock_guard<std::mutex> lock(lock_);
    // Wake the CPUs hosting a waiter whose wait includes this object. The
    // walk is bounded by the blocked population, which the empty-yield fast
    // path keeps small.
    for (int i = 0; i < kMaxCpus; ++i) {
      Cpu& cpu = cpus_[i];
      if (!cpu.blocked_head) {
        continue;
      }
      bool any_watcher = false;
      int watcher_prio = 0;
      for (XThread* t = cpu.blocked_head; t;
           t = t->scheduler_links().ready_next) {
        auto& links = t->scheduler_links();
        bool watches = t->cooperative_wait_object() == object;
        if (!watches) {
          for (uint8_t j = 0; j < links.wait_gate_count; ++j) {
            if (links.wait_gate_objects[j] == object) {
              watches = true;
              break;
            }
          }
        }
        if (watches) {
          int prio = ClampPriority(t->priority());
          watcher_prio = any_watcher ? std::max(watcher_prio, prio) : prio;
          any_watcher = true;
        }
      }
      if (!any_watcher) {
        continue;
      }
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      XThread* running = cpu.current_thread;
      if (running && watcher_prio > ClampPriority(running->priority())) {
        running->scheduler_links().preempted = true;
        running->thread_state()->context()->preempt_requested = 1;
      }
      wake[i] = true;
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    if (wake[i] && cpus_[i].parked.load() && cpus_[i].ready_event) {
      cpus_[i].ready_event->Set();
    }
  }
}

void GuestScheduler::NotifyThreadExited(XThread* thread) {
  if (!OnDispatchThread("NotifyThreadExited")) {
    return;
  }
  XELOGI("GuestScheduler: exited tid={:08X} '{}'", thread->thread_id(),
         thread->thread_name());
  // This CPU's dispatch loop reclaims it, since we can't drop the last handle
  // while running on its fiber.
  cpus_[t_current_cpu].exited_thread = thread;
}

void GuestScheduler::BlockCurrentThread(uint64_t deadline_ms,
                                        uint32_t wait_epoch, bool alertable,
                                        bool interruptible) {
  if (!OnDispatchThread("BlockCurrentThread")) {
    return;
  }
  if (interruptible) {
    ExitIfTerminated();
  }
  XThread* self = XThread::GetCurrentThread();
  int cpu_index = t_current_cpu;
  // Gate only types whose every satisfying transition calls
  // WakeCooperativeWaiters, anything else polls every pass.
  XObject* wait_object = self->cooperative_wait_object();
  bool gated = false;
  if (wait_object) {
    switch (wait_object->type()) {
      case XObject::Type::Event:
      case XObject::Type::Semaphore:
      case XObject::Type::Mutant:
        gated = true;
        break;
      default:
        break;
    }
  } else if (self->cooperative_wait_set_count()) {
    // Multi-wait: gated on the summed epoch of its whole set. Without this it
    // re-readied on every pass, which is most of the scheduler's churn in
    // titles that park worker pools on WaitForMultipleObjects.
    gated = true;
  } else if (deadline_ms && !alertable &&
             static_cast<XThread::CooperativeWaitKind>(
                 self->scheduler_links().wait_kind) ==
                 XThread::CooperativeWaitKind::kDelay) {
    // Pure timed sleep: only the clock can end it, so park until the deadline
    // instead of waking every kPollBackoffMs. Keyed to the delay wait kind -
    // a timed wait whose objects could not be tracked (a 9+ object
    // WaitMultiple) also arrives here objectless with a deadline, and gating
    // that would leave its signals unseen until the deadline or backstop.
    gated = true;
  }
  {
    std::lock_guard<std::mutex> lock(lock_);
    // A signal between the caller's failed poll and this park bumped the epoch
    // but walked the blocked list before we joined it, so nothing would re-poll
    // us before the backstop. Compare under the walk's own lock and poke this
    // CPU on a mismatch; returning to re-poll instead livelocks when the epoch
    // churns faster than a park roundtrip.
    if (gated) {
      uint32_t epoch_now = 0;
      bool epoch_gated = false;
      if (wait_object) {
        epoch_now = wait_object->cooperative_signal_epoch();
        epoch_gated = true;
      } else if (self->cooperative_wait_set_count()) {
        epoch_now = self->cooperative_wait_set_epoch();
        epoch_gated = true;
      }
      if (epoch_gated && epoch_now != wait_epoch) {
        cpus_[cpu_index].repoll_now.store(true, std::memory_order_relaxed);
      }
    }
    auto& links = self->scheduler_links();
    // Park self (running, in no list) on this CPU's blocked list.
    links.blocked = true;
    links.preempted = false;
    links.cpu = cpu_index;
    links.ready_next = nullptr;
    links.wait_gated = gated;
    links.wait_alertable = alertable;
    links.wait_epoch = wait_epoch;
    links.wait_deadline_ms = deadline_ms;
    // A wait consumes the slice.
    links.quantum_deadline_tick = 0;
    Cpu& cpu = cpus_[cpu_index];
    LinkTailLocked(cpu.blocked_head, cpu.blocked_tail, self);
    int prio = ClampPriority(self->priority());
    if (prio > cpu.max_blocked_prio) {
      cpu.max_blocked_prio = prio;
    }
    // Timed need of this waiter: the poll cadence for ungated and alertable
    // waits, a gated deadline, nothing for a quiet gated wait.
    uint64_t due = gated ? deadline_ms : 0;
    if (!gated || alertable) {
      uint64_t cadence = Clock::QueryHostUptimeMillis() + kPollBackoffMs;
      if (!due || cadence < due) {
        due = cadence;
      }
    }
    if (due && due < cpu.next_timed_repoll_ms) {
      cpu.next_timed_repoll_ms = due;
    }
    // seq_cst pairs with the wake pre-filters' loads: epoch-read-then-store
    // here vs bump-then-load there, so at least one side always sees the other.
    cpu.has_blocked.store(true);
  }
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_WAITING;
  YieldToScheduler();
  // Terminated while parked, TerminateThread re-readied us to exit here.
  if (interruptible) {
    ExitIfTerminated();
  }
}

void GuestScheduler::RereadyBlocked(int cpu_index) {
  uint32_t wake_mask = 0;
  stats_.repolls.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(lock_);
    Cpu& cpu = cpus_[cpu_index];
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    bool force_all = now_ms >= cpu.next_force_repoll_ms;
    if (force_all) {
      cpu.next_force_repoll_ms = now_ms + kRepollBackstopMs;
    }
    // Earliest timed need among the waiters kept parked, the backstop bounds
    // it. Waiters re-readied below re-park through BlockCurrentThread, which
    // lowers it again before this CPU can sleep.
    uint64_t next_due = cpu.next_force_repoll_ms;
    XThread* kept_head = nullptr;
    XThread* kept_tail = nullptr;
    int kept_max_prio = -1;
    XThread* t = cpu.blocked_head;
    while (t) {
      auto& links = t->scheduler_links();
      XThread* next = links.ready_next;
      // Skip a gated waiter whose wait cannot have resolved yet.
      if (links.wait_gated && !force_all) {
        // Three gate kinds: a single object's epoch, a multi-wait's summed
        // epoch, or (neither) a pure timed sleep that only the deadline below
        // can resolve.
        XObject* obj = t->cooperative_wait_object();
        bool may_have_resolved = false;
        if (obj) {
          may_have_resolved =
              obj->cooperative_signal_epoch() != links.wait_epoch;
        } else if (links.wait_gate_count) {
          may_have_resolved =
              t->cooperative_wait_set_epoch() != links.wait_epoch;
        }
        if (!may_have_resolved &&
            !(links.wait_deadline_ms && now_ms >= links.wait_deadline_ms) &&
            !(links.wait_alertable && t->HasPendingUserApc())) {
          links.ready_next = nullptr;
          LinkTailLocked(kept_head, kept_tail, t);
          int prio = ClampPriority(t->priority());
          if (prio > kept_max_prio) {
            kept_max_prio = prio;
          }
          if (links.wait_deadline_ms && links.wait_deadline_ms < next_due) {
            next_due = links.wait_deadline_ms;
          }
          if (links.wait_alertable && now_ms + kPollBackoffMs < next_due) {
            // APCs inserted without a WakeAll are only found by polling.
            next_due = now_ms + kPollBackoffMs;
          }
          t = next;
          continue;
        }
      }
      links.blocked = false;
      links.queued = true;
      stats_.rereadied.fetch_add(1, std::memory_order_relaxed);
      // Its current guest CPU, not the one it blocked on, since
      // KeSetAffinityThread may have moved it while blocked.
      int target = CpuOf(t);
      links.cpu = target;
      bool at_head = links.preempted;
      links.preempted = false;
      LinkReadyLocked(cpus_[target], t, at_head);
      if (target != cpu_index) {
        wake_mask |= uint32_t(1) << target;
      }
      t = next;
    }
    cpu.blocked_head = kept_head;
    cpu.blocked_tail = kept_tail;
    cpu.max_blocked_prio = kept_max_prio;
    cpu.next_timed_repoll_ms = next_due;
    cpu.has_blocked.store(kept_head != nullptr, std::memory_order_relaxed);
  }
  // Wake any other dispatch thread that received a ready fiber (this one runs).
  for (int i = 0; i < kMaxCpus; ++i) {
    if ((wake_mask & (uint32_t(1) << i)) && cpus_[i].parked.load() &&
        cpus_[i].ready_event) {
      cpus_[i].ready_event->Set();
    }
  }
}

void GuestScheduler::RunLoop(int cpu_index) {
  t_current_cpu = cpu_index;
  Profiler::ThreadEnter(
      ("GuestScheduler CPU " + std::to_string(cpu_index)).c_str());
  Cpu& cpu = cpus_[cpu_index];
  // Adopt this host thread's stack as this CPU's idle fiber.
  cpu.idle_fiber = xe::threading::Fiber::CreateFromThread();
  XELOGI("GuestScheduler: CPU {} dispatch loop started", cpu_index);

  while (!shutting_down_.load()) {
    if (cpu_index == 0) {
      ReportStatsIfDue();
    }
    // Re-poll blocked waiters on a timer even while other fibers run, or a
    // busy fiber that rarely waits would starve them. The timer runs at what
    // the parked waiters actually need, a wake skips it entirely.
    uint64_t now = Clock::QueryHostUptimeMillis();
    if (cpu.repoll_now.exchange(false, std::memory_order_relaxed) ||
        now >= cpu.next_timed_repoll_ms) {
      RereadyBlocked(cpu_index);
    }

    XThread* next = DequeueReady(cpu_index);
    if (next) {
      // Honor an affinity change that landed while it was queued or running
      // here. Safe now, an off-CPU thread's context is saved.
      int home = CpuOf(next);
      auto& links = next->scheduler_links();
      if (home != cpu_index &&
          !links.terminate_pending.load(std::memory_order_relaxed)) {
        {
          std::lock_guard<std::mutex> lock(lock_);
          links.running = false;
          links.queued = true;
          links.cpu = home;
          bool at_head = links.preempted;
          links.preempted = false;
          LinkReadyLocked(cpus_[home], next, at_head);
        }
        if (cpus_[home].parked.load() && cpus_[home].ready_event) {
          cpus_[home].ready_event->Set();
        }
        XELOGD("GuestScheduler: migrated tid={:08X} to CPU {}",
               next->thread_id(), home);
        continue;
      }
      // A suspend landing while the thread ran or was queued takes effect here.
      if (next->suspend_count() > 0 && ParkSuspended(next, cpu_index)) {
        continue;
      }
      cpu.exited_thread = nullptr;
      SwitchTo(next);
      if (cpu.exited_thread) {
        // On the idle fiber with the exited fiber parked on its final yield, so
        // reclaiming never frees a stack still in use.
        XThread* dead = cpu.exited_thread;
        cpu.exited_thread = nullptr;
        dead->ReclaimExited();
      }
      continue;
    }

    // Nothing ready, so sleep until the next re-poll if waiters are blocked (a
    // MarkReady wakes us sooner), otherwise idle until something is runnable.
    // Park before re-checking the queues, so a wake that saw parked still
    // false is caught here instead of slept through.
    cpu.parked.store(true);
    bool have_blocked;
    bool have_work;
    {
      std::lock_guard<std::mutex> lock(lock_);
      have_blocked = cpu.blocked_head != nullptr;
      have_work = cpu.ready_summary != 0 ||
                  cpu.repoll_now.load(std::memory_order_relaxed);
    }
    if (have_work) {
      cpu.parked.store(false);
      continue;
    }
    if (!have_blocked) {
      if (dispatched_any_.load()) {
        xe::threading::Wait(cpu.ready_event.get(), false);
        cpu.parked.store(false);
        continue;
      }
      // Nothing has ever run, so poll instead of sleeping forever and say so.
      xe::threading::Wait(cpu.ready_event.get(), false,
                          std::chrono::seconds(1));
      cpu.parked.store(false);
      bool warned = false;
      if (!dispatched_any_.load() &&
          never_dispatched_warned_.compare_exchange_strong(warned, true)) {
        XELOGW(
            "GuestScheduler: no guest fiber dispatched after 1s, every guest "
            "thread is unqueued (created suspended and never resumed?)");
      }
      continue;
    }
    now = Clock::QueryHostUptimeMillis();
    uint64_t due = cpu.next_timed_repoll_ms;
    uint64_t sleep_ms = due > now ? due - now : 0;
    xe::threading::Wait(cpu.ready_event.get(), false,
                        std::chrono::milliseconds(sleep_ms));
    cpu.parked.store(false);
    stats_.idle_wakes.fetch_add(1, std::memory_order_relaxed);
  }
  XELOGI("GuestScheduler: CPU {} dispatch loop exited (shutting_down={})",
         cpu_index, shutting_down_.load());
}

void GuestScheduler::NoteForcedPreempt() {
  stats_.forced_preempts.fetch_add(1, std::memory_order_relaxed);
}

void GuestScheduler::ReportStatsIfDue() {
  if (!cvars::guest_scheduler_stats) {
    return;
  }
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (now_ms - stats_last_report_ms_ < 1000) {
    return;
  }
  stats_last_report_ms_ = now_ms;
  auto take = [](std::atomic<uint64_t>& v) {
    return v.exchange(0, std::memory_order_relaxed);
  };
  uint64_t repolls = take(stats_.repolls);
  uint64_t rereadied = take(stats_.rereadied);
  uint64_t idle_wakes = take(stats_.idle_wakes);
  uint64_t switches = take(stats_.switches);
  uint64_t forced = take(stats_.forced_preempts);
  uint64_t io_calls = take(stats_.io_calls);
  uint64_t io_queue = take(stats_.io_queue_ns);
  uint64_t io_run = take(stats_.io_run_ns);
  uint64_t io_queue_max = take(stats_.io_queue_max_ns);
  double ticks_per_us = ticks_per_us_ > 0.0 ? ticks_per_us_ : 1.0;
  auto to_us = [ticks_per_us](uint64_t ticks) {
    return uint64_t(double(ticks) / ticks_per_us);
  };
  XELOGI(
      "GuestScheduler: repolls {}/s (rereadied {}), idle wakes {}, switches "
      "{}, forced preempts {} | io {} calls, queued avg {} us max {} us, ran "
      "avg {} us",
      repolls, rereadied, idle_wakes, switches, forced, io_calls,
      io_calls ? to_us(io_queue / io_calls) : 0, to_us(io_queue_max),
      io_calls ? to_us(io_run / io_calls) : 0);
}

// Names what a fiber is parked on, for the no-progress dump.
static const char* WaitObjectKind(XObject* object) {
  if (!object) {
    return "none";
  }
  switch (object->type()) {
    case XObject::Type::Event:
      return "event";
    case XObject::Type::Semaphore:
      return "semaphore";
    case XObject::Type::Mutant:
      return "mutant";
    case XObject::Type::Thread:
      return "thread";
    case XObject::Type::Timer:
      return "timer";
    default:
      return "other";
  }
}

namespace {
const char* CooperativeWaitKindName(uint8_t kind) {
  switch (static_cast<XThread::CooperativeWaitKind>(kind)) {
    case XThread::CooperativeWaitKind::kSingle:
      return "single";
    case XThread::CooperativeWaitKind::kMultiAny:
      return "multi-any";
    case XThread::CooperativeWaitKind::kMultiAll:
      return "multi-all";
    case XThread::CooperativeWaitKind::kDelay:
      return "delay";
    case XThread::CooperativeWaitKind::kFence:
      return "fence";
    case XThread::CooperativeWaitKind::kIoOffload:
      return "io-offload";
    default:
      return "none";
  }
}

// "multi-any[4] handles=F8000030,F8000034,..." - the handles cross-reference
// against the signal ring dumped alongside.
std::string FormatWaitShape(const XThread::SchedulerLinks& links) {
  std::string out = CooperativeWaitKindName(links.wait_kind);
  if (!links.wait_handle_count) {
    return out;
  }
  out += fmt::format("[{}] handles=", links.wait_handle_count);
  uint32_t shown = links.wait_handle_count < 8 ? links.wait_handle_count : 8;
  for (uint32_t i = 0; i < shown; ++i) {
    out += fmt::format("{}{:08X}", i ? "," : "", links.wait_handles[i]);
  }
  if (links.wait_handle_count > shown) {
    out += ",...";
  }
  return out;
}
}  // namespace

void GuestScheduler::ReportNoProgress() {
  XELOGW(
      "GuestScheduler: no guest frame presented in {} watchdog ticks while the "
      "dispatch threads keep switching. Every fiber below is waiting on "
      "something none of them is producing:",
      no_progress_ticks_);
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  std::lock_guard<std::mutex> lock(lock_);
  for (int i = 0; i < kMaxCpus; ++i) {
    Cpu& cpu = cpus_[i];
    if (XThread* running = cpu.current_thread) {
      auto* context = running->thread_state()->context();
      auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
      XELOGW(
          "  CPU {} running tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
          "irql={} preempt_requested={} ready_summary={:#x}",
          i, running->thread_id(), running->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          uint32_t(kpcr->current_irql), uint32_t(context->preempt_requested),
          cpu.ready_summary);
    } else {
      XELOGW("  CPU {} idle, ready_summary={:#x}", i, cpu.ready_summary);
    }
    // Parked fibers are the interesting half: the cycle is whatever they are
    // all waiting for.
    int listed = 0;
    for (XThread* t = cpu.blocked_head; t && listed < 8;
         t = t->scheduler_links().ready_next, ++listed) {
      auto& links = t->scheduler_links();
      XObject* obj = t->cooperative_wait_object();
      auto* context = t->thread_state()->context();
      int64_t due_in = links.wait_deadline_ms
                           ? int64_t(links.wait_deadline_ms) - int64_t(now_ms)
                           : -1;
      XELOGW(
          "    blocked tid={:08X} '{}' last_safepoint={:08X} lr={:08X} on {} "
          "obj={} wait={} gated={} alertable={} epoch={} deadline_in_ms={}",
          t->thread_id(), t->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          WaitObjectKind(obj), static_cast<const void*>(obj),
          FormatWaitShape(links), links.wait_gated ? 1 : 0,
          links.wait_alertable ? 1 : 0, links.wait_epoch, due_in);
    }
    // Ready-but-not-running fibers matter too: a queue that never drains
    // means the CPU is rotating between the same few spinners.
    for (int prio = 31; prio >= 0; --prio) {
      for (XThread* t = cpu.ready_head[prio]; t;
           t = t->scheduler_links().ready_next) {
        auto* context = t->thread_state()->context();
        XELOGW(
            "    ready   tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
            "prio={}",
            t->thread_id(), t->thread_name(),
            uint32_t(context->last_safepoint_pc), uint32_t(context->lr), prio);
      }
    }
  }
  // The other half of the picture: what was actually signalled recently. A
  // handle the fibers above wait on that never appears here names the producer
  // that stopped; one that appears repeatedly while a waiter stays parked
  // points at the wake being lost instead of never sent.
  auto signals = XObject::RecentCooperativeSignals(64);
  if (signals.empty()) {
    XELOGW("  recent cooperative signals: none recorded");
    return;
  }
  XELOGW("  last {} cooperative signals (oldest first):", signals.size());
  for (const auto& rec : signals) {
    XELOGW("    #{} handle={:08X} type={} by_tid={:08X} lr={:08X} uptime_ms={}",
           rec.seq, rec.handle, uint32_t(rec.type), rec.signaler_thread,
           rec.signaler_lr, rec.uptime_ms);
  }
}

void GuestScheduler::WatchdogLoop() {
  Profiler::ThreadEnter("GuestScheduler Watchdog");
  // Microseconds, not milliseconds: an integer division to ms floors every
  // sub-millisecond quantum to the same 1 ms tick, so the setting would stop
  // meaning anything below 1000. The host still adds wakeup slack, so a short
  // quantum is a target rather than a guarantee.
  uint64_t period_us = cvars::guest_scheduler_quantum_us;
  if (period_us < kMinWatchdogPeriodUs) {
    period_us = kMinWatchdogPeriodUs;
  }
  while (!shutting_down_.load()) {
    if (period_us >= 1000) {
      // Event wait so Shutdown's Set is observed immediately.
      xe::threading::Wait(watchdog_event_.get(), false,
                          std::chrono::milliseconds(period_us / 1000));
    } else {
      // Wait only takes milliseconds, so a sub-millisecond period sleeps
      // instead. Shutdown latency is then bounded by the period itself.
      xe::threading::PreciseSleep(std::chrono::microseconds(period_us));
    }
    if (shutting_down_.load()) {
      break;
    }
    // No-progress detection, outside lock_ (ReportNoProgress takes it).
    // Only meaningful once something has been dispatched, so a title still
    // loading is not reported.
    uint32_t frame = xe::logging::GetFrameNumber();
    if (frame != last_frame_number_ || !dispatched_any_.load()) {
      last_frame_number_ = frame;
      no_progress_ticks_ = 0;
      no_progress_reported_ = false;
    } else if (++no_progress_ticks_ >= kNoProgressReportTicks &&
               !no_progress_reported_) {
      no_progress_reported_ = true;
      ReportNoProgress();
    }

    uint64_t now = Clock::host_tick_count_raw();
    std::lock_guard<std::mutex> lock(lock_);
    for (int i = 0; i < kMaxCpus; ++i) {
      XThread* running = cpus_[i].current_thread;
      if (running && now >= cpus_[i].quantum_deadline_tick) {
        running->thread_state()->context()->preempt_requested = 1;
      }
      // Stall detector: a dispatch count that has not moved for a whole
      // window separates the wedge modes - flag still set means the fiber
      // never reaches a safepoint, flag cleared means it yields but makes no
      // progress.
      uint64_t seq = cpus_[i].switch_seq.load(std::memory_order_relaxed);
      if (!running || seq != stall_last_seq_[i]) {
        stall_last_seq_[i] = seq;
        stall_ticks_[i] = 0;
        stall_reported_[i] = false;
        continue;
      }
      if (++stall_ticks_[i] < kStallReportTicks || stall_reported_[i]) {
        continue;
      }
      stall_reported_[i] = true;
      auto* context = running->thread_state()->context();
      auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
      XELOGW(
          "GuestScheduler: CPU {} has not switched fibers in {} watchdog "
          "ticks. Running tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
          "irql={} preempt_requested={} irql_defers={} lock_defers={} "
          "ready_summary={:#x}",
          i, stall_ticks_[i], running->thread_id(), running->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          uint32_t(kpcr->current_irql), uint32_t(context->preempt_requested),
          running->scheduler_links().preempt_defers_irql,
          running->scheduler_links().preempt_defers_lock,
          cpus_[i].ready_summary);
    }
  }
}

}  // namespace kernel
}  // namespace xe
