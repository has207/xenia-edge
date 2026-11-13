/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/dash_mouse.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/string_util.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/title_id_utils.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
namespace kernel {
namespace xam {

// What a dashboard build doesn't export, by its 2.0.<build> version.
struct DashLocations {
  uint16_t build;
  // Sends a pointer message to the element under a point in its UI, with (root
  // or 0 for its own, message, value, user, x << 32 | y). Its Kinect support
  // moves a hand with 0x41 and presses with 0x42.
  uint32_t send_pointer;
  // The element its UI last sent a pointer move over.
  uint32_t pointer_over;
  // Whether an element is shown.
  uint32_t is_shown;
  // An element's data, which keeps its size and position in its parent.
  uint32_t get_element_data;
  // A list's first shown item, how many are shown, an item's element and
  // setting the current item.
  uint32_t get_top_item;
  uint32_t get_visible_item_count;
  uint32_t get_item_element;
  uint32_t set_current_item;
  // Its app and the app's call for the legend scene showing what the focus's
  // buttons do.
  uint32_t app;
  uint32_t get_legend_scene;
};

namespace {

constexpr DashLocations kDashLocations[] = {
    {
        .build = 17559,
        .send_pointer = 0x921E8938,
        .pointer_over = 0x929968A4,
        .is_shown = 0x927A5E40,
        .get_element_data = 0x921CD3D8,
        .get_top_item = 0x92287628,
        .get_visible_item_count = 0x92279A10,
        .get_item_element = 0x922865D0,
        .set_current_item = 0x9228C2C0,
        .app = 0x9296D990,
        .get_legend_scene = 0x92182CE0,
    },
};

// Its exports by ordinal, for whether an element holds or contains the focus,
// focusing an element for a user, an element's parent, the focus of a user and
// of any user, finding a class by name, whether an element is of a class and
// an element's object.
constexpr uint16_t kHasFocusOrdinal = 0x27D9;
constexpr uint16_t kSetFocusOrdinal = 0x27DC;
constexpr uint16_t kGetParentOrdinal = 0x27AD;
constexpr uint16_t kGetUserFocusOrdinal = 0x27DD;
constexpr uint16_t kGetFocusOrdinal = 0x27D5;
constexpr uint16_t kFindClassOrdinal = 0x2720;
constexpr uint16_t kIsClassOrdinal = 0x2730;
constexpr uint16_t kGetObjectOrdinal = 0x2729;

constexpr int32_t kWidth = 1280;
constexpr int32_t kHeight = 720;

// The user its controller navigation focuses for. Its UI keeps one focus for
// all users or one per user and focusing for one user clears the shared one,
// which leaves the pad's navigation nothing to move from.
constexpr uint32_t kAllUsers = 0xFF;
// Where an element's data keeps its width and height and its position in its
// parent.
constexpr uint32_t kElementWidth = 0x1C;
constexpr uint32_t kElementHeight = 0x20;
constexpr uint32_t kElementX = 0x28;
constexpr uint32_t kElementY = 0x2C;
// The classes its calls look up by name.
constexpr char16_t kListClassName[] = u"XuiList";
constexpr char16_t kLegendSceneClassName[] = u"ControlPackLegendScene";
// The legend scene keeps a 0x214 byte record per button from +0x18 in A, B, X,
// Y order. A record's text to show is at +0x10C, empty while the button does
// nothing.
constexpr uint32_t kLegendXText = 0x18 + 2 * 0x214 + 0x10C;

// The touch style messages its drag canvases pan with.
constexpr uint32_t kPointerDown = 0x30;
constexpr uint32_t kPointerUp = 0x31;
constexpr uint32_t kPointerMove = 0x33;
constexpr uint32_t kNuiPointerMove = 0x41;
constexpr uint32_t kNuiPress = 0x42;

// How far a press moves before it drags, as a touch would.
constexpr int32_t kDragDistance = 12;
constexpr size_t kMaxQueued = 64;
constexpr int kMaxHoverDepth = 8;
constexpr int kMaxTreeDepth = 32;
constexpr uint32_t kMaxListItems = 32;

}  // namespace

DashMouse::DashMouse(KernelState* kernel_state) : kernel_state_(kernel_state) {}

void DashMouse::Attach(cpu::XexModule* module) {
  active_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
  }
  buttons_.clear();
  click_pending_ = false;
  hover_ = false;
  pressed_ = false;
  dragging_ = false;
  hovered_ = 0;
  on_list_ = false;
  list_ = 0;
  list_item_ = ~0u;
  focus_ = 0;
  button_down_ = 0;
  locations_ = nullptr;

