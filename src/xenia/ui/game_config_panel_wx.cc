/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_config_panel_wx.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <wx/bitmap.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dcmemory.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/graphics.h>
#include <wx/image.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/tomlplusplus/toml.hpp"

#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/ui/config_helpers.h"

namespace xe {
namespace app {

namespace {

struct EditorBuild {
  wxWindow* editor;
  std::function<std::string()> get_value;
};

// A typed value settles before it is written, so keystrokes don't each cost a
// file write. Discrete pickers don't wait.
constexpr int kCommitDelayMs = 500;

// How long the saved message stays up. It has to go away again or the next
// save, writing the same word, would look like nothing happened.
constexpr int kStatusClearMs = 1500;

// Distinct ids so the two timers can be told apart at the handler.
constexpr int kCommitTimerId = wxID_HIGHEST + 1;
constexpr int kStatusTimerId = wxID_HIGHEST + 2;

EditorBuild BuildEditor(wxWindow* parent, cvar::IConfigVar* var,
                        const std::string& current_value) {
  // Bool dropdown when value reads as a literal bool, regardless of type.
  if (current_value == "true" || current_value == "false") {
    auto* choice = new wxChoice(parent, wxID_ANY);
    choice->Append("true");
    choice->Append("false");
    choice->SetSelection(current_value == "true" ? 0 : 1);
    return {choice,
            [choice]() { return choice->GetStringSelection().utf8_string(); }};
  }

  if (var) {
    const auto& enums = xe::ui::GetKnownEnumOptions();
    auto it = enums.find(var->name());
    if (it != enums.end()) {
      auto* choice = new wxChoice(parent, wxID_ANY);
      int sel = -1;
      for (size_t i = 0; i < it->second.size(); ++i) {
        choice->Append(wxString::FromUTF8(it->second[i]));
        if (it->second[i] == current_value) {
          sel = static_cast<int>(i);
        }
      }
      if (sel >= 0) {
        choice->SetSelection(sel);
      }
      return {choice, [choice]() {
                return choice->GetStringSelection().utf8_string();
              }};
    }

    if (dynamic_cast<cvar::ConfigVar<std::filesystem::path>*>(var)) {
      auto* container = new wxPanel(parent, wxID_ANY);
      auto* hsizer = new wxBoxSizer(wxHORIZONTAL);
      auto* text = new wxTextCtrl(container, wxID_ANY,
                                  wxString::FromUTF8(current_value));
      auto* browse = new wxButton(container, wxID_ANY, _("Browse..."));
      std::string name = var->name();
      browse->Bind(wxEVT_BUTTON, [container, text, name](wxCommandEvent&) {
        const auto* info = xe::ui::GetCvarPathInfo(name);
        wxString current = text->GetValue();
        wxString picked;
        if (info && info->kind != xe::ui::CvarPathKind::kDirectory) {
          long flags = info->kind == xe::ui::CvarPathKind::kFileSave
                           ? (wxFD_SAVE | wxFD_OVERWRITE_PROMPT)
                           : (wxFD_OPEN | wxFD_FILE_MUST_EXIST);
          wxFileDialog dlg(
              container,
              wxString::Format(_("Select %s"), wxString::FromUTF8(name)), "",
              current, wxString::FromUTF8(info->wildcard), flags);
          if (dlg.ShowModal() == wxID_OK) {
            picked = dlg.GetPath();
          }
        } else {
          wxDirDialog dlg(container,
                          wxString::Format(_("Select directory for %s"),
                                           wxString::FromUTF8(name)),
                          current);
          if (dlg.ShowModal() == wxID_OK) {
            picked = dlg.GetPath();
          }
        }
        if (!picked.IsEmpty()) {
          text->SetValue(picked);
        }
      });
      hsizer->Add(text, wxSizerFlags(1).CenterVertical());
      hsizer->Add(browse, wxSizerFlags().CenterVertical().Border(wxLEFT, 4));
      container->SetSizer(hsizer);
      return {container, [text]() { return text->GetValue().utf8_string(); }};
    }

    if (dynamic_cast<cvar::ConfigVar<int32_t>*>(var) ||
        dynamic_cast<cvar::ConfigVar<uint32_t>*>(var)) {
      auto* spin = new wxSpinCtrl(parent, wxID_ANY);
      spin->SetRange(std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::max());
      try {
        spin->SetValue(std::stoi(current_value));
      } catch (...) {
        spin->SetValue(0);
      }
      return {spin, [spin]() { return std::to_string(spin->GetValue()); }};
    }
  }

  auto* text =
      new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(current_value));
  return {text, [text]() { return text->GetValue().utf8_string(); }};
}

wxBitmap MakeCancelBitmap(int size) {
  wxImage img(size, size);
  img.InitAlpha();
  std::memset(img.GetAlpha(), 0,
              static_cast<size_t>(img.GetWidth()) * img.GetHeight());
  wxBitmap bmp(img);
  wxMemoryDC dc(bmp);
  dc.SetBackground(wxBrush(wxColour(0, 0, 0, wxALPHA_TRANSPARENT)));
  std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
  if (gc) {
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);
    gc->SetBrush(wxBrush(wxColour(160, 160, 160)));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawEllipse(0, 0, size, size);
    gc->SetPen(wxPen(*wxWHITE, std::max(2, size / 8)));
    double pad = size * 0.3;
    gc->StrokeLine(pad, pad, size - pad, size - pad);
    gc->StrokeLine(size - pad, pad, pad, size - pad);
  }
  dc.SelectObject(wxNullBitmap);
  return bmp;
}

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// The cvar's full name, with what it does underneath when it has a
// description. Used wherever a name is shown shortened or abbreviated.
wxString CvarTooltip(const std::string& name) {
  wxString tip = wxString::FromUTF8(name);
  auto* var = cvar::ConfigVars ? (*cvar::ConfigVars)[name] : nullptr;
  if (var && !var->description().empty()) {
    tip += "\n\n" + wxString::FromUTF8(var->description());
  }
  return tip;
}

// Modal cvar picker with a live filter on top, similar to the game list
// filter bar. Returns the selected name, or empty on cancel.
std::string PickCvarWithFilter(wxWindow* parent,
                               const std::vector<std::string>& names,
                               const std::vector<std::string>& display) {
  wxDialog dlg(parent, wxID_ANY, _("Add Override"), wxDefaultPosition,
               wxSize(520, 420), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  auto* search = new wxSearchCtrl(&dlg, wxID_ANY);
  search->ShowCancelButton(true);
  search->SetDescriptiveText(_("Filter cvars"));
  sizer->Add(search, wxSizerFlags().Expand().Border(wxALL, 8));

  wxArrayString choices;
  for (const auto& d : display) {
    choices.Add(wxString::FromUTF8(d));
  }
  auto* list = new wxListBox(&dlg, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             choices, wxLB_SINGLE);
  sizer->Add(list, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT, 8));

  auto* button_sizer = dlg.CreateButtonSizer(wxOK | wxCANCEL);
  if (button_sizer) {
    sizer->Add(button_sizer, wxSizerFlags().Right().Border(wxALL, 8));
  }
  dlg.SetSizer(sizer);

  std::vector<int> visible;
  visible.reserve(names.size());
  for (int i = 0; i < static_cast<int>(names.size()); ++i) {
    visible.push_back(i);
  }

  auto refilter = [&](const std::string& filter) {
    list->Clear();
    visible.clear();
    std::string f = ToLowerAscii(filter);
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
      if (!f.empty() && ToLowerAscii(display[i]).find(f) == std::string::npos) {
        continue;
      }
      list->Append(wxString::FromUTF8(display[i]));
      visible.push_back(i);
    }
    if (!visible.empty()) {
      list->SetSelection(0);
    }
  };

