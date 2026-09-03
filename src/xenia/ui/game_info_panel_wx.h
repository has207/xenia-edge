/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GAME_INFO_PANEL_WX_H_
#define XENIA_UI_GAME_INFO_PANEL_WX_H_

#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

#include "xenia/app/game_compat_db.h"
#include "xenia/app/game_library.h"
#include "xenia/xbox.h"

class wxBoxSizer;
class wxButton;
class wxFlexGridSizer;
class wxDataViewListCtrl;
class wxNotebook;
class wxPanel;
class wxScrolledWindow;
class wxStaticText;
class wxWindow;

namespace xe {
namespace kernel {
namespace xam {
class ContentManager;
class ProfileManager;
}  // namespace xam
}  // namespace kernel

namespace app {

class EmulatorWindow;

// One content folder: a content type under a profile. A section can cover
// more than one, because some titles install into the shared profile's saves
// rather than the DLC folder that data belongs in.
struct ContentLocation {
  XContentType content_type = XContentType::kInvalid;
  uint64_t xuid = 0;
};

// Everything known about one title: art, name and play progress in a header,
// then tabs for what identifies the release, its discs, and the content,
// patches and config overrides found on disk.
class GameInfoPanel : public wxPanel {
 public:
  // Play progress, owned by the caller because it comes from the profile GPDs
  // the game list already scans in bulk.
  struct TitleStats {
    time_t last_played = 0;
    uint32_t achievements_unlocked = 0;
    uint32_t achievements_total = 0;
    uint32_t gamerscore_earned = 0;
    uint32_t gamerscore_total = 0;
  };

  GameInfoPanel(wxWindow* parent, EmulatorWindow* emulator_window);

  // Retargets the pane, or re-reads the release already shown. Coalesced
  // behind a short timer so arrowing through the list doesn't stat every
  // title's content directories.
  void ShowTitle(const LibraryKey& key, CompatState compat,
                 const TitleStats& stats);
  void ShowNoSelection();

  // Fired after a disc edit changes what the game list would show.
  void SetLibraryChangedCallback(std::function<void()> cb) {
    library_changed_cb_ = std::move(cb);
  }

  // Fired when the pane's close button is pressed.
  void SetCloseCallback(std::function<void()> cb) { close_cb_ = std::move(cb); }

 private:
  // Tab order. Content is built per page, on demand.
  enum Page {
    kPageInfo,
    kPageContent,
    kPageSaves,
    kPagePatches,
    kPageConfig,
    kPageCount,
  };

  void Rebuild();
  // Fills one page if it is stale. Building only what is on screen keeps a
  // selection change off the disk: the patch and config editors parse files.
  void BuildPage(int page);
  // Empties a page and hands back the sizer its sections go into.
  wxBoxSizer* BeginPage(wxWindow* page);
  // Installs the sizer with the page's padding and resizes to fit.
  void EndPage(wxScrolledWindow* page, wxBoxSizer* sizer);

  void BuildHeader(wxWindow* parent, wxBoxSizer* sizer);
  void BuildDetailsSection(wxWindow* parent, wxBoxSizer* sizer);
  void BuildCompatSection(wxWindow* parent, wxBoxSizer* sizer);
  void BuildDiscsSection(wxWindow* parent, wxBoxSizer* sizer);
  // The content folders one section covers, listed together. `header_format`
  // is a complete translatable phrase holding a single %zu for the item count.
  // The first location is the section's own: what its folder button opens.
  // `note` replaces the rows when the content is not reachable (no profile
  // signed in).
  // `allow_import` offers an Add button for content the user can supply as
  // loose packages.
  void BuildContentSection(wxWindow* parent, wxBoxSizer* sizer,
                           const wxString& header_format,
                           std::vector<ContentLocation> locations,
                           const wxString& note, bool allow_import = false);
  // Vets picked packages against this title and moves each good one into the
  // location its own content type names. Rejects are reported together.
  void ImportContent(const std::vector<ContentLocation>& locations);
  void BuildPatchesSection(wxWindow* parent, wxBoxSizer* sizer);
  void BuildConfigSection(wxWindow* parent, wxBoxSizer* sizer);

  // "<text> (n)" with a right-aligned folder button, and an Add button when
  // `import_locations` name somewhere the user can drop packages. `open_path`
  // is created if it doesn't exist yet. Returns the label so a section can
  // restate its count later.
  wxStaticText* AddSectionHeader(
      wxWindow* parent, wxBoxSizer* sizer, const wxString& text,
      const std::filesystem::path& open_path,
      std::vector<ContentLocation> import_locations = {});
  void AddNoteRow(wxWindow* parent, wxBoxSizer* sizer, const wxString& text);
  // Two-column grid of dimmed labels against their values.
  wxFlexGridSizer* AddFieldGrid(wxBoxSizer* sizer);
  // `selectable` makes the value copyable, for ids worth pasting elsewhere.
  // Returns the value control so a caller can restyle it.
  wxWindow* AddField(wxWindow* parent, wxFlexGridSizer* grid,
                     const wxString& label, const wxString& value,
                     bool selectable = false);
  // Label that shortens to fit the pane, with the full text as its tooltip.
  wxStaticText* MakeElidedLabel(wxWindow* parent, const wxString& text);

  void ReloadDiscs();
  // Points the disc list's tooltip at one row's full path, or clears it
  // for wxNOT_FOUND. Cheap to call on every pointer move.
  void SetDiscTooltip(int row);
  // The packages for two releases of a game can be identical apart from their
  // media id, so nothing can name them apart; the user has to.
  void OnRenameTitle();
  void OnRemoveDisc();

  // Null until the kernel is up.
  kernel::xam::ProfileManager* profile_manager() const;
  kernel::xam::ContentManager* content_manager() const;

  EmulatorWindow* emulator_window_;
  LibraryKey key_;
  CompatState compat_ = CompatState::kUnknown;
  TitleStats stats_;
  std::function<void()> library_changed_cb_;
  std::function<void()> close_cb_;
  wxTimer refresh_timer_;

  // Art and identity, above the tabs and outside them.
  wxPanel* header_panel_ = nullptr;
  wxNotebook* notebook_ = nullptr;
  std::array<wxScrolledWindow*, kPageCount> pages_{};
  // Pages holding another title's content, or none yet.
  std::array<bool, kPageCount> page_stale_{};
  // All owned by a page and dropped on every Rebuild().
  wxStaticText* discs_header_ = nullptr;
  wxDataViewListCtrl* disc_list_ = nullptr;
  wxButton* remove_button_ = nullptr;
  // Row index -> disc path, rebuilt with the list.
  std::vector<std::filesystem::path> disc_paths_;
  // Row the disc list's tooltip currently describes, so pointer movement
  // within one row doesn't re-set it and make the tooltip flicker.
  int disc_tooltip_row_ = wxNOT_FOUND;

  int char_w_ = 8;
  int char_h_ = 16;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_UI_GAME_INFO_PANEL_WX_H_
