/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_info_panel_wx.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <wx/arrstr.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/filedlg.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/toplevel.h>
#include <wx/translation.h>
#include <wx/variant.h>

#include "third_party/fmt/include/fmt/chrono.h"
#include "third_party/fmt/include/fmt/format.h"

#include "xenia/app/emulator_window.h"
#include "xenia/app/game_library.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/patcher/patch_db.h"
#include "xenia/ui/compat_display_wx.h"
#include "xenia/ui/game_config_panel_wx.h"
#include "xenia/ui/icon_decode.h"
#include "xenia/ui/patches_panel_wx.h"
#include "xenia/vfs/devices/xcontent_container_device.h"

namespace xe {
namespace app {

namespace {

// Art edge in logical DIPs at 96 DPI; converted per-monitor via FromDIP().
constexpr int kArtSize = 96;
// Disc rows past this scroll inside the list rather than growing the page.
constexpr size_t kMaxVisibleDiscRows = 8;
// Selection changes coalesce for this long before the pane restats the disk.
constexpr int kRefreshDelayMs = 120;
// Matches wxSearchCtrl's own cancel glyph, which lightens the foreground.
constexpr int kCloseLightness = 160;

// Circle with the cross knocked out of it, the same shape wxSearchCtrl draws
// for its cancel button. Painted oversized and downscaled to antialias, and
// filled with the parent's background so it blends in.
wxBitmap MakeCloseBitmap(const wxWindow* win, int size_px, int lightness) {
  constexpr int kOversample = 4;
  const int px = size_px * kOversample;
  const wxColour bg = win->GetBackgroundColour();
  const wxColour fg = win->GetForegroundColour().ChangeLightness(lightness);

  wxBitmap oversized(px, px, 32);
  {
    wxMemoryDC mem(oversized);
    mem.SetBackground(wxBrush(bg));
    mem.Clear();
    mem.SetBrush(wxBrush(fg));
    mem.SetPen(wxPen(fg));
    mem.DrawCircle(px / 2, px / 2, px / 2 - kOversample);
    const int inset = px / 3;
    mem.SetPen(wxPen(bg, std::max(1, px / 10)));
    mem.DrawLine(inset, inset, px - inset, px - inset);
    mem.DrawLine(px - inset, inset, inset, px - inset);
  }

  wxImage image = oversized.ConvertToImage();
  image.Rescale(size_px, size_px, wxIMAGE_QUALITY_HIGH);
  return wxBitmap(image);
}

struct ContentItem {
  wxString name;
  uint64_t size_bytes = 0;
};

kernel::xam::XamState* XamStateOf(EmulatorWindow* emulator_window) {
  auto* emulator = emulator_window ? emulator_window->emulator() : nullptr;
  auto* kernel_state = emulator ? emulator->kernel_state() : nullptr;
  return kernel_state ? kernel_state->xam_state() : nullptr;
}

// Bytes held by a file, or by everything under a directory.
uint64_t PathSize(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
  }
  uint64_t total = 0;
  std::filesystem::recursive_directory_iterator it(path, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      const auto size = it->file_size(ec);
      if (!ec) {
        total += size;
      }
    }
  }
  return total;
}

wxString FormatSize(uint64_t bytes) {
  static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  const std::string text = unit == 0
                               ? fmt::format("{} {}", bytes, kUnits[unit])
                               : fmt::format("{:.1f} {}", value, kUnits[unit]);
  return wxString::FromUTF8(text);
}

wxString FormatLastPlayed(time_t timestamp) {
  if (timestamp == 0) {
    return _("Never");
  }
  std::tm tm = {};
#if XE_PLATFORM_WIN32
  localtime_s(&tm, &timestamp);
#else
  localtime_r(&timestamp, &tm);
#endif
  return wxString::FromUTF8(fmt::format("{:%Y-%m-%d %H:%M}", tm));
}

// Installed content of one type. The content manager resolves the friendly
// name out of each package header; without a kernel we can only show the file
// names on disk.
std::vector<ContentItem> ListContentItems(kernel::xam::ContentManager* manager,
                                          const std::filesystem::path& root,
                                          uint64_t xuid, uint32_t title_id,
                                          XContentType content_type) {
  std::vector<ContentItem> items;
  if (manager) {
    for (const auto& data :
         manager->ListContent(0, xuid, title_id, content_type)) {
      const auto display_name = xe::to_utf8(data.display_name());
      ContentItem item;
      item.name = wxString::FromUTF8(display_name.empty() ? data.file_name()
                                                          : display_name);
      item.size_bytes = PathSize(root / xe::to_path(data.file_name()));
      items.push_back(std::move(item));
    }
    return items;
  }
  for (const auto& info : xe::filesystem::ListFiles(root)) {
    ContentItem item;
    item.name = wxString::FromUTF8(xe::path_to_utf8(info.name));
    item.size_bytes = PathSize(info.path / info.name);
    items.push_back(std::move(item));
  }
  return items;
}

}  // namespace

