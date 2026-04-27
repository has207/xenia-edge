/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/game_list_dialog_qt.h"

#include <QApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QUrl>
#include <chrono>
#include <fstream>
#include <regex>
#include <thread>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/chrono.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/base/system.h"
#include "xenia/base/utf8.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/ui/gamercard_ui.h"
#include "xenia/kernel/xam/ui/title_info_ui.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xam/xdbf/gpd_info_profile.h"
#include "xenia/kernel/xam/xdbf/gpd_info_title.h"
#include "xenia/ui/achievements_dialog_qt.h"
#include "xenia/ui/game_config_dialog_qt.h"
#include "xenia/ui/patches_dialog_qt.h"
#include "xenia/ui/profile_dialog_qt.h"
#include "xenia/ui/profile_editor_dialog_qt.h"
#include "xenia/ui/qt_util.h"

DECLARE_uint32(font_size);

namespace xe {
namespace app {

using xe::ui::SafeQString;
using xe::ui::SafeStdString;

GameListDialogQt::GameListDialogQt(QWidget* parent,
                                   EmulatorWindow* emulator_window)
    : QWidget(parent), emulator_window_(emulator_window) {
  SetupUI();

  // Start timer to monitor game state and profile state
  game_state_timer_ = new QTimer(this);
  connect(game_state_timer_, &QTimer::timeout, this, [this]() {
    UpdatePlayButtonState();
    UpdateProfileButtonState();
  });
  game_state_timer_->start(500);  // Check every 500ms
}

GameListDialogQt::~GameListDialogQt() {
  if (game_state_timer_) {
    game_state_timer_->stop();
  }
  // Cleanup is handled automatically by Qt
}

void GameListDialogQt::RefreshFonts() {
  int toolbar_font_size = cvars::font_size > 0 ? cvars::font_size : 10;
  int search_font_size = cvars::font_size > 0 ? cvars::font_size * 1.5 : 16;

  QFont toolbar_font = QApplication::font();
  toolbar_font.setPointSize(toolbar_font_size);

  QFont search_font = QApplication::font();
  search_font.setPointSize(search_font_size);

  QFont question_font = QApplication::font();
  question_font.setPointSize(32);
  question_font.setBold(true);

  // Apply fonts to toolbar labels
  if (open_label_) open_label_->setFont(toolbar_font);
  if (play_label_) play_label_->setFont(toolbar_font);
  if (settings_label_) settings_label_->setFont(toolbar_font);
  if (profile_label_) profile_label_->setFont(toolbar_font);
  if (search_box_) search_box_->setFont(search_font);
  if (profile_question_label_) profile_question_label_->setFont(question_font);

  // Apply fonts to table header labels
  QFont header_font = QApplication::font();
  header_font.setBold(true);
  if (icon_header_) icon_header_->setFont(header_font);
  if (title_header_) title_header_->setFont(header_font);
  if (last_played_header_) last_played_header_->setFont(header_font);

  // Reload game list to refresh game item fonts
  LoadGameList();
}

void GameListDialogQt::SetupUI() {
  // No window title, modal, or size settings - widget fills parent
  setAttribute(Qt::WA_DeleteOnClose);

  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  // Icon toolbar
  auto* toolbar_widget = new QWidget(this);
  auto* toolbar_layout = new QHBoxLayout(toolbar_widget);
  toolbar_layout->setContentsMargins(10, 10, 10, 10);
  toolbar_layout->setSpacing(20);

  // Create a layout for each button with icon and text separately

  // Open button
  auto* open_container = new QWidget(this);
  auto* open_layout = new QVBoxLayout(open_container);
  open_layout->setContentsMargins(0, 0, 0, 0);
  open_layout->setSpacing(5);

  auto* open_button = new QToolButton(this);
  open_button->setIcon(QIcon(":/xenia/folder-icon.png"));
  open_button->setIconSize(QSize(64, 64));
  open_button->setMinimumSize(100, 80);
  open_button->setMaximumSize(100, 80);
  connect(open_button, &QToolButton::clicked,
          [this]() { emit fileOpenRequested(); });

  open_label_ = new QLabel("打开", this);
  open_label_->setAlignment(Qt::AlignCenter);

  open_layout->addWidget(open_button);
  open_layout->addWidget(open_label_);
  toolbar_layout->addWidget(open_container);

  // Play button
  auto* play_container = new QWidget(this);
  auto* play_layout = new QVBoxLayout(play_container);
  play_layout->setContentsMargins(0, 0, 0, 0);
  play_layout->setSpacing(5);

  // Use a button with an icon
  play_button_ = new QToolButton(this);
  play_button_->setIcon(QIcon(":/xenia/play-icon.png"));
  play_button_->setIconSize(QSize(64, 64));
  play_button_->setMinimumSize(100, 80);
  play_button_->setMaximumSize(100, 80);
  play_button_->setEnabled(false);

  // Create opacity effect for greying out
  play_opacity_ = new QGraphicsOpacityEffect(play_button_);
  play_opacity_->setOpacity(0.4);
  play_button_->setGraphicsEffect(play_opacity_);

  connect(play_button_, &QToolButton::clicked, this,
          &GameListDialogQt::OnPlayClicked);

  play_label_ = new QLabel("运行", this);
  play_label_->setAlignment(Qt::AlignCenter);

  // Create opacity effect for label
  play_label_opacity_ = new QGraphicsOpacityEffect(play_label_);
  play_label_opacity_->setOpacity(0.4);
  play_label_->setGraphicsEffect(play_label_opacity_);

  play_layout->addWidget(play_button_);
  play_layout->addWidget(play_label_);
  toolbar_layout->addWidget(play_container);

  // Settings button
  auto* settings_container = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings_container);
  settings_layout->setContentsMargins(0, 0, 0, 0);
  settings_layout->setSpacing(5);

  settings_button_ = new QToolButton(this);
  settings_button_->setIcon(QIcon(":/xenia/settings-icon.png"));
  settings_button_->setIconSize(QSize(64, 64));
  settings_button_->setMinimumSize(100, 80);
  settings_button_->setMaximumSize(100, 80);
  connect(settings_button_, &QToolButton::clicked, this,
          &GameListDialogQt::OnSettingsClicked);

  settings_label_ = new QLabel("设置", this);
  settings_label_->setAlignment(Qt::AlignCenter);

  settings_layout->addWidget(settings_button_);
  settings_layout->addWidget(settings_label_);
  toolbar_layout->addWidget(settings_container);

  // Spacer to push profile button to the right
  toolbar_layout->addStretch();

  // Profile button
  auto* profile_container = new QWidget(this);
  auto* profile_layout = new QVBoxLayout(profile_container);
  profile_layout->setContentsMargins(0, 0, 0, 0);
  profile_layout->setSpacing(5);

