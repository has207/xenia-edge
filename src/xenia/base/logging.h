/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_LOGGING_H_
#define XENIA_BASE_LOGGING_H_

#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>

#include "third_party/fmt/include/fmt/format.h"
#include "third_party/fmt/include/fmt/std.h"
#include "xenia/base/string.h"

namespace xe {

#define XE_OPTION_ENABLE_LOGGING 1

// Log level is a general indication of the importance of a given log line.
//
// While log levels are named, they are a rough correlation of what the log line
// may be related to. These names should not be taken as fact as what a given
// log line from any log level actually is.
enum class LogLevel {
  Disabled = -1,
  Error = 0,
  Warning,
  Info,
  Debug,
  Trace,
};

// bitmasks!
namespace LogSrc {
enum : uint32_t { Uncategorized = 0, Kernel = 1, Apu = 2, Cpu = 4, Gpu = 8 };
}

class LogSink {
 public:
  virtual ~LogSink() = default;

  virtual void Write(const char* buf, size_t size) = 0;
  virtual void Flush() = 0;
};

class FileLogSink final : public LogSink {
 public:
  explicit FileLogSink(FILE* file, bool own_file)
      : file_(file), owns_file_(own_file) {}
  ~FileLogSink();

  void Write(const char* buf, size_t size) override;
  void Flush() override;

 private:
  FILE* file_;
  bool owns_file_;
};

class DebugPrintLogSink final : public LogSink {
 public:
  DebugPrintLogSink() = default;

  void Write(const char* buf, size_t size) override;
  void Flush() override {}
};

// Initializes the logging system and any outputs requested.
// Must be called on startup.
void InitializeLogging(const std::string_view app_name);
void ShutdownLogging();
void FlushLog();

// Flushes all log sinks immediately.
// Useful before quick_exit() to ensure logs are written.
void FlushLog();
// Whether the calling thread is the one writing the log out, which would wait
// on itself to log.
bool IsLogWriterThread();

// Logs a message box, dialog or notification as it's shown, so the log has
// what the user was asked or told, whether or not anyone saw it (unattended).
void LogUIElement(const std::string_view kind, const std::string_view title,
                  const std::string_view text);

// Logs a ShowSimpleMessageBox box, and returns whether it should still be
// shown, which it isn't when unattended.
enum class SimpleMessageBoxType;
bool LogSimpleMessageBox(SimpleMessageBoxType type,
                         const std::string_view message);

// Whether nobody is there to answer the UI: unattended, or headless, which is
// unattended without the window.
bool IsUnattended();

// When unattended, states why the run is over and ends the process with
// exit_code, as nobody is there to close it.
[[noreturn]] void ExitUnattended(int exit_code, const std::string_view reason);

// The result of path_to_utf8, in filesystem.h.
class Utf8Path;

// Writes the user name in UTF-8 text, such as a path from path_to_utf8, as
// $USER (%USERNAME% on Windows), so logs can be shared without it.
std::string strip_pii(const std::string_view text);

namespace logging {

constexpr char kPrefixCharError = '!';
constexpr char kPrefixCharWarning = 'w';
constexpr char kPrefixCharInfo = 'i';
constexpr char kPrefixCharDebug = 'd';

bool ShouldLog(LogLevel log_level,
               uint32_t log_mask = xe::LogSrc::Uncategorized);

// Advances the present-frame counter shown in the log prefix.
// Called once per guest present (VdSwap).
void IncrementFrameNumber();
// The same counter, for anything needing a cheap "is the guest still
// presenting frames" signal (the guest scheduler's no-progress detector).
uint32_t GetFrameNumber();
namespace internal {

uint32_t GetLogLevel();
std::pair<char*, size_t> GetThreadBuffer();

XE_NOALIAS
void AppendLogLine(LogLevel log_level, const char prefix_char, size_t written);

}  // namespace internal

// Host paths carry the user name, so log calls only take them through
// strip_pii.
template <typename... Args>
concept NoHostPaths =
    !((std::is_same_v<std::remove_cvref_t<Args>, std::filesystem::path> ||
       std::is_same_v<std::remove_cvref_t<Args>, Utf8Path>) ||
      ...);

// technically, noalias is incorrect here, these functions do in fact alias
// global memory, but msvc will not optimize the calls away, and the global
// memory modified by the calls is limited to internal logging variables, so it
// might as well be noalias
template <typename... Args>
XE_NOALIAS XE_NOINLINE XE_COLD static void AppendLogLineFormat_Impl(
    LogLevel log_level, const char prefix_char, std::string_view format,
    const Args&... args) noexcept {
  auto target = internal::GetThreadBuffer();
  auto result = fmt::format_to_n(target.first, target.second,
                                 fmt::runtime(format), args...);
  internal::AppendLogLine(log_level, prefix_char, result.size);
}

// Appends a line to the log with {fmt}-style formatting.
// chrispy: inline the initial check, outline the append. the append should
// happen rarely for end users
template <typename... Args>
  requires NoHostPaths<Args...>
XE_FORCEINLINE static void AppendLogLineFormat(uint32_t log_src_mask,
                                               LogLevel log_level,
                                               const char prefix_char,
                                               std::string_view format,
                                               const Args&... args) noexcept {
  if (!ShouldLog(log_level, log_src_mask)) {
    return;
  }
  AppendLogLineFormat_Impl(log_level, prefix_char, format, args...);
}

// Appends a line to the log.
void AppendLogLine(LogLevel log_level, const char prefix_char,
                   const std::string_view str,
                   uint32_t log_mask = LogSrc::Uncategorized);

template <LogLevel ll>
struct LoggerBatch {
  char* thrd_buf;       // current position in thread buffer
  size_t thrd_buf_rem;  // num left in thrd buffer
  size_t total_size;