  // wxListBox has no per-item tooltips, so retarget the control's own as the
  // pointer moves between rows.
  int tooltip_item = wxNOT_FOUND;
  list->Bind(wxEVT_MOTION, [&](wxMouseEvent& event) {
    event.Skip();
    const int item = list->HitTest(event.GetPosition());
    if (item == tooltip_item) {
      return;
    }
    tooltip_item = item;
    if (item < 0 || item >= static_cast<int>(visible.size())) {
      list->UnsetToolTip();
      return;
    }
    list->SetToolTip(CvarTooltip(names[visible[item]]));
  });

  search->Bind(wxEVT_TEXT, [&](wxCommandEvent&) {
    tooltip_item = wxNOT_FOUND;
    refilter(search->GetValue().utf8_string());
  });
  search->Bind(wxEVT_SEARCH_CANCEL, [&](wxCommandEvent&) { search->Clear(); });
  list->Bind(wxEVT_LISTBOX_DCLICK,
             [&](wxCommandEvent&) { dlg.EndModal(wxID_OK); });

  search->SetFocus();
  if (dlg.ShowModal() != wxID_OK) {
    return {};
  }
  int sel = list->GetSelection();
  if (sel < 0 || sel >= static_cast<int>(visible.size())) {
    return {};
  }
  return names[visible[sel]];
}

