/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/window_qt.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QExposeEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMargins>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/windowed_app_context_qt.h"

#if defined(XE_PLATFORM_LINUX)
#include <QtGui/qguiapplication_platform.h>
#include "xenia/ui/surface_gnulinux.h"
#elif defined(XE_PLATFORM_WIN32)
#include "xenia/ui/surface_win.h"
#elif defined(XE_PLATFORM_MAC)
#include "xenia/ui/surface_mac.h"
#endif

#if defined(XE_PLATFORM_LINUX)
// Include Qt private headers for Wayland support on Linux only
// (path is relative to QtGui/<version>/QtGui which is in include path)
#include <qpa/qplatformwindow_p.h>
#endif

// Initialize Qt resources - must be outside of namespace scope
inline void InitializeUIResources() { Q_INIT_RESOURCE(ui_resources); }

namespace xe {
namespace ui {

// Internal QMainWindow for UI process (game list, menu bar)
class QtWindowInternal : public QMainWindow {
 public:
  explicit QtWindowInternal(QtWindow* window) : window_(window) {}

 protected:
  void closeEvent(QCloseEvent* event) override { window_->OnCloseEvent(event); }
  void resizeEvent(QResizeEvent* event) override {
    QMainWindow::resizeEvent(event);
    window_->OnResizeEvent(event);
  }
  void focusInEvent(QFocusEvent* event) override {
    QMainWindow::focusInEvent(event);
    window_->OnFocusInEvent(event);
  }
  void focusOutEvent(QFocusEvent* event) override {
    QMainWindow::focusOutEvent(event);
    window_->OnFocusOutEvent(event);
  }
  void changeEvent(QEvent* event) override {
    QMainWindow::changeEvent(event);
    // Handle window activation changes (more reliable than focus events for
    // detecting app-level focus like Alt+Tab)
    if (event->type() == QEvent::WindowActivate) {
      window_->OnFocusInEvent(nullptr);
    } else if (event->type() == QEvent::WindowDeactivate) {
      window_->OnFocusOutEvent(nullptr);
    }
  }
  void keyPressEvent(QKeyEvent* event) override {
    window_->OnKeyPressEvent(event);
    event->accept();
  }
  void keyReleaseEvent(QKeyEvent* event) override {
    window_->OnKeyReleaseEvent(event);
    event->accept();
  }
  bool eventFilter(QObject* obj, QEvent* event) override {
    return window_->OnEventFilter(obj, event);
  }

 private:
  QtWindow* window_;
};

// GameQWindow implementation - native QWindow for game process rendering
GameQWindow::GameQWindow(QtWindow* window) : QWindow(), window_(window) {
  // RasterSurface is appropriate for D3D12/Vulkan external rendering
  setSurfaceType(QSurface::RasterSurface);
}

void GameQWindow::exposeEvent(QExposeEvent* event) {
  QWindow::exposeEvent(event);
  if (isExposed() && window_) {
    window_->TriggerPaint();
  }
}

void GameQWindow::resizeEvent(QResizeEvent* event) {
  QWindow::resizeEvent(event);
  if (window_) {
    window_->OnResizeEvent(event);
  }
}

void GameQWindow::keyPressEvent(QKeyEvent* event) {
  if (window_) {
    window_->OnKeyPressEvent(event);
  }
  event->accept();
}

void GameQWindow::keyReleaseEvent(QKeyEvent* event) {
  if (window_) {
    window_->OnKeyReleaseEvent(event);
  }
  event->accept();
}

void GameQWindow::mouseMoveEvent(QMouseEvent* event) {
  if (window_) {
    window_->OnMouseMoveEvent(event);
  }
}

void GameQWindow::mousePressEvent(QMouseEvent* event) {
  if (window_) {
    window_->OnMousePressEvent(event);
  }
}

void GameQWindow::mouseReleaseEvent(QMouseEvent* event) {
  if (window_) {
    window_->OnMouseReleaseEvent(event);
  }
}

void GameQWindow::mouseDoubleClickEvent(QMouseEvent* event) {
  if (window_) {
    window_->OnMouseDoubleClickEvent(event);
  }
}

void GameQWindow::wheelEvent(QWheelEvent* event) {
  if (window_) {
    window_->OnWheelEvent(event);
  }
}

void GameQWindow::focusInEvent(QFocusEvent* event) {
  QWindow::focusInEvent(event);
  if (window_) {
    window_->OnFocusInEvent(event);
  }
}

void GameQWindow::focusOutEvent(QFocusEvent* event) {
  QWindow::focusOutEvent(event);
  if (window_) {
    window_->OnFocusOutEvent(event);
  }
}

bool GameQWindow::event(QEvent* event) {
  switch (event->type()) {
    case QEvent::Close:
      if (window_) {
        window_->OnCloseEvent(static_cast<QCloseEvent*>(event));
      }
      return true;
    case QEvent::WindowActivate:
      if (window_) {
        window_->OnFocusInEvent(nullptr);
      }
      break;
    case QEvent::WindowDeactivate:
      if (window_) {
        window_->OnFocusOutEvent(nullptr);
      }
      break;
    default:
      break;
  }
  return QWindow::event(event);
}

std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           const std::string& hotkey,
                                           std::function<void()> callback) {
  return std::make_unique<QtMenuItem>(type, text, hotkey, callback);
}

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height,
                                       bool is_game_process) {
  return std::make_unique<QtWindow>(app_context, title, desired_logical_width,
                                    desired_logical_height, is_game_process);
}