  void reset() {
    auto target = logging::internal::GetThreadBuffer();

    thrd_buf = target.first;
    thrd_buf_rem = target.second;
    total_size = 0;
  }

  LoggerBatch() { reset(); }
  template <size_t fmtlen, typename... Ts>
    requires NoHostPaths<Ts...>
  void operator()(const char (&fmt)[fmtlen], Ts&&... args) {
    auto tmpres =
        fmt::format_to_n(thrd_buf, thrd_buf_rem, fmt::runtime(fmt), args...);
    thrd_buf_rem -= tmpres.size;
    thrd_buf = tmpres.out;
    total_size += tmpres.size;
  }

  void submit(char prefix_char) {
    logging::internal::AppendLogLine(ll, prefix_char, total_size);
  }
};

}  // namespace logging

// Logs a fatal error and aborts the program.
[[noreturn]] void FatalError(const std::string_view str);

}  // namespace xe

#if XE_OPTION_ENABLE_LOGGING

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
XE_COLD void XELOGE(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(
      xe::LogSrc::Uncategorized, xe::LogLevel::Error,
      xe::logging::kPrefixCharError, format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
XE_COLD void XELOGW(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(
      xe::LogSrc::Uncategorized, xe::LogLevel::Warning,
      xe::logging::kPrefixCharWarning, format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
void XELOGI(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(
      xe::LogSrc::Uncategorized, xe::LogLevel::Info,
      xe::logging::kPrefixCharInfo, format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
void XELOGD(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(
      xe::LogSrc::Uncategorized, xe::LogLevel::Debug,
      xe::logging::kPrefixCharDebug, format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
void XELOGCPU(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(xe::LogSrc::Cpu, xe::LogLevel::Info, 'C',
                                   format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
void XELOGAPU(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(xe::LogSrc::Apu, xe::LogLevel::Debug, 'A',
                                   format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
void XELOGGPU(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(xe::LogSrc::Gpu, xe::LogLevel::Debug, 'G',
                                   format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
void XELOGKERNEL(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(xe::LogSrc::Kernel, xe::LogLevel::Info, 'K',
                                   format, args...);
}

template <typename... Args>
  requires xe::logging::NoHostPaths<Args...>
void XELOGFS(std::string_view format, const Args&... args) {
  xe::logging::AppendLogLineFormat(xe::LogSrc::Uncategorized,
                                   xe::LogLevel::Info, 'F', format, args...);
}

#else

#define __XELOGDUMMY \
  do {               \
  } while (false)

#define XELOGE(...) __XELOGDUMMY
#define XELOGW(...) __XELOGDUMMY
#define XELOGI(...) __XELOGDUMMY
#define XELOGD(...) __XELOGDUMMY
#define XELOGCPU(...) __XELOGDUMMY
#define XELOGAPU(...) __XELOGDUMMY
#define XELOGGPU(...) __XELOGDUMMY
#define XELOGKERNEL(...) __XELOGDUMMY
#define XELOGFS(...) __XELOGDUMMY

#endif  // ENABLE_LOGGING

#endif  // XENIA_BASE_LOGGING_H_
