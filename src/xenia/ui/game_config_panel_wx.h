/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GAME_CONFIG_PANEL_WX_H_
#define XENIA_UI_GAME_CONFIG_PANEL_WX_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

class wxBoxSizer;
class wxStaticText;
class wxWindow;

namespace xe {
namespace app {

class EmulatorWindow;

// Editor for one title's config overrides: a row per overridden cvar, saved
// as it is edited. Unscrolled; the host provides the scrolling.
class GameConfigPanel : public wxPanel {
 public:
  GameConfigPanel(wxWindow* parent, EmulatorWindow* emulator_window,
                  uint32_t title_id);
  ~GameConfigPanel() override;

  // Fired when rows come or go, so a scrolling host can re-measure.
  void SetContentChangedCallback(std::function<void()> cb) {
    content_changed_cb_ = std::move(cb);
  }

 private:
  struct Row {
    std::string name;
    wxBoxSizer* sizer = nullptr;
    wxWindow* editor = nullptr;
    std::function<std::string()> get_value;
  };

  void Build();
  void LoadOverrides();
  // What the rows say right now, keyed by cvar name.
  std::map<std::string, std::string> Snapshot() const;
  // Writes the rows out if they differ from what is on disk, and reports the
  // outcome. Every edit goes through here; there is no save button.
  void Commit();
  bool SaveOverrides();
  void OnAdd();
  void AddRow(const std::string& name, const std::string& value);
  void RemoveRow(Row* row);
  void NotifyContentChanged();

  EmulatorWindow* emulator_window_;
  uint32_t title_id_;
  // Set while LoadOverrides() builds rows, so filling an editor doesn't write
  // the half-built set back out.
  bool loading_ = false;
  // Last state written, so a commit that changes nothing writes nothing.
  std::map<std::string, std::string> committed_;
  // Typing commits on a pause rather than per keystroke.
  wxTimer commit_timer_;
  // Clears the saved message, so the next save is visibly its own.
  wxTimer status_timer_;
  wxBoxSizer* rows_sizer_ = nullptr;
  wxStaticText* status_ = nullptr;
  std::vector<Row*> rows_;
  std::function<void()> content_changed_cb_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_GAME_CONFIG_PANEL_WX_H_
