/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/profile_dialog_qt.h"

#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/logging.h"
#include "xenia/base/system.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/ui/title_info_ui.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/profile_editor_dialog_qt.h"
#include "xenia/ui/qt_util.h"
#include "xenia/ui/window_qt.h"

namespace xe {
namespace app {

using xe::ui::SafeQString;
using xe::ui::SafeStdString;

ProfileDialogQt::ProfileDialogQt(QWidget* parent,
                                 EmulatorWindow* emulator_window,
                                 hid::InputSystem* input_system)
    : ui::GamepadDialog(parent, input_system),
      emulator_window_(emulator_window) {
  SetupUI();
  LoadProfileIcons();
  PopulateProfileList();
}

ProfileDialogQt::~ProfileDialogQt() {
  // Cleanup handled by Qt
}

void ProfileDialogQt::SetupUI() {
  setWindowTitle("配置文件菜单");
  setModal(false);
  setAttribute(Qt::WA_DeleteOnClose);
  setMinimumSize(400, 500);

  auto* main_layout = new QVBoxLayout(this);

  // Profile list
  profile_list_ = new QListWidget(this);
  profile_list_->setContextMenuPolicy(Qt::CustomContextMenu);
  profile_list_->setIconSize(QSize(64, 64));
  connect(profile_list_, &QListWidget::itemClicked, this,
          &ProfileDialogQt::OnProfileItemClicked);
  connect(profile_list_, &QListWidget::itemDoubleClicked, this,
          &ProfileDialogQt::OnProfileItemDoubleClicked);
  connect(profile_list_, &QListWidget::customContextMenuRequested, this,
          &ProfileDialogQt::OnProfileContextMenu);

  main_layout->addWidget(profile_list_);

  // Buttons
  auto* button_layout = new QHBoxLayout();

  create_profile_button_ = new QPushButton("创建配置文件", this);
  connect(create_profile_button_, &QPushButton::clicked, this,
          &ProfileDialogQt::OnCreateProfileClicked);
  button_layout->addWidget(create_profile_button_);

  button_layout->addStretch();

  close_button_ = new QPushButton("关闭", this);
  connect(close_button_, &QPushButton::clicked, this,
          &ProfileDialogQt::OnCloseClicked);
  button_layout->addWidget(close_button_);

  main_layout->addLayout(button_layout);
}

void ProfileDialogQt::LoadProfileIcons() {
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

  // Load icons for all accounts (not just logged in profiles)
  auto accounts = profile_manager->GetAccounts();
  for (const auto& [xuid, account] : *accounts) {
    QPixmap icon = LoadProfileIcon(xuid);
    if (!icon.isNull()) {
      profile_icons_[xuid] = icon;
    }
  }
}

QPixmap ProfileDialogQt::LoadProfileIcon(uint64_t xuid) {
  if (!emulator_window_ || !emulator_window_->emulator()) {
    return QPixmap();
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    return QPixmap();
  }

  auto xam_state = kernel_state->xam_state();
  if (!xam_state) {
    return QPixmap();
  }

  auto profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    return QPixmap();
  }

  // First try to get the profile if it's already logged in
  const auto profile = profile_manager->GetProfile(xuid);
  std::vector<uint8_t> profile_icon_data;

  if (profile) {
    // Profile is logged in, get icon directly
    auto profile_icon_span =
        profile->GetProfileIcon(kernel::xam::XTileType::kGamerTile);
    profile_icon_data.assign(profile_icon_span.begin(),
                             profile_icon_span.end());
  } else {
    // Profile is not logged in, temporarily mount it to load the icon
    if (profile_manager->MountProfile(xuid)) {
      auto temp_profile = profile_manager->GetProfile(xuid);
      if (temp_profile) {
        auto profile_icon_span =
            temp_profile->GetProfileIcon(kernel::xam::XTileType::kGamerTile);
        profile_icon_data.assign(profile_icon_span.begin(),
                                 profile_icon_span.end());
      }
      profile_manager->DismountProfile(xuid);
    }
  }

