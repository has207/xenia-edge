/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <crt_externs.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
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
  // posix_spawnp returns the error number; it does not set errno.
  int error = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(),
                           *_NSGetEnviron());
  if (error) {
    XELOGE("posix_spawnp({}) failed: {} ({})", argv[0], strerror(error), error);
    return;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
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

std::string GetMachineId() {
  // The hardware UUID. MACH_PORT_NULL picks the default main port.
  io_service_t platform = IOServiceGetMatchingService(
      MACH_PORT_NULL, IOServiceMatching("IOPlatformExpertDevice"));
  if (!platform) {
    return {};
  }
  auto uuid = static_cast<CFStringRef>(IORegistryEntryCreateCFProperty(
      platform, CFSTR(kIOPlatformUUIDKey), kCFAllocatorDefault, 0));
  IOObjectRelease(platform);
  if (!uuid) {
    return {};
  }
  char buffer[64];
  const bool converted =
      CFStringGetCString(uuid, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(uuid);
  return converted ? std::string(buffer) : std::string();
}

bool ExecutableHasX86_64Slice() {
  static const bool has_slice = [] {
    CFArrayRef archs =
        CFBundleCopyExecutableArchitectures(CFBundleGetMainBundle());
    if (!archs) {
      return false;
    }
    int32_t x86_64 = kCFBundleExecutableArchitectureX86_64;
    CFNumberRef x86_64_number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &x86_64);
    bool result = CFArrayContainsValue(
        archs, CFRangeMake(0, CFArrayGetCount(archs)), x86_64_number);
    CFRelease(x86_64_number);
    CFRelease(archs);
    return result;
  }();
  return has_slice;
}

}  // namespace xe
