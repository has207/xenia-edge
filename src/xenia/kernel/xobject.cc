/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xobject.h"

#include <array>
#include <optional>

#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

XObject::XObject(Type type)
    : kernel_state_(nullptr), pointer_ref_count_(1), type_(type) {
  handles_.reserve(10);
}

XObject::XObject(KernelState* kernel_state, Type type, bool host_object)
    : kernel_state_(kernel_state),
      type_(type),
      pointer_ref_count_(1),
      guest_object_ptr_(0),
      allocated_guest_object_(false),
      host_object_(host_object) {
  handles_.reserve(10);

  // TODO: Assert kernel_state != nullptr in this constructor.
  if (kernel_state) {
    kernel_state->object_table()->AddHandle(this, nullptr);
  }
}

XObject::~XObject() {
  assert_true(handles_.empty());
  assert_zero(pointer_ref_count_);

  if (allocated_guest_object_) {
    uint32_t header_addr = guest_object_ptr_ - sizeof(X_OBJECT_HEADER);
    auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(header_addr);

    // Free the object creation info
    if (header->object_type_ptr) {
      memory()->SystemHeapFree(header->object_type_ptr);
    }

    memory()->SystemHeapFree(header_addr - kGuestObjectPrePad);
  }
}

Emulator* XObject::emulator() const { return kernel_state_->emulator_; }
KernelState* XObject::kernel_state() const { return kernel_state_; }
Memory* XObject::memory() const { return kernel_state_->memory(); }

XObject::Type XObject::type() const { return type_; }

void XObject::RetainHandle() {
  kernel_state_->object_table()->RetainHandle(handles_[0]);
}

bool XObject::ReleaseHandle() {
  // FIXME: Return true when handle is actually released.
  return kernel_state_->object_table()->ReleaseHandle(handles_[0]) ==
         X_STATUS_SUCCESS;
}

void XObject::Retain() { ++pointer_ref_count_; }

void XObject::Release() {
  if (--pointer_ref_count_ == 0) {
    delete this;
  }
}

X_STATUS XObject::Delete() {
  if (kernel_state_ == nullptr) {
    // Fake return value for api-scanner
    return X_STATUS_SUCCESS;
  } else {
    if (!name_.empty()) {
      kernel_state_->object_table()->RemoveNameMapping(name_);
    }
    return kernel_state_->object_table()->RemoveHandle(handles_[0]);
  }
}