  // If no custom icon found, return the default user icon
  if (profile_icon_data.empty()) {
    return QPixmap(":/xenia/user-icon.png");
  }

  QImage image;
  if (image.loadFromData(profile_icon_data.data(),
                         static_cast<int>(profile_icon_data.size()))) {
    return QPixmap::fromImage(image);
  }

  // Fallback to default icon if loading fails
  return QPixmap(":/xenia/user-icon.png");
}

void ProfileDialogQt::PopulateProfileList() {
  profile_list_->clear();

  if (!emulator_window_ || !emulator_window_->emulator()) {
    profile_list_->addItem("模拟器不可用");
    return;
  }

  auto kernel_state = emulator_window_->emulator()->kernel_state();
  if (!kernel_state) {
    profile_list_->addItem("内核状态不可用");
    return;
  }

  auto xam_state = kernel_state->xam_state();
  if (!xam_state) {
    profile_list_->addItem("XAM 状态不可用");
    return;
  }

  auto profile_manager = xam_state->profile_manager();
  if (!profile_manager) {
    profile_list_->addItem("配置文件管理器不可用");
    return;
  }

  auto profiles = profile_manager->GetAccounts();
  if (profiles->empty()) {
    profile_list_->addItem("未找到配置文件！");
    return;
  }

  for (auto& [xuid, account] : *profiles) {
    const uint8_t user_index =
        profile_manager->GetUserIndexAssignedToProfile(xuid);

    QString gamertag = SafeQString(account.GetGamertagString());
    QString status = (user_index == XUserIndexAny)
                         ? " (未登录)"
                         : fmt::format(" (槽位 {})", user_index + 1).c_str();

    auto* item = new QListWidgetItem(gamertag + status);
    item->setData(Qt::UserRole, QVariant::fromValue(xuid));

    // Set icon if available
    auto icon_it = profile_icons_.find(xuid);
    if (icon_it != profile_icons_.end()) {
      item->setIcon(QIcon(icon_it->second));
    }

    profile_list_->addItem(item);
  }
}

void ProfileDialogQt::RefreshProfiles() {
  // Clear existing icons first
  profile_icons_.clear();
  LoadProfileIcons();
  PopulateProfileList();
}

void ProfileDialogQt::OnProfileItemClicked(QListWidgetItem* item) {
  if (!item) {
    return;
  }

  selected_xuid_ = item->data(Qt::UserRole).toULongLong();
}

void ProfileDialogQt::OnProfileItemDoubleClicked(QListWidgetItem* item) {
  if (!item) {
    return;
  }

  uint64_t xuid = item->data(Qt::UserRole).toULongLong();
  if (xuid == 0) {
    return;  // Not a valid profile item
  }

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

  const uint8_t user_index =
      profile_manager->GetUserIndexAssignedToProfile(xuid);

  if (user_index == XUserIndexAny) {
    // Not logged in, so login
    profile_manager->Login(xuid);
    RefreshProfiles();
  } else {
    // Already logged in, so logout
    profile_manager->Logout(user_index);
    RefreshProfiles();
  }
}

void ProfileDialogQt::OnProfileContextMenu(const QPoint& pos) {
  QListWidgetItem* item = profile_list_->itemAt(pos);
  if (!item) {
    return;
  }

  uint64_t xuid = item->data(Qt::UserRole).toULongLong();
  if (xuid == 0) {
    return;  // Not a valid profile item
  }

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

  const uint8_t user_index =
      profile_manager->GetUserIndexAssignedToProfile(xuid);

  QMenu context_menu(this);

  if (user_index == XUserIndexAny) {
    QAction* login_action = context_menu.addAction("登录");
    connect(login_action, &QAction::triggered, [=, this]() {
      profile_manager->Login(xuid);
      RefreshProfiles();
    });

    QMenu* login_slot_menu = context_menu.addMenu("登录到槽位:");
    for (uint8_t i = 1; i <= XUserMaxUserCount; i++) {
      QAction* slot_action =
          login_slot_menu->addAction(SafeQString(fmt::format("槽位 {}", i)));
      connect(slot_action, &QAction::triggered, [=, this]() {
        profile_manager->Login(xuid, i - 1);
        RefreshProfiles();
      });
    }
  } else {
    QAction* logout_action = context_menu.addAction("登出");
    connect(logout_action, &QAction::triggered, [=, this]() {
      profile_manager->Logout(user_index);
      RefreshProfiles();
    });
  }

  // Modify (Gamercard)
  QAction* modify_action = context_menu.addAction("修改");
  connect(modify_action, &QAction::triggered, [=, this]() {
    auto* editor_dialog =
        new ProfileEditorDialogQt(nullptr, emulator_window_, xuid);
    editor_dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor_dialog, &QDialog::finished, [this](int result) {
      if (result == QDialog::Accepted) {
        RefreshProfiles();
      }
    });
    editor_dialog->show();
    editor_dialog->raise();
    editor_dialog->activateWindow();
  });