GameInfoPanel::GameInfoPanel(wxWindow* parent, EmulatorWindow* emulator_window)
    : wxPanel(parent, wxID_ANY), emulator_window_(emulator_window) {
  // Font-measured metrics scale with DPI and text scaling.
  wxClientDC dc(this);
  dc.SetFont(GetFont());
  char_w_ = dc.GetCharWidth();
  char_h_ = dc.GetCharHeight();

  header_panel_ = new wxPanel(this, wxID_ANY);
  notebook_ = new wxNotebook(this, wxID_ANY);
  auto add_page = [this](const wxString& label) {
    auto* page = new wxScrolledWindow(notebook_, wxID_ANY);
    page->SetScrollRate(0, 16);
    notebook_->AddPage(page, label);
    return page;
  };
  pages_[kPageInfo] = add_page(_("Info"));
  pages_[kPageContent] = add_page(_("Content"));
  pages_[kPageSaves] = add_page(_("Saves"));
  pages_[kPagePatches] = add_page(_("Patches"));
  pages_[kPageConfig] = add_page(_("Configuration"));
  notebook_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& event) {
    event.Skip();
    BuildPage(event.GetSelection());
  });

  // Outside the pages so it stays put through a Rebuild.
  auto* close_row = new wxBoxSizer(wxHORIZONTAL);
  close_row->AddStretchSpacer();
  // NewCloseButton() borrows the window title bar's close button, which the
  // Windows theme paints as a red square; draw the search bar's glyph instead.
  const int close_px = FromDIP(14);
  auto* close = new wxBitmapButton(
      this, wxID_ANY, MakeCloseBitmap(this, close_px, kCloseLightness),
      wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
  close->SetBitmapCurrent(MakeCloseBitmap(this, close_px, 200));
  close->SetBackgroundColour(GetBackgroundColour());
  close->SetToolTip(_("Close the info pane"));
  close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    if (close_cb_) {
      close_cb_();
    }
  });
  close_row->Add(close, 0);

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(close_row, 0, wxEXPAND | wxTOP | wxRIGHT, 4);
  sizer->Add(header_panel_, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
  sizer->Add(notebook_, 1, wxEXPAND | wxALL, 6);
  SetSizer(sizer);

  refresh_timer_.SetOwner(this);
  Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Rebuild(); });

  Rebuild();
}

kernel::xam::ProfileManager* GameInfoPanel::profile_manager() const {
  auto* xam_state = XamStateOf(emulator_window_);
  return xam_state ? xam_state->profile_manager() : nullptr;
}

kernel::xam::ContentManager* GameInfoPanel::content_manager() const {
  auto* xam_state = XamStateOf(emulator_window_);
  return xam_state ? xam_state->content_manager() : nullptr;
}

void GameInfoPanel::ShowTitle(const LibraryKey& key, CompatState compat,
                              const TitleStats& stats) {
  key_ = key;
  compat_ = compat;
  stats_ = stats;
  refresh_timer_.StartOnce(kRefreshDelayMs);
}

void GameInfoPanel::ShowNoSelection() {
  key_ = LibraryKey();
  stats_ = TitleStats();
  refresh_timer_.StartOnce(kRefreshDelayMs);
}

wxBoxSizer* GameInfoPanel::BeginPage(wxWindow* page) {
  page->DestroyChildren();
  page->SetSizer(nullptr);
  return new wxBoxSizer(wxVERTICAL);
}

void GameInfoPanel::EndPage(wxScrolledWindow* page, wxBoxSizer* sizer) {
  auto* outer = new wxBoxSizer(wxVERTICAL);
  outer->Add(sizer, 1, wxEXPAND | wxALL, 10);
  page->SetSizer(outer);
  page->FitInside();
}

