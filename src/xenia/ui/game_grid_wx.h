/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_GAME_GRID_WX_H_
#define XENIA_UI_GAME_GRID_WX_H_

#include <cstddef>
#include <functional>
#include <vector>

#include <wx/bitmap.h>
#include <wx/scrolwin.h>
#include <wx/string.h>

namespace xe {
namespace ui {

// Owner-drawn grid of title cards: art with a label under it, wrapping left to
// right and scrolling vertically. wxListCtrl's icon view is native only on
// Windows; the generic backend the other platforms use flows into columns and
// scrolls sideways instead, so this draws the grid itself and behaves the same
// everywhere.
//
// Cards are laid out on a uniform pitch, which makes hit testing arithmetic
// and lets a paint touch only the rows on screen.
class GameGrid : public wxScrolledCanvas {
 public:
  struct Item {
    // Empty until the caller decodes it; the placeholder stands in meanwhile.
    wxBitmap card;
    wxString label;
    // Second line, e.g. a disc count. Optional.
    wxString sublabel;
    wxString tooltip;
  };

  GameGrid(wxWindow* parent, int card_size_px);

  // Art edge in device pixels. Drives the whole cell pitch.
  void SetCardSize(int card_size_px);
  void SetPlaceholder(const wxBitmap& placeholder);

  // Replaces the contents. Selection is dropped; the caller restores it.
  void SetItems(std::vector<Item> items);
  // Fills in art that finished decoding after SetItems().
  void SetItemCard(size_t index, const wxBitmap& card);
  size_t GetItemCount() const { return items_.size(); }

  enum class Direction { kLeft, kRight, kUp, kDown };

  // Selected index, or -1 when nothing is selected.
  int GetSelection() const { return selection_; }
  void SetSelection(int index, bool notify = true);
  // Moves one card in a direction: sideways within the row, vertically by a
  // whole row.
  void MoveSelection(Direction direction);
  // Moves by `delta` cards, clamped. Wraps rows because the order is linear.
  void MoveSelection(int delta);
  void EnsureVisible(int index);

  void SetSelectionChangedCallback(std::function<void()> cb) {
    selection_changed_cb_ = std::move(cb);
  }
  void SetActivatedCallback(std::function<void(int)> cb) {
    activated_cb_ = std::move(cb);
  }
  // Point is in the grid's client coordinates.
  void SetContextMenuCallback(std::function<void(int, const wxPoint&)> cb) {
    context_menu_cb_ = std::move(cb);
  }

 private:
  void RecalculateMetrics();
  void RelayoutAndRefresh();
  // Card index under a client point, or -1 outside any cell.
  int HitTest(const wxPoint& client_point) const;
  // Cell rectangle in unscrolled coordinates.
  wxRect CellRect(int index) const;
  // Same rectangle in client coordinates, for repainting one card.
  wxRect ScrolledCellRect(int index) const;
  void DrawCard(wxDC& dc, int index, const wxRect& cell) const;
  // Label shortened to the cell width, cached because ellipsizing measures.
  void EllipsizeLabels();

  void OnPaint(wxPaintEvent& event);
  void OnSize(wxSizeEvent& event);
  void OnMouse(wxMouseEvent& event);
  void OnKeyDown(wxKeyEvent& event);

  std::vector<Item> items_;
  wxBitmap placeholder_;
  // Ellipsized copies of the labels, parallel to items_.
  std::vector<wxString> label_text_;

  int card_size_px_ = 0;
  int cell_w_ = 1;
  int cell_h_ = 1;
  int margin_ = 0;
  int text_h_ = 0;
  int columns_ = 1;

  int selection_ = -1;
  int hover_ = -1;

  std::function<void()> selection_changed_cb_;
  std::function<void(int)> activated_cb_;
  std::function<void(int, const wxPoint&)> context_menu_cb_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_GAME_GRID_WX_H_
