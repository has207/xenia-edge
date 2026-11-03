/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/ui/game_achievements_ui.h"

#include "xenia/hid/input.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

GameAchievementsUI::GameAchievementsUI(xe::ui::ImGuiDrawer* imgui_drawer,
                                       xe::hid::InputSystem* input_system,
                                       const TitleInfo* title_info,
                                       const UserProfile* profile)
    : XamGamepadDialog(imgui_drawer, input_system),
      title_info_(*title_info),
      profile_(profile),
      window_id_(GetWindowId()) {
  LoadAchievementsData();
}

GameAchievementsUI::~GameAchievementsUI() {
  for (auto& entry : achievements_icons_) {
    entry.second.release();
  }
}

bool GameAchievementsUI::LoadAchievementsData() {
  achievements_info_ =
      kernel_state()->xam_state()->achievement_manager()->GetTitleAchievements(
          profile_->xuid(), title_info_.id);

  if (achievements_info_.empty()) {
    return false;
  }

  xe::ui::IconsData data;

  for (const Achievement& entry : achievements_info_) {
    const auto icon =
        kernel_state()->xam_state()->achievement_manager()->GetAchievementIcon(
            profile_->xuid(), title_info_.id, entry.achievement_id);

    data.insert({entry.image_id, icon});
  }

  achievements_icons_ = imgui_drawer()->LoadIcons(data);
  return true;
}

std::string GameAchievementsUI::GetAchievementTitle(
    const Achievement& achievement_entry) const {
  std::string title = "Secret trophy";

  if (achievement_entry.IsUnlocked() || show_locked_info_ ||
      achievement_entry.flags &
          static_cast<uint32_t>(AchievementFlags::kShowUnachieved)) {
    title = xe::to_utf8(achievement_entry.achievement_name);
  }

  return title;
}

std::string GameAchievementsUI::GetAchievementDescription(
    const Achievement& achievement_entry) const {
  std::string description = "Hidden description";

  if (achievement_entry.flags &
      static_cast<uint32_t>(AchievementFlags::kShowUnachieved)) {
    description = xe::to_utf8(achievement_entry.locked_description);
  }

  if (achievement_entry.IsUnlocked() || show_locked_info_) {
    description = xe::to_utf8(achievement_entry.unlocked_description);
  }

  return description;
}

xe::ui::ImmediateTexture* GameAchievementsUI::GetIcon(
    const Achievement& achievement_entry) const {
  if (!achievement_entry.IsUnlocked() && !show_locked_info_) {
    return imgui_drawer()->GetLockedAchievementIcon();
  }

  if (achievements_icons_.count(achievement_entry.image_id)) {
    return achievements_icons_.at(achievement_entry.image_id).get();
  }

  if (achievement_entry.IsUnlocked()) {
    return nullptr;
  }
  return imgui_drawer()->GetLockedAchievementIcon();
}

std::string GameAchievementsUI::GetUnlockedTime(
    const Achievement& achievement_entry) const {
  if (achievement_entry.IsUnlockedOnline()) {
    const auto unlock_time =
        std::chrono::system_clock::to_time_t(chrono::WinSystemClock::to_sys(
            achievement_entry.unlock_time.to_time_point()));

    return fmt::format("Unlocked: Online {:%Y-%m-%d %H:%M}",
                       *std::localtime(&unlock_time));
  }

  if (achievement_entry.unlock_time.is_valid()) {
    const auto unlock_time =
        std::chrono::system_clock::to_time_t(chrono::WinSystemClock::to_sys(
            achievement_entry.unlock_time.to_time_point()));

    return fmt::format("Unlocked: Offline ({:%Y-%m-%d %H:%M})",
                       *std::localtime(&unlock_time));
  }
  return fmt::format("Unlocked: Offline");
}