std::string StripTomlQuotes(std::string value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

void InsertTypedValue(toml::table& dest, const std::string& key,
                      const std::string& value_str) {
  if (value_str == "true" || value_str == "false") {
    dest.insert_or_assign(key, value_str == "true");
    return;
  }
  char* end = nullptr;
  long long iv = std::strtoll(value_str.c_str(), &end, 10);
  if (end != value_str.c_str() && *end == '\0') {
    dest.insert_or_assign(key, iv);
    return;
  }
  end = nullptr;
  double dv = std::strtod(value_str.c_str(), &end);
  if (end != value_str.c_str() && *end == '\0') {
    dest.insert_or_assign(key, dv);
    return;
  }
  dest.insert_or_assign(key, value_str);
}

}  // namespace

GameConfigPanel::GameConfigPanel(wxWindow* parent,
                                 EmulatorWindow* emulator_window,
                                 uint32_t title_id)
    : wxPanel(parent, wxID_ANY),
      emulator_window_(emulator_window),
      title_id_(title_id),
      commit_timer_(this, kCommitTimerId),
      status_timer_(this, kStatusTimerId) {
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Commit(); }, kCommitTimerId);
  Bind(
      wxEVT_TIMER, [this](wxTimerEvent&) { status_->SetLabel(wxString()); },
      kStatusTimerId);
  Build();
  LoadOverrides();
}

GameConfigPanel::~GameConfigPanel() {
  // The pane is rebuilt when the selection changes, which can land inside the
  // pause after a keystroke. Children outlive this body, so the rows can still
  // be read.
  if (commit_timer_.IsRunning()) {
    commit_timer_.Stop();
    Commit();
  }
}

void GameConfigPanel::Build() {
  auto* sizer = new wxBoxSizer(wxVERTICAL);

  rows_sizer_ = new wxBoxSizer(wxVERTICAL);
  sizer->Add(rows_sizer_, wxSizerFlags().Expand());

  auto* button_row = new wxBoxSizer(wxHORIZONTAL);
  auto* add_btn = new wxButton(this, wxID_ANY, _("Add..."));
  add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnAdd(); });
  button_row->Add(add_btn, wxSizerFlags().Border(wxRIGHT, 4));

  sizer->Add(button_row, wxSizerFlags().Expand().Border(wxTOP, 8));

  status_ = new wxStaticText(this, wxID_ANY, wxString());
  status_->SetForegroundColour(
      wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
  sizer->Add(status_, wxSizerFlags().Border(wxTOP, 4));

  SetSizer(sizer);
}

void GameConfigPanel::NotifyContentChanged() {
  Layout();
  if (content_changed_cb_) {
    content_changed_cb_();
  }
}