QtWindow::QtWindow(WindowedAppContext& app_context,
                   const std::string_view title, uint32_t desired_logical_width,
                   uint32_t desired_logical_height, bool is_game_process)
    : Window(app_context, title, desired_logical_width, desired_logical_height),
      is_game_process_(is_game_process) {}

QtWindow::~QtWindow() {
  EnterDestructor();
  if (cursor_auto_hide_timer_) {
    cursor_auto_hide_timer_->stop();
    delete cursor_auto_hide_timer_;
    cursor_auto_hide_timer_ = nullptr;
  }
  if (game_qwindow_) {
    delete game_qwindow_;
    game_qwindow_ = nullptr;
  }
  if (qwindow_) {
    delete qwindow_;
    qwindow_ = nullptr;
  }
}

bool QtWindow::OpenImpl() {
  // Initialize Qt resources (needed for app icon)
  InitializeUIResources();

  // Branch based on window type
  if (is_game_process_) {
    return OpenGameWindow();
  } else {
    return OpenUIWindow();
  }
}

bool QtWindow::OpenGameWindow() {
  // Create native QWindow for game rendering (no Qt widget overhead)
  game_qwindow_ = new GameQWindow(this);
  game_qwindow_->setTitle(
      QString::fromUtf8(GetTitle().data(), GetTitle().size()));

  // Set default application icon
  QIcon app_icon(":/xenia/icon.png");
  if (!app_icon.isNull()) {
    game_qwindow_->setIcon(app_icon);
    QGuiApplication::setWindowIcon(app_icon);
  }

  // Use the screen where the cursor is, falling back to primary
  QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
  if (!screen) {
    screen = QGuiApplication::primaryScreen();
  }

  // Position window on target screen
  if (screen) {
    game_qwindow_->setScreen(screen);
    QRect screen_geometry = screen->availableGeometry();
    int x = screen_geometry.x() +
            (screen_geometry.width() - GetDesiredLogicalWidth()) / 2;
    int y = screen_geometry.y() +
            (screen_geometry.height() - GetDesiredLogicalHeight()) / 2;
    game_qwindow_->setPosition(x, y);
  }

  game_qwindow_->resize(GetDesiredLogicalWidth(), GetDesiredLogicalHeight());

  if (IsFullscreen()) {
    game_qwindow_->showFullScreen();
  } else {
    game_qwindow_->show();
  }

  // Bring window to foreground
  game_qwindow_->requestActivate();
  game_qwindow_->raise();

  // Ensure native window is created
  game_qwindow_->winId();

  {
    WindowDestructionReceiver destruction_receiver(this);

    // Notify that the window is on a monitor (required for presenter to paint)
    MonitorUpdateEvent monitor_event(this, false);
    OnMonitorUpdate(monitor_event);

    OnActualSizeUpdate(uint32_t(game_qwindow_->width()),
                       uint32_t(game_qwindow_->height()), destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) {
      return true;
    }

    if (game_qwindow_->isActive()) {
      OnFocusUpdate(true, destruction_receiver);
      if (destruction_receiver.IsWindowDestroyedOrClosed()) {
        return true;
      }
    }
  }

  // Initialize cursor visibility state
  cursor_currently_auto_hidden_ = false;
  CursorVisibility cursor_visibility = GetCursorVisibility();
  if (cursor_visibility == CursorVisibility::kAutoHidden) {
    cursor_auto_hide_last_pos_ = QCursor::pos();
    SetCursorAutoHideTimer();
  } else if (cursor_visibility == CursorVisibility::kHidden) {
    UpdateCursorVisibility();
  }

  return true;
}

