/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QWidget>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/config.h"
#include "xenia/ui/game_list_dialog_qt.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_qt.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/main_win.h"
#include "xenia/base/platform_win.h"
#endif

// Exists to create a mapping for positional arg.
DEFINE_transient_path(target, "", "Specifies the target file to run.",
                      "General");

DECLARE_path(custom_font_path);
DECLARE_uint32(font_size);

int main(int argc, char** argv) {
#if XE_PLATFORM_MAC
  // Strip macOS/Xcode-injected flags that aren't valid cvars.
  std::vector<std::string> filtered_args;
  filtered_args.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.rfind("-NSDocumentRevisionsDebugMode", 0) == 0 ||
        arg.rfind("-ApplePersistenceIgnoreState", 0) == 0 ||
        arg.rfind("-YES", 0) == 0 || arg.rfind("YES", 0) == 0) {
      continue;
    }
    filtered_args.emplace_back(argv[i]);
  }
  std::vector<char*> filtered_argv_vec;
  filtered_argv_vec.reserve(filtered_args.size());
  for (auto& s : filtered_args) {
    filtered_argv_vec.push_back(const_cast<char*>(s.c_str()));
  }
  int filtered_argc = static_cast<int>(filtered_argv_vec.size());
  char** filtered_argv = filtered_argv_vec.data();
#else
  int filtered_argc = argc;
  char** filtered_argv = argv;
#endif

  // Parse arguments early to determine if this is a game or UI process
  // Must happen before QApplication creation for QT_QPA_PLATFORM to take effect
  cvar::ParseLaunchArguments(filtered_argc, filtered_argv,
                             "[Path to .iso/.xex]", {"target"});

  bool is_game_process = !cvars::target.empty();

#if XE_PLATFORM_LINUX
  // UI process: Force X11 backend for proper Qt rendering
  // Game process: Use QT_QPA_PLATFORM if set, otherwise auto-detect
  if (!is_game_process) {
    qputenv("QT_QPA_PLATFORM", "xcb");
  } else {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
      qunsetenv("QT_QPA_PLATFORM");
    }
  }