  profile_button_ = new QToolButton(this);
  profile_button_->setIcon(QIcon(":/xenia/user-icon.png"));
  profile_button_->setIconSize(QSize(64, 64));
  profile_button_->setMinimumSize(100, 80);
  profile_button_->setMaximumSize(100, 80);
  profile_button_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(profile_button_, &QToolButton::clicked, this,
          &GameListDialogQt::OnProfileClicked);
  connect(profile_button_, &QToolButton::customContextMenuRequested, this,
          &GameListDialogQt::OnProfileContextMenu);

  // Question mark overlay for logged out state
  profile_question_label_ = new QLabel("?", profile_button_);
  profile_question_label_->setAlignment(Qt::AlignCenter);
  profile_question_label_->setGeometry(0, 0, 100, 80);
  profile_question_label_->setAttribute(Qt::WA_TransparentForMouseEvents);
  profile_question_label_->setStyleSheet(
      "color: white; background-color: rgba(0, 0, 0, 150);");
  profile_question_label_->setVisible(
      true);  // Will be controlled by UpdateProfileButtonState

  profile_label_ = new QLabel("Logged Out", this);
  profile_label_->setAlignment(Qt::AlignCenter);

  profile_layout->addWidget(profile_button_);
  profile_layout->addWidget(profile_label_);
  toolbar_layout->addWidget(profile_container);

  main_layout->addWidget(toolbar_widget);

  // Search box
  search_box_ = new QLineEdit(this);
  search_box_->setPlaceholderText("搜索游戏...");
  search_box_->setClearButtonEnabled(true);
  search_box_->setStyleSheet("padding-left: 8px;");
  connect(search_box_, &QLineEdit::textChanged, this,
          &GameListDialogQt::OnFilterTextChanged);

  // Only show search box if there are more than 5 games
  search_box_->setVisible(false);

  main_layout->addWidget(search_box_);

  // Custom header widget to match row layout
  auto* header_widget = new QWidget(this);
  auto* header_layout = new QHBoxLayout(header_widget);
  header_layout->setContentsMargins(10, 5, 10, 5);
  header_layout->setSpacing(15);

  icon_header_ = new QLabel("图标", this);
  icon_header_->setMinimumWidth(80);
  icon_header_->setMaximumWidth(80);
  header_layout->addWidget(icon_header_);

  title_header_ = new QLabel("标题", this);
  header_layout->addWidget(title_header_, 1);

  last_played_header_ = new QLabel("最后游玩", this);
  last_played_header_->setMinimumWidth(200);
  last_played_header_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  header_layout->addWidget(last_played_header_);

  main_layout->addWidget(header_widget);

  // Table widget
  table_widget_ = new QTableWidget(this);
  table_widget_->setColumnCount(1);
  table_widget_->horizontalHeader()->setVisible(false);
  table_widget_->horizontalHeader()->setStretchLastSection(true);
  table_widget_->verticalHeader()->setVisible(false);
  table_widget_->setShowGrid(false);
  table_widget_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_widget_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_widget_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_widget_->setContextMenuPolicy(Qt::CustomContextMenu);
  table_widget_->setMouseTracking(true);

  // Set Xbox green theme for table
  table_widget_->setStyleSheet(
      "QTableWidget::item:hover { background-color: transparent; }"
      "QTableWidget::item:selected { background-color: rgba(16, 124, 16, 120); "
      "}");

  // Auto-hide scrollbar - only show when hovering or scrolling
  table_widget_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  table_widget_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  // Install event filter to detect scrolling
  table_widget_->viewport()->installEventFilter(this);

  // Create timer for hiding scrollbar after scrolling stops
  scrollbar_hide_timer_ = new QTimer(this);
  scrollbar_hide_timer_->setSingleShot(true);
  scrollbar_hide_timer_->setInterval(1000);  // Hide after 1 second
  connect(scrollbar_hide_timer_, &QTimer::timeout, this,
          &GameListDialogQt::HideScrollbar);

  // Initialize scrollbar to hidden state
  HideScrollbar();

  connect(table_widget_, &QTableWidget::cellDoubleClicked, this,
          &GameListDialogQt::OnGameDoubleClicked);
  connect(table_widget_, &QTableWidget::customContextMenuRequested, this,
          &GameListDialogQt::OnGameRightClicked);
  connect(table_widget_, &QTableWidget::itemSelectionChanged, this,
          &GameListDialogQt::OnSelectionChanged);

  main_layout->addWidget(table_widget_);

  // Apply initial fonts to all widgets
  RefreshFonts();
}

void GameListDialogQt::LoadGameList() {
  game_entries_.clear();
  title_icons_.clear();

  if (!emulator_window_ || !emulator_window_->emulator()) {
    PopulateTable();
    return;
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    PopulateTable();
    return;
  }

  auto xam_state = kernel_state->xam_state();
  if (!xam_state) {
    PopulateTable();
    return;
  }

  auto profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    PopulateTable();
    return;
  }

  auto scanned_titles = profile_manager->ScanAllProfilesForTitles();

  for (const auto& scanned : scanned_titles) {
    std::vector<DiscInfo> all_discs;
    for (const auto& disc : scanned.all_discs) {
      all_discs.push_back({disc.path, disc.label});
    }

    game_entries_.push_back({scanned.title_name, scanned.path_to_file,
                             all_discs, scanned.last_run_time, scanned.title_id,
                             std::vector<uint8_t>()});
  }

  // Show/hide search box based on number of entries
  if (search_box_) {
    search_box_->setVisible(game_entries_.size() > 5);
  }

  TryLoadIcons();
  PopulateTable();
}