void GameInfoPanel::Rebuild() {
  Freeze();
  discs_header_ = nullptr;
  disc_list_ = nullptr;
  remove_button_ = nullptr;
  disc_paths_.clear();

  // Every page holds the previous title now, so empty them all; only the one
  // on screen is refilled, the rest when the user switches to them.
  for (auto* page : pages_) {
    page->DestroyChildren();
    page->SetSizer(nullptr);
  }
  page_stale_.fill(true);

  auto* header_sizer = BeginPage(header_panel_);

  if (key_.title_id == 0) {
    auto* label = new wxStaticText(header_panel_, wxID_ANY,
                                   _("Select a game for details."));
    label->SetForegroundColour(
        wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    header_sizer->Add(label, 0, wxTOP | wxBOTTOM, 6);
    header_panel_->SetSizer(header_sizer);
    notebook_->Hide();
    Layout();
    Thaw();
    return;
  }

  notebook_->Show();
  BuildHeader(header_panel_, header_sizer);
  header_panel_->SetSizer(header_sizer);

  BuildPage(notebook_->GetSelection());

  Layout();
  Thaw();
}

void GameInfoPanel::BuildPage(int page) {
  if (page < 0 || page >= kPageCount || !page_stale_[page] || !key_.title_id) {
    return;
  }
  page_stale_[page] = false;

  auto* window = pages_[page];
  auto* sizer = BeginPage(window);
  switch (page) {
    case kPageInfo:
      BuildDetailsSection(window, sizer);
      BuildCompatSection(window, sizer);
      break;
    case kPageContent:
      BuildDiscsSection(window, sizer);
      BuildContentSection(window, sizer, _("Title Updates (%zu)"),
                          {{XContentType::kInstaller, 0}}, wxString(),
                          /*allow_import=*/true);
      BuildContentSection(window, sizer, _("Installed Content (%zu)"),
                          {{XContentType::kMarketplaceContent, 0}}, wxString(),
                          /*allow_import=*/true);
      break;
    case kPageSaves: {
      // Saves live under the signed-in profile. Some titles write into the
      // shared profile's save folder instead, and a few put install data
      // there, but the folder is a save location whatever lands in it, so it
      // is listed here rather than with installed content.
      auto* profiles = profile_manager();
      auto* primary = profiles ? profiles->GetProfile(uint8_t(0)) : nullptr;
      std::vector<ContentLocation> locations;
      if (primary) {
        locations.push_back({XContentType::kSavedGame, primary->xuid()});
      }
      locations.push_back({XContentType::kSavedGame, 0});
      BuildContentSection(window, sizer, _("Saves (%zu)"), std::move(locations),
                          primary ? wxString() : _("No profile is signed in."));
      break;
    }
    case kPagePatches:
      BuildPatchesSection(window, sizer);
      break;
    case kPageConfig:
      BuildConfigSection(window, sizer);
      break;
    default:
      break;
  }
  EndPage(window, sizer);
  window->Layout();
}

void GameInfoPanel::BuildHeader(wxWindow* parent, wxBoxSizer* sizer) {
  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  const LibraryEntry* entry = library ? library->Find(key_) : nullptr;

  auto* row = new wxBoxSizer(wxHORIZONTAL);

  const int art_px = FromDIP(kArtSize);
  const double scale = GetDPIScaleFactor();
  wxBitmapBundle art;
  if (library) {
    art = ui::DecodePngIcon(
        xe::filesystem::ReadAllBytes(library->IconPath(key_)), art_px, scale);
  }
  if (!art.IsOk()) {
    art = ui::MakeTextPlaceholder(_("No art"), art_px, scale);
  }
  row->Add(new wxStaticBitmap(parent, wxID_ANY, art), 0, wxRIGHT, 10);

  auto* column = new wxBoxSizer(wxVERTICAL);

  wxString name = entry && !entry->name.empty()
                      ? wxString::FromUTF8(entry->name)
                      : _("Unknown title");
  auto* name_row = new wxBoxSizer(wxHORIZONTAL);
  auto* name_label = MakeElidedLabel(parent, name);
  wxFont name_font = name_label->GetFont();
  name_font.Scale(1.3f);
  name_font.MakeBold();
  name_label->SetFont(name_font);
  name_row->Add(name_label, 1, wxALIGN_CENTER_VERTICAL);

  auto* rename = new wxButton(parent, wxID_ANY, _("Rename..."),
                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  rename->SetToolTip(_("Give this version its own name"));
  rename->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnRenameTitle(); });
  name_row->Add(rename, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
  column->Add(name_row, 0, wxEXPAND | wxBOTTOM, 6);

  // SPA-driven totals aren't known until first launch, so show "?" rather
  // than a misleading 0/0 for titles that have never been launched.
  wxString achievements = "?";
  wxString gamerscore = "?";
  if (stats_.achievements_total > 0 || stats_.last_played != 0) {
    achievements = wxString::FromUTF8(fmt::format(
        "{}/{}", stats_.achievements_unlocked, stats_.achievements_total));
    gamerscore = wxString::FromUTF8(fmt::format(
        "{}/{} G", stats_.gamerscore_earned, stats_.gamerscore_total));
  }

  auto* fields = AddFieldGrid(column);
  AddField(parent, fields, _("Last Played"),
           FormatLastPlayed(stats_.last_played));
  AddField(parent, fields, _("Achievements"), achievements);
  AddField(parent, fields, _("Gamerscore"), gamerscore);

  row->Add(column, 1, wxALIGN_CENTER_VERTICAL);
  sizer->Add(row, 0, wxEXPAND);
}

void GameInfoPanel::BuildDetailsSection(wxWindow* parent, wxBoxSizer* sizer) {
  AddSectionHeader(parent, sizer, _("Details"), {});

  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  const LibraryEntry* entry = library ? library->Find(key_) : nullptr;

  auto* grid = AddFieldGrid(sizer);
  // Not translated: these name a field of the console's own headers.
  AddField(parent, grid, "Title ID", wxString::Format("%08X", key_.title_id),
           /*selectable=*/true);
  // The disc that launches. A set has one media id per disc, which is why the
  // version and not this names the release. The Content tab lists them all.
  wxString media_id = _("Unknown");
  if (entry && !entry->paths.empty() && entry->default_path().media_id) {
    media_id = wxString::Format("%08X", entry->default_path().media_id);
  }
  AddField(parent, grid, "Media ID", media_id, /*selectable=*/true);
  AddField(parent, grid, _("Version"),
           wxString::FromUTF8(VersionToString(key_.version)),
           /*selectable=*/true);
  auto* status = AddField(parent, grid, _("Status"), CompatStateName(compat_));
  status->SetForegroundColour(CompatColor(compat_));
}

void GameInfoPanel::BuildCompatSection(wxWindow* parent, wxBoxSizer* sizer) {
  AddSectionHeader(parent, sizer, _("Compatibility"), {});

  if (GetCompatState(key_.title_id) == CompatState::kUnknown) {
    AddNoteRow(parent, sizer,
               _("This title is not in the compatibility database."));
    return;
  }

  // Only a report we can link to directly earns a button. A search of the
  // tracker is not a report, so a tracker without one is left unmentioned.
  const CompatUrls urls = GetCompatUrls(key_.title_id);
  if (urls.empty()) {
    AddNoteRow(parent, sizer, _("No compatibility report is linked."));
    return;
  }
  auto add_link = [&](const wxString& label, const std::string& target) {
    if (target.empty()) {
      return;
    }
    auto* button = new wxButton(parent, wxID_ANY, label);
    button->Bind(wxEVT_BUTTON,
                 [target](wxCommandEvent&) { xe::LaunchWebBrowser(target); });
    sizer->Add(button, 0, wxBOTTOM, 2);
  };
  add_link(_("Canary"), urls.canary);
  add_link(_("Master"), urls.master);
}

void GameInfoPanel::BuildDiscsSection(wxWindow* parent, wxBoxSizer* sizer) {
  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  const LibraryEntry* entry = library ? library->Find(key_) : nullptr;

  // The folder button opens where the default disc lives. Unlike the content
  // roots this is not ours to create, so only offer it when it is there.
  std::filesystem::path disc_dir;
  std::error_code ec;
  if (entry && !entry->paths.empty()) {
    disc_dir = entry->default_path().path.parent_path();
    if (!std::filesystem::exists(disc_dir, ec)) {
      disc_dir.clear();
    }
  }
  discs_header_ = AddSectionHeader(parent, sizer, wxString(), disc_dir);

  // Font-measured widths feed wxDataViewCtrl as device pixels, so no FromDIP.
  wxClientDC dc(parent);
  dc.SetFont(parent->GetFont());
  auto fit = [&](const wxString& header, const wxString& sample) {
    return std::max(dc.GetTextExtent(header).GetWidth(),
                    dc.GetTextExtent(sample).GetWidth()) +
           char_w_ * 3;
  };
  // No wxDV_ROW_LINES: alternating row colours read as a selection against
  // the pane's own background, and the rows are short enough not to need it.
  disc_list_ = new wxDataViewListCtrl(parent, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, wxDV_SINGLE);
  // Unheaded: the word is long in some languages for a column this narrow,
  // and the marker reads on its own against the row it sits in.
  disc_list_->AppendTextColumn(wxString(), wxDATAVIEW_CELL_INERT,
                               fit(wxString(), "[*]"), wxALIGN_CENTER, 0);
  // Each disc of a set has its own, which is why the release is not named
  // after one of them.
  disc_list_->AppendTextColumn("Media ID", wxDATAVIEW_CELL_INERT,
                               fit("Media ID", "00000000"), wxALIGN_LEFT, 0);
  // Wider than the pane, so the list scrolls to it. The renderer still
  // ellipsizes a path longer than the column, which the tooltip below covers.
  disc_list_->AppendTextColumn(_("Path"), wxDATAVIEW_CELL_INERT, char_w_ * 40,
                               wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
  sizer->Add(disc_list_, 0, wxEXPAND);

  // wxDataViewCtrl has no per-item tooltip, so the row under the pointer sets
  // the control's. Mouse events land on the interior window, whose origin sits
  // below the header, so the point has to be put back into list coordinates.
  auto* list_area = disc_list_->GetMainWindow();
  list_area->Bind(wxEVT_MOTION, [this, list_area](wxMouseEvent& event) {
    event.Skip();
    const wxPoint in_list = disc_list_->ScreenToClient(
        list_area->ClientToScreen(event.GetPosition()));
    wxDataViewItem item;
    wxDataViewColumn* column = nullptr;
    disc_list_->HitTest(in_list, item, column);
    const int row = item.IsOk() ? disc_list_->ItemToRow(item) : wxNOT_FOUND;
    SetDiscTooltip(row);
  });
  list_area->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
    event.Skip();
    SetDiscTooltip(wxNOT_FOUND);
  });

  auto* buttons = new wxBoxSizer(wxHORIZONTAL);
  remove_button_ = new wxButton(parent, wxID_ANY, _("Remove"));
  remove_button_->Bind(wxEVT_BUTTON,
                       [this](wxCommandEvent&) { OnRemoveDisc(); });
  buttons->Add(remove_button_, 0);
  sizer->Add(buttons, 0, wxTOP, 6);

  ReloadDiscs();
}

