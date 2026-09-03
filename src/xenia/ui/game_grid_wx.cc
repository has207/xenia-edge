/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_grid_wx.h"

#include <algorithm>

#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/settings.h>

namespace xe {
namespace ui {

namespace {

// Gap around the grid and between cells, in logical DIPs at 96 DPI.
constexpr int kGridMargin = 8;
constexpr int kCellPadX = 16;
constexpr int kCellPadY = 14;
// Rounding on the selection and hover fills.
constexpr int kFillRadius = 4;

}  // namespace

GameGrid::GameGrid(wxWindow* parent, int card_size_px)
    : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxVSCROLL | wxWANTS_CHARS | wxBORDER_NONE),
      card_size_px_(card_size_px) {
  SetBackgroundStyle(wxBG_STYLE_PAINT);
  SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX));
  // One card per wheel notch keeps a full row from flying past.
  SetScrollRate(0, FromDIP(16));

  RecalculateMetrics();

  Bind(wxEVT_PAINT, &GameGrid::OnPaint, this);
  Bind(wxEVT_SIZE, &GameGrid::OnSize, this);
  Bind(wxEVT_KEY_DOWN, &GameGrid::OnKeyDown, this);
  for (auto event : {wxEVT_LEFT_DOWN, wxEVT_LEFT_DCLICK, wxEVT_RIGHT_DOWN,
                     wxEVT_MOTION, wxEVT_LEAVE_WINDOW}) {
    Bind(event, &GameGrid::OnMouse, this);
  }
}

void GameGrid::SetCardSize(int card_size_px) {
  if (card_size_px == card_size_px_) {
    return;
  }
  card_size_px_ = card_size_px;
  RecalculateMetrics();
  EllipsizeLabels();
  RelayoutAndRefresh();
}

void GameGrid::SetPlaceholder(const wxBitmap& placeholder) {
  placeholder_ = placeholder;
  Refresh();
}

void GameGrid::RecalculateMetrics() {
  margin_ = FromDIP(kGridMargin);

  wxClientDC dc(this);
  dc.SetFont(GetFont());
  text_h_ = dc.GetCharHeight();

  cell_w_ = card_size_px_ + FromDIP(kCellPadX);
  // Art, then two lines of label.
  cell_h_ = card_size_px_ + text_h_ * 2 + FromDIP(kCellPadY);
}

void GameGrid::SetItems(std::vector<Item> items) {
  items_ = std::move(items);
  selection_ = -1;
  hover_ = -1;
  EllipsizeLabels();
  RelayoutAndRefresh();
}

void GameGrid::SetItemCard(size_t index, const wxBitmap& card) {
  if (index >= items_.size()) {
    return;
  }
  items_[index].card = card;
  RefreshRect(ScrolledCellRect(static_cast<int>(index)));
}

void GameGrid::EllipsizeLabels() {
  label_text_.clear();
  label_text_.reserve(items_.size());

  wxClientDC dc(this);
  dc.SetFont(GetFont());
  // Leave the padding either side so neighbouring labels never touch.
  const int max_width = cell_w_ - FromDIP(6);
  for (const auto& item : items_) {
    label_text_.push_back(
        wxControl::Ellipsize(item.label, dc, wxELLIPSIZE_END, max_width));
  }
}

void GameGrid::RelayoutAndRefresh() {
  const wxSize client = GetClientSize();
  columns_ = std::max(1, (client.GetWidth() - 2 * margin_) / cell_w_);

  const int rows = (static_cast<int>(items_.size()) + columns_ - 1) / columns_;
  const wxSize virtual_size(client.GetWidth(), 2 * margin_ + rows * cell_h_);
  // Only when it actually changed: setting it can add or drop the scrollbar,
  // which resizes the client area and lands us back here.
  if (virtual_size != GetVirtualSize()) {
    SetVirtualSize(virtual_size);
  }
  Refresh();
}

