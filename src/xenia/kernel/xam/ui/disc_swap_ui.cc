/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/disc_swap_ui.h"

#include "third_party/imgui/imgui.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

DiscSwapUI::DiscSwapUI(xe::ui::ImGuiDrawer* imgui_drawer,
                       xe::hid::InputSystem* input_system,
                       const std::string& message,
                       const std::vector<DiscInfo>& discs, bool show_error)
    : XamGamepadDialog(imgui_drawer, input_system),
      discs_(discs),
      show_error_(show_error) {
  title_ = "Select Disc";

  // Parse error message if present
  if (show_error) {
    size_t error_pos = message.find("ERROR:");
    if (error_pos != std::string::npos) {
      error_message_ = message.substr(error_pos + 7);
      // Trim leading/trailing whitespace
      size_t start = error_message_.find_first_not_of(" \t\n\r");
      size_t end = error_message_.find_last_not_of(" \t\n\r");
      if (start != std::string::npos && end != std::string::npos) {
        error_message_ = error_message_.substr(start, end - start + 1);
      }
      message_ = message.substr(0, error_pos);
    } else {
      message_ = message;
    }
  } else {
    message_ = message;
  }
}

void DiscSwapUI::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup(title_.c_str());
    has_opened_ = true;
  }

  // Center the window on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  // Size constraints leave an auto-resize popup a pixel short of its contents
  const float font_size = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(font_size * 36.0f, 0.0f), ImGuiCond_Always);

  // Style like Xbox - white background, black text, Xbox green highlights
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  const ImVec4 error_red(0.8f, 0.2f, 0.2f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Header, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, xbox_green);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  const ImVec2 padding = ImGui::GetStyle().WindowPadding;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(padding.x * 2.0f, padding.y * 2.0f));

  if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Handle keyboard escape or gamepad B/Back to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      result_ = DiscSwapResult::kCancelled;
      ImGui::CloseCurrentPopup();
      Close();
    }

    // Show error message if present
    if (show_error_ && !error_message_.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, error_red);
      ImGui::TextWrapped("Error: %s", error_message_.c_str());
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();
    }

    // Show instruction message
    if (!message_.empty()) {
      ImGui::TextWrapped("%s", message_.c_str());
      ImGui::Spacing();
    }

    // Show disc count info and list
    if (!discs_.empty()) {
      ImGui::Text("This game has %zu discs. Select which disc to load:",
                  discs_.size());
      ImGui::Spacing();

      // Disc list - top level selectables for proper gamepad navigation
      for (size_t i = 0; i < discs_.size(); i++) {
        const auto& disc = discs_[i];
        std::string label =
            disc.label.empty() ? fmt::format("Disc {}", i + 1) : disc.label;

        bool is_selected = (selected_index_ == static_cast<int>(i));
        if (ImGui::Selectable(label.c_str(), is_selected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
          // Single click or Enter key - select and close
          selected_path_ = disc.path;
          result_ = DiscSwapResult::kSelected;
          XELOGI("DiscSwapUI: Selected disc from saved paths: {}",
                 selected_path_.string());
          ImGui::CloseCurrentPopup();
          Close();
        }

        // Update selection when focus changes (keyboard/gamepad navigation)
        if (ImGui::IsItemFocused()) {
          selected_index_ = static_cast<int>(i);
        }

        // Handle gamepad A button on focused item
        if (ImGui::IsItemFocused() &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
          selected_path_ = disc.path;
          result_ = DiscSwapResult::kSelected;
          XELOGI("DiscSwapUI: Selected disc {} from saved paths: {}", i + 1,
                 selected_path_.string());
          ImGui::CloseCurrentPopup();
          Close();
        }

        // Set initial focus to first item
        if (i == 0) {
          ImGui::SetItemDefaultFocus();
        }
      }

      ImGui::Spacing();
    }

    // Buttons
    if (ImGui::Button("Browse...")) {
      result_ = DiscSwapResult::kBrowse;
      ImGui::CloseCurrentPopup();
      Close();
    }

    // Push Cancel to the right edge, unless Browse already reaches it
    const ImGuiStyle& style = ImGui::GetStyle();
    float browse_end_x = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    float content_end_x =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    float cancel_width =
        ImGui::CalcTextSize("Cancel").x + style.FramePadding.x * 2.0f;
    float cancel_x = content_end_x - cancel_width;
    if (cancel_x > browse_end_x + style.ItemSpacing.x) {
      ImGui::SameLine(cancel_x);
    } else {
      ImGui::SameLine();
    }

    if (ImGui::Button("Cancel")) {
      result_ = DiscSwapResult::kCancelled;
      ImGui::CloseCurrentPopup();
      Close();
    }

    ImGui::EndPopup();
  } else {
    Close();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(14);
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
