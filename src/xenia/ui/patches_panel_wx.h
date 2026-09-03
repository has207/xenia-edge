/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_PATCHES_PANEL_WX_H_
#define XENIA_UI_PATCHES_PANEL_WX_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <wx/panel.h>
#include <wx/string.h>

#include "xenia/patcher/patch_db.h"
#include "xenia/patcher/patch_file_editor.h"

class wxSizeEvent;
class wxStaticText;

namespace xe {
namespace app {

class EmulatorWindow;

// Checkbox per patch in one bundled .patch.toml, writing each toggle straight
// through to the file. Unscrolled: the host provides the scrolling.
class PatchesPanel : public wxPanel {
 public:
  PatchesPanel(wxWindow* parent, EmulatorWindow* emulator_window,
               patcher::BundledPatchFile bundled);

  // Fired when re-wrapping changes the panel's height, so a scrolling host
  // can re-measure.
  void SetContentChangedCallback(std::function<void()> cb) {
    content_changed_cb_ = std::move(cb);
  }

 private:
  void Build();
  void OnToggle(size_t patch_index, bool new_value);
  void OnSize(wxSizeEvent& event);
  void RewrapDescriptions();

  EmulatorWindow* emulator_window_;
  std::unique_ptr<patcher::PatchFileEditor> editor_;
  wxStaticText* info_label_ = nullptr;
  std::vector<std::pair<wxStaticText*, wxString>> desc_labels_;
  std::function<void()> content_changed_cb_;
  int last_wrap_width_ = -1;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_PATCHES_PANEL_WX_H_