void GameInfoPanel::SetDiscTooltip(int row) {
  if (row == disc_tooltip_row_) {
    return;
  }
  disc_tooltip_row_ = row;
  if (row == wxNOT_FOUND || static_cast<size_t>(row) >= disc_paths_.size()) {
    disc_list_->UnsetToolTip();
    return;
  }
  disc_list_->SetToolTip(
      wxString::FromUTF8(xe::path_to_utf8(disc_paths_[row])));
}

void GameInfoPanel::ReloadDiscs() {
  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  const LibraryEntry* entry = library ? library->Find(key_) : nullptr;

  disc_paths_.clear();
  disc_list_->DeleteAllItems();
  // Rows are about to change under it.
  disc_tooltip_row_ = wxNOT_FOUND;
  disc_list_->UnsetToolTip();
  if (entry) {
    const std::filesystem::path default_path = entry->paths.empty()
                                                   ? std::filesystem::path()
                                                   : entry->default_path().path;
    for (const auto& p : entry->paths) {
      wxVector<wxVariant> row;
      row.push_back(
          wxVariant(p.path == default_path ? wxString("[*]") : wxString()));
      row.push_back(wxVariant(p.media_id ? wxString::Format("%08X", p.media_id)
                                         : wxString()));
      row.push_back(wxVariant(wxString::FromUTF8(xe::path_to_utf8(p.path))));
      disc_list_->AppendItem(row);
      disc_paths_.push_back(p.path);
    }
  }

  discs_header_->SetLabel(
      wxString::Format(_("Discs (%zu)"), disc_paths_.size()));

  const bool has_discs = !disc_paths_.empty();
  if (has_discs) {
    disc_list_->SelectRow(0);
  }
  remove_button_->Enable(has_discs);

  // wx sizes rows from the UI font, which has no glyphs for every script it
  // has to show. The fallback face is taller than that font reports, so its
  // text clips inside the row: give rows more room than wx would, and size
  // the pane from the same number so the two agree.
  const int row_h = std::max(char_h_, FromDIP(16)) + FromDIP(10);
  disc_list_->SetRowHeight(row_h);
  // Size to the row count so the pane scrolls, not the list inside it.
  const size_t visible =
      std::clamp<size_t>(disc_paths_.size(), 1, kMaxVisibleDiscRows);
  disc_list_->SetMinSize(
      wxSize(-1, row_h * static_cast<int>(visible + 1) + FromDIP(6)));
}

