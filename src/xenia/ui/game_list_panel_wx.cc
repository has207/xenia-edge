/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_list_panel_wx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <unordered_map>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/filedlg.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>

#include "xenia/app/emulator_window.h"
#include "xenia/app/game_compat_db.h"
#include "xenia/base/chrono.h"
#include "xenia/base/chrono_steady_cast.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_profile.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/ui/compat_display_wx.h"
#include "xenia/ui/icon_decode.h"

namespace xe {
namespace app {

namespace {

// Card art edge in logical DIPs at 96 DPI; the constructor converts to device
// pixels via FromDIP() for the current monitor. Stored art is 64x64, so this
// upscales.
constexpr int kCardArtSize = 96;

// Arrows are glyphs, not prose, so they stay out of the catalog.
wxString SortArrow(bool descending) {
  return wxString::FromUTF8(descending ? "\xe2\x86\x93" : "\xe2\x86\x91");
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

// Antialiased filled circle used as the compatibility badge on a card.
wxBitmap MakeCompatBall(CompatState state, int size_px) {
  const float radius = size_px * 0.42f;
  wxColour color = CompatColor(state);
  wxImage image(size_px, size_px);
  image.SetAlpha();
  unsigned char* rgb = image.GetData();
  unsigned char* alpha = image.GetAlpha();
  std::memset(alpha, 0, static_cast<size_t>(size_px) * size_px);
  const float cx = size_px * 0.5f;
  const float cy = size_px * 0.5f;
  for (int y = 0; y < size_px; ++y) {
    for (int x = 0; x < size_px; ++x) {
      float dx = x + 0.5f - cx;
      float dy = y + 0.5f - cy;
      float d = std::sqrt(dx * dx + dy * dy);
      float a = std::clamp(radius - d + 0.5f, 0.0f, 1.0f);
      if (a > 0.0f) {
        size_t pi = (static_cast<size_t>(y) * size_px + x) * 3;
        rgb[pi + 0] = color.Red();
        rgb[pi + 1] = color.Green();
        rgb[pi + 2] = color.Blue();
        alpha[y * size_px + x] = static_cast<unsigned char>(a * 255.0f + 0.5f);
      }
    }
  }
  return wxBitmap(image);
}

}  // namespace

GameListPanel::GameListPanel(wxWindow* parent, EmulatorWindow* emulator_window)
    : wxPanel(parent, wxID_ANY), emulator_window_(emulator_window) {
  splitter_ =
      new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxSP_LIVE_UPDATE | wxSP_THIN_SASH);
  list_side_ = new wxPanel(splitter_, wxID_ANY);
  info_panel_ = new GameInfoPanel(splitter_, emulator_window);
  info_panel_->SetLibraryChangedCallback([this]() { Reload(); });
  info_panel_->SetCloseCallback([this]() {
    info_pane_closed_ = true;
    ShowInfoPane(false);
  });

  search_ = new wxSearchCtrl(list_side_, wxID_ANY);
  search_->ShowCancelButton(true);
  search_->SetDescriptiveText(_("Search games..."));
  {
    wxFont f = search_->GetFont();
    f.Scale(1.4f);
    search_->SetFont(f);
  }