bool QtWindow::OpenUIWindow() {
  // Create QMainWindow for UI process (with menu bar support)
  // UI process uses pure Qt widgets - no rendering surface needed
  qwindow_ = new QtWindowInternal(this);
  qwindow_->setWindowTitle(
      QString::fromUtf8(GetTitle().data(), GetTitle().size()));

  // Set default application icon from resources
  QIcon app_icon(":/xenia/icon.png");
  if (!app_icon.isNull()) {
    qwindow_->setWindowIcon(app_icon);
    // Also set application-level icon for taskbar/dock
    QGuiApplication::setWindowIcon(app_icon);

#if defined(XE_PLATFORM_WIN32)
    // On Windows, also set via Win32 API for taskbar
    HWND hwnd = reinterpret_cast<HWND>(qwindow_->winId());
    if (hwnd) {
      HICON hicon = app_icon.pixmap(32, 32).toImage().toHICON();
      if (hicon) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hicon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hicon);
      }
    }
#endif
  } else {
    XELOGW("Failed to load window icon from Qt resources");
  }

  // Create simple central widget with layout for game list
  QWidget* central_widget = new QWidget(qwindow_);
  qwindow_->setCentralWidget(central_widget);

  QVBoxLayout* layout = new QVBoxLayout(central_widget);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // Note: No ExternalRenderWidget created - UI process uses pure Qt widgets
  // The GameListDialogQt will be added to this layout in OnEmulatorInitialized

  const auto* main_menu = dynamic_cast<const QtMenuItem*>(GetMainMenu());
  if (main_menu) {
    // For kNormal type (root menu), add each child menu to the menu bar
    for (size_t i = 0; i < main_menu->child_count(); ++i) {
      auto* child = dynamic_cast<QtMenuItem*>(main_menu->child(i));
      if (child && child->menu()) {
        qwindow_->menuBar()->addMenu(child->menu());
      }
    }
  }

  // Use the screen where the cursor is, falling back to primary
  QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
  if (!screen) {
    screen = QGuiApplication::primaryScreen();
  }

  // Position window on target screen
  if (screen) {
    QRect screen_geometry = screen->availableGeometry();
    int x = screen_geometry.x() +
            (screen_geometry.width() - GetDesiredLogicalWidth()) / 2;
    int y = screen_geometry.y() +
            (screen_geometry.height() - GetDesiredLogicalHeight()) / 2;
    qwindow_->move(x, y);
  }

  if (IsFullscreen()) {
    qwindow_->showFullScreen();
  } else {
    qwindow_->resize(GetDesiredLogicalWidth(), GetDesiredLogicalHeight());
    qwindow_->show();
  }

  // Bring window to foreground
  qwindow_->activateWindow();
  qwindow_->raise();

  return true;
}

void QtWindow::RequestCloseImpl() {
  if (game_qwindow_) {
    game_qwindow_->close();
  } else if (qwindow_) {
    qwindow_->close();
  }
}

void QtWindow::ApplyNewFullscreen() {
  WindowDestructionReceiver destruction_receiver(this);

  if (game_qwindow_) {
    // Game window (QWindow) - simple fullscreen toggle
    if (IsFullscreen()) {
      game_qwindow_->showFullScreen();
    } else {
      game_qwindow_->showNormal();
    }
  } else if (qwindow_) {
    // UI window (QMainWindow) - handle menu bar visibility
    if (IsFullscreen()) {
      qwindow_->menuBar()->hide();
      qwindow_->showFullScreen();
    } else {
      qwindow_->showNormal();
      if (GetMainMenu()) {
        qwindow_->menuBar()->show();
      }
    }
  } else {
    return;
  }

  if (!destruction_receiver.IsWindowDestroyedOrClosed()) {
    HandleSizeUpdate(destruction_receiver);
  }
}

void QtWindow::ApplyNewTitle() {
  QString title = QString::fromUtf8(GetTitle().data(), GetTitle().size());
  if (game_qwindow_) {
    game_qwindow_->setTitle(title);
  } else if (qwindow_) {
    qwindow_->setWindowTitle(title);
  }
}

