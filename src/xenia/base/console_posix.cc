/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <stdio.h>
#include <unistd.h>

#include "xenia/base/console.h"

namespace xe {

bool has_console_attached() { return isatty(fileno(stdin)) == 1; }

void AttachConsole() {}

// stdout already goes wherever the launcher pointed it.
void AttachConsoleForStdio(bool open_window) {}

FILE* console_echo_stream() { return nullptr; }

}  // namespace xe
