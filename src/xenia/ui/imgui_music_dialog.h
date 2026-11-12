/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_MUSIC_DIALOG_H_
#define XENIA_UI_IMGUI_MUSIC_DIALOG_H_

#include <functional>

#include "xenia/ui/imgui_gamepad_dialog.h"

namespace xe {
namespace apu {
class HostMediaSession;
}  // namespace apu
}  // namespace xe

namespace xe {
namespace ui {

// Select Music, picking which of the host's media apps plays the user's music.
class ImGuiMusicDialog : public ImGuiGamepadDialog {
 public:
  ImGuiMusicDialog(ImGuiDrawer* drawer, hid::InputSystem* input_system,
                   apu::HostMediaSession* host_media);

  void CloseDialog() { Close(); }

  void SetOnCloseCallback(std::function<void()> callback) {
    on_close_callback_ = std::move(callback);
  }

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  apu::HostMediaSession* host_media_;
  std::function<void()> on_close_callback_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_MUSIC_DIALOG_H_
