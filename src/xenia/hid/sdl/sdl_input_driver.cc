/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/sdl/sdl_input_driver.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"

#include <xinput.h>
#endif  // XE_PLATFORM_WIN32

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/embedded_bundle.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/helper/sdl/sdl_helper.h"
#include "xenia/ui/virtual_key.h"
#include "xenia/ui/window.h"

#include "embedded_bundle_gamecontrollerdb.h"

// TODO(joellinn) make this path relative to the config folder.
DEFINE_path(mappings_file, "",
            "Filename of a database with custom game controller mappings. "
            "Empty uses the bundled SDL_GameControllerDB.",
            "SDL");
UPDATE_from_path(mappings_file, 2026, 5, 21, 12, "gamecontrollerdb.txt");

namespace xe {
namespace hid {
namespace sdl {

#if XE_PLATFORM_WIN32
namespace {
struct XInputApi {
  decltype(&XInputGetCapabilities) get_capabilities = nullptr;
  // Ordinal 100 (XInputGetStateEx) also reports the guide button.
  decltype(&XInputGetState) get_state = nullptr;

  XInputApi() {
    HMODULE module = LoadLibraryW(L"xinput1_4.dll");
    if (!module) {
      return;
    }
    get_capabilities = reinterpret_cast<decltype(get_capabilities)>(
        GetProcAddress(module, "XInputGetCapabilities"));
    get_state = reinterpret_cast<decltype(get_state)>(
        GetProcAddress(module, MAKEINTRESOURCEA(100)));
    if (!get_state) {
      get_state = reinterpret_cast<decltype(get_state)>(
          GetProcAddress(module, "XInputGetState"));
    }
    if (!get_capabilities || !get_state) {
      get_capabilities = nullptr;
      get_state = nullptr;
    }
  }
};

const XInputApi& GetXInputApi() {
  static const XInputApi api;
  return api;
}
}  // namespace
#endif  // XE_PLATFORM_WIN32

// SDL's XInput backend names the device path "XInput#<user index>".
static int GetXInputSlot(SDL_Gamepad* gamepad) {
#if XE_PLATFORM_WIN32
  if (!GetXInputApi().get_capabilities) {
    return -1;
  }
  constexpr std::string_view kPrefix = "XInput#";
  const char* path = SDL_GetGamepadPath(gamepad);
  if (!path || std::string_view(path).substr(0, kPrefix.size()) != kPrefix) {
    return -1;
  }
  const std::string_view index(path + kPrefix.size());
  if (index.size() != 1 || index[0] < '0' ||
      index[0] >= '0' + XUSER_MAX_COUNT) {
    return -1;
  }
  return index[0] - '0';
#else
  return -1;
#endif  // XE_PLATFORM_WIN32
}

static bool PollXInput(int slot, X_INPUT_STATE* out_state) {
#if XE_PLATFORM_WIN32
  // XInputGetStateEx writes past XINPUT_STATE.
  struct {
    XINPUT_STATE state;
    DWORD reserved;
  } native;
  if (GetXInputApi().get_state(slot, &native.state) != ERROR_SUCCESS) {
    return false;
  }
  out_state->packet_number = native.state.dwPacketNumber;
  auto& pad = out_state->gamepad;
  pad.buttons = native.state.Gamepad.wButtons;
  pad.left_trigger = native.state.Gamepad.bLeftTrigger;
  pad.right_trigger = native.state.Gamepad.bRightTrigger;
  pad.thumb_lx = native.state.Gamepad.sThumbLX;
  pad.thumb_ly = native.state.Gamepad.sThumbLY;
  pad.thumb_rx = native.state.Gamepad.sThumbRX;
  pad.thumb_ry = native.state.Gamepad.sThumbRY;
  return true;
#else
  return false;
#endif  // XE_PLATFORM_WIN32
}

// SDL_GetGamepadName returns the mapping name, which the bundled DB replaces
// with generic ones like "XInput Controller".
static const char* GetDeviceName(SDL_Gamepad* gamepad) {
  const char* name = SDL_GetJoystickName(SDL_GetGamepadJoystick(gamepad));
  return name ? name : SDL_GetGamepadName(gamepad);
}

SDLInputDriver::SDLInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      sdl_gamepad_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_thread_should_exit_(false),
      controllers_(),
      keystroke_states_() {}

SDLInputDriver::~SDLInputDriver() {
  if (sdl_thread_.joinable()) {
    sdl_thread_should_exit_.store(true, std::memory_order_release);
    sdl_thread_.join();
  }
}

X_STATUS SDLInputDriver::Setup() {
  std::promise<X_STATUS> init_promise;
  auto init_future = init_promise.get_future();
  sdl_thread_ = std::thread(&SDLInputDriver::SDLEventThread, this,
                            std::move(init_promise));
  const X_STATUS result = init_future.get();
  if (result != X_STATUS_SUCCESS && sdl_thread_.joinable()) {
    sdl_thread_.join();
  }
  return result;
}

void SDLInputDriver::SDLEventThread(std::promise<X_STATUS> init_result) {
  xe::threading::set_name("SDL Input");

  // SDL_PumpEvents (and the Win32 hidden window SDL uses for hotplug
  // notifications) is bound to whichever thread initialized SDL_INIT_EVENTS.
  // We own that here so a wx menu/dialog modal loop on the UI thread can't
  // stall controller hotplug.
  if (!xe::helper::sdl::SDLHelper::Prepare()) {
    init_result.set_value(X_STATUS_UNSUCCESSFUL);
    return;
  }

  // Initialize the event system early, so we catch device events for already
  // connected controllers.
  if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
    init_result.set_value(X_STATUS_UNSUCCESSFUL);
    return;
  }
  sdl_events_initialized_ = true;

