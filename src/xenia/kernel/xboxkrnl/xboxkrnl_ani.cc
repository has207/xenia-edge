/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xmodule.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

constexpr uint16_t kPlayBootAnimationOrdinal = 1;
constexpr uint16_t kEndBootAnimationOrdinal = 2;
constexpr uint16_t kSetLogoOrdinal = 3;

static uint32_t GetBootAnimationExport(uint32_t hmodule, uint16_t ordinal) {
  auto module = XModule::GetFromHModule(
      kernel_state(), kernel_memory()->TranslateVirtual(hmodule));
  return module ? module->GetProcAddressByOrdinal(ordinal) : 0;
}

static void CallBootAnimationExport(uint32_t address, uint64_t* args,
                                    size_t arg_count) {
  kernel_state()->processor()->Execute(
      XThread::GetCurrentThread()->thread_state(), address, args, arg_count);
}

object_ref<XThread> XboxkrnlModule::StartBootAnimation(
    const object_ref<UserModule>& module, bool main_thread) {
  const uint32_t play =
      module->GetProcAddressByOrdinal(kPlayBootAnimationOrdinal);
  if (!play) {
    XELOGE("{} has no boot animation export", module->name());
    return nullptr;
  }
  const uint32_t hmodule = module->hmodule_ptr();

  // The first export takes the module handle.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state_, 0, 0, play, hmodule,
                  ThreadInitiallySuspended | PriorityClass1 | ReturnKThreadPtr |
                      AffinityCpu3,
                  true, main_thread));
  thread->set_name("Boot Animation");
  const X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create the boot animation thread: {:08X}", result);
    return nullptr;
  }
  thread->SetPriority(17);

  ani_ended_event_ = object_ref<XEvent>(new XEvent(kernel_state_, true));
  ani_ended_event_->Initialize(true, false);
  ani_thread_ = thread;
  ani_hmodule_ = hmodule;
  ani_started_ = true;
  return thread;
}

void XboxkrnlModule::EndBootAnimation(uint32_t terminate) {
  const uint32_t hmodule = ani_hmodule_.exchange(0);
  if (!hmodule) {
    // Another caller took the module and ends the animation.
    if (ani_started_) {
      xeKeWaitForSingleObject(ani_ended_event_->guest_object<X_KEVENT>(), 3, 1,
                              0, nullptr);
    }
    return;
  }

  if (const uint32_t end =
          GetBootAnimationExport(hmodule, kEndBootAnimationOrdinal)) {
    uint64_t args[] = {terminate};
    CallBootAnimationExport(end, args, xe::countof(args));
  }
  if (ani_thread_) {
    xeKeWaitForSingleObject(ani_thread_->guest_object<X_KTHREAD>(), 3, 1, 0,
                            nullptr);
    // A thread created for its pointer is dereferenced once it ends.
    ani_thread_->ReleaseHandle();
    ani_thread_.reset();
  }
  // Whoever loaded the module unloads it.
  ani_ended_event_->Set(1, false);
}

void XboxkrnlModule::SetBootAnimationLogo(uint32_t unk0, uint32_t unk1) {
  const uint32_t hmodule = ani_hmodule_;
  if (!hmodule) {
    return;
  }
  if (const uint32_t set_logo =
          GetBootAnimationExport(hmodule, kSetLogoOrdinal)) {
    uint64_t args[] = {unk0, unk1};
    CallBootAnimationExport(set_logo, args, xe::countof(args));
  }
}

void AniBlockOnAnimation_entry() { GetXboxkrnlModule()->EndBootAnimation(0); }
DECLARE_XBOXKRNL_EXPORT1(AniBlockOnAnimation, kVideo, kImplemented);

void AniTerminateAnimation_entry() { GetXboxkrnlModule()->EndBootAnimation(1); }
DECLARE_XBOXKRNL_EXPORT1(AniTerminateAnimation, kVideo, kImplemented);

void AniSetLogo_entry(dword_t unk0, dword_t unk1) {
  GetXboxkrnlModule()->SetBootAnimationLogo(unk0, unk1);
}
DECLARE_XBOXKRNL_EXPORT1(AniSetLogo, kVideo, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Ani);