void QtWindow::LoadAndApplyIcon(const void* buffer, size_t size,
                                bool can_apply_state_in_current_phase) {
  if (!game_qwindow_ && !qwindow_) {
    return;
  }

  bool reset = !buffer || !size;

  if (reset) {
    // Reset to default/no icon
    if (game_qwindow_) {
      game_qwindow_->setIcon(QIcon());
    } else if (qwindow_) {
      qwindow_->setWindowIcon(QIcon());
    }
  } else {
    // Load icon from buffer (typically from game executable)
    QPixmap pixmap;
    if (pixmap.loadFromData(static_cast<const uchar*>(buffer),
                            static_cast<uint>(size))) {
      QIcon icon(pixmap);
      QGuiApplication::setWindowIcon(icon);

      if (game_qwindow_) {
        game_qwindow_->setIcon(icon);
#if defined(XE_PLATFORM_WIN32)
        HWND hwnd = reinterpret_cast<HWND>(game_qwindow_->winId());
        if (hwnd) {
          HICON hicon = icon.pixmap(32, 32).toImage().toHICON();
          if (hicon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hicon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hicon);
          }
        }
#endif
      } else if (qwindow_) {
        qwindow_->setWindowIcon(icon);
#if defined(XE_PLATFORM_WIN32)
        HWND hwnd = reinterpret_cast<HWND>(qwindow_->winId());
        if (hwnd) {
          HICON hicon = icon.pixmap(32, 32).toImage().toHICON();
          if (hicon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hicon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hicon);
          }
        }
#endif
      }
    }
  }
}

void QtWindow::ApplyNewMainMenu(MenuItem* old_main_menu) {
  if (!qwindow_) return;

  // Clear old menu
  if (old_main_menu) {
    qwindow_->menuBar()->clear();
  }

  // Add new menu children
  const auto* new_main_menu = dynamic_cast<const QtMenuItem*>(GetMainMenu());
  if (new_main_menu) {
    for (size_t i = 0; i < new_main_menu->child_count(); ++i) {
      auto* child = dynamic_cast<QtMenuItem*>(new_main_menu->child(i));
      if (child && child->menu()) {
        qwindow_->menuBar()->addMenu(child->menu());
      }
    }
  }

  qwindow_->menuBar()->setVisible(GetMainMenu() != nullptr && !IsFullscreen());
}

void QtWindow::ApplyNewCursorVisibility(
    CursorVisibility old_cursor_visibility) {
  CursorVisibility new_cursor_visibility = GetCursorVisibility();
  cursor_currently_auto_hidden_ = false;

  if (new_cursor_visibility == CursorVisibility::kAutoHidden) {
    cursor_auto_hide_last_pos_ = QCursor::pos();
    cursor_currently_auto_hidden_ = true;
  } else if (old_cursor_visibility == CursorVisibility::kAutoHidden) {
    if (cursor_auto_hide_timer_) {
      cursor_auto_hide_timer_->stop();
      delete cursor_auto_hide_timer_;
      cursor_auto_hide_timer_ = nullptr;
    }
  }

  UpdateCursorVisibility();
}

void QtWindow::FocusImpl() {
  if (game_qwindow_) {
    game_qwindow_->requestActivate();
    game_qwindow_->raise();
  } else if (qwindow_) {
    qwindow_->activateWindow();
    qwindow_->raise();
  }
}

