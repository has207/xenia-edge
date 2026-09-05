/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSEMAPHORE_H_
#define XENIA_KERNEL_XSEMAPHORE_H_

#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

class XSemaphore : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Semaphore;

  explicit XSemaphore(KernelState* kernel_state, bool host_object = false);
  ~XSemaphore() override;

  [[nodiscard]] bool Initialize(int32_t initial_count, int32_t maximum_count);
  [[nodiscard]] bool InitializeNative(void* native_ptr,
                                      const X_DISPATCH_HEADER* header);

  [[nodiscard]] bool ReleaseSemaphore(int32_t release_count,
                                      int32_t* out_previous_count);

  bool Save(ByteStream* stream) override;
  static object_ref<XSemaphore> Restore(KernelState* kernel_state,
                                        ByteStream* stream);

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override {
    return semaphore_.get();
  }
  void WaitCallback() override;
  void SyncFromGuest() override;

  void CooperativeWaitBegin(XThread* thread) override;
  void CooperativeWaitEnd(XThread* thread) override;
  bool CooperativeMayAcquire(XThread* thread) override;

 private:
  std::unique_ptr<xe::threading::Semaphore> semaphore_;
  // Guards the count, the guest header it mirrors into and the host semaphore
  // together, so a reconcile never samples a kernel write half done.
  std::mutex count_lock_;
  // Last count this kernel wrote, so SyncFromGuest can tell a guest write from
  // one of ours. Guarded by count_lock_.
  int32_t host_count_ = 0;
  // Fibers waiting cooperatively, in order, for fair permit handout.
  CooperativeWaiterFifo waiters_;
  uint32_t maximum_count_ = 0;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSEMAPHORE_H_