void GameListDialogQt::TryLoadIcons() {
  // Icons and achievement data are only loaded if a user is logged in
  if (!emulator_window_ || !emulator_window_->emulator()) {
    has_logged_in_profile_ = false;
    return;
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    has_logged_in_profile_ = false;
    return;
  }

  auto xam_state = kernel_state->xam_state();
  if (!xam_state) {
    has_logged_in_profile_ = false;
    return;
  }

  auto profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    has_logged_in_profile_ = false;
    return;
  }

  // Check if any profile is logged in
  has_logged_in_profile_ = false;
  for (uint8_t user_index = 0; user_index < 4; user_index++) {
    const auto profile = profile_manager->GetProfile(user_index);
    if (profile) {
      has_logged_in_profile_ = true;

      // Load icons, achievement data, and profile-specific timestamps for all
      // game entries from this profile's title GPDs
      XELOGI(
          "Loading icons, achievement data, and timestamps for {} game entries "
          "from profile "
          "{:016X}",
          game_entries_.size(), profile->xuid());

      // Get the profile's dashboard GPD to get profile-specific timestamps
      const auto& dashboard_gpd = profile->dashboard_gpd();
      if (dashboard_gpd.IsValid()) {
        auto profile_titles_info = dashboard_gpd.GetTitlesInfo();

        for (auto& game_entry : game_entries_) {
          // Update timestamp to this profile's last played time
          bool found = false;
          for (const auto& title_info : profile_titles_info) {
            if (title_info->title_id == game_entry.title_id) {
              if (title_info->last_played.is_valid()) {
                auto last_played_tp = chrono::WinSystemClock::to_sys(
                    title_info->last_played.to_time_point());
                game_entry.last_run_time =
                    std::chrono::system_clock::to_time_t(last_played_tp);
              } else {
                game_entry.last_run_time =
                    0;  // Profile hasn't played this game
              }
              found = true;
              break;
            }
          }
          // If not found in this profile's dashboard, the profile hasn't played
          // it
          if (!found) {
            game_entry.last_run_time = 0;
          }
        }
      }

      for (auto& game_entry : game_entries_) {
        // Get achievement stats from the profile
        auto stats = profile->GetTitleAchievementStats(game_entry.title_id);
        game_entry.achievements_total = stats.achievements_total;
        game_entry.achievements_unlocked = stats.achievements_unlocked;
        game_entry.gamerscore_total = stats.gamerscore_total;
        game_entry.gamerscore_earned = stats.gamerscore_earned;

        if (stats.achievements_total > 0) {
          XELOGD("Title {:08X}: {}/{} achievements, {}/{} gamerscore",
                 game_entry.title_id, game_entry.achievements_unlocked,
                 game_entry.achievements_total, game_entry.gamerscore_earned,
                 game_entry.gamerscore_total);
        }

        // Skip if we already loaded this icon
        if (title_icons_.find(game_entry.title_id) != title_icons_.end()) {
          continue;
        }

        // Get the title icon from the profile's title GPD
        auto icon_data = profile->GetTitleIcon(game_entry.title_id);
        if (icon_data.empty()) {
          XELOGI("No icon data for title {:08X} from profile {:016X}",
                 game_entry.title_id, profile->xuid());
          continue;  // No icon available for this title
        }

        // Create QPixmap from icon data
        QPixmap pixmap = CreateIconPixmap(icon_data);
        if (!pixmap.isNull()) {
          XELOGD("Loaded icon for title {:08X}, size: {} bytes",
                 game_entry.title_id, icon_data.size());
          title_icons_[game_entry.title_id] = pixmap;
        } else {
          XELOGI("Failed to create QPixmap for title {:08X}",
                 game_entry.title_id);
        }
      }
    }
  }
}

QPixmap GameListDialogQt::CreateIconPixmap(
    const std::vector<uint8_t>& icon_data) {
  if (icon_data.empty()) {
    return QPixmap();
  }

  QImage image;
  if (image.loadFromData(icon_data.data(),
                         static_cast<int>(icon_data.size()))) {
    return QPixmap::fromImage(image);
  }

  return QPixmap();
}

void GameListDialogQt::RefreshIcons() {
  // Clear existing icons and reload the entire game list from GPD
  title_icons_.clear();
  LoadGameList();
}

void GameListDialogQt::PopulateTable() {
  table_widget_->setRowCount(0);

  QString filter_text = search_box_ ? search_box_->text().toLower() : "";

  for (size_t i = 0; i < game_entries_.size(); i++) {
    const auto& entry = game_entries_[i];

    // Apply filter
    if (!filter_text.isEmpty()) {
      QString title = SafeQString(entry.title_name).toLower();
      QString path =
          SafeQString(xe::path_to_utf8(entry.path_to_file)).toLower();

      if (!title.contains(filter_text) && !path.contains(filter_text)) {
        continue;
      }
    }

    int row = table_widget_->rowCount();
    table_widget_->insertRow(row);

    // Create a single container widget for the entire row
    auto* row_widget = new QWidget();
    row_widget->setAttribute(Qt::WA_Hover);
    row_widget->setObjectName("GameListRow");
    row_widget->setStyleSheet(
        "#GameListRow { border-bottom: 1px solid rgba(128, 128, 128, 50); }"
        "#GameListRow:hover { background-color: rgba(16, 124, 16, 80); }");
    auto* row_layout = new QHBoxLayout(row_widget);
    row_layout->setContentsMargins(10, 10, 10, 10);
    row_layout->setSpacing(15);

    // Icon
    auto* icon_label = new QLabel();
    icon_label->setAlignment(Qt::AlignCenter);
    icon_label->setMinimumSize(80, 80);
    icon_label->setMaximumSize(80, 80);
    icon_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon_label->setStyleSheet(
        "QLabel { border: 2px solid rgba(255, 255, 255, 100); border-radius: "
        "4px; }");

    auto icon_it = title_icons_.find(entry.title_id);
    if (icon_it != title_icons_.end() && !icon_it->second.isNull()) {
      icon_label->setPixmap(icon_it->second.scaled(80, 80, Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation));
    } else {
      if (!has_logged_in_profile_) {
        icon_label->setText("Not\nlogged\nin");
        icon_label->setStyleSheet(
            "QLabel { color: gray; border: 2px solid rgba(255, 255, 255, 100); "
            "border-radius: 4px; }");
      } else {
        // Logged in but no icon - user hasn't played this game
        icon_label->setText("Not\nplayed");
        icon_label->setStyleSheet(
            "QLabel { color: gray; border: 2px solid rgba(255, 255, 255, 100); "
            "border-radius: 4px; }");
      }
    }
    row_layout->addWidget(icon_label);

    // Title container with title and path
    auto* title_container = new QWidget();
    title_container->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* title_layout = new QVBoxLayout(title_container);
    title_layout->setContentsMargins(0, 0, 0, 0);
    title_layout->setSpacing(2);

    // Check if file is corrupted (GPD has no title name)
    bool file_corrupted = entry.title_name.empty();

    // Title - large font
    QString display_title;
    if (file_corrupted) {
      display_title = QString("File Corrupted");
    } else {
      // Use fromUtf8 with error handling for potentially invalid data
      display_title =
          QString::fromUtf8(entry.title_name.c_str(),
                            static_cast<qsizetype>(entry.title_name.size()));
      // If conversion failed or resulted in empty string, mark as corrupted
      if (display_title.isEmpty()) {
        display_title = QString("File Corrupted");
        file_corrupted = true;
      } else {
        // Add disc count for multi-disc games
        if (entry.all_discs.size() > 1) {
          display_title += QString(" (%1 discs)").arg(entry.all_discs.size());
        }
      }
    }
    auto* title_label = new QLabel(display_title);
    title_label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    title_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    QFont title_font = QApplication::font();
    title_font.setBold(true);
    int base_font_size = cvars::font_size > 0 ? cvars::font_size : 13;
    title_font.setPointSize(base_font_size * 1.5);  // 1.5x larger
    title_label->setFont(title_font);
    if (file_corrupted) {
      title_label->setStyleSheet("color: red;");
    }
    title_layout->addWidget(title_label);

    // Path - smaller font (only show if path is available)
    if (!entry.path_to_file.empty()) {
      auto* path_label =
          new QLabel(SafeQString(xe::path_to_utf8(entry.path_to_file)));
      path_label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
      path_label->setAttribute(Qt::WA_TransparentForMouseEvents);
      QFont path_font = QApplication::font();
      path_font.setPointSize(base_font_size * 0.8);  // Smaller font
      path_label->setFont(path_font);
      path_label->setStyleSheet("color: gray;");
      title_layout->addWidget(path_label);
    }

    // Achievement info - smaller font (only show if there are achievements)
    if (entry.achievements_total > 0) {
      QString achievement_text =
          QString("Achievements: %1/%2 | Gamerscore: %3/%4")
              .arg(entry.achievements_unlocked)
              .arg(entry.achievements_total)
              .arg(entry.gamerscore_earned)
              .arg(entry.gamerscore_total);
      auto* achievement_label = new QLabel(achievement_text);
      achievement_label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
      achievement_label->setAttribute(Qt::WA_TransparentForMouseEvents);
      QFont achievement_font = QApplication::font();
      achievement_font.setPointSize(base_font_size * 0.8);  // Smaller font
      achievement_label->setFont(achievement_font);

      // Color based on completion
      if (entry.achievements_unlocked == entry.achievements_total &&
          entry.achievements_total > 0) {
        achievement_label->setStyleSheet("color: #4CAF50;");  // Green for 100%
      } else if (entry.achievements_unlocked > 0) {
        achievement_label->setStyleSheet(
            "color: #FFA726;");  // Orange for partial
      } else {
        achievement_label->setStyleSheet("color: gray;");  // Gray for none
      }
      title_layout->addWidget(achievement_label);
    }

    row_layout->addWidget(title_container, 1);  // Stretch factor

    // Last played
    auto* last_played_label =
        new QLabel(SafeQString(FormatLastPlayed(entry.last_run_time)));
    last_played_label->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    last_played_label->setMinimumWidth(200);
    last_played_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    row_layout->addWidget(last_played_label);

    table_widget_->setCellWidget(row, 0, row_widget);
    table_widget_->setRowHeight(row, 100);

    // Store the path and title_id in the row for later retrieval
    table_widget_->setItem(row, 0, new QTableWidgetItem());
    table_widget_->item(row, 0)->setData(
        Qt::UserRole, SafeQString(xe::path_to_utf8(entry.path_to_file)));
    table_widget_->item(row, 0)->setData(Qt::UserRole + 1, entry.title_id);
  }
}

