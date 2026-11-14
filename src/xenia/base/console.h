/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_CONSOLE_H_
#define XENIA_BASE_CONSOLE_H_

#include <cstdio>

namespace xe {

// Returns true if there is a user-visible console attached to receive stdout.
bool has_console_attached();

void AttachConsole();

// For unattended and enable_console: keeps stdout and stderr where the launcher
// redirected them (a file or pipe), and binds them to the launching console
// otherwise. open_window opens a console window when there's no launching
// console, and gives it a copy of the log when stdout goes elsewhere.
void AttachConsoleForStdio(bool open_window);

// The console window's stream when it isn't stdout, or null.
FILE* console_echo_stream();

}  // namespace xe

#endif  // XENIA_BASE_CONSOLE_H_