  // With an event watch we will always get notified, even if the event queue
  // is full, which can happen if another subsystem does not clear its events.
  SDL_AddEventWatch(
      [](void* userdata, SDL_Event* event) -> bool {
        if (!userdata || !event) {
          assert_always();
          return false;
        }

        const auto type = event->type;
        if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION ||
            type >= SDL_EVENT_FINGER_DOWN) {
          return false;
        }

        // If another part of xenia uses another SDL subsystem that generates
        // events, this may seem like a bad idea. They will however not
        // subscribe to controller events so we get away with that.
        const auto driver = static_cast<SDLInputDriver*>(userdata);
        driver->HandleEvent(*event);

        return false;
      },
      this);

  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    sdl_events_initialized_ = false;
    init_result.set_value(X_STATUS_UNSUCCESSFUL);
    return;
  }
  sdl_gamepad_initialized_ = true;

  LoadGameControllerDB();

  init_result.set_value(X_STATUS_SUCCESS);

  while (!sdl_thread_should_exit_.load(std::memory_order_acquire)) {
    SDL_PumpEvents();
    xe::threading::Sleep(std::chrono::milliseconds(8));
  }

  // Tear down on the same thread that initialized SDL.
  for (size_t i = 0; i < controllers_.size(); i++) {
    if (controllers_.at(i).sdl) {
      SDL_CloseGamepad(controllers_.at(i).sdl);
      controllers_.at(i) = {};
    }
  }
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  sdl_gamepad_initialized_ = false;
  SDL_QuitSubSystem(SDL_INIT_EVENTS);
  sdl_events_initialized_ = false;
}