std::string GameListDialogQt::FormatLastPlayed(time_t timestamp) {
  if (timestamp == 0) {
    return "-";
  }

  return fmt::format("{:%Y-%m-%d %H:%M}", *std::localtime(&timestamp));
}

void GameListDialogQt::OnFilterTextChanged(const QString& text) {
  PopulateTable();
}

void GameListDialogQt::OnGameDoubleClicked(int row, int column) {
  auto* item = table_widget_->item(row, 0);
  if (!item) {
    return;
  }

  QString path_str = item->data(Qt::UserRole).toString();
  uint32_t title_id = item->data(Qt::UserRole + 1).toUInt();

  if (path_str.isEmpty()) {
    // No path available - show message and open file picker
    LaunchGameWithFilePicker();
    return;
  }

  // Check if this is a multi-disc game
  const GameListEntry* entry = nullptr;
  for (const auto& e : game_entries_) {
    if (e.title_id == title_id) {
      entry = &e;
      break;
    }
  }

  if (entry && entry->all_discs.size() > 1) {
    // Show disc selection dialog
    auto selected_path = ShowDiscSelectionDialog(entry);
    if (selected_path.has_value()) {
      LaunchGame(selected_path.value(), title_id);
    }
  } else {
    // Single disc game, launch directly
    std::filesystem::path path = xe::to_path(SafeStdString(path_str));
    LaunchGame(path, title_id);
  }
}