  sort_choice_ = new wxChoice(list_side_, wxID_ANY);
  sort_choice_->Append(_("Last Played"));
  sort_choice_->Append(_("Title"));
  sort_choice_->Append(_("Status"));
  sort_choice_->SetSelection(0);
  sort_dir_button_ =
      new wxButton(list_side_, wxID_ANY, SortArrow(sort_descending_),
                   wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
  sort_dir_button_->SetToolTip(_("Reverse the sort order"));

  // Card art is expressed in logical DIPs; the grid draws bitmaps at device
  // resolution, so everything downstream of here builds them at scale 1.
  icon_size_px_ = FromDIP(kCardArtSize);
  not_played_placeholder_ =
      ui::MakeTextPlaceholder(_("Not\nplayed"), icon_size_px_, 1.0)
          .GetBitmap(wxSize(icon_size_px_, icon_size_px_));
  const int ball_size_px = std::max(FromDIP(10), icon_size_px_ / 5);
  for (size_t i = 0; i < compat_balls_.size(); ++i) {
    compat_balls_[i] =
        MakeCompatBall(static_cast<CompatState>(i), ball_size_px);
  }

  grid_ = new ui::GameGrid(list_side_, icon_size_px_);
  grid_->SetPlaceholder(not_played_placeholder_);
  grid_->SetMinSize(wxSize(0, 0));
  grid_->SetSelectionChangedCallback([this]() { OnSelectionChanged(); });
  grid_->SetActivatedCallback([this](int index) { OnItemActivated(index); });
  grid_->SetContextMenuCallback(
      [this](int index, const wxPoint&) { OnItemContextMenu(index); });

  // Derive sizes from the panel's actual font so the layout grows with the
  // user's text-scaling setting instead of clipping at fixed pixels.
  wxClientDC dc(this);
  dc.SetFont(GetFont());
  const int char_w = dc.GetCharWidth();

  search_->Bind(wxEVT_TEXT, &GameListPanel::OnSearch, this);
  search_->Bind(wxEVT_SEARCH_CANCEL,
                [this](wxCommandEvent&) { search_->Clear(); });
  sort_choice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
    switch (sort_choice_->GetSelection()) {
      case 1:
        sort_key_ = SortKey::kTitle;
        sort_descending_ = false;
        break;
      case 2:
        sort_key_ = SortKey::kStatus;
        sort_descending_ = false;
        break;
      default:
        sort_key_ = SortKey::kLastPlayed;
        sort_descending_ = true;
        break;
    }
    OnSortChanged();
  });
  sort_dir_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
    sort_descending_ = !sort_descending_;
    OnSortChanged();
  });

  loading_panel_ = new wxPanel(list_side_, wxID_ANY);
  {
    auto* loading_sizer = new wxBoxSizer(wxVERTICAL);
    loading_sizer->AddStretchSpacer(1);
    auto* loading_label =
        new wxStaticText(loading_panel_, wxID_ANY, _("Loading titles..."));
    wxFont loading_font = loading_label->GetFont();
    loading_font.Scale(1.2f);
    loading_label->SetFont(loading_font);
    loading_sizer->Add(loading_label, 0, wxALIGN_CENTER_HORIZONTAL);
    loading_sizer->AddStretchSpacer(1);
    loading_panel_->SetSizer(loading_sizer);
  }
  grid_->Hide();

  auto* search_row = new wxBoxSizer(wxHORIZONTAL);
  search_row->Add(search_, wxSizerFlags(1).Expand());
  search_row->Add(sort_choice_,
                  wxSizerFlags().CenterVertical().Border(wxLEFT, 4));
  search_row->Add(sort_dir_button_,
                  wxSizerFlags().CenterVertical().Border(wxLEFT, 2));

  auto* side_sizer = new wxBoxSizer(wxVERTICAL);
  side_sizer->Add(search_row, wxSizerFlags().Expand().Border(wxALL, 4));
  side_sizer->Add(loading_panel_, wxSizerFlags(1).Expand().Border(
                                      wxLEFT | wxRIGHT | wxBOTTOM, 4));
  side_sizer->Add(
      grid_, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 4));
  list_side_->SetSizer(side_sizer);

  // Gravity 0 leaves the sash alone on resize; ApplySashPosition() places it
  // from the pane's share instead, which stays right across fold and unfold.
  splitter_->SetMinimumPaneSize(char_w * 24);
  splitter_->SetSashGravity(0.0);
  splitter_->Bind(
      wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& event) {
        event.Skip();
        const int width = GetClientSize().GetWidth();
        if (in_resize_ || width <= 0) {
          return;
        }
        pane_fraction_ = double(width - event.GetSashPosition()) / width;
      });
  info_panel_->Hide();
  splitter_->Initialize(list_side_);

  auto* sizer = new wxBoxSizer(wxVERTICAL);
  sizer->Add(splitter_, wxSizerFlags(1).Expand());
  SetSizer(sizer);

  // wxPanel's default size handler doesn't always relayout reliably when the
  // panel is mounted by AUI — call Layout() explicitly on every size event.
  Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
    in_resize_ = true;
    Layout();
    ApplySashPosition();
    in_resize_ = false;
    event.Skip();
  });
}

void GameListPanel::Reload() {
  entries_.clear();
  auto reveal_list = [this]() {
    if (loading_panel_ && loading_panel_->IsShown()) {
      loading_panel_->Hide();
      grid_->Show();
      list_side_->Layout();
    }
  };

  if (!emulator_window_) {
    Repopulate();
    reveal_list();
    return;
  }
  auto* library = emulator_window_->game_library();
  if (!library) {
    Repopulate();
    reveal_list();
    return;
  }

  const auto& games = library->entries();
  entries_.reserve(games.size());
  for (const auto& g : games) {
    Entry e;
    e.title_id = g.title_id;
    e.version = g.version;
    e.title_name = g.name;
    e.last_run_time = g.last_played;
    e.path = g.paths.empty() ? std::filesystem::path{} : g.default_path().path;
    e.discs.reserve(g.paths.size());
    for (const auto& p : g.paths) {
      e.discs.push_back(Disc{p.path});
    }
    entries_.push_back(std::move(e));
  }

  // Per-profile timestamps are cheap; icons are PNG decodes and dominate the
  // load. Apply timestamps now, render the list, then trickle icons in.
  LoadTimestampsFromProfiles();
  Repopulate();
  UpdateSearchPlaceholder();
  reveal_list();
}

void GameListPanel::UpdateSearchPlaceholder() {
  if (entries_.empty()) {
    search_->SetDescriptiveText(_("Search games..."));
    return;
  }
  search_->SetDescriptiveText(
      wxString::Format(_("Search %zu games..."), entries_.size()));
}

void GameListPanel::LoadTimestampsFromProfiles() {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }
  auto* kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }
  auto* xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return;
  }
  auto* profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    return;
  }

  std::unordered_map<uint32_t, size_t> releases_per_title;
  for (const auto& entry : entries_) {
    ++releases_per_title[entry.title_id];
  }

  for (uint8_t user_index = 0; user_index < 4; ++user_index) {
    auto* profile = profile_manager->GetProfile(user_index);
    if (!profile) {
      continue;
    }
    const auto& dashboard = profile->dashboard_gpd();
    if (!dashboard.IsValid()) {
      continue;
    }
    auto title_infos = dashboard.GetTitlesInfo();
    for (auto& entry : entries_) {
      // The GPD records one time per title, so it cannot say which release
      // ran. Take it only where the title has a single release, and only if
      // it beats what we have: a launch from outside the library lands there,
      // and this runs once per profile.
      const bool sole_release = releases_per_title[entry.title_id] == 1;
      for (const auto& info : title_infos) {
        if (!sole_release || info->title_id != entry.title_id ||
            !info->last_played.is_valid()) {
          continue;
        }
        auto last_played_tp =
            chrono::WinSystemClock::to_sys(info->last_played.to_time_point());
        const time_t last_played =
            std::chrono::system_clock::to_time_t(last_played_tp);
        if (last_played > entry.last_run_time) {
          entry.last_run_time = last_played;
        }
        break;
      }
      auto stats = profile->GetTitleAchievementStats(entry.title_id);
      if (stats.achievements_total > entry.achievements_total) {
        entry.achievements_total = stats.achievements_total;
        entry.achievements_unlocked = stats.achievements_unlocked;
        entry.gamerscore_total = stats.gamerscore_total;
        entry.gamerscore_earned = stats.gamerscore_earned;
      }
    }
  }
}

void GameListPanel::StartIconLoad() {
  ++icon_load_generation_;
  if (visible_indices_.empty()) {
    return;
  }
  int gen = icon_load_generation_;
  CallAfter([this, gen]() { ProcessIconChunk(0, gen); });
}

void GameListPanel::ProcessIconChunk(size_t start, int gen) {
  if (gen != icon_load_generation_) {
    return;
  }

  // Tune so each chunk stays well under one frame (~16ms): a PNG decode +
  // rescale is a few ms, so 8 per chunk keeps the UI responsive. Walk visible
  // rows so the cards on screen resolve first.
  constexpr size_t kChunkSize = 8;
  size_t end = std::min(start + kChunkSize, visible_indices_.size());
  for (size_t r = start; r < end; ++r) {
    auto& entry = entries_[visible_indices_[r]];
    if (!entry.card.IsOk()) {
      entry.card = MakeCard(entry);
      if (!entry.card.IsOk()) {
        continue;
      }
    }
    grid_->SetItemCard(r, entry.card);
  }
  if (end < visible_indices_.size()) {
    CallAfter([this, end, gen]() { ProcessIconChunk(end, gen); });
  }
}

wxBitmap GameListPanel::MakeCard(const Entry& entry) const {
  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  if (!library) {
    return wxBitmap();
  }
  // Scale 1: the image list addresses device pixels directly.
  wxBitmapBundle art = ui::DecodePngIcon(
      xe::filesystem::ReadAllBytes(library->IconPath(entry.key())),
      icon_size_px_, 1.0);

  wxBitmap card(icon_size_px_, icon_size_px_, 32);
  wxMemoryDC dc(card);
  dc.SetBackground(wxBrush(wxColour(40, 40, 40)));
  dc.Clear();
  dc.DrawBitmap(art.IsOk() ? art.GetBitmap(wxSize(icon_size_px_, icon_size_px_))
                           : not_played_placeholder_,
                0, 0, true);

  const wxBitmap& ball =
      compat_balls_[static_cast<size_t>(GetEntryCompatState(entry))];
  const int inset = std::max(1, icon_size_px_ / 32);
  dc.DrawBitmap(ball, icon_size_px_ - ball.GetWidth() - inset,
                icon_size_px_ - ball.GetHeight() - inset, true);
  dc.SelectObject(wxNullBitmap);
  return card;
}

void GameListPanel::OnSearch(wxCommandEvent&) {
  filter_lower_ = ToLower(search_->GetValue().utf8_string());
  Repopulate();
}

void GameListPanel::LaunchOrPrompt(const LibraryKey& key,
                                   const std::filesystem::path& path) {
  if (!launch_cb_) {
    return;
  }
  std::error_code ec;
  if (!path.empty() && std::filesystem::exists(path, ec)) {
    MarkPlayed(key);
    launch_cb_(path);
    return;
  }
  wxString warning = path.empty()
                         ? _("No file path is set for this title.")
                         : wxString::Format(_("File not found:\n%s"),
                                            wxString::FromUTF8(path.string()));
  wxMessageDialog confirm(this, warning + _("\n\nBrowse for the file?"),
                          _("Title not found"), wxYES_NO | wxICON_WARNING);
  if (confirm.ShowModal() != wxID_YES) {
    return;
  }
  std::filesystem::path initial_dir =
      path.empty() ? std::filesystem::path() : path.parent_path();
  wxFileDialog dlg(this, _("Select Content Package"),
                   initial_dir.empty()
                       ? wxString()
                       : wxString::FromUTF8(initial_dir.string()),
                   wxEmptyString,
                   _("Supported Files|*;*.iso;*.xex;*.zar|"
                     "Disc Image (*.iso)|*.iso|"
                     "Disc Archive (*.zar)|*.zar|"
                     "Xbox Executable (*.xex)|*.xex|"
                     "All Files (*.*)|*.*"),
                   wxFD_OPEN | wxFD_FILE_MUST_EXIST);
  if (dlg.ShowModal() == wxID_OK) {
    // Replacement chosen, drop the stale path.
    if (emulator_window_) {
      if (auto* library = emulator_window_->game_library()) {
        library->RemovePath(key, path);
      }
    }
    MarkPlayed(key);
    launch_cb_(std::filesystem::path(dlg.GetPath().utf8_string()));
  }
}

void GameListPanel::MarkPlayed(const LibraryKey& key) {
  auto* library = emulator_window_ ? emulator_window_->game_library() : nullptr;
  if (!library) {
    return;
  }
  const time_t now = std::time(nullptr);
  if (!library->MarkPlayed(key, now)) {
    return;
  }
  // Keep the row in step with what was just written, so a sort or the info
  // pane doesn't show a stale time until the next Reload().
  for (auto& entry : entries_) {
    if (entry.key() == key) {
      entry.last_run_time = now;
      break;
    }
  }
}

void GameListPanel::RefreshInfoPane() {
  if (!info_panel_) {
    return;
  }
  const Entry* entry = SelectedEntry();
  if (!entry) {
    // Keep the last id as a restore hint; a filter can hide the row without
    // the user having picked anything else.
    info_panel_->ShowNoSelection();
    ShowInfoPane(false);
    return;
  }
  ShowInfoPane(true);
  selected_key_ = entry->key();
  GameInfoPanel::TitleStats stats;
  stats.last_played = entry->last_run_time;
  stats.achievements_unlocked = entry->achievements_unlocked;
  stats.achievements_total = entry->achievements_total;
  stats.gamerscore_earned = entry->gamerscore_earned;
  stats.gamerscore_total = entry->gamerscore_total;
  info_panel_->ShowTitle(entry->key(), GetEntryCompatState(*entry), stats);
}

void GameListPanel::ShowInfoPane(bool show) {
  show = show && !info_pane_closed_;
  if (!splitter_ || !info_panel_ || show == splitter_->IsSplit()) {
    return;
  }
  if (!show) {
    splitter_->Unsplit(info_panel_);
    return;
  }
  // Sized from the width we have right now, never from one remembered while
  // the pane was folded away.
  const int width = GetClientSize().GetWidth();
  splitter_->SplitVertically(list_side_, info_panel_,
                             width - PaneWidthFor(width));
}

int GameListPanel::PaneWidthFor(int width) const {
  if (pane_fraction_ > 0.0) {
    return static_cast<int>(width * pane_fraction_ + 0.5);
  }
  // The right third, less a nudge that buys the grid one more column.
  return width - (width * 2 / 3 + FromDIP(10));
}

void GameListPanel::ApplySashPosition() {
  const int width = GetClientSize().GetWidth();
  if (!splitter_ || !splitter_->IsSplit() || width <= 0) {
    return;
  }
  splitter_->SetSashPosition(width - PaneWidthFor(width));
}

void GameListPanel::OnItemActivated(int index) {
  if (index < 0 || index >= static_cast<int>(visible_indices_.size())) {
    return;
  }
  size_t idx = visible_indices_[index];
  if (idx >= entries_.size()) {
    return;
  }
  LaunchOrPrompt(entries_[idx].key(), entries_[idx].path);
}

void GameListPanel::OnSelectionChanged() {
  info_pane_closed_ = false;
  RefreshInfoPane();
  if (selection_changed_cb_) {
    selection_changed_cb_();
  }
}

CompatState GameListPanel::GetEntryCompatState(const Entry& e) const {
  CompatState base = GetCompatState(e.title_id);
  // Best of (achievements unlocked %) and (gamerscore earned %).
  float pct = 0.0f;
  if (e.achievements_total > 0) {
    pct = std::max(pct, static_cast<float>(e.achievements_unlocked) /
                            static_cast<float>(e.achievements_total));
  }
  if (e.gamerscore_total > 0) {
    pct = std::max(pct, static_cast<float>(e.gamerscore_earned) /
                            static_cast<float>(e.gamerscore_total));
  }
  if (pct >= 0.80f) {
    return CompatState::kPlayable;
  }
  if (base == CompatState::kUnknown && pct >= 0.10f) {
    return CompatState::kGameplay;
  }
  return base;
}

