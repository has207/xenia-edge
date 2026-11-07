/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/imgui_dialog.h"

#include "third_party/imgui/imgui.h"
#include <typeinfo>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace ui {

std::atomic<uint64_t> ImGuiDialog::next_window_id_ = 0;

ImGuiDialog::ImGuiDialog(ImGuiDrawer* imgui_drawer)
    : imgui_drawer_(imgui_drawer) {
  imgui_drawer_->AddDialog(this);
  next_window_id_++;
}

ImGuiDialog::~ImGuiDialog() {
  imgui_drawer_->RemoveDialog(this);
  for (auto fence : waiting_fences_) {
    fence->Signal();
  }
}

void ImGuiDialog::Then(xe::threading::Fence* fence) {
  waiting_fences_.push_back(fence);
  if (xe::IsUnattended()) {
    DismissUnattended();
  }
}

void ImGuiDialog::DismissUnattended() {
  LogShown();
  OnUnattendedDismiss();
  XELOGI("Unattended: dismissed {}", GetLogTitle());
  Destroy();
}

std::string ImGuiDialog::GetLogTitle() const {
  // MSVC names it "class xe::...", which is clear enough for a log.
  return typeid(*this).name();
}

void ImGuiDialog::LogShown() {
  if (has_been_logged_) {
    return;
  }
  has_been_logged_ = true;
  LogUIElement("dialog", GetLogTitle(), GetLogText());
}

void ImGuiDialog::Close() { has_close_pending_ = true; }

ImGuiIO& ImGuiDialog::GetIO() { return imgui_drawer()->GetIO(); }

bool ImGuiDialog::ShouldCloseFromGamepad() const {
  return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) ||  // B button
         ImGui::IsKeyPressed(ImGuiKey_GamepadBack);         // Back button
}

void ImGuiDialog::Draw() {
  LogShown();
  // Draw UI.
  if (!has_close_pending_) {
    OnDraw(GetIO());
  }

  // Check to see if the UI closed itself and needs to be deleted.
  if (has_close_pending_) {
    Destroy();
  }
}

void ImGuiDialog::Destroy() {
  if (has_been_logged_) {
    std::string result = GetLogResult();
    if (!result.empty()) {
      XELOGI("UI dialog closed: {}: {}", GetLogTitle(), result);
    }
  }
  OnClose();
  delete this;
}

class MessageBoxDialog final : public ImGuiDialog {
 public:
  MessageBoxDialog(ImGuiDrawer* imgui_drawer, std::string title,
                   std::string body)
      : ImGuiDialog(imgui_drawer),
        title_(std::move(title)),
        body_(std::move(body)) {}

  std::string GetLogTitle() const override { return title_; }
  std::string GetLogText() const override { return body_; }

  void OnDraw(ImGuiIO& io) override {
    if (!has_opened_) {
      ImGui::OpenPopup(title_.c_str());
      has_opened_ = true;
    }
    if (ImGui::BeginPopupModal(title_.c_str(), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
      char* text = const_cast<char*>(body_.c_str());
      ImGui::InputTextMultiline(
          "##body", text, body_.size() + 1, ImVec2(600, 0),
          ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_ReadOnly);
      if (ImGui::Button("OK")) {
        ImGui::CloseCurrentPopup();
        Close();
      }
      ImGui::EndPopup();
    } else {
      Close();
    }
  }

 private:
  bool has_opened_ = false;
  std::string title_;
  std::string body_;
};

ImGuiDialog* ImGuiDialog::ShowMessageBox(ImGuiDrawer* imgui_drawer,
                                         std::string title, std::string body) {
  auto* dialog =
      new MessageBoxDialog(imgui_drawer, std::move(title), std::move(body));
  if (xe::IsUnattended()) {
    // After the current UI thread task, so a caller's Then() can still use the
    // dialog; that dismisses it first, and then there's nothing left to do.
    imgui_drawer->PostDeferredCallback([imgui_drawer, dialog]() {
      if (imgui_drawer->HasDialog(dialog)) {
        dialog->DismissUnattended();
      }
    });
  }
  return dialog;
}

}  // namespace ui
}  // namespace xe