void GameListDialogQt::OnGameRightClicked(const QPoint& pos) {
  int row = table_widget_->rowAt(pos.y());
  if (row < 0) {
    return;
  }

  auto* item = table_widget_->item(row, 0);
  if (!item) {
    return;
  }

  QString path_str = item->data(Qt::UserRole).toString();
  uint32_t title_id = item->data(Qt::UserRole + 1).toUInt();

  std::filesystem::path path = xe::to_path(SafeStdString(path_str));
  bool has_path = !path_str.isEmpty();

  // Get profile manager
  auto kernel_state = emulator_window_->emulator()->kernel_state();
  auto xam_state = kernel_state->xam_state();
  auto profile_manager = xam_state->profile_manager();

  QMenu context_menu;

  // Find the game entry to check for multi-disc
  const GameListEntry* entry = nullptr;
  for (const auto& e : game_entries_) {
    if (e.title_id == title_id) {
      entry = &e;
      break;
    }
  }

  // Always show launch/open option, but change text based on path availability
  QAction* launch_action = nullptr;
  QMenu* launch_menu = nullptr;
  QAction* open_folder_action = nullptr;

  if (has_path) {
    if (entry && entry->all_discs.size() > 1) {
      launch_menu = context_menu.addMenu("启动");
      size_t disc_num = 1;
      for (const auto& disc : entry->all_discs) {
        QString disc_label = disc.label.empty()
                                 ? QString("光盘 %1").arg(disc_num)
                                 : SafeQString(disc.label);
        QAction* disc_action = launch_menu->addAction(disc_label);
        disc_num++;

        std::filesystem::path disc_path_copy = disc.path;
        connect(disc_action, &QAction::triggered,
                [this, disc_path_copy, title_id]() {
                  LaunchGame(disc_path_copy, title_id);
                });
      }
    } else {
      launch_action = context_menu.addAction("启动");
    }
    open_folder_action = context_menu.addAction("打开所在文件夹");
  } else {
    launch_action = context_menu.addAction("打开");
  }

  // Get the primary profile (same one shown in profile button)
  bool is_signedin = false;
  uint64_t xuid = 0;

  // Use the profile from slot 0 as primary (matches profile button logic)
  const auto primary_profile =
      profile_manager->GetProfile(static_cast<uint8_t>(0));
  if (primary_profile) {
    is_signedin = true;
    xuid = primary_profile->xuid();
  }

  QAction* saves_action = nullptr;
  if (title_id != 0) {
    saves_action = context_menu.addAction("存档");
    if (!is_signedin) {
      saves_action->setEnabled(false);
    } else {
      auto saves_path = profile_manager->GetProfileContentPath(
          xuid, title_id, XContentType::kSavedGame);
      if (!std::filesystem::exists(saves_path)) {
        saves_action->setEnabled(false);
      }
    }
  }

  QAction* title_updates_action = nullptr;
  if (title_id != 0) {
    title_updates_action = context_menu.addAction("标题更新");
    auto tu_path = profile_manager->GetProfileContentPath(
        0, title_id, XContentType::kInstaller);
    if (!std::filesystem::exists(tu_path)) {
      title_updates_action->setEnabled(false);
    }
  }

  QAction* dlc_action = nullptr;
  if (title_id != 0) {
    dlc_action = context_menu.addAction("DLC");
    auto dlc_path = profile_manager->GetProfileContentPath(
        0, title_id, XContentType::kMarketplaceContent);
    if (!std::filesystem::exists(dlc_path)) {
      dlc_action->setEnabled(false);
    }
  }

  QMenu* patches_menu = nullptr;
  if (title_id != 0) {
    auto available_patches = FindPatchesForTitle(title_id);
    patches_menu = context_menu.addMenu("补丁");

    if (available_patches.empty()) {
      // No patches found - disable the menu
      patches_menu->setEnabled(false);
    } else {
      // Patches found - populate the submenu
      for (const auto& patch_path : available_patches) {
        // Extract a display name from the filename
        // Format: "TITLEID - Game Name (Version).patch.toml"
        std::string filename = xe::path_to_utf8(patch_path.filename());

        // Remove the title ID prefix and " - "
        std::string display_name = filename;
        size_t dash_pos = filename.find(" - ");
        if (dash_pos != std::string::npos) {
          display_name = filename.substr(dash_pos + 3);
        }

        // Remove ".patch.toml" suffix
        size_t suffix_pos = display_name.rfind(".patch.toml");
        if (suffix_pos != std::string::npos) {
          display_name = display_name.substr(0, suffix_pos);
        }

        QAction* patch_action =
            patches_menu->addAction(SafeQString(display_name));

        // Open patch management dialog - capture patch_path by value
        std::filesystem::path patch_path_copy = patch_path;
        connect(patch_action, &QAction::triggered,
                [this, title_id, patch_path_copy]() {
                  auto* dialog = new PatchesDialogQt(this, emulator_window_,
                                                     title_id, patch_path_copy);
                  dialog->exec();
                  delete dialog;
                });
      }
    }
  }

  QAction* achievements_action = nullptr;
  if (title_id != 0) {
    achievements_action = context_menu.addAction("成就");
    if (!is_signedin) {
      achievements_action->setEnabled(false);
    } else {
      std::string title_name_str;
      for (const auto& entry : game_entries_) {
        if (entry.title_id == title_id) {
          title_name_str = entry.title_name;
          break;
        }
      }

      connect(achievements_action, &QAction::triggered,
              [this, xuid, title_id, title_name_str]() {
                ShowAchievementsDialog(xuid, title_id,
                                       SafeQString(title_name_str));
              });
    }
  }

  QAction* config_action = nullptr;
  if (title_id != 0) {
    config_action = context_menu.addAction("配置覆盖...");
  }

  QMenu* compatibility_menu = nullptr;
  QAction* compatibility_canary_action = nullptr;
  QAction* compatibility_master_action = nullptr;
  if (title_id != 0) {
    compatibility_menu = context_menu.addMenu("兼容性");
    compatibility_canary_action = compatibility_menu->addAction("Canary");
    compatibility_master_action = compatibility_menu->addAction("Master");
  }

  context_menu.addSeparator();

  QAction* remove_action = context_menu.addAction("🗑 从列表中移除");

  QAction* selected = context_menu.exec(table_widget_->mapToGlobal(pos));

  if (selected == launch_action && launch_action) {
    if (has_path) {
      LaunchGame(path, title_id);
    } else {
      LaunchGameWithFilePicker();
    }
  } else if (selected == open_folder_action && open_folder_action) {
    OpenContainingFolder(path);
  } else if (selected == saves_action && saves_action) {
    // Open the saves folder for the primary profile
    auto saves_path = profile_manager->GetProfileContentPath(
        xuid, title_id, XContentType::kSavedGame);
    std::thread path_open(LaunchFileExplorer, saves_path);
    path_open.detach();
  } else if (selected == title_updates_action && title_updates_action) {
    // Open the title updates folder
    auto tu_path = profile_manager->GetProfileContentPath(
        0, title_id, XContentType::kInstaller);
    std::thread path_open(LaunchFileExplorer, tu_path);
    path_open.detach();
  } else if (selected == dlc_action && dlc_action) {
    // Open the DLC folder
    auto dlc_path = profile_manager->GetProfileContentPath(
        0, title_id, XContentType::kMarketplaceContent);
    std::thread path_open(LaunchFileExplorer, dlc_path);
    path_open.detach();
  } else if (selected == achievements_action && achievements_action) {
    // Achievements dialog will open in a separate call
  } else if (selected == config_action && config_action) {
    // Get the title name from the game entry
    std::string title_name_str;
    for (const auto& entry : game_entries_) {
      if (entry.title_id == title_id) {
        title_name_str = entry.title_name;
        break;
      }
    }
    if (title_name_str.empty()) {
      title_name_str = fmt::format("{:08X}", title_id);
    }

    auto* dialog = new GameConfigDialogQt(this, emulator_window_, title_id,
                                          title_name_str);
    dialog->exec();
    delete dialog;
  } else if (selected == compatibility_canary_action &&
             compatibility_canary_action) {
    // Open Canary game compatibility page with this title ID
    const std::string base_url =
        "https://github.com/xenia-canary/game-compatibility/issues";
    const std::string url =
        fmt::format("{}?q=is%3Aissue+is%3Aopen+{:08X}", base_url, title_id);
    QDesktopServices::openUrl(QUrl(SafeQString(url)));
  } else if (selected == compatibility_master_action &&
             compatibility_master_action) {
    // Open Master game compatibility page with this title ID
    const std::string base_url =
        "https://github.com/xenia-project/game-compatibility/issues";
    const std::string url =
        fmt::format("{}?q=is%3Aissue+is%3Aopen+{:08X}", base_url, title_id);
    QDesktopServices::openUrl(QUrl(SafeQString(url)));
  } else if (selected == remove_action) {
    RemoveTitleFromDashboard(title_id);
  }
}

void GameListDialogQt::LaunchGame(const std::filesystem::path& path,
                                  uint32_t title_id) {
  if (emulator_window_) {
    // Check if the file exists
    if (!std::filesystem::exists(path)) {
      // Show warning popup
      QMessageBox::warning(
          this, "Game File Not Found",
          QString("The game file does not exist:\n\n%1\n\n"
                  "The path will be removed from the game list.\n"
                  "Please select the correct game file.")
              .arg(SafeQString(xe::path_to_utf8(path))));

      // Remove the bad path from all dashboard GPDs using ProfileManager
      if (title_id != 0) {
        auto kernel_state = emulator_window_->emulator()->kernel_state();
        if (kernel_state) {
          auto xam_state = kernel_state->xam_state();
          if (xam_state) {
            auto profile_manager = xam_state->profile_manager();
            if (profile_manager) {
              profile_manager->ClearTitlePath(title_id);
            }
          }
        }
      }

      // Reload the game list to reflect the removed path
      LoadGameList();

      // Open file picker
      emit fileOpenRequested();
      return;
    }

    emit launchGameRequested(path);
  }
}