#endif

  QApplication qt_app(filtered_argc, filtered_argv);

  // Force Fusion style to ensure consistent styling across platforms
  qt_app.setStyle("Fusion");

  // Force dark theme with Xbox green highlights
  QPalette dark_palette;

  // Set colors for all color groups (Active, Inactive, Disabled) to avoid any
  // blue
  for (auto group :
       {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
    dark_palette.setColor(group, QPalette::Window, QColor(53, 53, 53));
    dark_palette.setColor(group, QPalette::WindowText, Qt::white);
    dark_palette.setColor(group, QPalette::Base, QColor(35, 35, 35));
    dark_palette.setColor(group, QPalette::AlternateBase, QColor(53, 53, 53));
    dark_palette.setColor(group, QPalette::ToolTipBase, QColor(25, 25, 25));
    dark_palette.setColor(group, QPalette::ToolTipText, Qt::white);
    dark_palette.setColor(
        group, QPalette::Text,
        group == QPalette::Disabled ? QColor(127, 127, 127) : Qt::white);
    dark_palette.setColor(group, QPalette::Button, QColor(53, 53, 53));
    dark_palette.setColor(
        group, QPalette::ButtonText,
        group == QPalette::Disabled ? QColor(127, 127, 127) : Qt::white);
    dark_palette.setColor(group, QPalette::BrightText, Qt::red);
    dark_palette.setColor(group, QPalette::Link, QColor(16, 124, 16));
    dark_palette.setColor(group, QPalette::LinkVisited, QColor(14, 106, 14));
    dark_palette.setColor(group, QPalette::Highlight,
                          group == QPalette::Disabled ? QColor(14, 106, 14)
                                                      : QColor(16, 124, 16));
    dark_palette.setColor(group, QPalette::HighlightedText, Qt::white);
    dark_palette.setColor(group, QPalette::Light, QColor(80, 80, 80));
    dark_palette.setColor(group, QPalette::Midlight, QColor(66, 66, 66));
    dark_palette.setColor(group, QPalette::Dark, QColor(26, 26, 26));
    dark_palette.setColor(group, QPalette::Mid, QColor(40, 40, 40));
    dark_palette.setColor(group, QPalette::Shadow, Qt::black);
  }

  qt_app.setPalette(dark_palette);

  // Fix menu spacing and styling
  qt_app.setStyleSheet(
      "QMenuBar::item { "
      "  padding: 5px 10px; "
      "} "
      "QMenuBar::item:selected { "
      "  background-color: rgb(16, 124, 16); "
      "  color: white; "
      "} "
      "QMenuBar::item:pressed { "
      "  background-color: rgb(16, 124, 16); "
      "  color: white; "
      "} "
      "QMenu { "
      "  background-color: rgb(53, 53, 53); "
      "  border: 1px solid rgb(80, 80, 80); "
      "  border-radius: 0px; "
      "} "
      "QMenu::item { "
      "  padding: 5px 25px 5px 10px; "
      "  background-color: transparent; "
      "} "
      "QMenu::item:selected { "
      "  background-color: rgb(16, 124, 16); "
      "  color: white; "
      "} "
      "QMenu::indicator { width: 0px; margin-left: 0px; } "
      // Scrollbars - Xbox green theme
      "QScrollBar:vertical { "
      "  border: none; "
      "  background: transparent; "
      "  width: 12px; "
      "  margin: 0px; "
      "} "
      "QScrollBar::handle:vertical { "
      "  background: rgba(16, 124, 16, 180); "
      "  min-height: 20px; "
      "  border-radius: 6px; "
      "} "
      "QScrollBar::handle:vertical:hover { "
      "  background: rgba(16, 124, 16, 220); "
      "} "
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { "
      "  height: 0px; "
      "} "
      "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { "
      "  background: none; "
      "} "
      "QScrollBar:horizontal { "
      "  border: none; "
      "  background: transparent; "
      "  height: 12px; "
      "  margin: 0px; "
      "} "
      "QScrollBar::handle:horizontal { "
      "  background: rgba(16, 124, 16, 180); "
      "  min-width: 20px; "
      "  border-radius: 6px; "
      "} "
      "QScrollBar::handle:horizontal:hover { "
      "  background: rgba(16, 124, 16, 220); "
      "} "
      "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { "
      "  width: 0px; "
      "} "
      "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { "
      "  background: none; "
      "} "
      // Table headers - Xbox green theme
      "QHeaderView::section { "
      "  background-color: rgb(45, 45, 45); "
      "  color: white; "
      "  padding: 5px; "
      "  border: 1px solid rgb(80, 80, 80); "
      "} "
      "QHeaderView::section:hover { "
      "  background-color: rgb(16, 124, 16); "
      "} "
      // Table widgets - Xbox green focus outline
      "QTableWidget:focus, QTableView:focus { "
      "  border: 2px solid rgb(16, 124, 16); "
      "  outline: none; "
      "} "
      "QTableWidget, QTableView { "
      "  border: 1px solid rgb(80, 80, 80); "
      "} "
      // Checkboxes - Xbox green theme
      "QCheckBox::indicator:checked { "
      "  background-color: rgb(16, 124, 16); "
      "  border: 1px solid rgb(16, 124, 16); "
      "} "
      "QCheckBox::indicator:hover { "
      "  border: 1px solid rgb(16, 124, 16); "
      "} "
      // Input fields - Xbox green focus
      "QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, "
      "QDoubleSpinBox:focus, QComboBox:focus { "
      "  border: 2px solid rgb(16, 124, 16); "
      "  outline: none; "
      "} "
      "QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, "
      "QComboBox { "
      "  border: 1px solid rgb(80, 80, 80); "
      "} "
      // ComboBox dropdown - Xbox green theme
      "QComboBox:hover { "
      "  border: 2px solid rgb(16, 124, 16); "
      "} "
      "QComboBox:on { "
      "  border: 2px solid rgb(16, 124, 16); "
      "} "
      "QComboBox::drop-down:hover { "
      "  background-color: rgba(16, 124, 16, 100); "
      "} "
      "QComboBox QAbstractItemView::item:hover { "
      "  background-color: rgba(16, 124, 16, 80); "
      "} "
      // List widgets - Xbox green theme
      "QListView::item:selected, QTreeView::item:selected { "
      "  background-color: rgb(16, 124, 16); "
      "  color: white; "
      "} "
      "QListView::item:hover, QTreeView::item:hover { "
      "  background-color: rgba(16, 124, 16, 80); "
      "} "
      // Buttons hover - Xbox green theme
      "QPushButton:hover { "
      "  background-color: rgba(16, 124, 16, 100); "
      "  border: 1px solid rgb(16, 124, 16); "
      "} "
      "QPushButton:pressed { "
      "  background-color: rgb(16, 124, 16); "
      "} "
      // Tab widget - Xbox green theme
      "QTabBar::tab:selected { "
      "  background-color: rgb(16, 124, 16); "
      "  color: white; "
      "} "
      "QTabBar::tab:hover { "
      "  background-color: rgba(16, 124, 16, 100); "
      "}");

  // Set different application name for game processes so they show as separate
  // dock entries. Don't set desktopFileName for game processes - this allows
  // the programmatic icon (set via QGuiApplication::setWindowIcon) to be used
  // for the taskbar/dock instead of looking up a non-existent .desktop file.
  if (is_game_process) {
    qt_app.setApplicationName("Xbox 360 Game");
  } else {
    qt_app.setApplicationName("Xenia Edge");
    qt_app.setDesktopFileName("xenia_edge");
  }

  // Use Qt's own menu bar instead of native on all platforms
  qt_app.setAttribute(Qt::AA_DontUseNativeMenuBar);

  int result;

  {
    xe::ui::QtWindowedAppContext app_context(&qt_app);

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

#if XE_PLATFORM_WIN32
    // Initialize COM for Windows
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
      return EXIT_FAILURE;
    }

    // Use Windows-specific initialization which properly sets up logging
    xe::InitializeWin32App(app->GetName(), is_game_process);
#else
    xe::InitializeLogging(app->GetName(), is_game_process);
#endif

    if (app->OnInitialize()) {
      // Store the default font to allow resetting
      QFont default_font = qt_app.font();

      // Helper to apply custom font and/or font size
      auto apply_font_settings = [&qt_app, default_font]() {
        // Start from default font to handle clearing custom font
        QFont app_font = default_font;

        // Load custom font if specified
        if (!cvars::custom_font_path.empty()) {
          // Resolve relative paths from executable directory
          std::filesystem::path font_path = cvars::custom_font_path;
          if (font_path.is_relative()) {
            font_path = xe::filesystem::GetExecutableFolder() / font_path;
          }

          if (std::filesystem::exists(font_path)) {
            QString font_path_str =
                QString::fromStdString(xe::path_to_utf8(font_path));
            int font_id = QFontDatabase::addApplicationFont(font_path_str);
            if (font_id != -1) {
              QStringList font_families =
                  QFontDatabase::applicationFontFamilies(font_id);
              if (!font_families.isEmpty()) {
                app_font.setFamily(font_families.first());
              }
            }
          }
        }

        // Apply font size (use default size of 14 if not specified)
        int font_size = cvars::font_size > 0 ? cvars::font_size : 13;
        app_font.setPointSize(font_size);

        qt_app.setFont(app_font);

        // Refresh game lists to re-apply font settings
        for (QWidget* widget : qt_app.allWidgets()) {
          if (auto* game_list =
                  qobject_cast<xe::app::GameListDialogQt*>(widget)) {
            game_list->RefreshFonts();
          }
        }
      };

      // Set application font immediately so new widgets inherit it
      apply_font_settings();

      // Re-apply font settings when config is saved (also used by
      // OnEmulatorInitialized to apply fonts after GameListDialogQt is created)
      config::SetConfigSavedCallback(apply_font_settings);

      app_context.RunMainQtLoop();

      // Clear callback before qt_app goes out of scope
      config::SetConfigSavedCallback(nullptr);

      result = EXIT_SUCCESS;
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

#if XE_PLATFORM_WIN32
  xe::ShutdownWin32App();
  CoUninitialize();
#else
  xe::ShutdownLogging();
#endif

  return result;
}
