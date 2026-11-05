/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <alloca.h>
#include <dlfcn.h>
#include <stdlib.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"

// Forward-declared instead of pulling in <SDL3/SDL.h> — xenia-base must not
// drag SDL onto its include path, and only the dlsym() cast needs the type.
// Flags and signature are stable across SDL2/3.
namespace {
using SDL_ShowSimpleMessageBox_fn = int (*)(uint32_t flags, const char* title,
                                            const char* message, void* window);
using SDL_OpenURL_fn = bool (*)(const char* url);
using SDL_GetError_fn = const char* (*)();
constexpr uint32_t kSDL_MessageBoxError = 0x00000010;
constexpr uint32_t kSDL_MessageBoxWarning = 0x00000020;
constexpr uint32_t kSDL_MessageBoxInformation = 0x00000040;
}  // namespace

namespace xe {

void* LoadSDL3Library() {
  // The versioned soname is what the runtime package installs. The unversioned
  // one only comes with the development package.
  void* libsdl = dlopen("libSDL3.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (!libsdl) {
    libsdl = dlopen("libSDL3.so", RTLD_LAZY | RTLD_LOCAL);
  }
  return libsdl;
}

namespace {
// Goes through the desktop portal or xdg-open, never a shell.
void OpenUrl(const std::string& url) {
  void* libsdl = LoadSDL3Library();
  if (!libsdl) {
    XELOGE("Failed to open {}: {}", xe::strip_pii(url), dlerror());
    return;
  }
  auto pSDL_OpenURL =
      reinterpret_cast<SDL_OpenURL_fn>(dlsym(libsdl, "SDL_OpenURL"));
  auto pSDL_GetError =
      reinterpret_cast<SDL_GetError_fn>(dlsym(libsdl, "SDL_GetError"));
  if (!pSDL_OpenURL) {
    XELOGE("Failed to open {}: SDL_OpenURL is unavailable", xe::strip_pii(url));
  } else if (!pSDL_OpenURL(url.c_str())) {
    XELOGE("Failed to open {}: {}", xe::strip_pii(url),
           pSDL_GetError ? pSDL_GetError() : "unknown error");
  }
  dlclose(libsdl);
}
}  // namespace

void LaunchWebBrowser(const std::string_view url) { OpenUrl(std::string(url)); }

void LaunchFileExplorer(const std::filesystem::path& path) {
  std::error_code ec;
  OpenUrl(std::filesystem::absolute(path, ec).string());
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  void* libsdl = LoadSDL3Library();
  assert_not_null(libsdl);
  if (libsdl) {
    auto pSDL_ShowSimpleMessageBox =
        reinterpret_cast<SDL_ShowSimpleMessageBox_fn>(
            dlsym(libsdl, "SDL_ShowSimpleMessageBox"));
    assert_not_null(pSDL_ShowSimpleMessageBox);
    if (pSDL_ShowSimpleMessageBox) {
      uint32_t flags;
      const char* title;
      char* message_copy = reinterpret_cast<char*>(alloca(message.size() + 1));
      std::memcpy(message_copy, message.data(), message.size());
      message_copy[message.size()] = '\0';

      switch (type) {
        default:
        case SimpleMessageBoxType::Help:
          title = "Xenia Help";
          flags = kSDL_MessageBoxInformation;
          break;
        case SimpleMessageBoxType::Warning:
          title = "Xenia Warning";
          flags = kSDL_MessageBoxWarning;
          break;
        case SimpleMessageBoxType::Error:
          title = "Xenia Error";
          flags = kSDL_MessageBoxError;
          break;
      }
      pSDL_ShowSimpleMessageBox(flags, title, message_copy, NULL);
    }
    dlclose(libsdl);
  }
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

std::string GetMachineId() {
  // systemd's, else the copy D-Bus kept before systemd.
  for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
    std::ifstream file(path);
    std::string id;
    if (file >> id) {
      return id;
    }
  }
  return {};
}
}  // namespace xe