void SDLInputDriver::LoadGameControllerDB() {
  // Empty cvar: use the bundled DB.
  if (cvars::mappings_file.empty()) {
    xe::EmbeddedBundle bundle(
        xe::embedded_bundle_gamecontrollerdb::kBundleData,
        xe::embedded_bundle_gamecontrollerdb::kBundleSize);
    if (!bundle.ok()) {
      XELOGW("SDL GameControllerDB: bundled mappings unavailable.");
      return;
    }
    bundle.ForEach([this](std::string_view name, std::string_view data) {
      if (name != "gamecontrollerdb.txt") {
        return;
      }
      XELOGI("SDL GameControllerDB: Loading bundled mappings");
      LoadMappingsFromMemory(data);
    });
    return;
  }

  if (!std::filesystem::exists(cvars::mappings_file)) {
    XELOGW("SDL GameControllerDB: file '{}' does not exist.",
           xe::strip_pii(xe::path_to_utf8(cvars::mappings_file)));
    return;
  }

  XELOGI("SDL GameControllerDB: Loading {}",
         xe::strip_pii(xe::path_to_utf8(cvars::mappings_file)));
  // ifstream for Unicode-safe paths.
  std::ifstream stream(cvars::mappings_file, std::ios::binary);
  if (!stream) {
    XELOGW("SDL GameControllerDB: could not open '{}'.",
           xe::strip_pii(xe::path_to_utf8(cvars::mappings_file)));
    return;
  }
  std::ostringstream ss;
  ss << stream.rdbuf();
  LoadMappingsFromMemory(ss.str());
}

void SDLInputDriver::LoadMappingsFromMemory(std::string_view data) {
  SDL_IOStream* io = SDL_IOFromConstMem(data.data(), data.size());
  if (!io) {
    XELOGW("SDL GameControllerDB: {}", SDL_GetError());
    return;
  }

  int added = SDL_AddGamepadMappingsFromIO(io, true /* closeio */);
  if (added < 0) {
    XELOGW("SDL GameControllerDB: failed to load mappings: {}", SDL_GetError());
    return;
  }

  for (uint32_t i = 0; i < HID_SDL_USER_COUNT; i++) {
    auto controller = GetControllerState(i);

    if (controller) {
      XELOGI("SDL Controller {}: {}", i,
             SDL_GetGamepadMapping(controller->sdl));
    }
  }

  XELOGI("SDL GameControllerDB: Added {} mappings.", added);
}