void GameListDialogQt::LaunchGameWithFilePicker() {
  // Show message to user
  QMessageBox::information(this, "Game Path Not Found",
                           "The installation path for this game is not known.\n"
                           "Please select the game file to launch it.");

  // Open file picker
  emit fileOpenRequested();
}

void GameListDialogQt::OpenContainingFolder(const std::filesystem::path& path) {
  std::filesystem::path folder = path.parent_path();
  std::thread path_open(LaunchFileExplorer, folder);
  path_open.detach();
}

void GameListDialogQt::RemoveTitleFromDashboard(uint32_t title_id) {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }

  auto xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return;
  }

  auto profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    return;
  }

  // Ask for confirmation
  QMessageBox::StandardButton reply;
  reply = QMessageBox::question(
      nullptr, "Remove Title",
      QString("Are you sure you want to remove this title from the game list?"),
      QMessageBox::Yes | QMessageBox::No);

  if (reply != QMessageBox::Yes) {
    return;
  }

  if (profile_manager->RemoveTitleFromAllProfiles(title_id)) {
    // Reload the game list
    LoadGameList();
  } else {
    QMessageBox::warning(nullptr, "Remove Title",
                         "Failed to remove title from dashboard.");
  }
}

void GameListDialogQt::OnPlayClicked() {
  if (!selected_game_path_.empty()) {
    // Check if this is a multi-disc game
    const GameListEntry* entry = nullptr;
    for (const auto& e : game_entries_) {
      if (e.title_id == selected_game_title_id_) {
        entry = &e;
        break;
      }
    }

    if (entry && entry->all_discs.size() > 1) {
      // Show disc selection dialog
      auto selected_path = ShowDiscSelectionDialog(entry);
      if (selected_path.has_value()) {
        LaunchGame(selected_path.value(), selected_game_title_id_);
      }
    } else {
      // Single disc game, launch directly
      LaunchGame(selected_game_path_, selected_game_title_id_);
    }
  } else {
    // No path available for selected game - show message and open file picker
    LaunchGameWithFilePicker();
  }
}

void GameListDialogQt::OnSettingsClicked() { emit settingsRequested(); }

void GameListDialogQt::OnProfileClicked() {
  // Left click always opens profile dialog (Qt version for UI process)
  if (profile_dialog_) {
    profile_dialog_->raise();
    profile_dialog_->activateWindow();
    return;
  }

  if (emulator_window_ && emulator_window_->emulator()) {
    profile_dialog_ =
        new ProfileDialogQt(nullptr, emulator_window_,
                            emulator_window_->emulator()->input_system());
    connect(profile_dialog_, &QDialog::finished, this, [this]() {
      profile_dialog_ = nullptr;
      UpdateProfileButtonState();
      RefreshIcons();
    });
    profile_dialog_->show();
    profile_dialog_->raise();
    profile_dialog_->activateWindow();
  }
}

void GameListDialogQt::OnSelectionChanged() {
  auto selected_items = table_widget_->selectedItems();
  if (!selected_items.isEmpty()) {
    int row = selected_items[0]->row();
    auto* item = table_widget_->item(row, 0);
    if (item) {
      QString path_str = item->data(Qt::UserRole).toString();
      uint32_t title_id = item->data(Qt::UserRole + 1).toUInt();
      if (!path_str.isEmpty()) {
        selected_game_path_ = xe::to_path(SafeStdString(path_str));
        selected_game_title_id_ = title_id;
        UpdatePlayButtonState();
        return;
      }
    }
  }

  // No selection
  selected_game_path_.clear();
  selected_game_title_id_ = 0;
  UpdatePlayButtonState();
}

void GameListDialogQt::UpdatePlayButtonState() {
  play_button_->setIcon(QIcon(":/xenia/play-icon.png"));
  play_label_->setText("Play");
  play_button_->setToolTip("Launch selected game");
  bool is_enabled = !selected_game_path_.empty();
  play_button_->setEnabled(is_enabled);

  // Greyed out when disabled, normal when enabled
  if (is_enabled) {
    play_opacity_->setOpacity(1.0);  // Normal appearance
    play_label_opacity_->setOpacity(1.0);
  } else {
    play_opacity_->setOpacity(0.4);  // Greyed out
    play_label_opacity_->setOpacity(0.4);
  }
}

void GameListDialogQt::UpdateProfileButtonState() {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    profile_label_->setText("Logged Out");
    profile_question_label_->setVisible(true);
    profile_button_->setIcon(QIcon(":/xenia/user-icon.png"));
    return;
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    profile_label_->setText("Logged Out");
    profile_question_label_->setVisible(true);
    profile_button_->setIcon(QIcon(":/xenia/user-icon.png"));
    return;
  }

  auto xam_state = kernel_state->xam_state();
  if (!xam_state) {
    profile_label_->setText("Logged Out");
    profile_question_label_->setVisible(true);
    profile_button_->setIcon(QIcon(":/xenia/user-icon.png"));
    return;
  }

  auto profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    profile_label_->setText("Logged Out");
    profile_question_label_->setVisible(true);
    profile_button_->setIcon(QIcon(":/xenia/user-icon.png"));
    return;
  }

  // Check if any profile is logged in
  for (uint8_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    const auto profile = profile_manager->GetProfile(user_index);
    if (profile) {
      // Found a logged in profile, show gamertag
      auto accounts = profile_manager->GetAccounts();
      auto account_it = accounts->find(profile->xuid());
      if (account_it != accounts->end()) {
        QString gamertag = SafeQString(account_it->second.GetGamertagString());
        profile_label_->setText(gamertag);
        profile_question_label_->setVisible(
            false);  // Hide question mark when logged in
        current_profile_xuid_ = profile->xuid();

        // Load and display profile picture
        const auto profile_icon =
            profile->GetProfileIcon(kernel::xam::XTileType::kPersonalGamerTile);
        if (!profile_icon.empty()) {
          // Use personal gamer tile if available
          QPixmap pixmap;
          if (pixmap.loadFromData(profile_icon.data(),
                                  static_cast<int>(profile_icon.size()))) {
            profile_button_->setIcon(QIcon(pixmap));
            return;
          }
        }

        // Fall back to regular gamer tile
        const auto gamer_tile =
            profile->GetProfileIcon(kernel::xam::XTileType::kGamerTile);
        if (!gamer_tile.empty()) {
          QPixmap pixmap;
          if (pixmap.loadFromData(gamer_tile.data(),
                                  static_cast<int>(gamer_tile.size()))) {
            profile_button_->setIcon(QIcon(pixmap));
            return;
          }
        }

        // If no profile icon is available, keep the default icon
        profile_button_->setIcon(QIcon(":/xenia/user-icon.png"));
        return;
      }
    }
  }

  // No profiles logged in
  profile_label_->setText("Logged Out");
  profile_question_label_->setVisible(true);
  profile_button_->setIcon(QIcon(":/xenia/user-icon.png"));
  current_profile_xuid_ = 0;
}