bool XObject::SaveObject(ByteStream* stream) {
  stream->Write<uint32_t>(allocated_guest_object_);
  stream->Write<uint32_t>(guest_object_ptr_);

  stream->Write(uint32_t(handles_.size()));
  stream->Write(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  return true;
}

bool XObject::RestoreObject(ByteStream* stream) {
  allocated_guest_object_ = stream->Read<uint32_t>() > 0;
  guest_object_ptr_ = stream->Read<uint32_t>();

  handles_.resize(stream->Read<uint32_t>());
  stream->Read(&handles_[0], handles_.size() * sizeof(X_HANDLE));

  // Restore our pointer to our handles in the object table.
  for (size_t i = 0; i < handles_.size(); i++) {
    kernel_state_->object_table()->RestoreHandle(handles_[i], this);
  }

  return true;
}

object_ref<XObject> XObject::Restore(KernelState* kernel_state, Type type,
                                     ByteStream* stream) {
  switch (type) {
    case Type::Enumerator:
      break;
    case Type::Event:
      return XEvent::Restore(kernel_state, stream);
    case Type::File:
      return XFile::Restore(kernel_state, stream);
    case Type::IOCompletion:
      break;
    case Type::Module:
      return XModule::Restore(kernel_state, stream);
    case Type::Mutant:
      return XMutant::Restore(kernel_state, stream);
    case Type::NotifyListener:
      return XNotifyListener::Restore(kernel_state, stream);
    case Type::Semaphore:
      return XSemaphore::Restore(kernel_state, stream);
    case Type::Session:
      break;
    case Type::Socket:
      break;
    case Type::SymbolicLink:
      return XSymbolicLink::Restore(kernel_state, stream);
    case Type::Thread:
      return XThread::Restore(kernel_state, stream);
    case Type::Timer:
      break;
    case Type::Undefined:
      break;
  }

  assert_always("No restore handler exists for this object!");
  return nullptr;
}

void XObject::SetAttributes(uint32_t obj_attributes_ptr) {
  if (!obj_attributes_ptr) {
    return;
  }

  auto name = util::TranslateAnsiStringAddress(
      memory(), xe::load_and_swap<uint32_t>(
                    memory()->TranslateVirtual(obj_attributes_ptr + 4)));
  if (!name.empty()) {
    name_ = std::string(name);
    kernel_state_->object_table()->AddNameMapping(name_, handles_[0]);
  }
}

uint32_t XObject::TimeoutTicksToMs(int64_t timeout_ticks) {
  if (timeout_ticks > 0) {
    // NetDll_WSAWaitForMultipleEvents provides timeout in form of MS.
    return (uint32_t)timeout_ticks;
  } else if (timeout_ticks < 0) {
    // Relative time.
    return (uint32_t)(-timeout_ticks / 10000);  // Ticks -> MS
  } else {
    return 0;
  }
}

namespace {
// Mirror NT-observable KTHREAD wait fields so guest code that inline-reads
// them gets live values. Returns null for non-guest host callers.
X_KTHREAD* WaitEnter(uint32_t wait_reason, uint32_t processor_mode,
                     uint32_t alertable) {
  // Waits can come from non-guest host threads (e.g. waiting on a thread object
  // during teardown), where IsInThread() avoids GetCurrentThread() asserting.
  if (!XThread::IsInThread()) {
    return nullptr;
  }
  XThread* self = XThread::GetCurrentThread();
  auto* kthread = self->guest_object<X_KTHREAD>();
  auto* context = self->thread_state()->context();
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  kthread->thread_state = KTHREAD_STATE_WAITING;
  kthread->wait_irql = kpcr->current_irql;
  kthread->wait_reason = static_cast<uint8_t>(wait_reason);
  kthread->processor_mode = static_cast<uint8_t>(processor_mode);
  kthread->alertable = alertable ? 1 : 0;
  return kthread;
}

void WaitExit(X_KTHREAD* kthread, X_STATUS result) {
  // Runs on every cooperative wait exit, so it is also where the diagnostic
  // wait shape is dropped. Unconditional: a thread that never waits again must
  // not keep reporting a stale handle set.
  if (auto* self = XThread::GetCurrentThread()) {
    self->clear_cooperative_wait_shape();
  }
  if (!kthread) {
    return;
  }
  kthread->thread_state = KTHREAD_STATE_RUNNING;
  kthread->wait_result = result;
}

// Drives the cooperative poll-yield loop for a fiber-backed waiter. Repeatedly
// runs |poll| (a zero-timeout acquire returning the terminal X_STATUS on
// success / abandon / failure, or std::nullopt while not yet signaled),
// yielding to the scheduler between attempts via BlockCurrentThread, until it
// resolves, an alertable user APC is pending, or |deadline_ms| (absolute host
// uptime, 0 = infinite) elapses. Polling the host primitive preserves its exact
// acquire semantics, only the blocking is made cooperative. |wait_object| is
// the single object waited on, null for a multi-wait.
template <typename PollFn>
X_STATUS CooperativeWait(GuestScheduler* scheduler, X_KTHREAD* kthread,
                         XObject* wait_object, bool alertable,
                         uint64_t deadline_ms, PollFn&& poll) {
  while (true) {
    // Alertable waits return on a queued user APC (the cooperative equivalent
    // of a host alertable-wait wake), then the caller runs xeProcessUserApcs.
    if (alertable && XThread::GetCurrentThread()->HasPendingUserApc()) {
      WaitExit(kthread, X_STATUS_USER_APC);
      return X_STATUS_USER_APC;
    }
    // Sampled before polling, so a signal landing after a failed poll changes
    // the epoch and the re-poll is not skipped.
    // Sampled before polling, so a signal landing after a failed poll changes
    // it and the re-poll is not skipped. A multi-wait has no single object, so
    // it uses the summed epoch of its set.
    uint32_t wait_epoch = 0;
    if (wait_object) {
      wait_epoch = wait_object->cooperative_signal_epoch();
    } else if (auto* self = XThread::GetCurrentThread()) {
      wait_epoch = self->cooperative_wait_set_epoch();
    }
    std::optional<X_STATUS> resolved = poll();
    if (resolved) {
      WaitExit(kthread, *resolved);
      return *resolved;
    }
    if (deadline_ms != 0 && Clock::QueryHostUptimeMillis() >= deadline_ms) {
      WaitExit(kthread, X_STATUS_TIMEOUT);
      return X_STATUS_TIMEOUT;
    }
    scheduler->BlockCurrentThread(deadline_ms, wait_epoch, alertable);
  }
}

// Signals like KeSetEvent, KeReleaseSemaphore or KeReleaseMutant, updating the
// guest signal_state and waking cooperative waiters.
static X_STATUS SignalObjectCooperatively(XObject* object) {
  switch (object->type()) {
    case XObject::Type::Event:
      static_cast<XEvent*>(object)->Set(0, false);
      return X_STATUS_SUCCESS;
    case XObject::Type::Semaphore: {
      int32_t previous = 0;
      if (!static_cast<XSemaphore*>(object)->ReleaseSemaphore(1, &previous)) {
        return X_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
      }
      return X_STATUS_SUCCESS;
    }
    case XObject::Type::Mutant:
      return static_cast<XMutant*>(object)->ReleaseMutant(0, false, false);
    default:
      XELOGW("SignalAndWait: signal object type {} is not signalable",
             static_cast<uint32_t>(object->type()));
      return X_STATUS_INVALID_HANDLE;
  }
}
}  // namespace

// Stand-in for an object the caller already satisfies, one per wait-array slot
// since a host wait rejects the same handle listed twice.
static xe::threading::WaitHandle* AlwaysSignaledHandle(size_t slot) {
  static const auto pool = []() {
    std::array<std::unique_ptr<xe::threading::Event>, 64> events;
    for (auto& event : events) {
      event = xe::threading::Event::CreateManualResetEvent(true);
    }
    return events;
  }();
  assert_true(slot < pool.size());
  return pool[slot < pool.size() ? slot : 0].get();
}

void CooperativeWaiterFifo::Add(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  for (auto* w : waiters_) {
    if (w == thread) {
      return;  // already queued
    }
  }
  waiters_.push_back(thread);
}

bool CooperativeWaiterFifo::Remove(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
    if (*it == thread) {
      waiters_.erase(it);
      break;
    }
  }
  return !waiters_.empty();
}