void GameInfoPanel::BuildContentSection(wxWindow* parent, wxBoxSizer* sizer,
                                        const wxString& header_format,
                                        std::vector<ContentLocation> locations,
                                        const wxString& note,
                                        bool allow_import) {
  auto* profiles = profile_manager();
  std::filesystem::path root;
  std::vector<ContentItem> items;
  if (note.empty() && profiles && !locations.empty()) {
    root = profiles->GetProfileContentPath(
        locations.front().xuid, key_.title_id, locations.front().content_type);
    for (const auto& location : locations) {
      auto found = ListContentItems(
          content_manager(),
          profiles->GetProfileContentPath(location.xuid, key_.title_id,
                                          location.content_type),
          location.xuid, key_.title_id, location.content_type);
      items.insert(items.end(), std::make_move_iterator(found.begin()),
                   std::make_move_iterator(found.end()));
    }
  }

  AddSectionHeader(parent, sizer, wxString::Format(header_format, items.size()),
                   root,
                   allow_import ? locations : std::vector<ContentLocation>());

  if (!note.empty()) {
    AddNoteRow(parent, sizer, note);
    return;
  }
  if (items.empty()) {
    AddNoteRow(parent, sizer, _("Nothing installed."));
    return;
  }

  auto* grid = new wxFlexGridSizer(2, FromDIP(2), char_w_ * 2);
  grid->AddGrowableCol(0);
  for (const auto& item : items) {
    grid->Add(MakeElidedLabel(parent, item.name), 0, wxEXPAND);
    grid->Add(new wxStaticText(parent, wxID_ANY, FormatSize(item.size_bytes)),
              0, wxALIGN_RIGHT);
  }
  sizer->Add(grid, 0, wxEXPAND);
}

