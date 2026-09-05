/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xmutant.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

namespace xe {
namespace kernel {

namespace {
// Append |mutant| to the tail of |owner_thread|'s KTHREAD.mutants_list and
// point KMUTANT.owner at the thread.
void InsertMutantOwned(Memory* memory, XThread* owner_thread, XMutant* mutant) {
  uint32_t kthread_addr = owner_thread->guest_object();
  uint32_t kmutant_addr = mutant->guest_object();
  auto* kthread = memory->TranslateVirtual<X_KTHREAD*>(kthread_addr);
  auto* kmutant = memory->TranslateVirtual<X_KMUTANT*>(kmutant_addr);
  uint32_t head_addr = kthread_addr + offsetof(X_KTHREAD, mutants_list);
  uint32_t link_addr = kmutant_addr + offsetof(X_KMUTANT, unk_list);
  uint32_t old_tail = kthread->mutants_list.blink_ptr;
  kmutant->unk_list.flink_ptr = head_addr;
  kmutant->unk_list.blink_ptr = old_tail;
  memory->TranslateVirtual<X_LIST_ENTRY*>(old_tail)->flink_ptr = link_addr;
  kthread->mutants_list.blink_ptr = link_addr;
  kmutant->owner = kthread_addr;
}

// Unlink |mutant| from its owner thread's mutants_list and clear owner.
void RemoveMutantOwned(Memory* memory, XMutant* mutant) {
  uint32_t kmutant_addr = mutant->guest_object();
  auto* kmutant = memory->TranslateVirtual<X_KMUTANT*>(kmutant_addr);
  if (!kmutant->owner) {
    return;
  }
  uint32_t prev = kmutant->unk_list.blink_ptr;
  uint32_t next = kmutant->unk_list.flink_ptr;
  if (prev) {
    memory->TranslateVirtual<X_LIST_ENTRY*>(prev)->flink_ptr = next;
  }
  if (next) {
    memory->TranslateVirtual<X_LIST_ENTRY*>(next)->blink_ptr = prev;
  }
  kmutant->unk_list.flink_ptr = 0;
  kmutant->unk_list.blink_ptr = 0;
  kmutant->owner = 0u;
}
}  // namespace

XMutant::XMutant(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XMutant::XMutant() : XObject(kObjectType) {}

XMutant::~XMutant() = default;

void XMutant::Initialize(bool initial_owner) {
  assert_false(free_signal_);

  // Skip on the Restore path -- RestoreObject already populated guest_object_.
  if (guest_object() == 0) {
    CreateNative<X_KMUTANT>();
    auto* kmutant = memory()->TranslateVirtual<X_KMUTANT*>(guest_object());
    // Don't touch header.wait_list: SetNativePointer stashes the handle there.
    kmutant->header.type = X_OBJECT_TYPES::MutantObject;
    kmutant->header.signal_state = initial_owner ? 0 : 1;
  }

  free_signal_ = xe::threading::Semaphore::Create(initial_owner ? 0 : 1, 1);
  assert_not_null(free_signal_);

  if (initial_owner) {
    // Initial-owner acquire doesn't go through Wait, so link the chain here.
    XThread* self = XThread::GetCurrentThread();
    if (self) {
      owning_thread_ = self;
      recursion_count_ = 1;
      InsertMutantOwned(memory(), self, this);
    }
  }
}

void XMutant::InitializeNative(void* native_ptr,
                               const X_DISPATCH_HEADER* header) {
  assert_false(free_signal_);

  auto* kmutant = reinterpret_cast<X_KMUTANT*>(native_ptr);
  // signal_state: 1 free, 0 held once, negative for recursive holds.
  int32_t signal_state = kmutant->header.signal_state;
  bool initial_owner = signal_state <= 0;
  free_signal_ = xe::threading::Semaphore::Create(initial_owner ? 0 : 1, 1);
  assert_not_null(free_signal_);
  SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);

  if (initial_owner) {
    // Recover the owner XThread from the stashed handle in its KTHREAD
    // dispatch header (same trick as GetNativeObject); current thread is the
    // fallback if the stash isn't where we expect.
    recursion_count_ = static_cast<uint32_t>(1 - signal_state);
    XThread* owner = nullptr;
    if (!!kmutant->owner) {
      auto* owner_kthread =
          memory()->TranslateVirtual<X_KTHREAD*>(kmutant->owner.m_ptr);
      if (owner_kthread->header.wait_list.flink_ptr == kXObjSignature) {
        owner = kernel_state()
                    ->object_table()
                    ->LookupObject<XThread>(
                        owner_kthread->header.wait_list.blink_ptr)
                    .get();
      }
    }
    if (!owner) {
      owner = XThread::GetCurrentThread();
    }
    if (owner) {
      owning_thread_ = owner;
      InsertMutantOwned(memory(), owner, this);
    }
  }
}

X_STATUS XMutant::ReleaseMutant(uint32_t priority_increment, bool abandon,
                                bool wait) {
  set_priority_increment(priority_increment);
  // TODO(benvanik): abandoning.
  assert_false(abandon);

  // Guest ownership decides, not the host primitive, so a release from the
  // wrong guest thread is rejected even when both share a dispatch thread.
  XThread* self = XThread::IsInThread() ? XThread::GetCurrentThread() : nullptr;
  if (!self || owning_thread_.load() != self) {
    return X_STATUS_MUTANT_NOT_OWNED;
  }

  --recursion_count_;
  bool freed = recursion_count_ == 0;
  if (freed) {
    // Clear ownership first: a waiter acquiring on another host thread must not
    // be stranded, and SyncFromGuest must not read this release as a guest one.
    owning_thread_ = nullptr;
  }
  auto& signal_state = memory()
                           ->TranslateVirtual<X_KMUTANT*>(guest_object())
                           ->header.signal_state;
  signal_state = signal_state + 1;

  if (freed) {
    RemoveMutantOwned(memory(), this);
    free_signal_->Release(1, nullptr);
    WakeCooperativeWaiters();
  }
  return X_STATUS_SUCCESS;
}

bool XMutant::Save(ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }

  XThread* owner = owning_thread_.load();
  uint32_t owning_thread_handle = owner ? owner->handle() : 0;
  stream->Write<uint32_t>(owning_thread_handle);
  XELOGD("XMutant {:08X} (owner: {:08X})", handle(), owning_thread_handle);

  return true;
}

object_ref<XMutant> XMutant::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  auto mutant = new XMutant();
  mutant->kernel_state_ = kernel_state;

  if (!mutant->RestoreObject(stream)) {
    delete mutant;
    return nullptr;
  }

  mutant->Initialize(false);

  auto owning_thread_handle = stream->Read<uint32_t>();
  if (owning_thread_handle) {
    // Do NOT pre-set owning_thread_: the queued Wait at thread start needs to
    // hit the first-acquire branch in WaitCallback. Pre-setting it would land
    // in the recursive branch and skip the chain insert.
    auto owner_thread = kernel_state->object_table()->LookupObject<XThread>(
        owning_thread_handle);
    owner_thread->AcquireMutantOnStartup(retain_object(mutant));
  }

  return object_ref<XMutant>(mutant);
}

void XMutant::AbandonAllOwnedByThread(KernelState* ks, XThread* thread) {
  // Released here rather than left to the OS, since a fiber-backed thread has
  // no host-thread exit to abandon the primitive for it.
  auto* memory = ks->memory();
  uint32_t kthread_addr = thread->guest_object();
  if (!kthread_addr) {
    return;
  }
  auto* kthread = memory->TranslateVirtual<X_KTHREAD*>(kthread_addr);
  uint32_t head_addr = kthread_addr + offsetof(X_KTHREAD, mutants_list);
  uint32_t entry = kthread->mutants_list.flink_ptr;
  while (entry != 0 && entry != head_addr) {
    uint32_t kmutant_addr = entry - offsetof(X_KMUTANT, unk_list);
    auto* kmutant = memory->TranslateVirtual<X_KMUTANT*>(kmutant_addr);
    uint32_t next = kmutant->unk_list.flink_ptr;
    kmutant->abandoned = 1;
    kmutant->owner = 0u;
    // Back to free, however deep the abandoned holder's recursion was.
    kmutant->header.signal_state = 1;
    kmutant->unk_list.flink_ptr = 0;
    kmutant->unk_list.blink_ptr = 0;
    if (kmutant->header.wait_list.flink_ptr == kXObjSignature) {
      auto obj = ks->object_table()->LookupObject<XMutant>(
          kmutant->header.wait_list.blink_ptr);
      if (obj && obj->owning_thread_.exchange(nullptr)) {
        // Only the thread that held it releases, however deep its recursion.
        obj->recursion_count_ = 0;
        obj->free_signal_->Release(1, nullptr);
        obj->WakeCooperativeWaiters();
      }
    }
    entry = next;
  }
  kthread->mutants_list.flink_ptr = head_addr;
  kthread->mutants_list.blink_ptr = head_addr;
}