  if (!module || kernel_state_->title_id() != kDashboardID) {
    return;
  }
  const xex2_opt_execution_info* info = module->opt_execution_info();
  const uint32_t build = info ? uint32_t(info->version().build) : 0;
  const DashLocations* locations = std::find_if(
      std::begin(kDashLocations), std::end(kDashLocations),
      [build](const DashLocations& entry) { return entry.build == build; });
  if (locations == std::end(kDashLocations)) {
    XELOGI("Dashboard mouse input: no locations for dashboard 2.0.{}", build);
    return;
  }
  auto resolve = [module, build](uint16_t ordinal, uint32_t& address) {
    address = module->GetProcAddress(ordinal);
    if (address && module->ContainsAddress(address)) {
      return true;
    }
    XELOGW("Dashboard mouse input: dashboard 2.0.{} doesn't export {:04X}",
           build, ordinal);
    return false;
  };
  if (!resolve(kHasFocusOrdinal, exports_.has_focus) ||
      !resolve(kSetFocusOrdinal, exports_.set_focus) ||
      !resolve(kGetParentOrdinal, exports_.get_parent) ||
      !resolve(kGetUserFocusOrdinal, exports_.get_user_focus) ||
      !resolve(kGetFocusOrdinal, exports_.get_focus) ||
      !resolve(kFindClassOrdinal, exports_.find_class) ||
      !resolve(kIsClassOrdinal, exports_.is_class) ||
      !resolve(kGetObjectOrdinal, exports_.get_object)) {
    return;
  }
  if (!scratch_) {
    auto memory = kernel_state_->memory();
    const uint32_t scratch = memory->SystemHeapAlloc(
        4 + sizeof(kListClassName) + sizeof(kLegendSceneClassName));
    if (!scratch) {
      return;
    }
    list_class_name_ = scratch + 4;
    legend_class_name_ = list_class_name_ + sizeof(kListClassName);
    xe::string_util::copy_and_swap_truncating(
        memory->TranslateVirtual<char16_t*>(list_class_name_), kListClassName,
        std::size(kListClassName));
    xe::string_util::copy_and_swap_truncating(
        memory->TranslateVirtual<char16_t*>(legend_class_name_),
        kLegendSceneClassName, std::size(kLegendSceneClassName));
    scratch_ = scratch;
  }
  locations_ = locations;
  active_ = true;
  XELOGI("Dashboard mouse input enabled for dashboard 2.0.{}", build);
}

void DashMouse::Queue(EventType type, float x, float y, uint32_t value) {
  if (!active_) {
    return;
  }
  const Event event = {type, std::clamp(int32_t(x * kWidth), 0, kWidth - 1),
                       std::clamp(int32_t(y * kHeight), 0, kHeight - 1), value};
  std::lock_guard<std::mutex> lock(mutex_);
  // Only the newest of several moves or cancels matters.
  if ((type == EventType::kMove || type == EventType::kCancel) &&
      !events_.empty() && events_.back().type == type) {
    events_.back() = event;
  } else {
    // Past the cap, as while the dashboard stops reading, the queue starts over
    // from a cancel so a lost release leaves no press held.
    if (events_.size() >= kMaxQueued) {
      events_.clear();
      events_.push_back({EventType::kCancel, 0, 0, 0});
    }
    events_.push_back(event);
  }
}

void DashMouse::Move(float x, float y) { Queue(EventType::kMove, x, y); }

void DashMouse::Press(float x, float y) { Queue(EventType::kPress, x, y); }

void DashMouse::Release(float x, float y,
                        std::chrono::milliseconds double_click_time) {
  Queue(EventType::kRelease, x, y,
        uint32_t(std::max<int64_t>(double_click_time.count(), 0)));
}

void DashMouse::DoubleClick() { Queue(EventType::kDoubleClick, 0.0f, 0.0f); }

void DashMouse::Cancel() { Queue(EventType::kCancel, 0.0f, 0.0f); }

void DashMouse::Back() { Queue(EventType::kBack, 0.0f, 0.0f); }

void DashMouse::Wheel(bool away) {
  Queue(EventType::kWheel, 0.0f, 0.0f, away ? 1 : 0);
}

bool DashMouse::Update(hid::X_INPUT_KEYSTROKE* keystroke, bool ui_active) {
  if (!active_) {
    return false;
  }
  std::vector<Event> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    events.swap(events_);
  }
  // Nothing reaches the dashboard under system UI and nothing waits for it.
  if (ui_active) {
    if (dragging_) {
      Send(kPointerUp, last_x_, last_y_);
    }
    buttons_.clear();
    click_pending_ = false;
    pressed_ = false;
    dragging_ = false;
    hovered_ = 0;
    return false;
  }