void GameAchievementsUI::DrawTitleAchievementInfo(
    ImGuiIO& io, const Achievement& achievement_entry) const {
  const auto start_drawing_pos = ImGui::GetCursorPos();

  ImGui::TableSetColumnIndex(0);

  const auto icon = GetIcon(achievement_entry);
  if (icon) {
    ImGui::Image(reinterpret_cast<ImTextureID>(GetIcon(achievement_entry)),
                 xe::ui::default_image_icon_size);
  } else {
    ImGui::Dummy(xe::ui::default_image_icon_size);
  }
  ImGui::TableNextColumn();

  ImGui::PushFont(imgui_drawer()->GetTitleFont());
  const auto primary_line_height = ImGui::GetTextLineHeight();
  ImGui::Text("%s", GetAchievementTitle(achievement_entry).c_str());
  ImGui::PopFont();

  ImGui::PushTextWrapPos(ImGui::GetMainViewport()->Size.x * 0.5f);
  ImGui::TextWrapped("%s",
                     GetAchievementDescription(achievement_entry).c_str());
  ImGui::PopTextWrapPos();

  ImGui::SetCursorPosY(start_drawing_pos.y + xe::ui::default_image_icon_size.x -
                       ImGui::GetTextLineHeight());

  if (achievement_entry.IsUnlocked()) {
    ImGui::Text("%s", GetUnlockedTime(achievement_entry).c_str());
  }

  ImGui::TableNextColumn();

  // TODO(Gliniak): There is no easy way to align text to middle, so I have to
  // do it manually.
  const float achievement_row_middle_alignment =
      ((xe::ui::default_image_icon_size.x / 2.f) -
       ImGui::GetTextLineHeight() / 2.f) *
      0.85f;

  ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                       achievement_row_middle_alignment);
  ImGui::PushFont(imgui_drawer()->GetTitleFont());
  ImGui::TextUnformatted(
      fmt::format("{} G", achievement_entry.gamerscore).c_str());
  ImGui::PopFont();
}

void GameAchievementsUI::OnDraw(ImGuiIO& io) {
  // Handle keyboard escape or gamepad B/Back to close
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ShouldCloseFromGamepad()) {
    Close();
    return;
  }

  // Poll right stick for smooth scrolling
  int scroll_pixels = 0;
  if (input_system_) {
    for (uint32_t i = 0; i < 4; ++i) {
      hid::X_INPUT_STATE state;
      if (input_system_->GetStateForUI(i, 1, &state) == 0) {
        int16_t ry = state.gamepad.thumb_ry;
        if (std::abs(ry) > hid::X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) {
          // Invert Y axis (up is positive in gamepad coords)
          // Accumulate fractional scrolling for smooth movement
          scroll_accumulator_ += -static_cast<float>(ry) / 2000.0f;

          scroll_pixels = static_cast<int>(scroll_accumulator_);
          if (scroll_pixels != 0) {
            scroll_accumulator_ -= scroll_pixels;  // Keep fractional part
          }
        } else {
          scroll_accumulator_ = 0.0f;  // Reset when stick released
        }
        break;
      }
    }
  }

  const auto xenia_window_size = ImGui::GetMainViewport()->Size;

  // Center the window on screen
  ImVec2 center =
      ImVec2(xenia_window_size.x * 0.5f, xenia_window_size.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

  // Fixed size: 600px wide, max 500px tall
  ImGui::SetNextWindowSizeConstraints(ImVec2(600, 200), ImVec2(600, 500));

  // Style like context menu - white background, black text, Xbox green
  // highlights
  const ImVec4 xbox_green(0.063f, 0.486f, 0.063f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBg, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                        ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,
                        ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, xbox_green);
  ImGui::PushStyleColor(ImGuiCol_TableBorderLight,
                        ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));

  bool dialog_open = true;

  std::string title_name = xe::to_utf8(title_info_.title_name);
  title_name.erase(std::ranges::remove(title_name, '\0').begin(),
                   title_name.end());

  const std::string window_name =
      fmt::format("{} - Achievements###{}", title_name, window_id_);
  if (!ImGui::Begin(
          window_name.c_str(), &dialog_open,
          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(13);
    Close();
    ImGui::End();
    return;
  }

  // Apply right stick scrolling
  if (scroll_pixels != 0) {
    ImGui::SetScrollY(ImGui::GetScrollY() + scroll_pixels);
  }

  // Checkbox - ImGui handles A button toggle natively
  ImGui::Checkbox("Show locked achievements information", &show_locked_info_);
  ImGui::Separator();

  if (achievements_info_.empty()) {
    ImGui::TextUnformatted(fmt::format("No achievements data!").c_str());
  } else {
    if (ImGui::BeginTable("", 3, ImGuiTableFlags_BordersInnerH)) {
      for (const auto& entry : achievements_info_) {
        ImGui::TableNextRow(0, xe::ui::default_image_icon_size.y);
        DrawTitleAchievementInfo(io, entry);
      }
      ImGui::EndTable();
    }
  }

  if (!dialog_open) {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(13);
    Close();
    ImGui::End();
    return;
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(13);
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