wxRect GameGrid::CellRect(int index) const {
  const int row = index / columns_;
  const int column = index % columns_;
  return wxRect(margin_ + column * cell_w_, margin_ + row * cell_h_, cell_w_,
                cell_h_);
}

wxRect GameGrid::ScrolledCellRect(int index) const {
  wxRect rect = CellRect(index);
  rect.SetPosition(CalcScrolledPosition(rect.GetPosition()));
  return rect;
}

int GameGrid::HitTest(const wxPoint& client_point) const {
  const wxPoint point = CalcUnscrolledPosition(client_point);
  if (point.x < margin_ || point.y < margin_) {
    return -1;
  }
  const int column = (point.x - margin_) / cell_w_;
  const int row = (point.y - margin_) / cell_h_;
  if (column >= columns_) {
    return -1;
  }
  const int index = row * columns_ + column;
  return index < static_cast<int>(items_.size()) ? index : -1;
}

void GameGrid::SetSelection(int index, bool notify) {
  if (index >= static_cast<int>(items_.size())) {
    index = -1;
  }
  if (index == selection_) {
    return;
  }
  const int previous = selection_;
  selection_ = index;
  if (previous >= 0) {
    RefreshRect(ScrolledCellRect(previous));
  }
  if (selection_ >= 0) {
    RefreshRect(ScrolledCellRect(selection_));
  }
  if (notify && selection_changed_cb_) {
    selection_changed_cb_();
  }
}

void GameGrid::MoveSelection(Direction direction) {
  switch (direction) {
    case Direction::kLeft:
      MoveSelection(-1);
      break;
    case Direction::kRight:
      MoveSelection(1);
      break;
    case Direction::kUp:
      MoveSelection(-columns_);
      break;
    case Direction::kDown:
      MoveSelection(columns_);
      break;
  }
}

void GameGrid::MoveSelection(int delta) {
  if (items_.empty()) {
    return;
  }
  int index = selection_;
  if (index < 0) {
    index = delta > 0 ? 0 : static_cast<int>(items_.size()) - 1;
  } else {
    index = std::clamp(index + delta, 0, static_cast<int>(items_.size()) - 1);
  }
  SetSelection(index);
  EnsureVisible(index);
}

void GameGrid::EnsureVisible(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    return;
  }
  const wxRect cell = CellRect(index);
  const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
  const int view_height = GetClientSize().GetHeight();

  int target = origin.y;
  if (cell.GetTop() < origin.y) {
    target = cell.GetTop() - margin_;
  } else if (cell.GetBottom() > origin.y + view_height) {
    target = cell.GetBottom() - view_height + margin_;
  } else {
    return;
  }

  int unit_x = 0, unit_y = 0;
  GetScrollPixelsPerUnit(&unit_x, &unit_y);
  if (unit_y > 0) {
    Scroll(-1, std::max(0, target) / unit_y);
  }
}

void GameGrid::DrawCard(wxDC& dc, int index, const wxRect& cell) const {
  const Item& item = items_[index];

  if (index == selection_ || index == hover_) {
    const wxColour base = wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
    // Nudge toward the middle either way so the tint reads on light and dark.
    const wxColour fill =
        index == selection_
            ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
            : base.ChangeLightness(base.GetLuminance() > 0.5 ? 94 : 130);
    wxRect fill_rect = cell;
    fill_rect.Deflate(FromDIP(2));
    dc.SetBrush(wxBrush(fill));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(fill_rect, FromDIP(kFillRadius));
  }

  const wxBitmap& card = item.card.IsOk() ? item.card : placeholder_;
  if (card.IsOk()) {
    dc.DrawBitmap(card, cell.x + (cell.width - card.GetWidth()) / 2,
                  cell.y + FromDIP(4), true);
  }

  dc.SetTextForeground(
      index == selection_
          ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
          : wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOXTEXT));

  wxRect text_rect(cell.x, cell.y + card_size_px_ + FromDIP(6), cell.width,
                   text_h_);
  dc.DrawLabel(label_text_[index], text_rect, wxALIGN_CENTER_HORIZONTAL);
  if (!item.sublabel.empty()) {
    text_rect.y += text_h_;
    dc.DrawLabel(item.sublabel, text_rect, wxALIGN_CENTER_HORIZONTAL);
  }
}