  auto queue_button = [this](ui::VirtualKey button) {
    if (buttons_.size() < kMaxQueued) {
      buttons_.push_back(uint16_t(button));
    }
  };
  // The focus stays while pressed, while a click is pending, and until the
  // buttons for it are given.
  auto update_hover = [this]() {
    if (!hover_ || click_pending_ || button_down_ || !buttons_.empty()) {
      return;
    }
    hover_ = false;
    if (HoverList(last_x_, last_y_)) {
      hovered_ = 0;
    } else {
      Hover();
    }
  };
  for (const Event& event : events) {
    switch (event.type) {
      case EventType::kMove:
        if (!pressed_) {
          hover_ = true;
        }
        if (pressed_ && !dragging_ &&
            (std::abs(event.x - press_x_) > kDragDistance ||
             std::abs(event.y - press_y_) > kDragDistance)) {
          dragging_ = true;
          Send(kPointerDown, press_x_, press_y_);
        }
        Send(dragging_ ? kPointerMove : kNuiPointerMove, event.x, event.y);
        break;
      case EventType::kPress:
        // A press away from the pending click drops it.
        if (click_pending_ && (std::abs(event.x - press_x_) > kDragDistance ||
                               std::abs(event.y - press_y_) > kDragDistance)) {
          click_pending_ = false;
        }
        // A drag whose release went missing ends first.
        if (dragging_) {
          Send(kPointerUp, last_x_, last_y_);
        }
        // A press in the same read as a move is on what that move is over.
        update_hover();
        pressed_ = true;
        dragging_ = false;
        press_x_ = event.x;
        press_y_ = event.y;
        break;
      case EventType::kRelease:
        // A click elsewhere drops the one waiting.
        click_pending_ = false;
        if (dragging_) {
          Send(kPointerUp, event.x, event.y);
        } else if (pressed_ && on_list_) {
          queue_button(ui::VirtualKey::kXInputPadA);
        } else if (pressed_ && HasXAction(event.x, event.y)) {
          click_pending_ = true;
          click_deadline_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(event.value);
        } else if (pressed_) {
          Send(kNuiPress, event.x, event.y);
        }
        pressed_ = false;
        dragging_ = false;
        break;
      case EventType::kDoubleClick:
        // Some hosts send a press before the double click, which mustn't
        // click on its release.
        if (click_pending_) {
          click_pending_ = false;
          queue_button(ui::VirtualKey::kXInputPadA);
        }
        pressed_ = false;
        dragging_ = false;
        break;
      case EventType::kCancel:
        if (dragging_) {
          Send(kPointerUp, last_x_, last_y_);
        }
        click_pending_ = false;
        pressed_ = false;
        dragging_ = false;
        hovered_ = 0;
        break;
      case EventType::kBack:
        click_pending_ = false;
        queue_button(ui::VirtualKey::kXInputPadB);
        break;
      case EventType::kWheel:
        click_pending_ = false;
        if (on_list_) {
          queue_button(event.value ? ui::VirtualKey::kXInputPadDpadUp
                                   : ui::VirtualKey::kXInputPadDpadDown);
        } else {
          queue_button(event.value ? ui::VirtualKey::kXInputPadLShoulder
                                   : ui::VirtualKey::kXInputPadRShoulder);
        }
        break;
    }
    if (event.type == EventType::kMove || event.type == EventType::kPress ||
        event.type == EventType::kRelease) {
      last_x_ = event.x;
      last_y_ = event.y;
    }
  }