std::unique_ptr<Surface> QtWindow::CreateSurfaceImpl(
    Surface::TypeFlags allowed_types) {
  // UI process doesn't do any rendering - pure Qt widgets
  if (!is_game_process_) {
    return nullptr;
  }

  // Game process needs a rendering surface
  if (!game_qwindow_) {
    XELOGE("CreateSurfaceImpl: game_qwindow_ is null for game process");
    return nullptr;
  }

#if defined(XE_PLATFORM_LINUX)
  XELOGI("CreateSurfaceImpl: platform={}, allowed_types=0x{:x}",
         QGuiApplication::platformName().toStdString(), allowed_types);

  // Try Wayland surface first
  if (allowed_types & Surface::kTypeFlag_WaylandWindow) {
    auto* waylandApp =
        qApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (waylandApp) {
      wl_display* display = waylandApp->display();
      if (display) {
        auto* waylandWindow =
            game_qwindow_
                ->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        if (waylandWindow) {
          wl_surface* surface = waylandWindow->surface();
          if (surface) {
            uint32_t width = game_qwindow_->width();
            uint32_t height = game_qwindow_->height();
            XELOGI("CreateSurfaceImpl: Created Wayland surface, size={}x{}",
                   width, height);
            return std::make_unique<WaylandWindowSurface>(display, surface,
                                                          width, height);
          }
        }
        XELOGW(
            "CreateSurfaceImpl: Failed to get Wayland surface, falling back to "
            "XCB");
      }
    }
  }

  // Fall back to XCB
  if (allowed_types & Surface::kTypeFlag_XcbWindow) {
    auto* x11App = qApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11App) {
      XELOGE("CreateSurfaceImpl: QX11Application is null");
      return nullptr;
    }
    xcb_connection_t* connection = x11App->connection();
    if (!connection) {
      XELOGE("CreateSurfaceImpl: XCB connection is null");
      return nullptr;
    }
    xcb_window_t window = static_cast<xcb_window_t>(game_qwindow_->winId());
    XELOGI("CreateSurfaceImpl: Created XCB surface with window ID: 0x{:x}",
           window);
    return std::make_unique<XcbWindowSurface>(connection, window);
  }

  XELOGE("CreateSurfaceImpl: No supported surface type in allowed_types ({})",
         allowed_types);
  return nullptr;
#elif defined(XE_PLATFORM_WIN32)
  if (allowed_types & Surface::kTypeFlag_Win32Hwnd) {
    auto& qt_context = static_cast<const QtWindowedAppContext&>(app_context());
    HWND hwnd = reinterpret_cast<HWND>(game_qwindow_->winId());
    if (!hwnd || !IsWindow(hwnd)) {
      XELOGE("CreateSurfaceImpl: Invalid HWND");
      return nullptr;
    }
    return std::make_unique<Win32HwndSurface>(qt_context.hinstance(), hwnd);
  }
  return nullptr;
#elif defined(XE_PLATFORM_MAC)
  if (allowed_types & Surface::kTypeFlag_MacNSView) {
    // On macOS, Qt's winId() returns an NSView*
    NSView* view = reinterpret_cast<NSView*>(game_qwindow_->winId());
    if (!view) {
      XELOGE("CreateSurfaceImpl: Invalid NSView");
      return nullptr;
    }
    return std::make_unique<MacNSViewSurface>(view);
  }
  return nullptr;
#else
  return nullptr;
#endif
}

void QtWindow::RequestPaintImpl() {
  // Only game process does rendering
  if (!game_qwindow_) {
    return;
  }

  // For QWindow, we need to trigger a paint as immediately as possible.
  // QTimer::singleShot(0, ...) executes on the next event loop iteration,
  // which is more immediate than requestUpdate() which may be batched.
  QTimer::singleShot(0, game_qwindow_, [this]() {
    if (game_qwindow_) {
      TriggerPaint();
    }
  });
}

void QtWindow::HandleSizeUpdate(
    WindowDestructionReceiver& destruction_receiver) {
  // Only game process needs size updates for rendering
  if (in_size_update_ || !game_qwindow_) {
    return;
  }

  in_size_update_ = true;
  OnActualSizeUpdate(uint32_t(game_qwindow_->width()),
                     uint32_t(game_qwindow_->height()), destruction_receiver);
  in_size_update_ = false;
}

