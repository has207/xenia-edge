/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_UI_H_
#define XENIA_KERNEL_XAM_XAM_UI_H_

#include <algorithm>
#include <string_view>

#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_gamepad_dialog.h"

namespace xe {
namespace hid {
class InputSystem;
}  // namespace hid
}  // namespace xe

namespace xe {
namespace kernel {
namespace xam {

// XAM dialog for a title. Holds the title's input while open and hides the
// buttons held when it closes until they're released.
class XamGamepadDialog : public xe::ui::ImGuiGamepadDialog {
 public:
  void set_close_callback(std::function<void()> close_callback) {
    close_callback_ = close_callback;
  }

 protected:
  XamGamepadDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                   xe::hid::InputSystem* input_system)
      : xe::ui::ImGuiGamepadDialog(imgui_drawer, input_system, true) {}

  virtual ~XamGamepadDialog() {};

  void OnClose() override {
    if (close_callback_) {
      close_callback_();
    }
  }

 private:
  std::function<void()> close_callback_ = nullptr;
};

class MessageBoxDialog : public XamGamepadDialog {
 public:
  MessageBoxDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                   xe::hid::InputSystem* input_system, const std::string& title,
                   const std::string& description,
                   std::vector<std::string> buttons, uint32_t default_button)
      : XamGamepadDialog(imgui_drawer, input_system),
        title_(title),
        description_(description),
        buttons_(std::move(buttons)),
        default_button_(default_button),
        chosen_button_(default_button) {
    if (!title_.size()) {
      title_ = "Message Box";
    }
  }

  uint32_t chosen_button() const { return chosen_button_; }
  // Closed with B or Escape, without choosing a button
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override;
  virtual ~MessageBoxDialog() {}

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string description_;
  std::vector<std::string> buttons_;
  uint32_t default_button_ = 0;
  uint32_t chosen_button_ = 0;
  bool cancelled_ = false;
};

class KeyboardInputDialog : public XamGamepadDialog {
 public:
  KeyboardInputDialog(xe::ui::ImGuiDrawer* imgui_drawer,
                      xe::hid::InputSystem* input_system, std::string& title,
                      std::string& description, std::string& default_text,
                      size_t max_length)
      : XamGamepadDialog(imgui_drawer, input_system),
        title_(title),
        description_(description),
        default_text_(default_text),
        max_chars_(max_length ? max_length - 1 : 0),
        text_buffer_() {
    if (!title_.size()) {
      if (!description_.size()) {
        title_ = "Keyboard Input";
      } else {
        title_ = description_;
        description_ = "";
      }
    }
    text_ = default_text;
    // A UTF-16 code unit takes at most 3 bytes of UTF-8.
    text_buffer_.resize(max_chars_ * 3 + 1);
    CopyText(default_text_);
  }
  virtual ~KeyboardInputDialog() {}

  const std::string& text() const { return text_; }
  bool cancelled() const { return cancelled_; }

  void OnDraw(ImGuiIO& io) override;

 private:
  // Copies the whole characters of |text| that fit in the title's buffer.
  void CopyText(std::string_view text);

  bool has_opened_ = false;
  bool focus_set_ = false;
  std::string title_;
  std::string description_;
  std::string default_text_;
  // UTF-16 code units the title's buffer holds, without the terminator.
  size_t max_chars_ = 0;
  std::vector<char> text_buffer_;
  std::string text_ = "";
  bool cancelled_ = true;
};

bool xeDrawProfileContent(xe::ui::ImGuiDrawer* imgui_drawer,
                          const uint64_t xuid, const uint8_t user_index,
                          const X_XAMACCOUNTINFO* account,
                          const xe::ui::ImmediateTexture* profile_icon,
                          std::function<bool()> context_menu,
                          std::function<void()> on_profile_change,
                          uint64_t* selected_xuid);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif
