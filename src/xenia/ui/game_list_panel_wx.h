/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GAME_LIST_PANEL_WX_H_
#define XENIA_UI_GAME_LIST_PANEL_WX_H_

#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/panel.h>
#include <wx/srchctrl.h>

#include "xenia/app/game_compat_db.h"
#include "xenia/app/game_library.h"
#include "xenia/ui/game_grid_wx.h"
#include "xenia/ui/game_info_panel_wx.h"

class wxButton;
class wxChoice;
class wxSplitterWindow;

namespace xe {
namespace app {

class EmulatorWindow;

class GameListPanel : public wxPanel {
 public:
  // Card grids have no column headers, so the order comes from a picker.
  enum class SortKey { kLastPlayed, kTitle, kStatus };

  using LaunchCallback = std::function<void(const std::filesystem::path&)>;
  using SelectionChangedCallback = std::function<void()>;

  GameListPanel(wxWindow* parent, EmulatorWindow* emulator_window);
  ~GameListPanel() override = default;

  void Reload();
  void SetLaunchCallback(LaunchCallback cb) { launch_cb_ = std::move(cb); }
  void SetSelectionChangedCallback(SelectionChangedCallback cb) {
    selection_changed_cb_ = std::move(cb);
  }
  // Returns the selected title's path, or empty if nothing selected.
  std::filesystem::path GetSelectedPath() const;

  // Which way a d-pad or arrow press moves the selection.
  using Direction = ui::GameGrid::Direction;
  // Moves one card. Selects an end card if nothing was selected.
  void MoveSelection(Direction direction);
  // Launches the selected title via the launch callback (if one is wired).
  void ActivateSelected();
  void FocusSearch();

 private:
  struct Disc {
    std::filesystem::path path;
  };
  struct Entry {
    uint32_t title_id = 0;
    // Two releases of one title are separate entries; the pair identifies
    // which library folder this row came from.
    uint32_t version = 0;
    std::string title_name;
    std::filesystem::path path;
    std::vector<Disc> discs;
    time_t last_run_time = 0;
    // Art with the compat badge burned in, at card size. Built on demand.
    wxBitmap card;
    uint32_t achievements_total = 0;
    uint32_t achievements_unlocked = 0;
    uint32_t gamerscore_total = 0;
    uint32_t gamerscore_earned = 0;

    LibraryKey key() const { return {title_id, version}; }
  };

  // Selected entry, or nullptr if none.
  const Entry* SelectedEntry() const;

  // Launches `path` if it exists, else offers to browse for a replacement and
  // drops the stale `path` from the release it belonged to.
  void LaunchOrPrompt(const LibraryKey& key, const std::filesystem::path& path);

  // Stamps `key` as launched now, in the library and in the loaded row.
  void MarkPlayed(const LibraryKey& key);

  // Retargets the info pane at whatever is selected now.
  void RefreshInfoPane();
  // Splits the pane back in, or folds it away when nothing is selected.
  void ShowInfoPane(bool show);
  // The pane's width for a given panel width, honouring a dragged sash.
  int PaneWidthFor(int width) const;
  // Repositions the sash so the pane keeps its share after a resize.
  void ApplySashPosition();

  void OnSearch(wxCommandEvent& event);
  void OnItemActivated(int index);
  void OnSelectionChanged();
  void OnSortChanged();
  // Compat DB state for `e`, optimistically upgraded by the user's local
  // achievement progress (10%+ promotes Unknown to Gameplay; 80%+ promotes
  // anything to Playable).
  CompatState GetEntryCompatState(const Entry& e) const;
  void SortEntries();
  // Card art at icon_size_px_ with the compat badge composited into a corner.
  wxBitmap MakeCard(const Entry& entry) const;

  SortKey sort_key_ = SortKey::kLastPlayed;
  bool sort_descending_ = true;
  void OnItemContextMenu(int index);
  void Repopulate();
  void LoadTimestampsFromProfiles();
  void StartIconLoad();
  void ProcessIconChunk(size_t start, int gen);
  void UpdateSearchPlaceholder();

  EmulatorWindow* emulator_window_;
  wxSplitterWindow* splitter_ = nullptr;
  wxPanel* list_side_ = nullptr;
  GameInfoPanel* info_panel_ = nullptr;
  wxSearchCtrl* search_ = nullptr;
  wxChoice* sort_choice_ = nullptr;
  wxButton* sort_dir_button_ = nullptr;
  ui::GameGrid* grid_ = nullptr;
  wxPanel* loading_panel_ = nullptr;
  // The pane's share of the width. Kept as a fraction rather than a pixel
  // count so folding it away, or resizing the window while it is folded,
  // cannot leave a width that was right for some other window size. Zero
  // until the user drags the sash; before that the default share applies.
  double pane_fraction_ = 0.0;
  // Set while our own size handler runs, so the splitter's internal sash
  // adjustments are not mistaken for the user dragging it.
  bool in_resize_ = false;
  // Set by the pane's close button. Sorting and filtering leave it shut;
  // picking a game clears it.
  bool info_pane_closed_ = false;
  // Survives the entries_ rebuild so sort/filter/reload can restore the row.
  LibraryKey selected_key_;
  std::vector<Entry> entries_;
  std::vector<size_t> visible_indices_;
  LaunchCallback launch_cb_;
  SelectionChangedCallback selection_changed_cb_;
  std::string filter_lower_;
  // Bumped on each Reload so in-flight icon-load chunks abort.
  int icon_load_generation_ = 0;
  // Resolved at construction time from the panel's monitor; used for sizing
  // bitmaps, column widths, and the row height so the list grows with the
  // current display's DPI scale.
  int icon_size_px_ = 0;
  wxBitmap not_played_placeholder_;
  std::array<wxBitmap, 5> compat_balls_;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_GAME_LIST_PANEL_WX_H_