void GameGrid::OnPaint(wxPaintEvent&) {
  wxAutoBufferedPaintDC dc(this);
  dc.SetBackground(wxBrush(GetBackgroundColour()));
  dc.Clear();
  DoPrepareDC(dc);

  if (items_.empty()) {
    return;
  }
  dc.SetFont(GetFont());

  // Only the rows on screen: the grid is uniform, so the range is arithmetic.
  const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
  const int view_height = GetClientSize().GetHeight();
  const int count = static_cast<int>(items_.size());
  const int first_row = std::max(0, (origin.y - margin_) / cell_h_);
  const int last_row = (origin.y + view_height - margin_) / cell_h_;

  for (int row = first_row; row <= last_row; ++row) {
    for (int column = 0; column < columns_; ++column) {
      const int index = row * columns_ + column;
      if (index >= count) {
        return;
      }
      DrawCard(dc, index, CellRect(index));
    }
  }
}

void GameGrid::OnSize(wxSizeEvent& event) {
  event.Skip();
  const int previous_columns = columns_;
  RelayoutAndRefresh();
  if (columns_ != previous_columns) {
    EnsureVisible(selection_);
  }
}

void GameGrid::OnMouse(wxMouseEvent& event) {
  event.Skip();
  const wxEventType type = event.GetEventType();

  if (type == wxEVT_LEAVE_WINDOW) {
    if (hover_ >= 0) {
      const int previous = hover_;
      hover_ = -1;
      RefreshRect(ScrolledCellRect(previous));
      UnsetToolTip();
    }
    return;
  }

  const int index = HitTest(event.GetPosition());

  if (type == wxEVT_MOTION) {
    if (index == hover_) {
      return;
    }
    const int previous = hover_;
    hover_ = index;
    if (previous >= 0) {
      RefreshRect(ScrolledCellRect(previous));
    }
    if (hover_ >= 0) {
      RefreshRect(ScrolledCellRect(hover_));
      SetToolTip(items_[hover_].tooltip);
    } else {
      UnsetToolTip();
    }
    return;
  }

  if (type == wxEVT_LEFT_DCLICK) {
    if (index >= 0 && activated_cb_) {
      activated_cb_(index);
    }
    return;
  }

  // Both button presses select first so the pane and menu act on the card
  // under the cursor.
  SetFocus();
  if (index >= 0) {
    SetSelection(index);
  }
  if (type == wxEVT_RIGHT_DOWN && index >= 0 && context_menu_cb_) {
    context_menu_cb_(index, event.GetPosition());
  }
}

void GameGrid::OnKeyDown(wxKeyEvent& event) {
  if (items_.empty()) {
    event.Skip();
    return;
  }
  const int count = static_cast<int>(items_.size());
  const int rows_per_page = std::max(1, GetClientSize().GetHeight() / cell_h_);

  switch (event.GetKeyCode()) {
    case WXK_LEFT:
      MoveSelection(Direction::kLeft);
      break;
    case WXK_RIGHT:
      MoveSelection(Direction::kRight);
      break;
    case WXK_UP:
      MoveSelection(Direction::kUp);
      break;
    case WXK_DOWN:
      MoveSelection(Direction::kDown);
      break;
    case WXK_PAGEUP:
      MoveSelection(-rows_per_page * columns_);
      break;
    case WXK_PAGEDOWN:
      MoveSelection(rows_per_page * columns_);
      break;
    case WXK_HOME:
      SetSelection(0);
      EnsureVisible(0);
      break;
    case WXK_END:
      SetSelection(count - 1);
      EnsureVisible(count - 1);
      break;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
      if (selection_ >= 0 && activated_cb_) {
        activated_cb_(selection_);
      }
      break;
    default:
      event.Skip();
      break;
  }
}

}  // namespace ui
}  // namespace xe