void GameConfigPanel::AddRow(const std::string& name,
                             const std::string& value) {
  auto* row = new Row;
  row->name = name;
  row->sizer = new wxBoxSizer(wxHORIZONTAL);
  auto* label =
      new wxStaticText(this, wxID_ANY, wxString::FromUTF8(name),
                       wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
  // The name is shortened to share the row, so the tooltip carries it whole.
  label->SetMinSize(wxSize(FromDIP(60), -1));
  label->SetToolTip(CvarTooltip(name));
  auto* var = cvar::ConfigVars ? (*cvar::ConfigVars)[name] : nullptr;
  // Integer cvars shown as string dropdowns display their option name.
  EditorBuild built =
      BuildEditor(this, var, xe::ui::IntCvarValueToDisplayName(name, value));
  row->editor = built.editor;
  row->get_value = std::move(built.get_value);
  // Editors size themselves to their content, which for a long enum option is
  // wider than the pane; let the sizer shrink them.
  row->editor->SetMinSize(wxSize(FromDIP(60), -1));
  // wxEVT_TEXT is a command event, so it reaches here from the text control
  // inside a path editor's container too.
  row->editor->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
    event.Skip();
    if (!loading_) {
      commit_timer_.StartOnce(kCommitDelayMs);
    }
  });
  row->editor->Bind(wxEVT_CHOICE, [this](wxCommandEvent& event) {
    event.Skip();
    Commit();
  });
  row->editor->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent& event) {
    event.Skip();
    Commit();
  });
  static const wxBitmap kCancelBitmap = MakeCancelBitmap(16);
  auto* del_btn =
      new wxBitmapButton(this, wxID_ANY, kCancelBitmap, wxDefaultPosition,
                         wxDefaultSize, wxBORDER_NONE | wxBU_EXACTFIT);
  del_btn->SetToolTip(_("Remove this override"));
  del_btn->Bind(wxEVT_BUTTON, [this, row](wxCommandEvent&) { RemoveRow(row); });

  // Names are long and values are mostly short, so the row splits 80/20
  // between them. Both can shrink; the button keeps its size.
  row->sizer->Add(label, wxSizerFlags(4).CenterVertical().Border(wxALL, 4));
  row->sizer->Add(row->editor,
                  wxSizerFlags(1).CenterVertical().Border(wxALL, 4));
  row->sizer->Add(del_btn, wxSizerFlags().CenterVertical().Border(wxALL, 4));
  rows_sizer_->Add(row->sizer, wxSizerFlags().Expand());
  rows_.push_back(row);
  NotifyContentChanged();
}

void GameConfigPanel::RemoveRow(Row* row) {
  auto it = std::find(rows_.begin(), rows_.end(), row);
  if (it == rows_.end()) {
    return;
  }
  rows_sizer_->Detach(row->sizer);
  row->sizer->Clear(true);
  delete row->sizer;
  delete row;
  rows_.erase(it);
  Commit();
  NotifyContentChanged();
}

void GameConfigPanel::LoadOverrides() {
  loading_ = true;
  for (auto* row : rows_) {
    rows_sizer_->Detach(row->sizer);
    row->sizer->Clear(true);
    delete row->sizer;
    delete row;
  }
  rows_.clear();

  toml::table table;
  try {
    table = config::LoadGameConfig(title_id_);
  } catch (const std::exception& e) {
    XELOGE("GameConfigPanel: failed to load config: {}", e.what());
    loading_ = false;
    return;
  }
  if (!cvar::ConfigVars) {
    loading_ = false;
    return;
  }
  std::map<std::string, std::string> rows;
  for (auto& [name, var] : *cvar::ConfigVars) {
    auto* config_var = static_cast<cvar::IConfigVar*>(var);
    auto path = toml::path(config_var->category() + "." + config_var->name());
    auto node = table.at_path(path);
    if (!node) {
      continue;
    }
    void* saved = config_var->SaveConfigValueState();
    config_var->LoadConfigValue(node.node());
    std::string value = StripTomlQuotes(config_var->config_value());
    config_var->RestoreConfigValueState(saved);
    // Evict an override whose value is no longer a valid option for an enum
    // cvar (e.g. one we renamed): drop the row so it is not saved back, letting
    // the setting fall back to its default instead of pinning a bogus override.
    const auto& enums = xe::ui::GetKnownEnumOptions();
    auto enum_it = enums.find(config_var->name());
    if (enum_it != enums.end()) {
      const std::string display =
          xe::ui::IntCvarValueToDisplayName(config_var->name(), value);
      if (std::find(enum_it->second.begin(), enum_it->second.end(), display) ==
          enum_it->second.end()) {
        continue;
      }
    }
    rows.emplace(config_var->name(), value);
  }
  for (const auto& [k, v] : rows) {
    AddRow(k, v);
  }
  loading_ = false;
  // Filling the editors may have queued a write of what we just read.
  commit_timer_.Stop();
  committed_ = Snapshot();
}

