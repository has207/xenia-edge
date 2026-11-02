/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULE_H_
#define XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULE_H_

#include <atomic>
#include <optional>

#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ordinals.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"

// All of the exported functions:
#include "xenia/kernel/xboxkrnl/xboxkrnl_rtl.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

class XboxkrnlModule : public KernelModule {
 public:
  static constexpr size_t kExLoadedImageNameSize = 255 + 1;
  static constexpr size_t kExLoadedCommandLineSize =
      1024;  // Based on max size provided for XamLoaderGetLaunchData

  XboxkrnlModule(Emulator* emulator, KernelState* kernel_state);
  virtual ~XboxkrnlModule();

  static void RegisterExportTable(xe::cpu::ExportResolver* export_resolver);

  bool SendPIXCommand(const char* cmd);

  void set_pix_function(uint32_t addr) { pix_function_ = addr; }

  // What XAudioSetSpeakerConfig set during this run, if it was called.
  std::optional<uint32_t> speaker_config() const {
    if (!speaker_config_set_) {
      return std::nullopt;
    }
    return speaker_config_.load();
  }
  void set_speaker_config(uint32_t config) {
    speaker_config_ = config;
    speaker_config_set_ = true;
  }

  // Runs the boot animation |module| from its first export, as the title's
  // |main_thread| when it was launched itself. Returns its thread suspended,
  // or nullptr when it can't start.
  object_ref<XThread> StartBootAnimation(const object_ref<UserModule>& module,
                                         bool main_thread);
  // Ends the boot animation and waits for its thread to exit.
  void EndBootAnimation(uint32_t terminate);
  void SetBootAnimationLogo(uint32_t unk0, uint32_t unk1);

 protected:
  uint32_t pix_function_ = 0;
  std::atomic<uint32_t> speaker_config_ = 0;
  std::atomic<bool> speaker_config_set_ = false;

  std::atomic<uint32_t> ani_hmodule_ = 0;
  std::atomic<bool> ani_started_ = false;
  object_ref<XThread> ani_thread_;
  object_ref<XEvent> ani_ended_event_;
};

XboxkrnlModule* GetXboxkrnlModule();

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XBOXKRNL_XBOXKRNL_MODULE_H_