void GameInfoPanel::BuildPatchesSection(wxWindow* parent, wxBoxSizer* sizer) {
  auto bundled = patcher::EnumerateBundledPatchesForTitle(key_.title_id);
  if (bundled.empty()) {
    AddSectionHeader(parent, sizer, _("Patches"), {});
    AddNoteRow(parent, sizer, _("No patches are bundled for this title."));
    return;
  }
  // One editor per bundled file, headed by the file it writes to.
  for (const auto& file : bundled) {
    AddSectionHeader(parent, sizer,
                     wxString::FromUTF8(patcher::BundledPatchDisplayName(file)),
                     {});
    auto* patches = new PatchesPanel(parent, emulator_window_, file);
    // Wrapping a description changes its height, so the page must re-measure.
    patches->SetContentChangedCallback(
        [this]() { pages_[kPagePatches]->FitInside(); });
    sizer->Add(patches, 0, wxEXPAND);
  }
}

void GameInfoPanel::BuildConfigSection(wxWindow* parent, wxBoxSizer* sizer) {
  auto* editor = new GameConfigPanel(parent, emulator_window_, key_.title_id);
  // Adding or removing a row changes its height, so the page must re-measure.
  editor->SetContentChangedCallback(
      [this]() { pages_[kPageConfig]->FitInside(); });
  sizer->Add(editor, 1, wxEXPAND);
}

