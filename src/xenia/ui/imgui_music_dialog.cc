/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_music_dialog.h"

#include <algorithm>
#include <cfloat>
#include <string>

#include "third_party/imgui/imgui.h"
#include "xenia/apu/host_media_session.h"
#include "xenia/base/string.h"

namespace xe {
namespace ui {

ImGuiMusicDialog::ImGuiMusicDialog(ImGuiDrawer* drawer,
                                   hid::InputSystem* input_system,
                                   apu::HostMediaSession* host_media)
    : ImGuiGamepadDialog(drawer, input_system), host_media_(host_media) {}

void ImGuiMusicDialog::OnClose() {
  if (on_close_callback_) {
    on_close_callback_();
  }
}

void ImGuiMusicDialog::OnDraw(ImGuiIO& io) {
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.063f, 0.486f, 0.063f, 0.35f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                        ImVec4(0.063f, 0.486f, 0.063f, 0.2f));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                        ImVec4(0.063f, 0.486f, 0.063f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, xbox_green);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

  ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

  const char* title = "Select Music";
  float title_width = ImGui::CalcTextSize(title).x;
  float min_width = (title_width + ImGui::GetStyle().WindowPadding.x * 2 +
                     ImGui::GetFrameHeight()) *
                    2.0f;
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(min_width, 0),
      ImVec2(std::max(min_width, io.DisplaySize.x * 0.6f), FLT_MAX));

  bool is_open = true;
  if (ImGui::Begin(title, &is_open,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_NoScrollbar)) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
      Close();
    }

    const auto sources = host_media_->GetSources();
    if (sources.empty()) {
      ImGui::Text("No media apps are open.");
    }
    bool any_selected = false;
    for (const apu::HostMediaSource& source : sources) {
      const apu::HostMedia& media = source.media;
      any_selected |= source.selected;
      std::string text = xe::to_utf8(source.name);
      if (media.status == apu::HostMedia::Status::kPlaying) {
        text += " (Playing)";
      } else if (media.status == apu::HostMedia::Status::kPaused) {
        text += " (Paused)";
      }
      if (!media.title.empty()) {
        text += "\n" + xe::to_utf8(media.title);
        if (!media.artist.empty()) {
          text += " - " + xe::to_utf8(media.artist);
        }
      }
      // Keyed by app, as the text changes with the track. The text goes over
      // the row unformatted so a "##" in it shows.
      ImGui::PushID(xe::to_utf8(source.id).c_str());
      const ImVec2 text_pos = ImGui::GetCursorPos();
      if (ImGui::Selectable("##source", source.selected, 0,
                            ImVec2(0, ImGui::CalcTextSize(text.c_str()).y))) {
        host_media_->Select(source.id);
      }
      if (source.selected) {
        ImGui::SetItemDefaultFocus();
      }
      ImGui::SetCursorPos(text_pos);
      ImGui::TextUnformatted(text.c_str());
      ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!any_selected);
    if (ImGui::Button("Stop")) {
      host_media_->Stop();
    }
    ImGui::EndDisabled();
  }
  ImGui::End();

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(12);

  if (!is_open) {
    Close();
  }
}

}  // namespace ui
}  // namespace xe