void GameListDialogQt::OnProfileContextMenu(const QPoint& pos) {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }

  auto xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return;
  }

  auto profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    return;
  }

  // If there's a logged in profile, show the context menu for it
  if (current_profile_xuid_ == 0) {
    // No profile logged in, open the profile dialog
    OnProfileClicked();
    return;
  }

  uint64_t xuid = current_profile_xuid_;
  const uint8_t user_index =
      profile_manager->GetUserIndexAssignedToProfile(xuid);

  QMenu context_menu;

  if (user_index != XUserIndexAny) {
    QAction* logout_action = context_menu.addAction("Logout");
    connect(logout_action, &QAction::triggered, [=, this]() {
      profile_manager->Logout(user_index);
      UpdateProfileButtonState();
      RefreshIcons();  // Refresh icons after logout
    });
  }

  // Modify (Gamercard)
  QAction* modify_action = context_menu.addAction("Modify");
  connect(modify_action, &QAction::triggered, [=, this]() {
    auto* editor_dialog =
        new ProfileEditorDialogQt(nullptr, emulator_window_, xuid);
    editor_dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor_dialog, &QDialog::finished, [this](int result) {
      if (result == QDialog::Accepted) {
        UpdateProfileButtonState();
        RefreshIcons();
      }
    });
    editor_dialog->show();
    editor_dialog->raise();
    editor_dialog->activateWindow();
  });

  QAction* show_content_action =
      context_menu.addAction("Show Content Directory");
  connect(show_content_action, &QAction::triggered, [=, this]() {
    const auto path = profile_manager->GetProfileContentPath(
        xuid, emulator_window_->emulator()->kernel_state()->title_id());

    if (!std::filesystem::exists(path)) {
      std::filesystem::create_directories(path);
    }

    std::thread path_open(LaunchFileExplorer, path);
    path_open.detach();
  });

  context_menu.addSeparator();

  // Profiles Menu
  QAction* profiles_menu_action = context_menu.addAction("Profiles Menu");
  connect(profiles_menu_action, &QAction::triggered, [this]() {
    // Use Qt profile dialog directly for UI process
    if (profile_dialog_) {
      profile_dialog_->raise();
      profile_dialog_->activateWindow();
      return;
    }

    if (emulator_window_ && emulator_window_->emulator()) {
      profile_dialog_ =
          new ProfileDialogQt(nullptr, emulator_window_,
                              emulator_window_->emulator()->input_system());
      connect(profile_dialog_, &QDialog::finished, this, [this]() {
        profile_dialog_ = nullptr;
        UpdateProfileButtonState();
        RefreshIcons();
      });
      profile_dialog_->show();
      profile_dialog_->raise();
      profile_dialog_->activateWindow();
    }
  });

  context_menu.exec(profile_button_->mapToGlobal(pos));
}

bool GameListDialogQt::eventFilter(QObject* obj, QEvent* event) {
  if (obj == table_widget_->viewport() && event->type() == QEvent::Wheel) {
    ShowScrollbar();
    return false;  // Let the event propagate
  }
  return QWidget::eventFilter(obj, event);
}

void GameListDialogQt::ShowScrollbar() {
  if (!table_widget_) {
    return;
  }

  // Show scrollbar by making handle visible
  table_widget_->verticalScrollBar()->setStyleSheet(
      "QScrollBar:vertical {"
      "    border: none;"
      "    background: transparent;"
      "    width: 10px;"
      "    margin: 0px;"
      "}"
      "QScrollBar::handle:vertical {"
      "    background: rgba(16, 124, 16, 180);"
      "    min-height: 20px;"
      "    border-radius: 5px;"
      "}"
      "QScrollBar::handle:vertical:hover {"
      "    background: rgba(16, 124, 16, 220);"
      "}"
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
      "    height: 0px;"
      "}"
      "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
      "    background: none;"
      "}");

  // Restart hide timer
  if (scrollbar_hide_timer_) {
    scrollbar_hide_timer_->start();
  }
}

void GameListDialogQt::HideScrollbar() {
  if (!table_widget_) {
    return;
  }

  // Hide scrollbar by making handle transparent
  table_widget_->verticalScrollBar()->setStyleSheet(
      "QScrollBar:vertical {"
      "    border: none;"
      "    background: transparent;"
      "    width: 10px;"
      "    margin: 0px;"
      "}"
      "QScrollBar::handle:vertical {"
      "    background: rgba(16, 124, 16, 0);"
      "    min-height: 20px;"
      "    border-radius: 5px;"
      "}"
      "QScrollBar::handle:vertical:hover {"
      "    background: rgba(16, 124, 16, 180);"
      "}"
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
      "    height: 0px;"
      "}"
      "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
      "    background: none;"
      "}");
}

std::vector<std::filesystem::path> GameListDialogQt::FindPatchesForTitle(
    uint32_t title_id) {
  std::vector<std::filesystem::path> patches;
  std::map<std::string, std::filesystem::path> unique_patches;

  if (title_id == 0) {
    return patches;
  }

  if (!emulator_window_ || !emulator_window_->emulator()) {
    return patches;
  }

  // Create the title ID prefix to search for (e.g., "4D5307D5")
  std::string title_id_prefix = fmt::format("{:08X}", title_id);

  // Helper lambda to scan a directory for patches
  auto scan_patch_directory = [&](const std::filesystem::path& dir,
                                  bool allow_overwrite) {
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
      return;
    }

    try {
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
          continue;
        }

        auto filename = xe::path_to_utf8(entry.path().filename());

        // Check if filename starts with the title ID and ends with .toml
        if (filename.size() >= title_id_prefix.size() &&
            filename.compare(0, title_id_prefix.size(), title_id_prefix) == 0 &&
            filename.ends_with(".toml")) {
          // Only add if we're allowed to overwrite or if not already present
          if (allow_overwrite ||
              unique_patches.find(filename) == unique_patches.end()) {
            unique_patches[filename] = entry.path();
          }
        }
      }
    } catch (const std::filesystem::filesystem_error& e) {
      XELOGE("Error scanning patches directory {}: {}", xe::path_to_utf8(dir),
             e.what());
    }
  };

  // First, scan storage_root/patches (writable, user-managed patches)
  auto storage_patches_dir =
      emulator_window_->emulator()->storage_root() / "patches";
  scan_patch_directory(storage_patches_dir, true);

  // Second, scan executable_dir/game_patches (bundled, read-only patches)
  auto bundled_patches_dir = config::GetBundledDataPath("game_patches");
  scan_patch_directory(bundled_patches_dir,
                       false);  // Don't overwrite storage_root patches

  // Convert map to vector and sort by filename
  for (const auto& [filename, path] : unique_patches) {
    patches.push_back(path);
  }

  std::sort(patches.begin(), patches.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
              return xe::path_to_utf8(a.filename()) <
                     xe::path_to_utf8(b.filename());
            });

  return patches;
}