bool CooperativeWaiterFifo::MayAcquire(XThread* thread) {
  std::lock_guard<std::mutex> lock(lock_);
  return waiters_.empty() || waiters_.front() == thread;
}

bool CooperativeWaiterFifo::HasWaiters() {
  std::lock_guard<std::mutex> lock(lock_);
  return !waiters_.empty();
}

namespace {
// Signal ring. Small, lock-guarded and write-mostly: it is touched once per
// cooperative wake, which is rare next to the poll traffic it helps explain.
constexpr size_t kSignalRingSize = 256;
std::mutex g_signal_ring_lock;
XObject::SignalRecord g_signal_ring[kSignalRingSize];
uint64_t g_signal_ring_seq = 0;
}  // namespace

void XObject::RecordCooperativeSignal(XObject* object) {
  SignalRecord rec = {};
  rec.handle = object->handle();
  rec.type = static_cast<uint8_t>(object->type());
  rec.uptime_ms = uint32_t(Clock::QueryGuestUptimeMillis());
  if (auto* thread = XThread::GetCurrentThread()) {
    rec.signaler_thread = thread->handle();
    if (auto* state = thread->thread_state()) {
      rec.signaler_lr = uint32_t(state->context()->lr);
    }
  } else {
    rec.signaler_thread = 0xFFFFFFFF;  // host-side signaller
  }
  std::lock_guard<std::mutex> lock(g_signal_ring_lock);
  rec.seq = ++g_signal_ring_seq;
  g_signal_ring[(rec.seq - 1) % kSignalRingSize] = rec;
}

std::vector<XObject::SignalRecord> XObject::RecentCooperativeSignals(
    size_t max) {
  std::vector<SignalRecord> out;
  std::lock_guard<std::mutex> lock(g_signal_ring_lock);
  uint64_t total = g_signal_ring_seq;
  size_t have = size_t(total < kSignalRingSize ? total : kSignalRingSize);
  size_t want = have < max ? have : max;
  out.reserve(want);
  for (size_t i = have - want; i < have; ++i) {
    out.push_back(g_signal_ring[(total - have + i) % kSignalRingSize]);
  }
  return out;
}

void XObject::WakeCooperativeWaiters() {
  cooperative_signal_epoch_.fetch_add(1);
  RecordCooperativeSignal(this);
  // Wake only the CPUs that can act on this signal instead of every CPU with
  // any blocked thread - broadcast wakes on every semaphore release were most
  // of some titles' kernel time (Eternal Sonata: 69% of CPU in futex wakes
  // and the resulting empty scheduling passes). Every watcher's CPU is woken,
  // not just the permit FIFO front's, which loses the wake whenever that
  // waiter leaves between the signal and its dispatch.
  kernel_state()->guest_scheduler()->WakeForSignal(this);
}

void XObject::EnterCooperativeWait(XThread* thread) {
  if (!thread) {
    return;
  }
  CooperativeWaitBegin(thread);
  thread->set_cooperative_wait_object(this);
}

void XObject::LeaveCooperativeWait(XThread* thread) {
  if (!thread) {
    return;
  }
  thread->set_cooperative_wait_object(nullptr);
  CooperativeWaitEnd(thread);
}