VirtualKey QtWindow::TranslateVirtualKey(int qt_key) {
  switch (qt_key) {
    case Qt::Key_Backspace:
      return VirtualKey::kBack;
    case Qt::Key_Tab:
      return VirtualKey::kTab;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      return VirtualKey::kReturn;
    case Qt::Key_Shift:
      return VirtualKey::kShift;
    case Qt::Key_Control:
      return VirtualKey::kControl;
    case Qt::Key_Alt:
      return VirtualKey::kMenu;
    case Qt::Key_Pause:
      return VirtualKey::kPause;
    case Qt::Key_CapsLock:
      return VirtualKey::kCapital;
    case Qt::Key_Escape:
      return VirtualKey::kEscape;
    case Qt::Key_Space:
      return VirtualKey::kSpace;
    case Qt::Key_PageUp:
      return VirtualKey::kPrior;
    case Qt::Key_PageDown:
      return VirtualKey::kNext;
    case Qt::Key_End:
      return VirtualKey::kEnd;
    case Qt::Key_Home:
      return VirtualKey::kHome;
    case Qt::Key_Left:
      return VirtualKey::kLeft;
    case Qt::Key_Up:
      return VirtualKey::kUp;
    case Qt::Key_Right:
      return VirtualKey::kRight;
    case Qt::Key_Down:
      return VirtualKey::kDown;
    case Qt::Key_Insert:
      return VirtualKey::kInsert;
    case Qt::Key_Delete:
      return VirtualKey::kDelete;
    case Qt::Key_0:
      return VirtualKey::k0;
    case Qt::Key_1:
      return VirtualKey::k1;
    case Qt::Key_2:
      return VirtualKey::k2;
    case Qt::Key_3:
      return VirtualKey::k3;
    case Qt::Key_4:
      return VirtualKey::k4;
    case Qt::Key_5:
      return VirtualKey::k5;
    case Qt::Key_6:
      return VirtualKey::k6;
    case Qt::Key_7:
      return VirtualKey::k7;
    case Qt::Key_8:
      return VirtualKey::k8;
    case Qt::Key_9:
      return VirtualKey::k9;
    case Qt::Key_A:
      return VirtualKey::kA;
    case Qt::Key_B:
      return VirtualKey::kB;
    case Qt::Key_C:
      return VirtualKey::kC;
    case Qt::Key_D:
      return VirtualKey::kD;
    case Qt::Key_E:
      return VirtualKey::kE;
    case Qt::Key_F:
      return VirtualKey::kF;
    case Qt::Key_G:
      return VirtualKey::kG;
    case Qt::Key_H:
      return VirtualKey::kH;
    case Qt::Key_I:
      return VirtualKey::kI;
    case Qt::Key_J:
      return VirtualKey::kJ;
    case Qt::Key_K:
      return VirtualKey::kK;
    case Qt::Key_L:
      return VirtualKey::kL;
    case Qt::Key_M:
      return VirtualKey::kM;
    case Qt::Key_N:
      return VirtualKey::kN;
    case Qt::Key_O:
      return VirtualKey::kO;
    case Qt::Key_P:
      return VirtualKey::kP;
    case Qt::Key_Q:
      return VirtualKey::kQ;
    case Qt::Key_R:
      return VirtualKey::kR;
    case Qt::Key_S:
      return VirtualKey::kS;
    case Qt::Key_T:
      return VirtualKey::kT;
    case Qt::Key_U:
      return VirtualKey::kU;
    case Qt::Key_V:
      return VirtualKey::kV;
    case Qt::Key_W:
      return VirtualKey::kW;
    case Qt::Key_X:
      return VirtualKey::kX;
    case Qt::Key_Y:
      return VirtualKey::kY;
    case Qt::Key_Z:
      return VirtualKey::kZ;
    case Qt::Key_F1:
      return VirtualKey::kF1;
    case Qt::Key_F2:
      return VirtualKey::kF2;
    case Qt::Key_F3:
      return VirtualKey::kF3;
    case Qt::Key_F4:
      return VirtualKey::kF4;
    case Qt::Key_F5:
      return VirtualKey::kF5;
    case Qt::Key_F6:
      return VirtualKey::kF6;
    case Qt::Key_F7:
      return VirtualKey::kF7;
    case Qt::Key_F8:
      return VirtualKey::kF8;
    case Qt::Key_F9:
      return VirtualKey::kF9;
    case Qt::Key_F10:
      return VirtualKey::kF10;
    case Qt::Key_F11:
      return VirtualKey::kF11;
    case Qt::Key_F12:
      return VirtualKey::kF12;
    case Qt::Key_Asterisk:
      return VirtualKey::kMultiply;
    case Qt::Key_Plus:
      return VirtualKey::kAdd;
    case Qt::Key_Minus:
      return VirtualKey::kSubtract;
    default:
      return VirtualKey::kNone;
  }
}

bool QtWindow::OnEventFilter(QObject* obj, QEvent* event) {
  // With native window rendering, we don't need to handle paint events
  // The presenter renders directly to the native surface
  return false;
}

void QtWindow::OnCloseEvent(QCloseEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) {
    event->accept();
    return;
  }

  OnAfterClose();
  event->accept();
}

void QtWindow::OnResizeEvent(QResizeEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);
  // Use the event's size which is more reliable than reading widget dimensions
  if (!in_size_update_ && event) {
    in_size_update_ = true;
    OnActualSizeUpdate(uint32_t(event->size().width()),
                       uint32_t(event->size().height()), destruction_receiver);
    in_size_update_ = false;
  }
}

