/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_WINDOW_WX_H_
#define XENIA_UI_WINDOW_WX_H_

#include <functional>
#include <memory>
#include <string>

#include <wx/aui/framemanager.h>
#include <wx/dnd.h>
#include <wx/wx.h>

#include "xenia/base/platform.h"
#include "xenia/ui/window.h"
#if XE_PLATFORM_LINUX
#include "xenia/ui/surface_gnulinux.h"
#endif

#if XE_PLATFORM_WIN32
#include <Dbt.h>
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace ui {

class WxWindow : public Window {
 public:
  WxWindow(WindowedAppContext& app_context, const std::string_view title,
           uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~WxWindow() override;

  wxFrame* frame() const { return frame_; }
  wxWindow* render_panel() const { return render_panel_; }
  wxAuiManager* aui_manager() { return &aui_manager_; }

  // Invoked after each fullscreen toggle so the embedder can re-apply its
  // own pane visibility rules on top of the frame's new state.
  void SetContentVisibilityCallback(std::function<void()> cb);

  // Grow the frame so render_panel_ matches the desired logical size after
  // AUI panes (toolbar) are laid out. No-op if the user has saved geometry,
  // or the frame is maximized/fullscreen.
  void EnsureInitialRenderSurfaceSize();

#if XE_PLATFORM_WIN32
  // Top-level HWND for the presenter's monitor detection.
  HWND hwnd() const {
    return frame_ ? static_cast<HWND>(frame_->GetHandle()) : nullptr;
  }
#endif

  uint32_t GetMediumDpi() const override { return 96; }

  bool GetMousePosition(int32_t& x, int32_t& y) const override;

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  uint32_t GetLatestDpiImpl() const override;
  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void LoadAndApplyIcon(const void* buffer, size_t size,
                        bool can_apply_state_in_current_phase) override;
  void ApplyNewMainMenu(MenuItem* old_main_menu) override;
  void CompleteMainMenuItemsUpdateImpl() override;
  void ApplyNewMouseCapture() override;
  void ApplyNewMouseRelease() override;
  void ApplyNewCursorVisibility(
      CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(
      Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
#if XE_PLATFORM_WIN32
  class XeniaFrame;
  class RenderPanel;
#endif
  class FileDropTargetImpl;

  static VirtualKey TranslateKeyCode(int wx_key);

  void OnFrameClose(wxCloseEvent& event);
  void OnFrameSize(wxSizeEvent& event);
  void OnFrameDpiChanged(wxDPIChangedEvent& event);
  void OnFrameSetFocus(wxFocusEvent& event);
  void OnFrameKillFocus(wxFocusEvent& event);
  void OnPanelSetFocus(wxFocusEvent& event);
  void OnPanelKillFocus(wxFocusEvent& event);

  void OnPanelKeyDown(wxKeyEvent& event);
  void OnPanelKeyUp(wxKeyEvent& event);
  void OnPanelChar(wxKeyEvent& event);
  void OnPanelDoubleClick(wxMouseEvent& event);
  void OnPanelMouseDown(wxMouseEvent& event);
  void OnPanelMouseUp(wxMouseEvent& event);
  void OnPanelMouseMove(wxMouseEvent& event);
  void OnPanelMouseWheel(wxMouseEvent& event);
  void OnPanelPaint(wxPaintEvent& event);
  void OnPanelEraseBackground(wxEraseEvent& event);

  void OnCursorAutoHideTimer(wxTimerEvent& event);
  void OnResizeDebounceTimer(wxTimerEvent& event);
  void OnMenuOpen(wxMenuEvent& event);
  void OnMenuClose(wxMenuEvent& event);

  void UnbindFrameAndPanelEvents();

  void SaveGeometryToConfig();
  void RestoreGeometryFromConfig();

  void DropStartupCoverIfPresent();

  wxWindow* render_target() const;

  wxFrame* frame_ = nullptr;
  // Child of frame_ holding the render surface; mouse/key/paint events
  // bind here, and CreateSurfaceImpl uses this window's HWND/X11 handle.
  wxWindow* render_panel_ = nullptr;
  wxAuiManager aui_manager_;
  // Black child covering the render area until the swap chain has presented
  // its first frame. Dropped from the WM_PAINT path when HasSurface() flips
  // true.
  wxWindow* startup_cover_ = nullptr;
  wxTimer cursor_auto_hide_timer_;
  wxTimer resize_debounce_timer_;
  wxSize pending_resize_size_;
  bool cursor_currently_auto_hidden_ = false;
  bool menu_open_ = false;
  uint32_t dpi_ = 96;
  std::function<void()> content_visibility_callback_;

#if XE_PLATFORM_WIN32
  HDEVNOTIFY usb_device_notify_ = nullptr;
#endif

#if XE_PLATFORM_LINUX
  unsigned int pending_paint_source_ = 0;
  GtkSurfaceFactory gtk_surface_factory_;
#endif
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_WINDOW_WX_H_