  if (click_pending_ && std::chrono::steady_clock::now() >= click_deadline_) {
    click_pending_ = false;
    queue_button(ui::VirtualKey::kXInputPadX);
  }
  update_hover();

  // A button goes down on one read and up on the next.
  if (button_down_) {
    keystroke->virtual_key = button_down_;
    keystroke->flags = uint16_t(hid::X_INPUT_KEYSTROKE_KEYUP);
    button_down_ = 0;
    return true;
  }
  if (!buttons_.empty()) {
    button_down_ = buttons_.front();
    buttons_.pop_front();
    keystroke->virtual_key = button_down_;
    keystroke->flags = uint16_t(hid::X_INPUT_KEYSTROKE_KEYDOWN);
    return true;
  }
  return false;
}

uint32_t DashMouse::Call(uint32_t function,
                         std::initializer_list<uint64_t> args) {
  uint64_t values[8] = {};
  size_t count = 0;
  for (const uint64_t arg : args) {
    values[count++] = arg;
  }
  return uint32_t(kernel_state_->processor()->Execute(cpu::ThreadState::Get(),
                                                      function, values, count));
}

void DashMouse::Send(uint32_t message, int32_t x, int32_t y) {
  // As the hand of the first user.
  Call(locations_->send_pointer,
       {0, message, 1, 0, (uint64_t(uint32_t(x)) << 32) | uint32_t(y)});
}

uint32_t DashMouse::GetParent(uint32_t element) {
  auto memory = kernel_state_->memory();
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(scratch_), 0);
  if (int32_t(Call(exports_.get_parent, {element, scratch_})) < 0) {
    return 0;
  }
  return xe::load_and_swap<uint32_t>(memory->TranslateVirtual(scratch_));
}

void DashMouse::Hover() {
  // Kinect focuses what the hand is over so the pointer focuses the first
  // element up from it that takes focus, unless one already has it.
  auto memory = kernel_state_->memory();
  uint32_t element = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual(locations_->pointer_over));
  if (element == hovered_) {
    return;
  }
  hovered_ = element;
  for (int depth = 0; element && depth < kMaxHoverDepth; ++depth) {
    if (Call(exports_.has_focus, {element})) {
      return;
    }
    if (int32_t(Call(exports_.set_focus, {element, kAllUsers})) >= 0) {
      focus_ = GetFocus();
      // Back on the list, its row takes the focus again.
      list_item_ = ~0u;
      return;
    }
    element = GetParent(element);
  }
}

uint32_t DashMouse::FindList(uint32_t element) {
  const uint32_t list_class = Call(exports_.find_class, {list_class_name_});
  if (!list_class) {
    return 0;
  }
  for (int depth = 0; element && depth < kMaxTreeDepth; ++depth) {
    if (Call(exports_.is_class, {element, list_class})) {
      return element;
    }
    element = GetParent(element);
  }
  return 0;
}

bool DashMouse::GetBounds(uint32_t element, uint32_t ancestor, float& x,
                          float& y, float& width, float& height) {
  auto memory = kernel_state_->memory();
  auto data_of = [this, memory](uint32_t handle) -> uint8_t* {
    xe::store_and_swap<uint32_t>(memory->TranslateVirtual(scratch_), 0);
    if (int32_t(Call(locations_->get_element_data, {handle, scratch_})) < 0) {
      return nullptr;
    }
    const uint32_t data =
        xe::load_and_swap<uint32_t>(memory->TranslateVirtual(scratch_));
    return data ? memory->TranslateVirtual(data) : nullptr;
  };
  const uint8_t* data = data_of(element);
  if (!data) {
    return false;
  }
  width = xe::load_and_swap<float>(data + kElementWidth);
  height = xe::load_and_swap<float>(data + kElementHeight);
  x = 0.0f;
  y = 0.0f;
  // Positions are in the parent so they add up to the ancestor's, or to the
  // UI's without one.
  for (int depth = 0; element && depth < kMaxTreeDepth; ++depth) {
    if (element == ancestor) {
      return true;
    }
    data = data_of(element);
    if (!data) {
      return false;
    }
    x += xe::load_and_swap<float>(data + kElementX);
    y += xe::load_and_swap<float>(data + kElementY);
    element = GetParent(element);
  }
  return !ancestor;
}

