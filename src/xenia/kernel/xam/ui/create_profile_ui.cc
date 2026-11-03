/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/create_profile_ui.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

void CreateProfileUI::OnDraw(ImGuiIO& io) {
  if (!has_opened_) {
    ImGui::OpenPopup("Create Profile");
    has_opened_ = true;
  }

  // Center the window on screen
  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(350, 0), ImVec2(400, 300));

  // Style like Xbox - white background, black text, Xbox green highlights
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));

  if (ImGui::BeginPopupModal("Create Profile", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    // Handle B button or Back to cancel
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      ImGui::CloseCurrentPopup();
      Close();
      ImGui::EndPopup();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(10);
      return;
    }

    auto profile_manager =
        emulator_->kernel_state()->xam_state()->profile_manager();

    ImGui::Text("Gamertag:");
    // Set focus to InputText on first frame
    if (!focus_set_) {
      ImGui::SetKeyboardFocusHere();
      focus_set_ = true;
    }
    bool enter_pressed = ImGui::InputText("##gt", gamertag_, sizeof(gamertag_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);

    std::string gt(gamertag_);
    bool valid = profile_manager->IsGamertagValid(gt);

    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Create") || (enter_pressed && valid)) {
      bool autologin = (profile_manager->GetAccountCount() == 0);
      profile_manager->CreateProfile(gt, autologin);
      ImGui::CloseCurrentPopup();
      Close();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
      Close();
    }

    ImGui::EndPopup();
  } else {
    Close();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(10);
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