std::map<std::string, std::string> GameConfigPanel::Snapshot() const {
  std::map<std::string, std::string> values;
  for (const auto* row : rows_) {
    if (!row->name.empty() && row->get_value) {
      values.emplace(row->name, row->get_value());
    }
  }
  return values;
}

void GameConfigPanel::Commit() {
  if (loading_) {
    return;
  }
  commit_timer_.Stop();
  auto values = Snapshot();
  if (values == committed_) {
    return;
  }
  if (!SaveOverrides()) {
    // A failure stays up: it is a state to notice, not an acknowledgement.
    status_timer_.Stop();
    status_->SetLabel(_("Failed to save."));
    return;
  }
  committed_ = std::move(values);
  status_->SetLabel(_("Saved."));
  status_timer_.StartOnce(kStatusClearMs);
}

bool GameConfigPanel::SaveOverrides() {
  toml::table out;
  std::map<std::string, toml::table> by_category;
  for (auto* row : rows_) {
    std::string value = row->get_value ? row->get_value() : std::string();
    if (row->name.empty()) {
      continue;
    }
    auto* var = cvar::ConfigVars ? (*cvar::ConfigVars)[row->name] : nullptr;
    if (!var || var->is_transient()) {
      continue;
    }
    InsertTypedValue(by_category[var->category()], row->name,
                     xe::ui::DisplayNameToIntCvarValue(row->name, value));
  }
  for (auto& [cat, tbl] : by_category) {
    if (auto* dest = config::ResolveSectionTable(out, cat)) {
      for (auto&& [key, value] : tbl) {
        dest->insert_or_assign(key, value);
      }
    }
  }
  try {
    config::SaveGameConfig(title_id_, out);
  } catch (const std::exception& e) {
    // No dialog: this runs on every edit, so a modal per keystroke would be
    // worse than the status line saying it didn't take.
    XELOGE("GameConfigPanel: failed to save config: {}", e.what());
    return false;
  }
  return true;
}

void GameConfigPanel::OnAdd() {
  if (!cvar::ConfigVars) {
    return;
  }
  std::set<std::string> existing;
  for (auto* row : rows_) {
    existing.insert(row->name);
  }

  std::vector<std::string> names;
  std::vector<std::string> display;
  for (auto& [name, var] : *cvar::ConfigVars) {
    if (var->is_transient()) {
      continue;
    }
    if (existing.count(name)) {
      continue;
    }
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  display.reserve(names.size());
  for (const auto& n : names) {
    auto* var = (*cvar::ConfigVars)[n];
    display.push_back(fmt::format("{} ({})", n, var->category()));
  }
  if (names.empty()) {
    wxMessageBox(_("All cvars are already overridden."), _("Add Override"),
                 wxOK | wxICON_INFORMATION, this);
    return;
  }
  std::string name = PickCvarWithFilter(this, names, display);
  if (name.empty()) {
    return;
  }
  auto* var = (*cvar::ConfigVars)[name];
  std::string default_value = StripTomlQuotes(var->config_value());
  AddRow(name, default_value);
  Commit();
}

}  // namespace app
}  // namespace xe
