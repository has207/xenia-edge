/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/patches_panel_wx.h"

#include <wx/checkbox.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "xenia/app/emulator_window.h"
#include "xenia/base/filesystem.h"
#include "xenia/emulator.h"

namespace xe {
namespace app {

namespace {

// Description text is indented under its checkbox, in logical DIPs.
constexpr int kDescriptionIndent = 16;

}  // namespace

PatchesPanel::PatchesPanel(wxWindow* parent, EmulatorWindow* emulator_window,
                           patcher::BundledPatchFile bundled)
    : wxPanel(parent, wxID_ANY), emulator_window_(emulator_window) {
  std::filesystem::path storage_path;
  if (emulator_window_ && emulator_window_->emulator()) {
    storage_path = emulator_window_->emulator()->storage_root() / "patches" /
                   xe::to_path(bundled.filename);
  }

  std::string source_text;
  if (!storage_path.empty()) {
    source_text = xe::filesystem::ReadAllText(storage_path);
  }
  if (source_text.empty()) {
    source_text = std::move(bundled.toml_content);
  }

  editor_ =
      std::make_unique<patcher::PatchFileEditor>(source_text, storage_path);

  Build();
}

void PatchesPanel::Build() {
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  const auto& patches = editor_->patches();
  for (size_t i = 0; i < patches.size(); ++i) {
    const auto& info = patches[i];
    wxString display_name;
    if (info.name.empty()) {
      display_name = wxString::Format(_("Patch #%zu"), i + 1);
    } else {
      display_name = wxString::FromUTF8(info.name);
    }

    auto* checkbox = new wxCheckBox(this, wxID_ANY, display_name);
    checkbox->SetValue(info.is_enabled);
    checkbox->Bind(wxEVT_CHECKBOX,
                   [this, idx = i, cb = checkbox](wxCommandEvent&) {
                     OnToggle(idx, cb->GetValue());
                   });
    sizer->Add(checkbox, wxSizerFlags().Border(wxTOP, 4));

    if (!info.description.empty() || !info.author.empty()) {
      wxString detail;
      if (!info.description.empty()) {
        detail = wxString::FromUTF8(info.description);
      }
      if (!info.author.empty()) {
        if (!detail.empty()) {
          detail += wxT("\n");
        }
        detail += wxString::Format(_("by %s"), wxString::FromUTF8(info.author));
      }
      auto* label = new wxStaticText(this, wxID_ANY, detail);
      label->SetForegroundColour(
          wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
      sizer->Add(
          label,
          wxSizerFlags().Border(wxLEFT, FromDIP(kDescriptionIndent)).Expand());
      desc_labels_.emplace_back(label, detail);
    }
  }

  info_label_ = new wxStaticText(this, wxID_ANY,
                                 _("Toggles take effect on next launch."));
  info_label_->SetForegroundColour(
      wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
  sizer->Add(info_label_, wxSizerFlags().Border(wxTOP, 8));

  SetSizer(sizer);
  Bind(wxEVT_SIZE, &PatchesPanel::OnSize, this);
}

void PatchesPanel::OnToggle(size_t patch_index, bool new_value) {
  if (!editor_->SetEnabled(patch_index, new_value)) {
    info_label_->SetLabel(_("Failed to save changes."));
    return;
  }
  info_label_->SetLabel(_("Saved. Takes effect on next launch."));
}

void PatchesPanel::OnSize(wxSizeEvent& event) {
  event.Skip();
  RewrapDescriptions();
}

void PatchesPanel::RewrapDescriptions() {
  if (desc_labels_.empty()) {
    return;
  }
  const int width = GetClientSize().GetWidth() - FromDIP(kDescriptionIndent);
  if (width <= 0 || width == last_wrap_width_) {
    return;
  }
  last_wrap_width_ = width;
  for (auto& [label, text] : desc_labels_) {
    label->SetLabel(text);
    label->Wrap(width);
  }
  if (auto* sizer = GetSizer()) {
    sizer->Layout();
  }
  // Wrapping changes how tall the panel wants to be, and the host measured it
  // before that happened.
  InvalidateBestSize();
  if (content_changed_cb_) {
    content_changed_cb_();
  }
}

}  // namespace app
}  // namespace xe