bool XMutant::IsReenteredByCurrentThread() {
  // GetCurrentThread asserts on the host threads that wait on a guest mutant
  // during teardown, so gate on IsInThread like the wait path does.
  if (!XThread::IsInThread()) {
    return false;
  }
  XThread* owner = owning_thread_.load();
  return owner && owner == XThread::GetCurrentThread();
}

X_STATUS XMutant::AcquireStatus() {
  auto* kmutant = memory()->TranslateVirtual<X_KMUTANT*>(guest_object());
  if (kmutant->abandoned) {
    kmutant->abandoned = 0;
    return X_STATUS_ABANDONED_WAIT_0;
  }
  return X_STATUS_SUCCESS;
}

void XMutant::WaitCallback() {
  XThread* self = XThread::GetCurrentThread();
  if (!self) {
    return;
  }
  // Header before ownership: SyncFromGuest samples ownership first, so an
  // acquire in flight never looks like a guest-side release. Only the acquirer
  // runs here, so the load/store pair needs no exchange.
  auto& signal_state = memory()
                           ->TranslateVirtual<X_KMUTANT*>(guest_object())
                           ->header.signal_state;
  signal_state = signal_state - 1;
  XThread* prev = owning_thread_.load();
  owning_thread_.store(self);
  if (prev != self) {
    recursion_count_ = 1;
    InsertMutantOwned(memory(), self, this);
  } else {
    ++recursion_count_;
  }
}

void XMutant::SyncFromGuest() {
  if (!guest_object()) {
    return;
  }
  // Only the guest-says-free direction is actionable: an acquire has to name an
  // owner thread and a header write names none. A title reaches here by
  // re-initializing a KMUTANT in place.
  XThread* owner = owning_thread_.load();
  if (!owner) {
    return;
  }
  auto* kmutant = memory()->TranslateVirtual<X_KMUTANT*>(guest_object());
  if (static_cast<int32_t>(kmutant->header.signal_state) < 1) {
    return;
  }
  // A release clears ownership before it bumps the header, so losing the swap
  // means this free came from the kernel rather than the guest.
  if (!owning_thread_.compare_exchange_strong(owner, nullptr)) {
    return;
  }
  recursion_count_ = 0;
  RemoveMutantOwned(memory(), this);
  free_signal_->Release(1, nullptr);
  WakeCooperativeWaiters();
}

void XMutant::CooperativeWaitBegin(XThread* thread) { waiters_.Add(thread); }

void XMutant::CooperativeWaitEnd(XThread* thread) {
  // Poke the new front so it re-polls now.
  if (waiters_.Remove(thread)) {
    WakeCooperativeWaiters();
  }
}

bool XMutant::CooperativeMayAcquire(XThread* thread) {
  // The owner bypasses the queue so a recursive acquire cannot self-deadlock
  // behind its own waiters.
  return owning_thread_.load() == thread || waiters_.MayAcquire(thread);
}

}  // namespace kernel
}  // namespace xe