void QtWindow::OnFocusInEvent(QFocusEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(true, destruction_receiver);
}

void QtWindow::OnFocusOutEvent(QFocusEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(false, destruction_receiver);
}

void QtWindow::OnKeyPressEvent(QKeyEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);

  KeyEvent e(this, TranslateVirtualKey(event->key()), 1, false,
             event->modifiers() & Qt::ShiftModifier,
             event->modifiers() & Qt::ControlModifier,
             event->modifiers() & Qt::AltModifier, false);
  OnKeyDown(e, destruction_receiver);

  if (!event->text().isEmpty()) {
    KeyEvent char_event(this, VirtualKey(event->text()[0].unicode()), 1, false,
                        event->modifiers() & Qt::ShiftModifier,
                        event->modifiers() & Qt::ControlModifier,
                        event->modifiers() & Qt::AltModifier, false);
    OnKeyChar(char_event, destruction_receiver);
  }
}

void QtWindow::OnKeyReleaseEvent(QKeyEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);

  KeyEvent e(this, TranslateVirtualKey(event->key()), 1, true,
             event->modifiers() & Qt::ShiftModifier,
             event->modifiers() & Qt::ControlModifier,
             event->modifiers() & Qt::AltModifier, false);
  OnKeyUp(e, destruction_receiver);
}

void QtWindow::OnMousePressEvent(QMouseEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);

  // Handle cursor auto-hide - always reveal on click
  if (GetCursorVisibility() == CursorVisibility::kAutoHidden) {
    cursor_currently_auto_hidden_ = false;
    SetCursorAutoHideTimer();
    UpdateCursorVisibility();
  }

  MouseEvent::Button button = MouseEvent::Button::kNone;
  switch (event->button()) {
    case Qt::LeftButton:
      button = MouseEvent::Button::kLeft;
      break;
    case Qt::RightButton:
      button = MouseEvent::Button::kRight;
      break;
    case Qt::MiddleButton:
      button = MouseEvent::Button::kMiddle;
      break;
    case Qt::XButton1:
      button = MouseEvent::Button::kX1;
      break;
    case Qt::XButton2:
      button = MouseEvent::Button::kX2;
      break;
    default:
      break;
  }

  MouseEvent e(this, button, event->position().x(), event->position().y());
  OnMouseDown(e, destruction_receiver);
}

void QtWindow::OnMouseReleaseEvent(QMouseEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);

  MouseEvent::Button button = MouseEvent::Button::kNone;
  switch (event->button()) {
    case Qt::LeftButton:
      button = MouseEvent::Button::kLeft;
      break;
    case Qt::RightButton:
      button = MouseEvent::Button::kRight;
      break;
    case Qt::MiddleButton:
      button = MouseEvent::Button::kMiddle;
      break;
    case Qt::XButton1:
      button = MouseEvent::Button::kX1;
      break;
    case Qt::XButton2:
      button = MouseEvent::Button::kX2;
      break;
    default:
      break;
  }

  MouseEvent e(this, button, event->position().x(), event->position().y());
  OnMouseUp(e, destruction_receiver);
}

void QtWindow::OnMouseDoubleClickEvent(QMouseEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);

  // Only handle left button double-click
  if (event->button() != Qt::LeftButton) {
    return;
  }

  MouseEvent::Button button = MouseEvent::Button::kLeft;
  MouseEvent e(this, button, event->position().x(), event->position().y());
  OnMouseDoubleClick(e, destruction_receiver);
}

void QtWindow::OnMouseMoveEvent(QMouseEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);

  // Handle cursor auto-hide
  if (GetCursorVisibility() == CursorVisibility::kAutoHidden) {
    QPoint global_pos = QCursor::pos();
    if (global_pos != cursor_auto_hide_last_pos_) {
      cursor_currently_auto_hidden_ = false;
      SetCursorAutoHideTimer();
      cursor_auto_hide_last_pos_ = global_pos;
      UpdateCursorVisibility();
    }
  }

  MouseEvent e(this, MouseEvent::Button::kNone, event->position().x(),
               event->position().y());
  OnMouseMove(e, destruction_receiver);
}

void QtWindow::OnWheelEvent(QWheelEvent* event) {
  WindowDestructionReceiver destruction_receiver(this);

  int delta = event->angleDelta().y() / 8;
  MouseEvent e(this, MouseEvent::Button::kNone, event->position().x(),
               event->position().y(), delta);
  OnMouseWheel(e, destruction_receiver);
}