uint32_t DashMouse::GetFocus() {
  const uint32_t focus = Call(exports_.get_user_focus, {0});
  return focus ? focus : Call(exports_.get_focus, {});
}

bool DashMouse::HasXAction(int32_t x, int32_t y) {
  // The legend shows what X does for the focus and a click on the focus
  // takes it.
  const uint32_t focus = GetFocus();
  float focus_x, focus_y, width, height;
  if (!focus || !GetBounds(focus, 0, focus_x, focus_y, width, height) ||
      x < focus_x || y < focus_y || x >= focus_x + width ||
      y >= focus_y + height) {
    return false;
  }
  const uint32_t scene = Call(locations_->get_legend_scene, {locations_->app});
  const uint32_t legend_class = Call(exports_.find_class, {legend_class_name_});
  if (!scene || !legend_class ||
      !Call(exports_.is_class, {scene, legend_class})) {
    return false;
  }
  auto memory = kernel_state_->memory();
  xe::store_and_swap<uint32_t>(memory->TranslateVirtual(scratch_), 0);
  Call(exports_.get_object, {scene, scratch_});
  const uint32_t legend =
      xe::load_and_swap<uint32_t>(memory->TranslateVirtual(scratch_));
  return legend && xe::load_and_swap<uint16_t>(
                       memory->TranslateVirtual(legend + kLegendXText)) != 0;
}

bool DashMouse::HoverList(int32_t x, int32_t y) {
  // A list routes the pointer no further than itself or what holds it so the
  // item under the pointer comes from where the list shows its items.
  // The list stays while the focus is where the pointer left it and goes
  // when the dashboard moves the focus elsewhere, as to another scene.
  const uint32_t focus = GetFocus();
  if (const uint32_t list = FindList(focus)) {
    if (list != list_) {
      list_ = list;
      list_item_ = ~0u;
    }
  } else if (focus != focus_ && list_) {
    list_ = 0;
    list_item_ = ~0u;
  }
  focus_ = focus;
  on_list_ = false;
  float list_x, list_y, list_width, list_height;
  if (!list_ || !Call(locations_->is_shown, {list_}) ||
      !GetBounds(list_, 0, list_x, list_y, list_width, list_height)) {
    return false;
  }
  if (x < list_x || y < list_y || x >= list_x + list_width ||
      y >= list_y + list_height) {
    return false;
  }
  const uint32_t top = Call(locations_->get_top_item, {list_});
  const uint32_t count = std::min(
      Call(locations_->get_visible_item_count, {list_}), kMaxListItems);
  // Only a list running down, as a menu does, since a row of items keeps the
  // pointer and wheel it has.
  float first_x, first_y, second_x, second_y, width, height;
  if (count >= 2 &&
      GetBounds(Call(locations_->get_item_element, {list_, top}), list_,
                first_x, first_y, width, height) &&
      GetBounds(Call(locations_->get_item_element, {list_, top + 1}), list_,
                second_x, second_y, width, height) &&
      std::abs(second_x - first_x) > std::abs(second_y - first_y)) {
    return false;
  }
  on_list_ = true;
  for (uint32_t index = top; index < top + count; ++index) {
    const uint32_t item = Call(locations_->get_item_element, {list_, index});
    float item_x, item_y, item_width, item_height;
    if (!item ||
        !GetBounds(item, list_, item_x, item_y, item_width, item_height)) {
      continue;
    }
    item_x += list_x;
    item_y += list_y;
    if (x < item_x || y < item_y || x >= item_x + item_width ||
        y >= item_y + item_height) {
      continue;
    }
    if (index != list_item_) {
      list_item_ = index;
      if (!Call(exports_.has_focus, {list_})) {
        Call(exports_.set_focus, {list_, kAllUsers});
      }
      Call(locations_->set_current_item, {list_, index});
      focus_ = GetFocus();
    }
    break;
  }
  return true;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
