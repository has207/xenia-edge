/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XMUTANT_H_
#define XENIA_KERNEL_XMUTANT_H_

#include <atomic>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
class XThread;

class XMutant : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Mutant;

  explicit XMutant(KernelState* kernel_state, bool host_object = false);
  ~XMutant() override;

  void Initialize(bool initial_owner);
  void InitializeNative(void* native_ptr, const X_DISPATCH_HEADER* header);

  X_STATUS ReleaseMutant(uint32_t priority_increment, bool abandon, bool wait);

  bool Save(ByteStream* stream) override;
  static object_ref<XMutant> Restore(KernelState* kernel_state,
                                     ByteStream* stream);

  // Mark every mutant in |thread|'s mutants_list abandoned and unlink it.
  // Called from XThread::Exit/Terminate.
  static void AbandonAllOwnedByThread(KernelState* kernel_state,
                                      XThread* thread);

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override {
    return free_signal_.get();
  }
  void WaitCallback() override;
  void SyncFromGuest() override;
  bool IsReenteredByCurrentThread() override;
  X_STATUS AcquireStatus() override;

  void CooperativeWaitBegin(XThread* thread) override;
  void CooperativeWaitEnd(XThread* thread) override;
  bool CooperativeMayAcquire(XThread* thread) override;

 private:
  XMutant();

  // Signaled while unowned. A count rather than a host mutant, whose owner is
  // the host thread, which many guest threads share and can migrate between.
  std::unique_ptr<xe::threading::Semaphore> free_signal_;
  // The only source of truth for ownership.
  std::atomic<XThread*> owning_thread_{nullptr};
  // Recursive acquires never touch free_signal_, so count them here. Only the
  // current owner mutates it, so no synchronization.
  uint32_t recursion_count_ = 0;
  // Parked fibers waiting to acquire, in order. Without this a running fiber
  // that re-acquires in a loop starves a parked waiter forever, where NT hands
  // a released mutant to the waiter.
  CooperativeWaiterFifo waiters_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XMUTANT_H_