void GameListPanel::OnItemContextMenu(int index) {
  if (index < 0 || index >= static_cast<int>(visible_indices_.size())) {
    return;
  }
  size_t idx = visible_indices_[index];
  if (idx >= entries_.size()) {
    return;
  }
  const Entry& entry = entries_[idx];

  // Everything else a title affords lives in the info pane; the menu keeps
  // only the two actions the pane has no place for.
  wxMenu menu;
  if (!entry.path.empty()) {
    if (entry.discs.size() > 1) {
      auto* launch_submenu = new wxMenu;
      size_t disc_num = 1;
      for (const auto& disc : entry.discs) {
        auto* item = launch_submenu->Append(
            wxID_ANY, wxString::Format(_("Disc %zu"), disc_num));
        menu.Bind(
            wxEVT_MENU,
            [this, key = entry.key(), path = disc.path](wxCommandEvent&) {
              LaunchOrPrompt(key, path);
            },
            item->GetId());
        ++disc_num;
      }
      menu.AppendSubMenu(launch_submenu, _("Launch"));
    } else {
      auto* launch = menu.Append(wxID_ANY, _("Launch"));
      menu.Bind(
          wxEVT_MENU,
          [this, key = entry.key(), path = entry.path](wxCommandEvent&) {
            LaunchOrPrompt(key, path);
          },
          launch->GetId());
    }
    menu.AppendSeparator();
  }

  auto* remove = menu.Append(wxID_ANY, _("Remove from list"));
  menu.Bind(
      wxEVT_MENU,
      [this, key = entry.key(),
       display_name = entry.title_name](wxCommandEvent&) {
        wxString name = display_name.empty()
                            ? wxString::Format(_("title %08X"), key.title_id)
                            : wxString::FromUTF8(display_name);
        wxString message = wxString::Format(
            _("Remove %s from the list?\n\nThis only removes it from the "
              "list in Xenia; game files on disk are kept."),
            name);
        int reply =
            wxMessageBox(message, _("Remove from list"),
                         wxYES_NO | wxICON_QUESTION, wxGetTopLevelParent(this));
        if (reply != wxYES) {
          return;
        }
        auto* library =
            emulator_window_ ? emulator_window_->game_library() : nullptr;
        if (!library) {
          return;
        }
        library->Remove(key);
        Reload();
      },
      remove->GetId());

  // 2px upward offset puts the cursor in the menu's top padding so X11's
  // trailing right-release doesn't activate the first item.
  // TODO(has207): retest on wxWidgets update — drop if PopupMenu starts
  // anchoring via gtk_menu_popup_at_widget.
  wxPoint pos = ScreenToClient(wxGetMousePosition());
  pos.y -= 2;
  PopupMenu(&menu, pos);
}

const GameListPanel::Entry* GameListPanel::SelectedEntry() const {
  if (!grid_) {
    return nullptr;
  }
  const int row = grid_->GetSelection();
  if (row < 0 || row >= static_cast<int>(visible_indices_.size())) {
    return nullptr;
  }
  size_t idx = visible_indices_[row];
  if (idx >= entries_.size()) {
    return nullptr;
  }
  return &entries_[idx];
}

std::filesystem::path GameListPanel::GetSelectedPath() const {
  const Entry* e = SelectedEntry();
  return e ? e->path : std::filesystem::path{};
}

void GameListPanel::MoveSelection(Direction direction) {
  if (!grid_ || visible_indices_.empty()) {
    return;
  }
  // The grid reports the change back, which refreshes the pane and callback.
  grid_->SetFocus();
  grid_->MoveSelection(direction);
}

void GameListPanel::ActivateSelected() {
  const Entry* e = SelectedEntry();
  if (e && !e->path.empty()) {
    LaunchOrPrompt(e->key(), e->path);
  }
}