QtMenuItem::QtMenuItem(Type type, const std::string& text,
                       const std::string& hotkey,
                       std::function<void()> callback)
    : MenuItem(type, text, hotkey, std::move(callback)) {
  // Ensure Qt application is running before creating Qt objects
  if (!QCoreApplication::instance()) {
    // Qt not initialized yet - defer creation
    return;
  }

  // Get the active window as parent, or nullptr if none exists yet
  QWidget* parent = qobject_cast<QWidget*>(QApplication::activeWindow());

  if (type == MenuItem::Type::kSeparator) {
    action_ = new QAction(parent);
    action_->setSeparator(true);
  } else if (type == MenuItem::Type::kPopup) {
    QString qtext =
        QString::fromUtf8(text.c_str(), static_cast<int>(text.size()));
    menu_ = new QMenu(qtext, parent);
    action_ = menu_->menuAction();
  } else {
    QString qtext =
        QString::fromUtf8(text.c_str(), static_cast<int>(text.size()));
    action_ = new QAction(qtext, parent);
    if (!hotkey.empty()) {
      QString qhotkey =
          QString::fromUtf8(hotkey.c_str(), static_cast<int>(hotkey.size()));
      action_->setShortcut(QKeySequence(qhotkey));
    }
    QObject::connect(action_, &QAction::triggered, action_,
                     [this]() { OnTriggered(); });
  }
}

QtMenuItem::~QtMenuItem() {
  if (menu_) {
    delete menu_;
  } else if (action_) {
    delete action_;
  }
}

void QtMenuItem::OnChildAdded(MenuItem* child_item) {
  auto* qt_child = dynamic_cast<QtMenuItem*>(child_item);
  if (!qt_child || !menu_) {
    return;
  }

  if (qt_child->menu()) {
    menu_->addMenu(qt_child->menu());
  } else if (qt_child->action()) {
    menu_->addAction(qt_child->action());
  }
}

void QtMenuItem::OnChildRemoved(MenuItem* child_item) {
  auto* qt_child = dynamic_cast<QtMenuItem*>(child_item);
  if (!qt_child || !menu_) {
    return;
  }

  if (qt_child->action()) {
    menu_->removeAction(qt_child->action());
  }
}

void QtMenuItem::OnTriggered() { OnSelected(); }

void QtWindow::SetCursorAutoHideTimer() {
  // Delete existing timer if any
  if (cursor_auto_hide_timer_) {
    cursor_auto_hide_timer_->stop();
    delete cursor_auto_hide_timer_;
    cursor_auto_hide_timer_ = nullptr;
  }

  // Create new single-shot timer - parent to the active window object
  QObject* parent = game_qwindow_ ? static_cast<QObject*>(game_qwindow_)
                                  : static_cast<QObject*>(qwindow_);
  if (!parent) {
    return;
  }

  cursor_auto_hide_timer_ = new QTimer(parent);
  cursor_auto_hide_timer_->setSingleShot(true);
  QObject::connect(cursor_auto_hide_timer_, &QTimer::timeout, parent,
                   [this]() { OnCursorAutoHideTimeout(); });
  cursor_auto_hide_timer_->start(kDefaultCursorAutoHideMilliseconds);
}

void QtWindow::OnCursorAutoHideTimeout() {
  if (GetCursorVisibility() == CursorVisibility::kAutoHidden) {
    cursor_currently_auto_hidden_ = true;
    UpdateCursorVisibility();
  }

  // Clean up timer
  if (cursor_auto_hide_timer_) {
    delete cursor_auto_hide_timer_;
    cursor_auto_hide_timer_ = nullptr;
  }
}

void QtWindow::UpdateCursorVisibility() {
  CursorVisibility visibility = GetCursorVisibility();
  bool should_hide = (visibility == CursorVisibility::kHidden ||
                      (visibility == CursorVisibility::kAutoHidden &&
                       cursor_currently_auto_hidden_));

  Qt::CursorShape cursor = should_hide ? Qt::BlankCursor : Qt::ArrowCursor;

  if (game_qwindow_) {
    game_qwindow_->setCursor(cursor);
  } else if (qwindow_) {
    qwindow_->setCursor(cursor);
  }
}

}  // namespace ui
}  // namespace xe
