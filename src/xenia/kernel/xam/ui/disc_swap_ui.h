/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_UI_DISC_SWAP_UI_H_
#define XENIA_KERNEL_XAM_UI_DISC_SWAP_UI_H_

#include <filesystem>
#include <string>
#include <vector>

#include "xenia/kernel/xam/xam_ui.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

// Result of the disc swap dialog
enum class DiscSwapResult {
  kSelected,   // User selected a disc from the list
  kBrowse,     // User clicked Browse to use file picker
  kCancelled,  // User cancelled the dialog
};

class DiscSwapUI final : public XamGamepadDialog {
 public:
  struct DiscInfo {
    std::string label;
    std::filesystem::path path;
  };

  DiscSwapUI(xe::ui::ImGuiDrawer* imgui_drawer,
             xe::hid::InputSystem* input_system, const std::string& message,
             const std::vector<DiscInfo>& discs, bool show_error);

  DiscSwapResult result() const { return result_; }
  const std::filesystem::path& selected_path() const { return selected_path_; }

  void OnDraw(ImGuiIO& io) override;

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string message_;
  std::string error_message_;
  bool show_error_ = false;
  std::vector<DiscInfo> discs_;
  int selected_index_ = 0;
  DiscSwapResult result_ = DiscSwapResult::kCancelled;
  std::filesystem::path selected_path_;
};

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_UI_DISC_SWAP_UI_H_
