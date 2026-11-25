/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

#include "third_party/fmt/include/fmt/format.h"

namespace xe {
namespace kernel {
namespace xam {

// X_TASK_ARGS flags. The low bits are the task type.
constexpr uint32_t kTaskTypeMask = 0x1F;
// A task with a thread of its own.
constexpr uint32_t kTaskTypeDedicated = 2;
// Runs at the title's priorities rather than the system's.
constexpr uint32_t kTaskForeground = 0x00080000;
// Keeps kTaskForeground for a title without the ForegroundTasks privilege.
constexpr uint32_t kTaskForegroundUnchecked = 0x20000000;
// value2 is the CPU a dedicated task runs on.
constexpr uint32_t kTaskCpuInValue2 = 0x80000000;
// Where XAM puts a dedicated task otherwise.
constexpr uint32_t kTaskDedicatedCpu = 5;

static bool KeepsForeground(uint32_t flags) {
  if (!(flags & kTaskForeground)) {
    return false;
  }
  if (flags & kTaskForegroundUnchecked) {
    return true;
  }
  auto module = kernel_state()->GetExecutableModule();
  uint32_t system_flags = 0;
  return module &&
         XSUCCEEDED(module->GetOptHeader<uint32_t>(XEX_HEADER_SYSTEM_FLAGS,
                                                   &system_flags)) &&
         (system_flags & XEX_SYSTEM_FOREGROUND_TASKS);
}

dword_result_t XamTaskSchedule_entry(lpvoid_t callback, lpvoid_t message,
                                     pointer_t<X_TASK_ARGS> optional_ptr,
                                     lpdword_t handle_ptr,
                                     const ppc_context_t& ctx) {
  // TODO(gibbed): figure out what this is for
  *handle_ptr = 12345;

  uint32_t flags = 0;
  uint32_t value2 = 0;
  if (optional_ptr) {
    flags = optional_ptr->flags;
    value2 = optional_ptr->value2;
    XELOGI("Got xam task args: v1 = {:08X}, v2 = {:08X}", flags, value2);
  }
  const bool dedicated =
      optional_ptr && (flags & kTaskTypeMask) == kTaskTypeDedicated;

  uint32_t stack_size = kernel_state()->GetExecutableModule()->stack_size();

  // Stack must be aligned to 16kb pages
  stack_size = std::max((uint32_t)0x4000, ((stack_size + 0xFFF) & 0xFFFFF000));

  // XAM creates a dedicated task's thread suspended, sets its CPU and
  // priorities, then resumes it.
  uint32_t creation_flags = 0;
  if (dedicated) {
    uint32_t cpu = kTaskDedicatedCpu;
    // An out-of-range CPU keeps the default.
    if ((flags & kTaskCpuInValue2) && value2 < 6) {
      cpu = value2;
    }
    creation_flags = X_CREATE_SUSPENDED | (uint32_t(1) << (24 + cpu));
  }

  // TODO: A type 8 task goes on a queue XAM's worker pool (CPUs 3 and 4)
  // takes tasks from, rather than getting a thread of its own.
  auto thread = object_ref<XThread>(new XThread(
      kernel_state(), stack_size, 0, callback, message.guest_address(),
      creation_flags, true, false, kernel_state()->GetSystemProcess()));

  X_STATUS result = thread->Create();

  if (XFAILED(result)) {
    // Failed!
    XELOGE("XAM task creation failed: {:08X}", result);
    return result;
  }

  if (dedicated) {
    if (KeepsForeground(flags)) {
      // XAM's KeSetPriorityClassThread(1) gives the thread the title's
      // priority class, and its KeSetBasePriorityThread(0) the title's base.
      auto* process = kernel_state()->memory()->TranslateVirtual<X_KPROCESS*>(
          kernel_state()->GetTitleProcess());
      auto* kthread = thread->guest_object<X_KTHREAD>();
      kthread->process_priority_class = process->process_priority_class;
      kthread->base_priority_copy = process->default_thread_priority;
      kthread->max_dynamic_priority = process->max_dynamic_priority;
      thread->SetBasePriority(0);
    }
    thread->Resume();
  }

  XELOGD("XAM task ({:08X}) scheduled asynchronously",
         callback.guest_address());

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamTaskSchedule, kNone, kImplemented, kSketchy);

dword_result_t XamTaskCloseHandle_entry(dword_t obj_handle) {
  const X_STATUS error_code = xboxkrnl::NtClose(obj_handle);

  if (XFAILED(error_code)) {
    const uint32_t result = xboxkrnl::xeRtlNtStatusToDosError(error_code);
    XThread::SetLastError(result);
    return false;
  }

  return true;
}
DECLARE_XAM_EXPORT1(XamTaskCloseHandle, kNone, kImplemented);

dword_result_t XamTaskShouldExit_entry() { return 0; }
DECLARE_XAM_EXPORT1(XamTaskShouldExit, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Task);