void XObject::AbandonCooperativeWait(XThread* thread) {
  if (!thread) {
    return;
  }
  if (XObject* object = thread->cooperative_wait_object()) {
    object->LeaveCooperativeWait(thread);
  }
  // A terminated fiber exits inside the wait, skipping WaitExit, so the gate
  // pointers into its abandoned wait frame are dropped here.
  thread->clear_cooperative_wait_shape();
}

xe::threading::WaitHandle* XObject::GetWaitHandleForCurrentThread(size_t slot) {
  if (IsReenteredByCurrentThread()) {
    return AlwaysSignaledHandle(slot);
  }
  return GetWaitHandle();
}

X_STATUS XObject::Wait(uint32_t wait_reason, uint32_t processor_mode,
                       uint32_t alertable, uint64_t* opt_timeout) {
  auto wait_handle = GetWaitHandleForCurrentThread(0);
  if (!wait_handle) {
    // Object doesn't support waiting.
    return X_STATUS_SUCCESS;
  }

  if (GuestScheduler::enabled() && XThread::GetCurrentFiberThread()) {
    // Cooperative path: poll the host primitive (preserving its exact
    // semantics) and yield the fiber between polls instead of blocking the
    // dispatch host thread.
    auto* scheduler = kernel_state()->guest_scheduler();
    auto* self = XThread::GetCurrentThread();
    X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
    uint64_t deadline_ms = opt_timeout ? Clock::QueryHostUptimeMillis() +
                                             Clock::ScaleGuestDurationMillis(
                                                 TimeoutTicksToMs(*opt_timeout))
                                       : 0;
    const uint32_t entry_pulse_epoch = cooperative_pulse_epoch();
    EnterCooperativeWait(self);  // FIFO fairness for semaphores
    if (self) {
      const uint32_t wait_handle_id = handle();
      self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kSingle,
                                       &wait_handle_id, 1);
    }
    X_STATUS status = CooperativeWait(
        scheduler, kthread, this, alertable != 0, deadline_ms,
        [&]() -> std::optional<X_STATUS> {
          // Released by a pulse that already reset the host primitive.
          if (cooperative_pulse_epoch() != entry_pulse_epoch) {
            if (self) {
              self->BoostOnWake(priority_increment());
            }
            WaitCallback();
            return AcquireStatus();
          }
          // Only the front-of-queue fiber may take a permit (no-op for events).
          if (!CooperativeMayAcquire(self)) {
            return std::nullopt;
          }
          auto poll = xe::threading::Wait(wait_handle, alertable ? true : false,
                                          std::chrono::milliseconds(0));
          switch (poll) {
            case xe::threading::WaitResult::kSuccess: {
              if (self) {
                self->BoostOnWake(priority_increment());
              }
              WaitCallback();
              return AcquireStatus();
            }
            case xe::threading::WaitResult::kUserCallback:
              return X_STATUS_USER_APC;
            case xe::threading::WaitResult::kTimeout:
              return std::nullopt;  // not signaled yet
            default:
            case xe::threading::WaitResult::kAbandoned:
            case xe::threading::WaitResult::kFailed:
              return X_STATUS_ABANDONED_WAIT_0;
          }
        });
    LeaveCooperativeWait(self);
    return status;
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
  xe::threading::WaitResult result;
  if (timeout_ms == std::chrono::milliseconds::max()) {
    // Infinite host wait, e.g. guest code running on the kernel dispatch
    // thread. Tripwire in slices so a deadlock names itself in the log.
    int waited_s = 0;
    while ((result = xe::threading::Wait(wait_handle, alertable ? true : false,
                                         std::chrono::seconds(30))) ==
           xe::threading::WaitResult::kTimeout) {
      waited_s += 30;
      XELOGW(
          "XObject::Wait: host thread has waited {}s on a {} (tid={:08X})",
          waited_s, static_cast<uint32_t>(type()),
          XThread::IsInThread() ? XThread::GetCurrentThread()->thread_id() : 0);
    }
  } else {
    result =
        xe::threading::Wait(wait_handle, alertable ? true : false, timeout_ms);
  }

  switch (result) {
    case xe::threading::WaitResult::kSuccess:
    case xe::threading::WaitResult::kUserCallback: {
      if (XThread::IsInThread()) {
        XThread::GetCurrentThread()->BoostOnWake(priority_increment());
      }
      if (result == xe::threading::WaitResult::kSuccess) {
        WaitCallback();
        X_STATUS status = AcquireStatus();
        WaitExit(kthread, status);
        return status;
      }
      WaitExit(kthread, X_STATUS_USER_APC);
      return X_STATUS_USER_APC;
    }
    case xe::threading::WaitResult::kTimeout:
      xe::threading::MaybeYield();
      WaitExit(kthread, X_STATUS_TIMEOUT);
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      WaitExit(kthread, X_STATUS_ABANDONED_WAIT_0);
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::SignalAndWait(XObject* signal_object, XObject* wait_object,
                                uint32_t wait_reason, uint32_t processor_mode,
                                uint32_t alertable, uint64_t* opt_timeout) {
  if (GuestScheduler::enabled() && XThread::GetCurrentFiberThread()) {
    auto* scheduler = wait_object->kernel_state()->guest_scheduler();
    auto* self = XThread::GetCurrentThread();
    X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
    uint64_t deadline_ms = opt_timeout ? Clock::QueryHostUptimeMillis() +
                                             Clock::ScaleGuestDurationMillis(
                                                 TimeoutTicksToMs(*opt_timeout))
                                       : 0;
    // Queued before the signal so a wake cannot land before we are a waiter.
    const uint32_t entry_pulse_epoch = wait_object->cooperative_pulse_epoch();
    wait_object->EnterCooperativeWait(self);  // FIFO fairness for semaphores
    if (self) {
      const uint32_t wait_handle_id = wait_object->handle();
      self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kSingle,
                                       &wait_handle_id, 1);
    }
    X_STATUS signal_status = SignalObjectCooperatively(signal_object);
    if (XFAILED(signal_status)) {
      wait_object->LeaveCooperativeWait(self);
      WaitExit(kthread, signal_status);
      return signal_status;
    }
    X_STATUS status = CooperativeWait(
        scheduler, kthread, wait_object, alertable != 0, deadline_ms,
        [&]() -> std::optional<X_STATUS> {
          // Released by a pulse that already reset the host primitive.
          if (wait_object->cooperative_pulse_epoch() != entry_pulse_epoch) {
            if (self) {
              self->BoostOnWake(wait_object->priority_increment());
            }
            wait_object->WaitCallback();
            return wait_object->AcquireStatus();
          }
          // Only the front-of-queue fiber may take a permit (no-op for events).
          if (!wait_object->CooperativeMayAcquire(self)) {
            return std::nullopt;
          }
          auto poll = xe::threading::Wait(
              wait_object->GetWaitHandleForCurrentThread(0),
              alertable ? true : false, std::chrono::milliseconds(0));
          switch (poll) {
            case xe::threading::WaitResult::kSuccess: {
              if (self) {
                self->BoostOnWake(wait_object->priority_increment());
              }
              wait_object->WaitCallback();
              return wait_object->AcquireStatus();
            }
            case xe::threading::WaitResult::kUserCallback:
              return X_STATUS_USER_APC;
            case xe::threading::WaitResult::kTimeout:
              return std::nullopt;
            default:
            case xe::threading::WaitResult::kAbandoned:
            case xe::threading::WaitResult::kFailed:
              return X_STATUS_ABANDONED_WAIT_0;
          }
        });
    wait_object->LeaveCooperativeWait(self);
    return status;
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
  auto result = xe::threading::SignalAndWait(
      signal_object->GetWaitHandle(),
      wait_object->GetWaitHandleForCurrentThread(0), alertable ? true : false,
      timeout_ms);

  switch (result) {
    case xe::threading::WaitResult::kSuccess:
    case xe::threading::WaitResult::kUserCallback: {
      if (XThread::IsInThread()) {
        XThread::GetCurrentThread()->BoostOnWake(
            wait_object->priority_increment());
      }
      if (result == xe::threading::WaitResult::kSuccess) {
        wait_object->WaitCallback();
        X_STATUS status = wait_object->AcquireStatus();
        WaitExit(kthread, status);
        return status;
      }
      WaitExit(kthread, X_STATUS_USER_APC);
      return X_STATUS_USER_APC;
    }
    case xe::threading::WaitResult::kTimeout:
      xe::threading::MaybeYield();
      WaitExit(kthread, X_STATUS_TIMEOUT);
      return X_STATUS_TIMEOUT;
    default:
    case xe::threading::WaitResult::kAbandoned:
    case xe::threading::WaitResult::kFailed:
      WaitExit(kthread, X_STATUS_ABANDONED_WAIT_0);
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

X_STATUS XObject::WaitMultiple(uint32_t count, XObject** objects,
                               uint32_t wait_type, uint32_t wait_reason,
                               uint32_t processor_mode, uint32_t alertable,
                               uint64_t* opt_timeout) {
  static constexpr uint32_t kMaxWaitHandles = 64;
  xe::threading::WaitHandle* wait_handles[kMaxWaitHandles];
  // Both the handle array and the stand-in pool are sized for this.
  if (count > kMaxWaitHandles) {
    return X_STATUS_INVALID_PARAMETER;
  }

  // Resolved per caller, so a mutant this thread already owns cannot deadlock
  // a WaitAll on itself.
  auto resolve_handles = [&]() {
    for (size_t i = 0; i < count; ++i) {
      wait_handles[i] = objects[i]->GetWaitHandleForCurrentThread(i);
      assert_not_null(wait_handles[i]);
    }
  };
  resolve_handles();

  if (GuestScheduler::enabled() && count > 0 &&
      XThread::GetCurrentFiberThread()) {
    // Cooperative path: poll (WaitAny/WaitAll at zero timeout, preserving the
    // host primitives' atomic acquire) and yield between polls. WaitMultiple is
    // static, so reach the scheduler through an object.
    auto* scheduler = objects[0]->kernel_state()->guest_scheduler();
    X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
    uint64_t deadline_ms = opt_timeout ? Clock::QueryHostUptimeMillis() +
                                             Clock::ScaleGuestDurationMillis(
                                                 TimeoutTicksToMs(*opt_timeout))
                                       : 0;
    uint32_t entry_pulse_epochs[kMaxWaitHandles];
    uint32_t handle_ids[kMaxWaitHandles];
    for (size_t i = 0; i < count; ++i) {
      entry_pulse_epochs[i] = objects[i]->cooperative_pulse_epoch();
      handle_ids[i] = objects[i]->handle();
    }
    // A multi-wait registers no single wait object, so this is the only
    // record of what it blocks on. It also gates re-polling: only
    // Event/Semaphore/Mutant bump the signal epoch, so a set holding any other
    // type must stay ungated or its transitions wait for the backstop.
    bool gateable_set = count <= 8;
    for (size_t i = 0; gateable_set && i < count; ++i) {
      switch (objects[i]->type()) {
        case XObject::Type::Event:
        case XObject::Type::Semaphore:
        case XObject::Type::Mutant:
          break;
        default:
          gateable_set = false;
          break;
      }
    }
    if (auto* self = XThread::GetCurrentThread()) {
      self->set_cooperative_wait_shape(
          wait_type ? XThread::CooperativeWaitKind::kMultiAny
                    : XThread::CooperativeWaitKind::kMultiAll,
          handle_ids, count, gateable_set ? objects : nullptr);
    }
    return CooperativeWait(
        scheduler, kthread, nullptr, alertable != 0, deadline_ms,
        [&]() -> std::optional<X_STATUS> {
          resolve_handles();
          if (wait_type) {
            // WaitAny only: WaitAll needs every object signaled at once, which
            // a pulse this waiter already missed cannot give it.
            for (uint32_t i = 0; i < count; ++i) {
              if (objects[i]->cooperative_pulse_epoch() !=
                  entry_pulse_epochs[i]) {
                objects[i]->WaitCallback();
                if (auto* current = XThread::GetCurrentThread()) {
                  current->BoostOnWake(objects[i]->priority_increment());
                }
                return objects[i]->AcquireStatus() == X_STATUS_SUCCESS
                           ? X_STATUS(i)
                           : X_STATUS(X_STATUS_ABANDONED_WAIT_0 + i);
              }
            }
            auto r = xe::threading::WaitAny(wait_handles, count,
                                            alertable ? true : false,
                                            std::chrono::milliseconds(0));
            switch (r.first) {
              case xe::threading::WaitResult::kSuccess: {
                objects[r.second]->WaitCallback();
                auto* current = XThread::GetCurrentThread();
                if (current) {
                  current->BoostOnWake(objects[r.second]->priority_increment());
                }
                X_STATUS status = objects[r.second]->AcquireStatus();
                return status == X_STATUS_SUCCESS
                           ? X_STATUS(r.second)
                           : X_STATUS(X_STATUS_ABANDONED_WAIT_0 + r.second);
              }
              case xe::threading::WaitResult::kUserCallback:
                return X_STATUS_USER_APC;
              case xe::threading::WaitResult::kTimeout:
                return std::nullopt;
              case xe::threading::WaitResult::kAbandoned:
                return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + r.second);
              default:
              case xe::threading::WaitResult::kFailed:
                return X_STATUS_UNSUCCESSFUL;
            }
          }
          auto r = xe::threading::WaitAll(wait_handles, count,
                                          alertable ? true : false,
                                          std::chrono::milliseconds(0));
          switch (r) {
            case xe::threading::WaitResult::kSuccess: {
              uint32_t boost_increment = 0;
              X_STATUS status = X_STATUS_SUCCESS;
              for (uint32_t i = 0; i < count; i++) {
                objects[i]->WaitCallback();
                if (objects[i]->AcquireStatus() != X_STATUS_SUCCESS) {
                  status = X_STATUS_ABANDONED_WAIT_0;
                }
                if (objects[i]->priority_increment() > boost_increment) {
                  boost_increment = objects[i]->priority_increment();
                }
              }
              auto* current = XThread::GetCurrentThread();
              if (current) {
                current->BoostOnWake(boost_increment);
              }
              return status;
            }
            case xe::threading::WaitResult::kUserCallback:
              return X_STATUS_USER_APC;
            case xe::threading::WaitResult::kTimeout:
              return std::nullopt;
            default:
            case xe::threading::WaitResult::kAbandoned:
            case xe::threading::WaitResult::kFailed:
              return X_STATUS_ABANDONED_WAIT_0;
          }
        });
  }

  auto timeout_ms =
      opt_timeout ? std::chrono::milliseconds(Clock::ScaleGuestDurationMillis(
                        TimeoutTicksToMs(*opt_timeout)))
                  : std::chrono::milliseconds::max();

  X_KTHREAD* kthread = WaitEnter(wait_reason, processor_mode, alertable);
  X_STATUS status;
  uint32_t boost_increment = 0;
  if (wait_type) {
    auto result = xe::threading::WaitAny(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result.first) {
      case xe::threading::WaitResult::kSuccess:
        objects[result.second]->WaitCallback();
        boost_increment = objects[result.second]->priority_increment();
        status = objects[result.second]->AcquireStatus() == X_STATUS_SUCCESS
                     ? X_STATUS(result.second)
                     : X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
        break;
      case xe::threading::WaitResult::kUserCallback:
        status = X_STATUS_USER_APC;
        break;
      case xe::threading::WaitResult::kTimeout:
        xe::threading::MaybeYield();
        status = X_STATUS_TIMEOUT;
        break;
      case xe::threading::WaitResult::kAbandoned:
        status = X_STATUS(X_STATUS_ABANDONED_WAIT_0 + result.second);
        break;
      default:
      case xe::threading::WaitResult::kFailed:
        status = X_STATUS_UNSUCCESSFUL;
        break;
    }
  } else {
    auto result = xe::threading::WaitAll(wait_handles, count,
                                         alertable ? true : false, timeout_ms);
    switch (result) {
      case xe::threading::WaitResult::kSuccess:
        status = X_STATUS_SUCCESS;
        for (uint32_t i = 0; i < count; i++) {
          objects[i]->WaitCallback();
          if (objects[i]->AcquireStatus() != X_STATUS_SUCCESS) {
            status = X_STATUS_ABANDONED_WAIT_0;
          }
          // Use the largest increment among the signaled objects.
          if (objects[i]->priority_increment() > boost_increment) {
            boost_increment = objects[i]->priority_increment();
          }
        }
        break;
      case xe::threading::WaitResult::kUserCallback:
        status = X_STATUS_USER_APC;
        break;
      case xe::threading::WaitResult::kTimeout:
        xe::threading::MaybeYield();
        status = X_STATUS_TIMEOUT;
        break;
      default:
      case xe::threading::WaitResult::kAbandoned:
      case xe::threading::WaitResult::kFailed:
        status = X_STATUS_ABANDONED_WAIT_0;
        break;
    }
  }

  // Apply priority boost if the thread actually blocked (not on
  // timeout/failure).
  if (status != X_STATUS_TIMEOUT && status != X_STATUS_UNSUCCESSFUL &&
      status != X_STATUS_ABANDONED_WAIT_0) {
    if (XThread::IsInThread()) {
      XThread::GetCurrentThread()->BoostOnWake(boost_increment);
    }
  }
  WaitExit(kthread, status);
  return status;
}

uint8_t* XObject::CreateNative(uint32_t size) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  static_assert((kGuestObjectPrePad + sizeof(X_OBJECT_HEADER)) % 32 == 0);

  uint32_t total_size = kGuestObjectPrePad + sizeof(X_OBJECT_HEADER) + size;

  auto mem = memory()->SystemHeapAlloc(total_size);
  if (!mem) {
    // Out of memory!
    return nullptr;
  }

  allocated_guest_object_ = true;
  memory()->Zero(mem, total_size);
  uint32_t header_addr = mem + kGuestObjectPrePad;
  SetNativePointer(header_addr + sizeof(X_OBJECT_HEADER), true);

  auto header = memory()->TranslateVirtual<X_OBJECT_HEADER*>(header_addr);

  auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));
  if (object_type) {
    // Set it up in the header.
    // Some kernel method is accessing this struct and dereferencing a member
    // @ offset 0x14
    header->object_type_ptr = object_type;
  }

  return memory()->TranslateVirtual(guest_object_ptr_);
}