void GameInfoPanel::ImportContent(
    const std::vector<ContentLocation>& locations) {
  auto* profiles = profile_manager();
  if (!profiles || locations.empty()) {
    return;
  }
  wxFileDialog picker(wxGetTopLevelParent(this), _("Select content packages"),
                      wxString(), wxString(), _("All Files (*.*)|*.*"),
                      wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
  if (picker.ShowModal() != wxID_OK) {
    return;
  }
  wxArrayString picked;
  picker.GetPaths(picked);

  wxArrayString rejected;
  size_t added = 0;
  for (const auto& picked_path : picked) {
    const auto source = xe::to_path(picked_path.utf8_string());
    const wxString name =
        wxString::FromUTF8(xe::path_to_utf8(source.filename()));

    // Same header the content list reads to name these packages.
    const auto header =
        vfs::XContentContainerDevice::ReadContainerHeader(source);
    if (!header || !header->content_header.is_magic_valid()) {
      rejected.Add(wxString::Format(_("%s is not a content package."), name));
      continue;
    }
    const auto data =
        vfs::XContentContainerDevice::ContentDataFromHeader(*header);
    if (data.title_id != key_.title_id) {
      rejected.Add(wxString::Format(_("%s belongs to title %08X."), name,
                                    data.title_id.get()));
      continue;
    }
    // A section can cover more than one folder, so the package's own type
    // picks the one it lands in.
    const auto location =
        std::find_if(locations.begin(), locations.end(),
                     [&data](const ContentLocation& candidate) {
                       return data.content_type == candidate.content_type;
                     });
    if (location == locations.end()) {
      rejected.Add(
          wxString::Format(_("%s is a different kind of content."), name));
      continue;
    }
    // A title update's header describes the release it patches rather than
    // itself, so its version names that release the same way ours does. Media
    // id cannot: a set has one per disc, and the update carries only the disc
    // it was built against. An unknown on either side skips the check rather
    // than refusing on missing information.
    const uint32_t version =
        header->content_metadata.execution_info.version_value;
    if (data.content_type == XContentType::kInstaller && key_.version &&
        version && version != key_.version) {
      rejected.Add(wxString::Format(
          _("%s is for a different release of this game."), name));
      continue;
    }

    const auto root = profiles->GetProfileContentPath(
        location->xuid, key_.title_id, location->content_type);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const auto destination = root / source.filename();
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
      rejected.Add(wxString::Format(_("%s could not be copied: %s"), name,
                                    wxString::FromUTF8(ec.message())));
      continue;
    }
    ++added;
  }

  if (!rejected.IsEmpty()) {
    wxMessageBox(wxJoin(rejected, '\n'), _("Some packages were not added"),
                 wxOK | wxICON_WARNING, wxGetTopLevelParent(this));
  }
  if (added) {
    // Deferred: this runs from the Add button, which the rebuild destroys.
    CallAfter([this]() { Rebuild(); });
  }
}

