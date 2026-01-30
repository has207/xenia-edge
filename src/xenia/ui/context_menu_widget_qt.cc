/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/context_menu_widget_qt.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>

#include "xenia/hid/input_system.h"

namespace xe {
namespace app {

ContextMenuWidgetQt::ContextMenuWidgetQt(QWidget* parent,
                                         hid::InputSystem* input_system)
    : QWidget(parent),
      input_system_(input_system),
      poll_timer_(nullptr),
      focused_index_(-1),
      prev_buttons_(0) {
  setAttribute(Qt::WA_DeleteOnClose);
  setAutoFillBackground(true);
  setAttribute(Qt::WA_StyledBackground, true);

  setStyleSheet(
      "background-color: rgb(45, 45, 45); "
      "border: 1px solid rgb(85, 85, 85);");

  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(2, 2, 2, 2);
  layout_->setSpacing(0);

  // Install global event filter to close menu on clicks outside
  qApp->installEventFilter(this);

  // Start gamepad polling if input system is available
  if (input_system_) {
    // Block input to the game while this menu is open
    input_system_->AddUIInputBlocker();

    // Initialize prev_buttons_ to current state so held buttons aren't
    // detected as "just pressed" when the menu opens
    hid::X_INPUT_STATE state;
    for (uint32_t user_index = 0; user_index < 4; user_index++) {
      if (input_system_->GetStateForUI(user_index, 1, &state) == 0) {
        prev_buttons_ = state.gamepad.buttons;
        break;
      }
    }

    poll_timer_ = new QTimer(this);
    connect(poll_timer_, &QTimer::timeout, this,
            &ContextMenuWidgetQt::PollGamepad);
    poll_timer_->start(16);  // ~60fps polling
  }
}

ContextMenuWidgetQt::~ContextMenuWidgetQt() {
  if (poll_timer_) {
    poll_timer_->stop();
  }
  if (input_system_) {
    // Unblock input to the game
    input_system_->RemoveUIInputBlocker();
  }
}

void ContextMenuWidgetQt::AddAction(const QString& text,
                                    std::function<void()> callback,
                                    const QString& shortcut) {
  // Make the item clickable
  class ClickableItem : public QWidget {
   public:
    ClickableItem(const QString& text, const QString& shortcut, QWidget* parent,
                  std::function<void()> cb)
        : QWidget(parent), callback_(cb) {
      auto* layout = new QHBoxLayout(this);
      layout->setContentsMargins(12, 5, 12, 5);
      layout->setSpacing(20);

      auto* text_label = new QLabel(text, this);
      text_label->setStyleSheet(
          "color: white; background: transparent; border: none;");
      layout->addWidget(text_label);

      layout->addStretch();

      if (!shortcut.isEmpty()) {
        auto* shortcut_label = new QLabel(shortcut, this);
        shortcut_label->setStyleSheet(
            "color: #888888; background: transparent; border: none;");
        layout->addWidget(shortcut_label);
      }

      setStyleSheet("background: transparent; border: none;");
    }

    void SetFocused(bool focused) {
      if (focused) {
        setStyleSheet("background-color: rgb(74, 74, 74); border: none;");
      } else {
        setStyleSheet("background: transparent; border: none;");
      }
    }

   protected:
    void mousePressEvent(QMouseEvent* event) override {
      if (event->button() == Qt::LeftButton && callback_) {
        callback_();
        // Close the menu
        if (parentWidget()) {
          parentWidget()->close();
        }
      }
    }

    void enterEvent(QEnterEvent* event) override {
      setStyleSheet("background-color: rgb(74, 74, 74); border: none;");
    }

    void leaveEvent(QEvent* event) override {
      setStyleSheet("background: transparent; border: none;");
    }

   private:
    std::function<void()> callback_;
  };

  auto* clickable = new ClickableItem(text, shortcut, this, callback);
  layout_->addWidget(clickable);
  menu_items_.push_back({clickable, callback});
}

void ContextMenuWidgetQt::AddSeparator() {
  auto* separator = new QFrame(this);
  separator->setFrameShape(QFrame::HLine);
  separator->setStyleSheet("QFrame { background-color: rgb(85, 85, 85); }");
  separator->setFixedHeight(1);
  layout_->addWidget(separator);
}

void ContextMenuWidgetQt::ShowAt(const QPoint& global_pos) {
  if (!parentWidget()) {
    return;
  }

  adjustSize();

  // Convert global position to parent widget coordinates
  QPoint local_pos = parentWidget()->mapFromGlobal(global_pos);

  // Adjust if menu would go off screen
  QWidget* parent = parentWidget();
  int x = local_pos.x();
  int y = local_pos.y();

  if (x + width() > parent->width()) {
    x = parent->width() - width();
  }
  if (y + height() > parent->height()) {
    y = parent->height() - height();
  }

  move(x, y);

  show();
  raise();
  setFocus(Qt::PopupFocusReason);

  // If we have gamepad support, start with first item focused
  if (input_system_ && !menu_items_.empty()) {
    UpdateFocusedItem(0);
  }
}

void ContextMenuWidgetQt::keyPressEvent(QKeyEvent* event) {
  switch (event->key()) {
    case Qt::Key_Escape:
      close();
      break;
    case Qt::Key_Up:
      if (!menu_items_.empty()) {
        int new_index = focused_index_ <= 0
                            ? static_cast<int>(menu_items_.size()) - 1
                            : focused_index_ - 1;
        UpdateFocusedItem(new_index);
      }
      break;
    case Qt::Key_Down:
      if (!menu_items_.empty()) {
        int new_index =
            focused_index_ >= static_cast<int>(menu_items_.size()) - 1
                ? 0
                : focused_index_ + 1;
        UpdateFocusedItem(new_index);
      }
      break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      ActivateFocusedItem();
      break;
    default:
      QWidget::keyPressEvent(event);
  }
}

bool ContextMenuWidgetQt::eventFilter(QObject* obj, QEvent* event) {
  if (event->type() == QEvent::MouseButtonPress) {
    QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);

    if (mouseEvent->button() == Qt::LeftButton) {
      // Check if click is outside this menu widget
      QWidget* widget = qobject_cast<QWidget*>(obj);
      if (widget) {
        QPoint globalPos = widget->mapToGlobal(mouseEvent->pos());
        QPoint localPos = mapFromGlobal(globalPos);

        if (!rect().contains(localPos)) {
          // Click outside menu - close it and consume the event
          close();
          return true;
        }
      }
    }
  }
  return false;
}

void ContextMenuWidgetQt::PollGamepad() {
  if (!input_system_ || menu_items_.empty()) {
    return;
  }

  // Poll first connected controller
  // Use GetStateForUI to bypass the input blocker (we ARE the UI)
  for (uint32_t i = 0; i < 4; ++i) {
    hid::X_INPUT_STATE state;
    if (input_system_->GetStateForUI(i, 1, &state) == 0) {
      uint16_t buttons = state.gamepad.buttons;
      uint16_t pressed = buttons & ~prev_buttons_;  // Edge detection

      // D-pad navigation
      if (pressed & 0x0001) {  // D-pad up
        int new_index = focused_index_ <= 0
                            ? static_cast<int>(menu_items_.size()) - 1
                            : focused_index_ - 1;
        UpdateFocusedItem(new_index);
      }
      if (pressed & 0x0002) {  // D-pad down
        int new_index =
            focused_index_ >= static_cast<int>(menu_items_.size()) - 1
                ? 0
                : focused_index_ + 1;
        UpdateFocusedItem(new_index);
      }

      // A button to select
      if (pressed & 0x1000) {
        ActivateFocusedItem();
      }

      // B button, Back button, or Guide button to close
      if (pressed & (0x2000 | 0x0020 | 0x0400)) {
        close();
      }

      prev_buttons_ = buttons;
      break;  // Only use first connected controller
    }
  }
}

void ContextMenuWidgetQt::UpdateFocusedItem(int index) {
  if (index < 0 || index >= static_cast<int>(menu_items_.size())) {
    return;
  }

  // Clear previous focus
  if (focused_index_ >= 0 &&
      focused_index_ < static_cast<int>(menu_items_.size())) {
    auto* prev_item = menu_items_[focused_index_].first;
    prev_item->setStyleSheet("background: transparent; border: none;");
  }

  // Set new focus
  focused_index_ = index;
  auto* item = menu_items_[focused_index_].first;
  item->setStyleSheet("background-color: rgb(74, 74, 74); border: none;");
}

void ContextMenuWidgetQt::ActivateFocusedItem() {
  if (focused_index_ >= 0 &&
      focused_index_ < static_cast<int>(menu_items_.size())) {
    auto callback = menu_items_[focused_index_].second;
    close();
    if (callback) {
      // Call callback after menu is closed to avoid event conflicts
      QTimer::singleShot(0, callback);
    }
  }
}

}  // namespace app
}  // namespace xe