std::optional<std::filesystem::path> GameListDialogQt::ShowDiscSelectionDialog(
    const GameListEntry* entry) {
  if (!entry || entry->all_discs.size() <= 1) {
    return std::nullopt;
  }

  QDialog disc_dialog(this);
  disc_dialog.setWindowTitle("Select Disc");
  disc_dialog.setMinimumWidth(500);

  auto* layout = new QVBoxLayout(&disc_dialog);

  std::string label_text =
      fmt::format("This game has {} discs. Select which disc to launch:",
                  entry->all_discs.size());
  auto* label = new QLabel(SafeQString(label_text));
  label->setWordWrap(true);
  layout->addWidget(label);

  auto* list_widget = new QListWidget();
  size_t disc_num = 1;
  for (const auto& disc : entry->all_discs) {
    QString disc_label = disc.label.empty() ? QString("Disc %1").arg(disc_num)
                                            : SafeQString(disc.label);
    auto* list_item = new QListWidgetItem(disc_label);
    list_item->setData(Qt::UserRole, SafeQString(xe::path_to_utf8(disc.path)));
    list_widget->addItem(list_item);
    disc_num++;
  }
  list_widget->setCurrentRow(0);
  layout->addWidget(list_widget);

  auto* button_layout = new QHBoxLayout();

  auto* rename_button = new QPushButton("Rename...");
  auto* delete_button = new QPushButton("Delete");
  button_layout->addWidget(rename_button);
  button_layout->addWidget(delete_button);
  button_layout->addStretch();

  auto* launch_button = new QPushButton("Launch");
  auto* cancel_button = new QPushButton("Cancel");

  connect(launch_button, &QPushButton::clicked, &disc_dialog, &QDialog::accept);
  connect(cancel_button, &QPushButton::clicked, &disc_dialog, &QDialog::reject);
  connect(list_widget, &QListWidget::itemDoubleClicked, &disc_dialog,
          &QDialog::accept);

  // Delete button handler
  connect(
      delete_button, &QPushButton::clicked,
      [this, list_widget, entry, &disc_dialog]() {
        auto* selected_item = list_widget->currentItem();
        if (!selected_item) {
          return;
        }

        QString path_str = selected_item->data(Qt::UserRole).toString();
        std::filesystem::path disc_path = xe::to_path(SafeStdString(path_str));

        auto result =
            QMessageBox::question(this, "Delete Disc Entry",
                                  QString("Are you sure you want to remove "
                                          "this disc from the list?\n\n%1")
                                      .arg(selected_item->text()),
                                  QMessageBox::Yes | QMessageBox::No);

        if (result == QMessageBox::Yes) {
          if (emulator_window_ && emulator_window_->emulator()) {
            auto kernel_state = emulator_window_->emulator()->kernel_state();
            if (kernel_state) {
              auto profile_manager =
                  kernel_state->xam_state()->profile_manager();
              auto profile =
                  profile_manager->GetProfile(static_cast<uint8_t>(0));
              if (profile) {
                profile->RemoveDiscPath(entry->title_id, disc_path);

                // Remove from list widget
                delete list_widget->takeItem(list_widget->currentRow());

                // If no more discs, close the dialog
                if (list_widget->count() == 0) {
                  disc_dialog.reject();
                }

                // Reload the game list to refresh
                const_cast<GameListDialogQt*>(this)->LoadGameList();
              }
            }
          }
        }
      });

  // Rename button handler
  connect(rename_button, &QPushButton::clicked, [this, list_widget, entry]() {
    auto* selected_item = list_widget->currentItem();
    if (!selected_item) {
      return;
    }

    QString current_label = selected_item->text();
    QString path_str = selected_item->data(Qt::UserRole).toString();
    std::filesystem::path disc_path = xe::to_path(SafeStdString(path_str));

    bool ok;
    QString new_label = QInputDialog::getText(
        this, "Rename Disc",
        "Enter new label for this disc:", QLineEdit::Normal, current_label,
        &ok);

    if (ok && !new_label.isEmpty()) {
      std::string label_std = SafeStdString(new_label);

      // Validate label doesn't contain ::
      if (label_std.find("::") != std::string::npos) {
        QMessageBox::warning(this, "Invalid Label",
                             "Label cannot contain '::' sequence");
        return;
      }

      // Update the label in the GPD
      if (emulator_window_ && emulator_window_->emulator()) {
        auto kernel_state = emulator_window_->emulator()->kernel_state();
        if (kernel_state) {
          auto profile_manager = kernel_state->xam_state()->profile_manager();
          auto profile = profile_manager->GetProfile(static_cast<uint8_t>(0));
          if (profile) {
            profile->SetDiscLabel(entry->title_id, disc_path, label_std);

            // Update the list item
            selected_item->setText(SafeQString(label_std));

            // Reload the game list to refresh
            const_cast<GameListDialogQt*>(this)->LoadGameList();
          }
        }
      }
    }
  });

  button_layout->addWidget(launch_button);
  button_layout->addWidget(cancel_button);
  layout->addLayout(button_layout);

  if (disc_dialog.exec() == QDialog::Accepted) {
    auto* selected_item = list_widget->currentItem();
    if (selected_item) {
      QString selected_path = selected_item->data(Qt::UserRole).toString();
      return xe::to_path(SafeStdString(selected_path));
    }
  }

  return std::nullopt;
}

void GameListDialogQt::ShowAchievementsDialog(uint64_t xuid, uint32_t title_id,
                                              const QString& title_name) {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return;
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return;
  }

  auto user_tracker = kernel_state->xam_state()->user_tracker();
  const auto title_info = user_tracker->GetUserTitleInfo(xuid, title_id);
  if (!title_info) {
    return;
  }

  const auto profile = kernel_state->xam_state()->GetUserProfile(xuid);
  if (!profile) {
    return;
  }

  auto* achievements_dialog = new xe::ui::AchievementsDialogQt(
      nullptr, kernel_state, &title_info.value(), profile);
  achievements_dialog->setAttribute(Qt::WA_DeleteOnClose);
  achievements_dialog->show();
  achievements_dialog->raise();
  achievements_dialog->activateWindow();
}

}  // namespace app
}  // namespace xe