wxStaticText* GameInfoPanel::AddSectionHeader(
    wxWindow* parent, wxBoxSizer* sizer, const wxString& text,
    const std::filesystem::path& open_path,
    std::vector<ContentLocation> import_locations) {
  auto* row = new wxBoxSizer(wxHORIZONTAL);
  auto* label = new wxStaticText(parent, wxID_ANY, text);
  label->SetFont(label->GetFont().Bold());
  row->Add(label, 0, wxALIGN_CENTER_VERTICAL);
  row->AddStretchSpacer();

  if (!import_locations.empty() && !open_path.empty()) {
    auto* add = new wxButton(parent, wxID_ANY, _("Add..."), wxDefaultPosition,
                             wxDefaultSize, wxBU_EXACTFIT);
    add->Bind(wxEVT_BUTTON, [this, import_locations](wxCommandEvent&) {
      ImportContent(import_locations);
    });
    row->Add(add, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
  }

  if (!open_path.empty()) {
    auto* open = new wxButton(parent, wxID_ANY, _("Open folder"),
                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    open->Bind(wxEVT_BUTTON, [open_path](wxCommandEvent&) {
      // A content folder only exists once something is in it.
      std::error_code ec;
      std::filesystem::create_directories(open_path, ec);
      std::thread(xe::LaunchFileExplorer, open_path).detach();
    });
    row->Add(open, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
  }

  sizer->Add(row, 0, wxEXPAND | wxTOP, 12);
  sizer->Add(new wxStaticLine(parent), 0, wxEXPAND | wxBOTTOM, 6);
  return label;
}

wxFlexGridSizer* GameInfoPanel::AddFieldGrid(wxBoxSizer* sizer) {
  auto* grid = new wxFlexGridSizer(2, FromDIP(2), char_w_ * 2);
  grid->AddGrowableCol(1);
  sizer->Add(grid, 0, wxEXPAND);
  return grid;
}

wxWindow* GameInfoPanel::AddField(wxWindow* parent, wxFlexGridSizer* grid,
                                  const wxString& label, const wxString& value,
                                  bool selectable) {
  auto* name = new wxStaticText(parent, wxID_ANY, label);
  name->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
  grid->Add(name, 0, wxALIGN_CENTER_VERTICAL);

  wxWindow* value_window = nullptr;
  if (selectable) {
    // Static text can't be selected, so a read-only entry stands in; without
    // its border and with the pane's colours it still reads as a label.
    auto* field = new wxTextCtrl(parent, wxID_ANY, value, wxDefaultPosition,
                                 wxDefaultSize, wxTE_READONLY | wxBORDER_NONE);
    field->SetBackgroundColour(parent->GetBackgroundColour());
    field->SetForegroundColour(parent->GetForegroundColour());
    value_window = field;
  } else {
    value_window = new wxStaticText(parent, wxID_ANY, value);
  }
  grid->Add(value_window, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
  return value_window;
}

void GameInfoPanel::AddNoteRow(wxWindow* parent, wxBoxSizer* sizer,
                               const wxString& text) {
  auto* label = new wxStaticText(parent, wxID_ANY, text);
  label->SetForegroundColour(
      wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
  sizer->Add(label, 0);
}

wxStaticText* GameInfoPanel::MakeElidedLabel(wxWindow* parent,
                                             const wxString& text) {
  auto* label = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition,
                                 wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
  label->SetToolTip(text);
  // Ellipsizing only changes what is drawn: the best size is still the whole
  // string, which would widen the page past the pane instead of shortening.
  label->SetMinSize(wxSize(FromDIP(60), -1));
  return label;
}

void GameInfoPanel::OnRenameTitle() {
  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  const auto* found = library ? library->Find(key_) : nullptr;
  if (!found) {
    return;
  }

  wxTextEntryDialog input(wxGetTopLevelParent(this),
                          _("Enter a name for this version:"), _("Rename Game"),
                          wxString::FromUTF8(found->name));
  if (input.ShowModal() != wxID_OK) {
    return;
  }
  const std::string new_name = input.GetValue().utf8_string();
  // An empty name would leave the card labelled as though it were broken.
  if (new_name.empty() || new_name == found->name) {
    return;
  }

  // Mutate a copy: Upsert may append to the vector `found` points into.
  LibraryEntry entry = *found;
  entry.name = new_name;
  if (!library->Upsert(std::move(entry))) {
    return;
  }

  // Deferred: this runs from the Rename button, which the rebuild destroys.
  CallAfter([this]() {
    Rebuild();
    if (library_changed_cb_) {
      library_changed_cb_();
    }
  });
}

void GameInfoPanel::OnRemoveDisc() {
  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  const int sel = disc_list_->GetSelectedRow();
  if (!library || sel == wxNOT_FOUND ||
      static_cast<size_t>(sel) >= disc_paths_.size()) {
    return;
  }
  const auto row = static_cast<unsigned int>(sel);
  const auto disc_path = disc_paths_[row];
  const auto* found = library->Find(key_);
  if (!found) {
    return;
  }

  wxMessageDialog confirm(
      wxGetTopLevelParent(this),
      wxString::Format(_("Remove '%s' from the disc list?"),
                       wxString::FromUTF8(xe::path_to_utf8(disc_path))),
      _("Remove Disc"), wxYES_NO | wxICON_WARNING);
  if (confirm.ShowModal() != wxID_YES) {
    return;
  }

  // Mutate a copy: Upsert may append to the vector `found` points into.
  LibraryEntry entry = *found;
  entry.paths.erase(
      std::remove_if(entry.paths.begin(), entry.paths.end(),
                     [&](const LibraryPath& p) { return p.path == disc_path; }),
      entry.paths.end());
  if (entry.paths.empty()) {
    // The last disc is gone, so the title has nothing left to launch.
    library->Remove(key_);
  } else if (!library->Upsert(std::move(entry))) {
    return;
  }

  // Deferred: this runs from the Remove button's own handler, and the rebuild
  // destroys that button.
  CallAfter([this]() {
    Rebuild();
    if (library_changed_cb_) {
      library_changed_cb_();
    }
  });
}

}  // namespace app
}  // namespace xe