void GameListPanel::FocusSearch() {
  if (search_) {
    search_->SetFocus();
  }
}
void GameListPanel::SortEntries() {
  auto cmp = [this](const Entry& a, const Entry& b) -> bool {
    auto less_then = [this](bool a_lt_b) {
      return sort_descending_ ? !a_lt_b : a_lt_b;
    };
    // Every key tiebreaks by title. Two releases of a game share a name until
    // one is renamed, and equal names have to compare equivalent both ways or
    // the inversion makes each less than the other, which is not a strict
    // weak ordering.
    auto by_title = [&]() {
      const std::string la = ToLower(a.title_name);
      const std::string lb = ToLower(b.title_name);
      return la == lb ? false : less_then(la < lb);
    };
    switch (sort_key_) {
      case SortKey::kStatus: {
        // Ascending puts the best (Playable) first.
        auto sa = static_cast<uint8_t>(GetEntryCompatState(a));
        auto sb = static_cast<uint8_t>(GetEntryCompatState(b));
        return sa != sb ? less_then(sa > sb) : by_title();
      }
      case SortKey::kLastPlayed:
        return a.last_run_time != b.last_run_time
                   ? less_then(a.last_run_time < b.last_run_time)
                   : by_title();
      case SortKey::kTitle:
      default:
        return by_title();
    }
  };
  std::stable_sort(entries_.begin(), entries_.end(), cmp);
}

void GameListPanel::OnSortChanged() {
  sort_dir_button_->SetLabel(SortArrow(sort_descending_));
  Repopulate();
}

void GameListPanel::Repopulate() {
  SortEntries();
  visible_indices_.clear();
  visible_indices_.reserve(entries_.size());

  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (!filter_lower_.empty()) {
      std::string title_lower = ToLower(e.title_name);
      std::string path_lower = ToLower(xe::path_to_utf8(e.path));
      if (title_lower.find(filter_lower_) == std::string::npos &&
          path_lower.find(filter_lower_) == std::string::npos) {
        continue;
      }
    }
    visible_indices_.push_back(i);
  }

  std::vector<ui::GameGrid::Item> items;
  items.reserve(visible_indices_.size());
  for (size_t idx : visible_indices_) {
    const auto& e = entries_[idx];
    ui::GameGrid::Item item;
    // Cards not decoded yet leave this empty and the grid draws the
    // placeholder until ProcessIconChunk fills them in.
    item.card = e.card;
    item.label = e.title_name.empty() ? _("File Corrupted")
                                      : wxString::FromUTF8(e.title_name);
    if (e.discs.size() > 1) {
      item.sublabel = wxString::Format(_("(%zu discs)"), e.discs.size());
    }
    // The card shows a shortened title and a colored dot; spell both out.
    const wxString version = wxString::FromUTF8(VersionToString(e.version));
    item.tooltip = item.label + "\n" +
                   wxString::Format(_("Version: %s"), version) + "\n" +
                   CompatStateName(GetEntryCompatState(e));
    if (!e.path.empty()) {
      item.tooltip += "\n" + wxString::FromUTF8(xe::path_to_utf8(e.path));
    }
    items.push_back(std::move(item));
  }
  grid_->SetItems(std::move(items));

  // Sorting and filtering rebuild every card, so re-find what was selected;
  // the info pane would otherwise blank on each keystroke. Restoring it is
  // not the user picking a game, so it is done without notifying.
  if (selected_key_.title_id) {
    for (size_t r = 0; r < visible_indices_.size(); ++r) {
      if (entries_[visible_indices_[r]].key() == selected_key_) {
        grid_->SetSelection(static_cast<int>(r), /*notify=*/false);
        grid_->EnsureVisible(static_cast<int>(r));
        break;
      }
    }
  }
  RefreshInfoPane();
  StartIconLoad();
}

}  // namespace app
}  // namespace xe
