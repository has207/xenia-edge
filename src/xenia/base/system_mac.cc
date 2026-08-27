/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <crt_externs.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "xenia/base/system.h"

namespace xe {
namespace {

void SpawnAndWait(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(),
                   *_NSGetEnviron())) {
    return;
  }
  int status = 0;
  waitpid(pid, &status, 0);
}

}  // namespace

void LaunchWebBrowser(const std::string_view url) {
  SpawnAndWait({"open", std::string(url)});
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  SpawnAndWait({"open", path.string()});
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* icon;
  switch (type) {
    case SimpleMessageBoxType::Help:
      icon = "note";
      break;
    case SimpleMessageBoxType::Warning:
      icon = "caution";
      break;
    default:
    case SimpleMessageBoxType::Error:
      icon = "stop";
      break;
  }
  std::string script = "display dialog \"";
  // A raw newline is a syntax error inside an AppleScript string.
  for (char c : message) {
    switch (c) {
      case '"':
      case '\\':
        script += '\\';
        script += c;
        break;
      case '\n':
        script += "\\n";
        break;
      case '\r':
        script += "\\r";
        break;
      case '\t':
        script += "\\t";
        break;
      default:
        script += c;
        break;
    }
  }
  script += "\" with icon ";
  script += icon;
  script += " buttons {\"OK\"} default button \"OK\" with title \"Xenia\"";
  SpawnAndWait({"osascript", "-e", script});
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