  QAction* show_content_action =
      context_menu.addAction("显示内容目录");
  connect(show_content_action, &QAction::triggered, [=, this]() {
    const auto path = profile_manager->GetProfileContentPath(
        xuid, emulator_window_->emulator()->kernel_state()->title_id());

    if (!std::filesystem::exists(path)) {
      std::filesystem::create_directories(path);
    }

    std::thread path_open(LaunchFileExplorer, path);
    path_open.detach();
  });

  if (!emulator_window_->emulator()->is_title_open()) {
    context_menu.addSeparator();
    QAction* delete_action = context_menu.addAction("删除配置文件...");
    connect(delete_action, &QAction::triggered, [=, this]() {
      auto profiles = profile_manager->GetAccounts();
      auto profile_it = profiles->find(xuid);
      if (profile_it == profiles->end()) {
        return;
      }

      QString gamertag = SafeQString(profile_it->second.GetGamertagString());

      QMessageBox::StandardButton reply = QMessageBox::question(
          this, "删除配置文件",
          QString("确定要删除配置文件: %1 (XUID: %2) 吗？\n\n"
                  "这将删除所有与此配置文件相关的数据，包括存档文件。")
              .arg(gamertag)
              .arg(xuid, 16, 16, QChar('0')),
          QMessageBox::Yes | QMessageBox::No);

      if (reply == QMessageBox::Yes) {
        profile_manager->DeleteProfile(xuid);
        RefreshProfiles();
      }
    });
  }

  context_menu.exec(profile_list_->mapToGlobal(pos));
}

void ProfileDialogQt::OnCreateProfileClicked() {
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

  // Prompt for gamertag
  bool ok;
  QString gamertag = QInputDialog::getText(
      this, "创建配置文件",
      "输入玩家代号 (3-15个字符):", QLineEdit::Normal, "", &ok);

  if (!ok || gamertag.isEmpty()) {
    return;
  }

  // Validate gamertag length
  if (gamertag.length() < 3 || gamertag.length() > 15) {
    QMessageBox::warning(this, "无效的代号",
                         "玩家代号必须在 3 到 15 个字符之间。");
    return;
  }

  // Create the profile
  std::string gamertag_string = SafeStdString(gamertag);
  bool autologin = (profile_manager->GetAccountCount() == 0);

  if (profile_manager->CreateProfile(gamertag_string, autologin, false)) {
    RefreshProfiles();
    QMessageBox::information(
        this, "配置文件已创建",
        QString("配置文件 '%1' 创建成功！").arg(gamertag));
  } else {
    QMessageBox::critical(this, "错误",
                          "创建配置文件失败，请重试。");
  }
}

void ProfileDialogQt::OnCloseClicked() { close(); }

}  // namespace app
}  // namespace xe
