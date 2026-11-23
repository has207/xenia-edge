/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>

#include "xenia/base/console.h"
#include "xenia/base/platform_win.h"

namespace xe {

// TODO(Triang3l): Set the default depending on the actual subsystem. Currently
// it inhibits message boxes.
static bool has_console_attached_ = false;

bool has_console_attached() { return has_console_attached_; }

static bool has_shell_environment_variable() {
  size_t size = 0;
  // Check if SHELL exists
  // If it doesn't, then we are in a Windows Terminal
  auto error = _wgetenv_s(&size, nullptr, 0, L"SHELL");
  if (error) {
    return false;
  }
  return !!size;
}

void AttachConsole() {
  bool has_console = ::AttachConsole(ATTACH_PARENT_PROCESS) == TRUE;
#if 0
  if (!has_console || !has_shell_environment_variable()) {
    // We weren't launched from a console, so just return.
    has_console_attached_ = false;
    return;
  }
#endif
  AllocConsole();

  has_console_attached_ = true;

  FILE* fp;
  freopen_s(&fp, "CONOUT$", "w", stdout);
  freopen_s(&fp, "CONOUT$", "w", stderr);
}

// Whether the launcher gave this GUI process a file or pipe for the handle,
// which the CRT has already bound its stream to.
static bool IsStdHandleRedirected(DWORD std_handle) {
  HANDLE handle = GetStdHandle(std_handle);
  if (!handle || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD type = GetFileType(handle);
  return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

void AttachConsoleForStdio() {
  const bool stdout_redirected = IsStdHandleRedirected(STD_OUTPUT_HANDLE);
  const bool stderr_redirected = IsStdHandleRedirected(STD_ERROR_HANDLE);
  if ((stdout_redirected && stderr_redirected) ||
      !::AttachConsole(ATTACH_PARENT_PROCESS)) {
    return;
  }
  has_console_attached_ = true;

  FILE* fp;
  if (!stdout_redirected) {
    freopen_s(&fp, "CONOUT$", "w", stdout);
  }
  if (!stderr_redirected) {
    freopen_s(&fp, "CONOUT$", "w", stderr);
  }
}

}  // namespace xe
