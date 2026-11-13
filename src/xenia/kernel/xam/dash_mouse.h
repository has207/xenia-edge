/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_DASH_MOUSE_H_
#define XENIA_KERNEL_XAM_DASH_MOUSE_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <vector>

#include "xenia/hid/input.h"

namespace xe {
namespace cpu {
class XexModule;
}  // namespace cpu
namespace kernel {
class KernelState;
namespace xam {

struct DashLocations;

// Gives the host mouse to the dashboard as the pointer its UI takes from
// Kinect, with back and the wheel as its pad buttons.
class DashMouse {
 public:
  explicit DashMouse(KernelState* kernel_state);

  // Takes the mouse when the launched title is a dashboard build it has the
  // locations of.
  void Attach(cpu::XexModule* module);
  bool active() const { return active_.load(); }

  // From the window, at fractions of the guest output.
  void Move(float x, float y);
  void Press(float x, float y);
  // A click on an item with an X action, such as a game showing its details,
  // gives X once the double click time passes without a double click.
  void Release(float x, float y, std::chrono::milliseconds double_click_time);
  // The second press of a double click, which gives A in place of that X and
  // otherwise doesn't click.
  void DoubleClick();
  // Ends a press without clicking.
  void Cancel();
  void Back();
  void Wheel(bool away);

  // From the dashboard's keystroke reads on its UI thread. Sends the pointer
  // events since the last read and returns true with a pad button to give,
  // dropping them while system UI is up.
  bool Update(hid::X_INPUT_KEYSTROKE* keystroke, bool ui_active);

 private:
  enum class EventType {
    kMove,
    kPress,
    kRelease,
    kDoubleClick,
    kCancel,
    kBack,
    kWheel,
  };
  struct Event {
    EventType type;
    int32_t x;
    int32_t y;
    // The double click time of a release in milliseconds, or whether a wheel
    // turned away.
    uint32_t value;
  };

  void Queue(EventType type, float x, float y, uint32_t value = 0);
  uint32_t Call(uint32_t function, std::initializer_list<uint64_t> args);
  void Send(uint32_t message, int32_t x, int32_t y);
  void Hover();
  bool HoverList(int32_t x, int32_t y);
  uint32_t GetFocus();
  bool HasXAction(int32_t x, int32_t y);
  uint32_t GetParent(uint32_t element);
  uint32_t FindList(uint32_t element);
  bool GetBounds(uint32_t element, uint32_t ancestor, float& x, float& y,
                 float& width, float& height);

  // The dashboard's exports it calls.
  struct Exports {
    uint32_t has_focus = 0;
    uint32_t set_focus = 0;
    uint32_t get_parent = 0;
    uint32_t get_user_focus = 0;
    uint32_t get_focus = 0;
    uint32_t find_class = 0;
    uint32_t is_class = 0;
    uint32_t get_object = 0;
  };

  KernelState* kernel_state_;
  std::atomic<bool> active_{false};
  const DashLocations* locations_ = nullptr;
  Exports exports_;
  // Guest memory for values the dashboard's calls return through a pointer,
  // and the class names they look up.
  uint32_t scratch_ = 0;
  uint32_t list_class_name_ = 0;
  uint32_t legend_class_name_ = 0;

  std::mutex mutex_;
  std::vector<Event> events_;

  // Only the dashboard's UI thread uses these.
  std::deque<uint16_t> buttons_;
  // Whether the pointer is over a list, which takes its pointer by element and
  // not by item so the wheel moves its selection and a click presses it.
  bool on_list_ = false;
  // A click waiting out the double click time to give X.
  bool click_pending_ = false;
  std::chrono::steady_clock::time_point click_deadline_;
  // Whether the pointer moved since it last focused what it's over.
  bool hover_ = false;
  bool pressed_ = false;
  bool dragging_ = false;
  int32_t press_x_ = 0;
  int32_t press_y_ = 0;
  int32_t last_x_ = 0;
  int32_t last_y_ = 0;
  uint32_t hovered_ = 0;
  // The list last holding the focus, which the pointer selects in by item.
  uint32_t list_ = 0;
  uint32_t list_item_ = ~0u;
  // The focus as the pointer last saw or set it.
  uint32_t focus_ = 0;
  uint16_t button_down_ = 0;
};

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_DASH_MOUSE_H_
