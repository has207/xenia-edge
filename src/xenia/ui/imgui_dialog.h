/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_IMGUI_DIALOG_H_
#define XENIA_UI_IMGUI_DIALOG_H_

#include <memory>
#include <string>

#include "xenia/base/threading.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window_listener.h"

namespace xe {
namespace ui {

class ImGuiDialog {
 public:
  virtual ~ImGuiDialog();

  // Shows a simple message box containing a text message.
  // Callers can want for the dialog to close with Wait().
  // Dialogs retain themselves and will delete themselves when closed.
  static ImGuiDialog* ShowMessageBox(ImGuiDrawer* imgui_drawer,
                                     std::string title, std::string body);

  // A fence to signal when the dialog is closed. When unattended, the dialog
  // is dismissed right away, as nobody can answer it.
  void Then(xe::threading::Fence* fence);

  // When unattended, logs the dialog and closes it with its default answer.
  // Deletes the dialog.
  void DismissUnattended();

  void Draw();
  // Closes and deletes the dialog now rather than on its next draw.
  void Destroy();

  bool IsClosing() const { return has_close_pending_; }

 protected:
  ImGuiDialog(ImGuiDrawer* imgui_drawer);

  ImGuiDrawer* imgui_drawer() const { return imgui_drawer_; }
  ImGuiIO& GetIO();

  uint64_t GetWindowId() const { return next_window_id_; }
  // Closes the dialog and returns to any waiters.
  void Close();

  // Returns true if B button or Back button was pressed (for closing dialogs)
  bool ShouldCloseFromGamepad() const;

  virtual void OnShow() {}
  virtual void OnClose() {}
  virtual void OnDraw(ImGuiIO& io) {}

  // What the log says about the dialog when it's shown: a title, and the text
  // it asks or tells the user.
  virtual std::string GetLogTitle() const;
  virtual std::string GetLogText() const { return {}; }
  // What the user answered, logged when the dialog closes.
  virtual std::string GetLogResult() const { return {}; }
  // Picks the answer an unattended run gives: whatever closing it unanswered gives,
  // unless overridden.
  virtual void OnUnattendedDismiss() {}

 private:
  static std::atomic<uint64_t> next_window_id_;

  void LogShown();

  ImGuiDrawer* imgui_drawer_ = nullptr;
  bool has_close_pending_ = false;
  bool has_been_logged_ = false;
  std::vector<xe::threading::Fence*> waiting_fences_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_IMGUI_DIALOG_H_