void XObject::SetNativePointer(uint32_t native_ptr, bool uninitialized) {
  auto global_lock = xe::global_critical_region::AcquireDirect();

  // If hit: We've already setup the native ptr with CreateNative!
  assert_zero(guest_object_ptr_);

  auto header =
      kernel_state_->memory()->TranslateVirtual<X_DISPATCH_HEADER*>(native_ptr);

  // Memory uninitialized, so don't bother with the check.
  if (!uninitialized) {
    assert_true(!(header->wait_list.blink_ptr & 0x1));
  }

  // Stash pointer in struct.
  // FIXME: This assumes the object has a dispatch header (some don't!)
  StashHandle(header, handle());

  guest_object_ptr_ = native_ptr;
}

object_ref<XObject> XObject::GetNativeObject(KernelState* kernel_state,
                                             void* native_ptr,
                                             X_OBJECT_TYPES as_type,
                                             bool already_locked) {
  assert_not_null(native_ptr);

  // Unfortunately the XDK seems to inline some KeInitialize calls, meaning
  // we never see it and just randomly start getting passed events/timers/etc.
  // Luckily it seems like all other calls (Set/Reset/Wait/etc) are used and
  // we don't have to worry about PPC code poking the struct. Because of that,
  // we init on first use, store our handle in the struct, and dereference it
  // each time.
  // We identify this by setting wait_list.flink_ptr to a magic value. When set,
  // wait_list.blink_ptr will hold a handle to our object.
  if (!already_locked) {
    global_critical_region::mutex().lock();
  }

  XObject* result = nullptr;

  auto header = reinterpret_cast<X_DISPATCH_HEADER*>(native_ptr);
  X_OBJECT_TYPES type = as_type;

  if (as_type == X_OBJECT_TYPES::UndefinedObject) {
    type = header->type;
  }

  if (header->wait_list.flink_ptr == kXObjSignature) {
    // Already initialized.
    // TODO: assert if the type of the object != as_type
    uint32_t handle = header->wait_list.blink_ptr;
    result = kernel_state->object_table()
                 ->LookupObject<XObject>(handle, true)
                 .release();
    // Handles are recycled across types, so a dead object's header can name a
    // handle that now belongs to something else.
    uint32_t guest_ptr = kernel_state->memory()->HostToGuestVirtual(native_ptr);
    if (result &&
        static_cast<const XObject*>(result)->guest_object() != guest_ptr) {
      result->Release();
      result = nullptr;
    }
  } else {
    // First use, create new.
    // https://www.nirsoft.net/kernel_struct/vista/KOBJECTS.html
    switch (type) {
      case X_OBJECT_TYPES::EventNotificationObject:
      case X_OBJECT_TYPES::EventSynchronizationObject: {
        auto ev = new XEvent(kernel_state);
        ev->InitializeNative(native_ptr, header);
        result = ev;
      } break;
      case X_OBJECT_TYPES::MutantObject: {
        auto mutant = new XMutant(kernel_state);
        mutant->InitializeNative(native_ptr, header);
        result = mutant;
      } break;
      case X_OBJECT_TYPES::SemaphoreObject: {
        auto sem = new XSemaphore(kernel_state);
        auto success = sem->InitializeNative(native_ptr, header);
        // Can't report failure to the guest at late initialization:
        assert_true(success);
        result = sem;
      } break;
      case X_OBJECT_TYPES::ProcessObject:
      case X_OBJECT_TYPES::QueueObject:
      case X_OBJECT_TYPES::ThreadObject:
      case X_OBJECT_TYPES::Spare1Object:
      case X_OBJECT_TYPES::TimerNotificationObject:
      case X_OBJECT_TYPES::TimerSynchronizationObject:
      case X_OBJECT_TYPES::ApcObject:
      case X_OBJECT_TYPES::DpcObject:
      case X_OBJECT_TYPES::DeviceQueueObject:
      case X_OBJECT_TYPES::EventPairObject:
      case X_OBJECT_TYPES::InterruptObject:
      case X_OBJECT_TYPES::ProfileObject:
      default:
        // Unimplemented object type - just log and return nullptr
        XELOGW("GetNativeObject: Unimplemented object type {}",
               static_cast<uint8_t>(type));
        result = nullptr;
    }
    // InitializeNative paths call SetNativePointer, which stashes the handle.
    // New object types (when implemented) must do the same.
  }

  if (!already_locked) {
    global_critical_region::mutex().unlock();
  }
  return object_ref<XObject>(result);
}

}  // namespace kernel
}  // namespace xe