X_RESULT SDLInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && sdl_gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && sdl_gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (controller->native()) {
    return PollXInput(controller->xinput_slot, out_state)
               ? X_ERROR_SUCCESS
               : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (controller->state_changed) {
    controller->state.packet_number++;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetState(uint32_t user_index,
                                  X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && sdl_gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (!SDL_RumbleGamepad(controller->sdl, vibration->left_motor_speed,
                         vibration->right_motor_speed, 0)) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && sdl_gamepad_initialized_);
  bool user_any = users == XUserIndexAny;
  if (users >= HID_SDL_USER_COUNT && !user_any) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<ui::VirtualKey, 35> kVkLookup = {
      // 00 - True buttons from xinput button field
      ui::VirtualKey::kXInputPadDpadUp,
      ui::VirtualKey::kXInputPadDpadDown,
      ui::VirtualKey::kXInputPadDpadLeft,
      ui::VirtualKey::kXInputPadDpadRight,
      ui::VirtualKey::kXInputPadStart,
      ui::VirtualKey::kXInputPadBack,
      ui::VirtualKey::kXInputPadLThumbPress,
      ui::VirtualKey::kXInputPadRThumbPress,
      ui::VirtualKey::kXInputPadLShoulder,
      ui::VirtualKey::kXInputPadRShoulder,
      // Guide has no VK (kNone), however using kXInputPadGuide.
      ui::VirtualKey::kXInputPadGuide,
      ui::VirtualKey::kNone, /* Unknown */
      ui::VirtualKey::kXInputPadA,
      ui::VirtualKey::kXInputPadB,
      ui::VirtualKey::kXInputPadX,
      ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      ui::VirtualKey::kXInputPadLTrigger,
      ui::VirtualKey::kXInputPadRTrigger,
      // 18
      ui::VirtualKey::kXInputPadLThumbUp,
      ui::VirtualKey::kXInputPadLThumbDown,
      ui::VirtualKey::kXInputPadLThumbRight,
      ui::VirtualKey::kXInputPadLThumbLeft,
      ui::VirtualKey::kXInputPadLThumbUpLeft,
      ui::VirtualKey::kXInputPadLThumbUpRight,
      ui::VirtualKey::kXInputPadLThumbDownRight,
      ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      ui::VirtualKey::kXInputPadRThumbUp,
      ui::VirtualKey::kXInputPadRThumbDown,
      ui::VirtualKey::kXInputPadRThumbRight,
      ui::VirtualKey::kXInputPadRThumbLeft,
      ui::VirtualKey::kXInputPadRThumbUpLeft,
      ui::VirtualKey::kXInputPadRThumbUpRight,
      ui::VirtualKey::kXInputPadRThumbDownRight,
      ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  for (uint32_t user_index = (user_any ? 0 : users);
       user_index < (user_any ? HID_SDL_USER_COUNT : users + 1); user_index++) {
    auto controller = GetControllerState(user_index);
    const bool native = controller && controller->native();
    X_INPUT_STATE native_state = {};
    if (!controller ||
        (native && !PollXInput(controller->xinput_slot, &native_state))) {
      if (user_any) {
        continue;
      } else {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }

    // If input is not active (e.g. due to a dialog overlay), force buttons to
    // "unpressed". The algorithm will automatically send UP events when
    // `is_active()` goes low and DOWN events when it goes high again.
    const X_INPUT_GAMEPAD& gamepad =
        native ? native_state.gamepad : controller->state.gamepad;
    const uint64_t curr_butts = gamepad.buttons | AnalogToKeyfield(gamepad);
    KeystrokeState& last = keystroke_states_.at(user_index);

    // Handle repeating
    auto guest_now = Clock::QueryGuestUptimeMillis();
    static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
    // A button let go while the title wasn't polling gets its up event below
    // rather than a repeat.
    const bool repeat_held = (curr_butts >> last.repeat_butt_idx) & 1;
    if (repeat_held && last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (repeat_held && last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
      last.repeat_time = guest_now;
      ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags =
          X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    auto butts_changed = curr_butts ^ last.buttons;
    if (!butts_changed) {
      continue;
    }

    // First try to clear buttons with up events. This is to match xinput
    // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
    // up before THUMB_LEFT is down.
    for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2;
         clear_pass = false, i++) {
      for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
        auto fbutton = uint64_t(1) << i;
        if (!(butts_changed & fbutton)) {
          continue;
        }
        ui::VirtualKey vk = kVkLookup.at(i);
        if (vk == ui::VirtualKey::kNone) {
          continue;
        }

        out_keystroke->virtual_key = uint16_t(vk);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = user_index;
        out_keystroke->hid_code = 0;

        bool is_pressed = curr_butts & fbutton;
        if (clear_pass && !is_pressed) {
          // up
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          last.buttons &= ~fbutton;
          last.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && is_pressed) {
          // down
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          last.buttons |= fbutton;
          last.repeat_state = RepeatState::Waiting;
          last.repeat_butt_idx = i;
          last.repeat_time = guest_now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

InputType SDLInputDriver::GetInputType() const { return InputType::Controller; }

static const char* JoystickTypeName(SDL_JoystickType t) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_UNKNOWN:
      return "Unknown";
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      return "Gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL:
      return "Wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return "ArcadeStick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return "FlightStick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return "DancePad";
    case SDL_JOYSTICK_TYPE_GUITAR:
      return "Guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return "DrumKit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return "ArcadePad";
    case SDL_JOYSTICK_TYPE_THROTTLE:
      return "Throttle";
    default:
      return "?";
  }
}

static const char* XInputSubTypeName(uint8_t s) {
  switch (s) {
    case 0x01:
      return "GAMEPAD";
    case 0x02:
      return "WHEEL";
    case 0x03:
      return "ARCADE_STICK";
    case 0x04:
      return "FLIGHT_STICK";
    case 0x05:
      return "DANCE_PAD";
    case 0x06:
      return "GUITAR";
    case 0x07:
      return "GUITAR_ALTERNATE";
    case 0x08:
      return "DRUM_KIT";
    case 0x0B:
      return "GUITAR_BASS";
    case 0x13:
      return "ARCADE_PAD";
    default:
      return "?";
  }
}

// Case-insensitive substring match. `needle` must be lowercase ASCII.
static bool NameContainsCI(const char* name, std::string_view needle) {
  if (!name || needle.empty()) {
    return false;
  }
  const std::string_view haystack(name);
  return std::search(haystack.begin(), haystack.end(), needle.begin(),
                     needle.end(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              static_cast<unsigned char>(b);
                     }) != haystack.end();
}

// SDL_JoystickType numbering diverges from XINPUT_DEVSUBTYPE_* past value 6.
// GAMEPAD doesn't expose form factor; fall back to name keywords.
static uint8_t SdlTypeToXInputSubType(SDL_JoystickType t, const char* name) {
  switch (t) {
    case SDL_JOYSTICK_TYPE_GAMEPAD:
      // SDL's XInput backend bakes the SubType into the device name (e.g.
      // "XInput Guitar #1", "XInput DrumKit #1"), so a name keyword recovers
      // the form factor for free.
      if (NameContainsCI(name, "guitar")) {
        return 0x06;  // XINPUT_DEVSUBTYPE_GUITAR
      }
      if (NameContainsCI(name, "drum")) {
        return 0x08;  // XINPUT_DEVSUBTYPE_DRUM_KIT
      }
      if (NameContainsCI(name, "wheel")) {
        return 0x02;  // XINPUT_DEVSUBTYPE_WHEEL
      }
      if (NameContainsCI(name, "dancepad") ||
          NameContainsCI(name, "dance pad")) {
        return 0x05;  // XINPUT_DEVSUBTYPE_DANCE_PAD
      }
      if (NameContainsCI(name, "flightstick") ||
          NameContainsCI(name, "flight stick") ||
          NameContainsCI(name, "hotas")) {
        return 0x04;  // XINPUT_DEVSUBTYPE_FLIGHT_STICK
      }
      if (NameContainsCI(name, "arcadepad") ||
          NameContainsCI(name, "arcade pad")) {
        return 0x13;  // XINPUT_DEVSUBTYPE_ARCADE_PAD
      }
      if (NameContainsCI(name, "arcade")) {
        return 0x03;  // XINPUT_DEVSUBTYPE_ARCADE_STICK
      }
      return 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    case SDL_JOYSTICK_TYPE_WHEEL:
      return 0x02;  // XINPUT_DEVSUBTYPE_WHEEL
    case SDL_JOYSTICK_TYPE_ARCADE_STICK:
      return 0x03;  // XINPUT_DEVSUBTYPE_ARCADE_STICK
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK:
      return 0x04;  // XINPUT_DEVSUBTYPE_FLIGHT_STICK
    case SDL_JOYSTICK_TYPE_DANCE_PAD:
      return 0x05;  // XINPUT_DEVSUBTYPE_DANCE_PAD
    case SDL_JOYSTICK_TYPE_GUITAR:
      return 0x06;  // XINPUT_DEVSUBTYPE_GUITAR
    case SDL_JOYSTICK_TYPE_DRUM_KIT:
      return 0x08;  // XINPUT_DEVSUBTYPE_DRUM_KIT
    case SDL_JOYSTICK_TYPE_ARCADE_PAD:
      return 0x13;  // XINPUT_DEVSUBTYPE_ARCADE_PAD
    default:
      return 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  }
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST,
                        SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }
  switch (event.type) {
    case SDL_EVENT_JOYSTICK_ADDED: {
      // Logged before the controller event (if any) so unmapped devices
      // (some guitars/drums) leave a paper trail even when SDL can't promote
      // them to a game controller.
      const SDL_JoystickID id = event.jdevice.which;
      const char* name = SDL_GetJoystickNameForID(id);
      char guid_str[33] = {};
      SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_str,
                       sizeof(guid_str));
      const bool is_controller = SDL_IsGamepad(id);
      XELOGI(
          "SDL JoystickAdded: \"{}\", VendorID(0x{:04X}), "
          "ProductID(0x{:04X}), GUID({}), HasControllerMapping({})",
          name ? name : "?", SDL_GetJoystickVendorForID(id),
          SDL_GetJoystickProductForID(id), guid_str, is_controller);
      if (!is_controller) {
        XELOGW(
            "SDL JoystickAdded: \"{}\" has no game controller mapping; "
            "device will not be usable. Add a mapping to gamecontrollerdb.txt.",
            name ? name : "?");
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAdded(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemoved(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotion(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChanged(event);
      break;
    default:
      break;
  }
  return;
}

void SDLInputDriver::OnControllerDeviceAdded(const SDL_Event& event) {
  // Open the controller.
  const auto controller = SDL_OpenGamepad(event.gdevice.which);
  if (!controller) {
    assert_always();
    return;
  }

  char guid_str[33];

  SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(controller)),
                   guid_str, 33);

  const SDL_JoystickType joy_type =
      SDL_GetJoystickType(SDL_GetGamepadJoystick(controller));
  const char* controller_name = GetDeviceName(controller);
  const int xinput_slot = GetXInputSlot(controller);
  XELOGI(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "XInputSlot({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X}), "
      "GUID({})",
      controller_name ? controller_name : "?", JoystickTypeName(joy_type),
      static_cast<uint32_t>(SDL_GetGamepadType(controller)), xinput_slot,
      SDL_GetGamepadVendor(controller), SDL_GetGamepadProduct(controller),
      guid_str);
  // Check if the controller has a player index LED.
  int user_id = SDL_GetGamepadPlayerIndex(controller);
  // Is that id already taken?
  if (user_id < 0 || user_id >= static_cast<int>(controllers_.size()) ||
      controllers_.at(user_id).sdl) {
    user_id = -1;
  }
  // No player index or already taken, just take the first free slot.
  if (user_id < 0) {
    for (size_t i = 0; i < controllers_.size(); i++) {
      if (!controllers_.at(i).sdl) {
        user_id = static_cast<int>(i);
        SDL_SetGamepadPlayerIndex(controller, user_id);
        break;
      }
    }
  }
  if (user_id >= 0) {
    auto& state = controllers_.at(user_id);
    state = {controller, {}};
    state.xinput_slot = xinput_slot;
    // XInput seems to start with packet_number = 1 .
    state.state_changed = true;
    UpdateXCapabilities(state);

    XELOGI(
        "SDL OnControllerDeviceAdded: Added at index {}, "
        "XInputSubType({} = 0x{:02X}).",
        user_id, XInputSubTypeName(state.caps.sub_type), state.caps.sub_type);
    if (xinput_slot >= 0) {
      const auto& c = state.native_caps;
      XELOGI(
          "SDL Controller {}: XInput caps Type(0x{:02X}) SubType(0x{:02X}) "
          "Flags(0x{:04X}) Buttons(0x{:04X}) Triggers(0x{:02X}, 0x{:02X}) "
          "LeftThumb(0x{:04X}, 0x{:04X}) RightThumb(0x{:04X}, 0x{:04X}) "
          "Vibration(0x{:04X}, 0x{:04X})",
          user_id, c.type, c.sub_type, uint16_t(c.flags),
          uint16_t(c.gamepad.buttons), c.gamepad.left_trigger,
          c.gamepad.right_trigger, uint16_t(c.gamepad.thumb_lx),
          uint16_t(c.gamepad.thumb_ly), uint16_t(c.gamepad.thumb_rx),
          uint16_t(c.gamepad.thumb_ry), uint16_t(c.vibration.left_motor_speed),
          uint16_t(c.vibration.right_motor_speed));
    }
    XELOGI("SDL Controller {}: {}", user_id, SDL_GetGamepadMapping(controller));
    NotifyDevicesChanged();
  } else {
    // No more controllers needed, close it.
    SDL_CloseGamepad(controller);
    XELOGW("SDL OnControllerDeviceAdded: Ignored. No free slots.");
  }
}

void SDLInputDriver::OnControllerDeviceRemoved(const SDL_Event& event) {
  // Find the disconnected gamepad and close it.
  auto idx = GetControllerIndexFromInstanceID(event.gdevice.which);
  if (idx) {
    auto* sdl = controllers_.at(*idx).sdl;
    const char* name = GetDeviceName(sdl);
    char guid_str[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUID(SDL_GetGamepadJoystick(sdl)), guid_str,
                     sizeof(guid_str));
    XELOGI("SDL OnControllerDeviceRemoved: \"{}\", GUID({}), driver_slot({}).",
           name ? name : "?", guid_str, *idx);
    SDL_CloseGamepad(sdl);
    controllers_.at(*idx) = {};
    keystroke_states_.at(*idx) = {};
    NotifyDevicesChanged();
  } else {
    // Can happen in case all slots where full previously.
    XELOGW(
        "SDL OnControllerDeviceRemoved: Ignored, instance_id({}) not in use.",
        event.gdevice.which);
  }
}

void SDLInputDriver::OnControllerDeviceAxisMotion(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  assert(idx);
  auto& pad = controllers_.at(*idx).state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  controllers_.at(*idx).state_changed = true;
}

void SDLInputDriver::OnControllerDeviceButtonChanged(const SDL_Event& event) {
  // Define a lookup table to map between SDL and XInput button codes.
  // These need to be in the order of the SDL_GamepadButton enum.
  static constexpr std::array<
      std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          // Standard buttons (SDL3 uses position-based names: SOUTH/EAST/
          // WEST/NORTH = the Xbox A/B/X/Y physical positions):
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          // There are additional buttons only available on some controllers.
          // For now just assign sensible defaults
          // Misc:
          X_INPUT_GAMEPAD_GUIDE,
          // Xbox Elite paddles (SDL3 indices preserve physical position even
          // though the SDL2 PADDLE1..PADDLE4 names became RIGHT_PADDLE1,
          // LEFT_PADDLE1, RIGHT_PADDLE2, LEFT_PADDLE2):
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          // PS touchpad button
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gbutton.which);
  assert(idx);
  auto& controller = controllers_.at(*idx);

  uint16_t xbuttons = controller.state.gamepad.buttons;
  // Lookup the XInput button code.
  if (event.gbutton.button >= xbutton_lookup.size()) {
    // A newer SDL Version may have added new buttons.
    XELOGI("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = xbutton_lookup.at(event.gbutton.button);
  // Pressed or released?
  if (event.gbutton.down) {
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(
    SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    auto controller = controllers_.at(i).sdl;
    if (!controller) {
      continue;
    }
    auto joystick = SDL_GetGamepadJoystick(controller);
    assert(joystick);
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

SDLInputDriver::ControllerState* SDLInputDriver::GetControllerState(
    uint32_t user_index) {
  if (user_index >= controllers_.size()) {
    return nullptr;
  }
  auto controller = &controllers_.at(user_index);
  if (!controller->sdl) {
    return nullptr;
  }
  return controller;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(state.sdl);
#if XE_PLATFORM_WIN32
  XINPUT_CAPABILITIES native;
  if (state.xinput_slot >= 0 &&
      GetXInputApi().get_capabilities(state.xinput_slot, 0, &native) ==
          ERROR_SUCCESS) {
    auto& c = state.native_caps;
    c.type = native.Type;
    c.sub_type = native.SubType;
    c.flags = native.Flags;
    c.gamepad.buttons = native.Gamepad.wButtons;
    c.gamepad.left_trigger = native.Gamepad.bLeftTrigger;
    c.gamepad.right_trigger = native.Gamepad.bRightTrigger;
    c.gamepad.thumb_lx = native.Gamepad.sThumbLX;
    c.gamepad.thumb_ly = native.Gamepad.sThumbLY;
    c.gamepad.thumb_rx = native.Gamepad.sThumbRX;
    c.gamepad.thumb_ry = native.Gamepad.sThumbRY;
    c.vibration.left_motor_speed = native.Vibration.wLeftMotorSpeed;
    c.vibration.right_motor_speed = native.Vibration.wRightMotorSpeed;
  }
  // A transient failure keeps the last caps read from XInput.
  if (state.native()) {
    state.caps = state.native_caps;
    return;
  }
#endif  // XE_PLATFORM_WIN32

  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  // Guess if we are wireless: a battery-backed power state means the device
  // is running on its own power.
  const SDL_PowerState power_state =
      SDL_GetJoystickPowerInfo(SDL_GetGamepadJoystick(state.sdl), nullptr);
  if (power_state == SDL_POWERSTATE_ON_BATTERY ||
      power_state == SDL_POWERSTATE_CHARGING ||
      power_state == SDL_POWERSTATE_CHARGED) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  // Check if all navigational buttons are present
  static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
      SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,
      SDL_GAMEPAD_BUTTON_DPAD_UP,   SDL_GAMEPAD_BUTTON_DPAD_DOWN,
      SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  };
  for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
    if (!SDL_GamepadHasButton(state.sdl, *it)) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
      break;
    }
  }

  auto& c = state.caps;
  c.type = 0x01;  // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = SdlTypeToXInputSubType(
      SDL_GetJoystickType(SDL_GetGamepadJoystick(state.sdl)),
      GetDeviceName(state.sdl));
  c.flags = cap_flags;
  c.gamepad.buttons = 0xF3FF | X_INPUT_GAMEPAD_GUIDE;
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  c.vibration.left_motor_speed = 0xFFFFu;
  c.vibration.right_motor_speed = 0xFFFFu;
}

std::vector<InputDeviceInfo> SDLInputDriver::EnumerateDevices() {
  std::vector<InputDeviceInfo> out;
  for (size_t i = 0; i < controllers_.size(); ++i) {
    auto* sdl = controllers_.at(i).sdl;
    if (!sdl) {
      continue;
    }
    InputDeviceInfo info{};
    info.driver_slot = static_cast<uint8_t>(i);
    const char* name = GetDeviceName(sdl);
    auto* joystick = SDL_GetGamepadJoystick(sdl);
    if (joystick) {
      char guid_buf[33] = {};
      SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid_buf,
                       sizeof(guid_buf));
      info.stable_id = guid_buf;
      info.subtype =
          SdlTypeToXInputSubType(SDL_GetJoystickType(joystick), name);
      if (controllers_.at(i).xinput_slot >= 0) {
        info.native_subtype = controllers_.at(i).native_caps.sub_type;
      }
    }
    info.display_name = name ? name : "Controller";
    out.push_back(std::move(info));
  }
  return out;
}

void SDLInputDriver::SetNativeMode(uint8_t driver_slot, bool enabled) {
  if (driver_slot >= controllers_.size()) {
    return;
  }
  auto& controller = controllers_.at(driver_slot);
  if (controller.native_mode == enabled) {
    return;
  }
  controller.native_mode = enabled;
  if (controller.sdl) {
    UpdateXCapabilities(controller);
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(
    const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = gamepad.thumb_lx;
  auto thumb_y = gamepad.thumb_ly;
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = gamepad.thumb_rx;
    thumb_y = gamepad.thumb_ry;
  }
  return f;
}

}  // namespace sdl
}  // namespace hid
}  // namespace xe
