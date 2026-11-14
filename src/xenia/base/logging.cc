/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "third_party/disruptorplus/include/disruptorplus/multi_threaded_claim_strategy.hpp"
#include "third_party/disruptorplus/include/disruptorplus/ring_buffer.hpp"
#include "third_party/disruptorplus/include/disruptorplus/sequence_barrier.hpp"
#include "third_party/disruptorplus/include/disruptorplus/spin_wait_strategy.hpp"
#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/console.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/base/string.h"
#include "xenia/base/system.h"
#include "xenia/base/threading.h"
#include "xenia/base/utf8.h"

// TODO(benvanik): generic API? logging_win.cc?
#if XE_PLATFORM_ANDROID
#include <android/log.h>
#elif XE_PLATFORM_WIN32
#include <fcntl.h>
#include <io.h>

// For MessageBox:
#include "xenia/base/platform_win.h"
#endif  // XE_PLATFORM

#include "third_party/fmt/include/fmt/format.h"

#if XE_PLATFORM_ANDROID
DEFINE_bool(log_to_logcat, true, "Write log output to Android Logcat.",
            "Logging");
#else
DEFINE_path(log_file, "", "Logs are written to the given file", "Logging");
DEFINE_transient_bool(log_append, false,
                      "Append to existing log file instead of overwriting. "
                      "Set for the processes a running xenia starts.",
                      "Logging");
DEFINE_bool(log_to_stdout, true, "Write log output to stdout", "Logging");
DEFINE_bool(log_to_debugprint, false, "Dump the log to DebugPrint.", "Logging");
#endif  // XE_PLATFORM_ANDROID
DEFINE_bool(flush_log, true, "Flush log file after each log line batch.",
            "Logging");

DEFINE_transient_bool(
    unattended, false,
    "Runs the target with nobody there to answer: the log goes to stdout, "
    "message boxes and dialogs are logged and dismissed with their default "
    "choice, and the process exits when the title exits (0) or crashes (1).",
    "General");
DEFINE_transient_bool(headless, false,
                      "Runs unattended without showing the window.",
                      "General");

DEFINE_uint32(log_mask, 0,
              "Disables specific categorizes for more granular debug logging. "
              "Kernel = 1, Apu = 2, Cpu = 4, Gpu = 8.",
              "Logging");

DEFINE_int32(
    log_level, 2,
    "Maximum level to be logged. (0=error, 1=warning, 2=info, 3=debug)",
    "Logging");

namespace dp = disruptorplus;
using namespace xe::literals;
using namespace std::chrono_literals;

namespace xe {

class Logger;

Logger* logger_ = nullptr;

// Present-frame counter, advanced once per guest present. Stamped into each
// log line at emit time.
std::atomic<uint32_t> global_frame_number_{0};

struct LogLine {
  size_t buffer_length;
  uint32_t thread_id;
  uint32_t frame_number;
  uint16_t _pad_0;  // (2b) padding
  bool terminate;
  char prefix_char;
};

thread_local char thread_log_buffer_[64_KiB];

static std::string FindUserName() {
#if XE_PLATFORM_WIN32
  const wchar_t* name = _wgetenv(L"USERNAME");
#else
  const char* name = std::getenv("USER");
#endif  // XE_PLATFORM_WIN32
  return name ? xe::path_to_utf8(name) : std::string();
}

std::string strip_pii(const std::string_view text) {
  // Leaked as threads still logging at exit may strip text.
  static const std::string& user_name = *new std::string(FindUserName());
  if (user_name.empty()) {
    return std::string(text);
  }
  // A quote ends a path in text such as an exception message. The placeholder
  // is the variable the name comes from, so a pasted path still works.
#if XE_PLATFORM_WIN32
  constexpr std::string_view kDelimiters = "\\/\"";
  constexpr std::string_view kPlaceholder = "%USERNAME%";
#else
  constexpr std::string_view kDelimiters = "/\"";
  constexpr std::string_view kPlaceholder = "$USER";
#endif  // XE_PLATFORM_WIN32
  std::string stripped;
  for (size_t start = 0; start <= text.size();) {
    size_t end = std::min(text.find_first_of(kDelimiters, start), text.size());
    std::string_view name = text.substr(start, end - start);
    if (xe::utf8::equal_case(name, user_name)) {
      name = kPlaceholder;
    }
    stripped += name;
    if (end < text.size()) {
      stripped += text[end];
    }
    start = end + 1;
  }
  return stripped;
}

FileLogSink::~FileLogSink() {
  if (file_) {
    fflush(file_);
    if (owns_file_) {
      fclose(file_);
    }
  }
}

void FileLogSink::Write(const char* buf, size_t size) {
  if (file_) {
    fwrite(buf, 1, size, file_);
  }
}

void FileLogSink::Flush() {
  if (file_) {
    fflush(file_);
  }
}

void DebugPrintLogSink::Write(const char* buf, size_t size) {
  debugging::DebugPrint("{}", std::string_view(buf, size));
}

#if XE_PLATFORM_ANDROID
class AndroidLogSink final : public LogSink {
 public:
  explicit AndroidLogSink(const std::string_view tag) : tag_(tag) {}

  void Write(const char* buf, size_t size) override;
  void Flush() override;

 private:
  // May be called with an empty line_buffer_ to write an empty log message (if
  // the original message contains a blank line as a separator, for instance).
  void WriteLineBuffer();

  const std::string tag_;
  android_LogPriority current_priority_ = ANDROID_LOG_INFO;
  bool is_line_continuation_ = false;
  size_t line_buffer_used_ = 0;
  // LOGGER_ENTRY_MAX_PAYLOAD is defined as 4076 on older Android versions and
  // as 4068 on newer ones. An attempt to write more than that amount to the
  // kernel logger will result in a truncated log entry. 4000 is the commonly
  // used limit considered safe.
  // However, "Log message text may be truncated to less than an
  // implementation-specific limit (1023 bytes)" - android/log.h.
  char line_buffer_[1023 + 1];
};

void AndroidLogSink::Write(const char* buf, size_t size) {
  // A null character, if appears, is fine - it will just truncate a single
  // line, but since every message ends with a newline, and WriteLineBuffer is
  // done after every line, it won't have lasting effects.
  // Using memchr and memcpy as they are vectorized in Bionic.
  while (size) {
    const void* newline = std::memchr(buf, '\n', size);
    size_t line_remaining =
        newline ? size_t(static_cast<const char*>(newline) - buf) : size;
    while (line_remaining) {
      assert_true(line_buffer_used_ < xe::countof(line_buffer_));
      size_t append_count =
          std::min(line_remaining,
                   xe::countof(line_buffer_) - size_t(1) - line_buffer_used_);
      std::memcpy(line_buffer_ + line_buffer_used_, buf, append_count);
      buf += append_count;
      size -= append_count;
      line_remaining -= append_count;
      line_buffer_used_ += append_count;
      if (line_buffer_used_ >= xe::countof(line_buffer_) - size_t(1)) {
        WriteLineBuffer();
        // The next WriteLineBuffer belongs to the same line.
        is_line_continuation_ = true;
      }
    }
    if (newline) {
      // If the end of the buffer was reached right before a newline character,
      // so line_buffer_used_ is 0, and is_line_continuation_ has been set, the
      // line break has already been done by Android itself - don't write an
      // empty message. However, if the message intentionally contains blank
      // lines, write them.
      if (line_buffer_used_ || !is_line_continuation_) {
        WriteLineBuffer();
      }
      is_line_continuation_ = false;
      ++buf;
      --size;
    }
  }
}

void AndroidLogSink::Flush() {
  if (line_buffer_used_) {
    WriteLineBuffer();
  }
}

void AndroidLogSink::WriteLineBuffer() {
  // If this is a new line, check if it's a new log message, and if it is,
  // update the priority based on the prefix.
  if (!is_line_continuation_ && line_buffer_used_ >= 3 &&
      line_buffer_[1] == '>' && line_buffer_[2] == ' ') {
    switch (line_buffer_[0]) {
      case 'd':
        current_priority_ = ANDROID_LOG_DEBUG;
        break;
      case 'w':
        current_priority_ = ANDROID_LOG_WARN;
        break;
      case '!':
        current_priority_ = ANDROID_LOG_ERROR;
        break;
      case 'x':
        current_priority_ = ANDROID_LOG_FATAL;
        break;
      default:
        current_priority_ = ANDROID_LOG_INFO;
        break;
    }
  }
  // Android skips blank lines, but if writing a blank line was requested
  // explicitly for formatting, write a non-newline character.
  if (!line_buffer_used_) {
    line_buffer_[line_buffer_used_++] = ' ';
  }
  // Terminate the text.
  assert_true(line_buffer_used_ < xe::countof(line_buffer_));
  line_buffer_[line_buffer_used_] = '\0';
  // Write.
  __android_log_write(current_priority_, tag_.c_str(), line_buffer_);
  line_buffer_used_ = 0;
}
#endif  // XE_PLATFORM_ANDROID

class Logger {
 public:
  explicit Logger(const std::string_view app_name)
      : wait_strategy_(),
        claim_strategy_(kBlockCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);

    write_thread_ =
        xe::threading::Thread::Create({}, [this]() { WriteThread(); });
    assert_not_null(write_thread_);
    write_thread_->set_name("Logging Writer");
  }

  ~Logger() {
    AppendLine(0, '\0', nullptr, 0, true);  // append a terminator
    xe::threading::Wait(write_thread_.get(), true);
  }

  void AddLogSink(std::unique_ptr<LogSink>&& sink) {
    sinks_.push_back(std::move(sink));
  }

  void FlushAllSinks() {
    for (const auto& sink : sinks_) {
      sink->Flush();
    }
  }

  bool IsWriteThread() const {
    return xe::threading::current_thread_system_id() ==
           write_thread_id_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr size_t kBufferSize = 8_MiB;
  uint8_t buffer_[kBufferSize] = {};

  static constexpr size_t kBlockSize = 256;
  static constexpr size_t kBlockCount = kBufferSize / kBlockSize;
  static constexpr size_t kBlockIndexMask = kBlockCount - 1;

  static const size_t kClaimStrategyFootprint =
      sizeof(std::atomic<dp::sequence_t>[kBlockCount]);

  static size_t BlockOffset(dp::sequence_t sequence) {
    return (sequence & kBlockIndexMask) * kBlockSize;
  }

  static size_t BlockCount(size_t byte_size) {
    return (byte_size + (kBlockSize - 1)) / kBlockSize;
  }

  dp::spin_wait_strategy wait_strategy_;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim_strategy_;
  dp::sequence_barrier<dp::spin_wait_strategy> consumed_;

  std::vector<std::unique_ptr<LogSink>> sinks_;

  std::unique_ptr<xe::threading::Thread> write_thread_;
  std::atomic<uint32_t> write_thread_id_{0};

  void Write(const char* buf, size_t size) {
    for (const auto& sink : sinks_) {
      sink->Write(buf, size);
    }
  }

  void WriteThread() {
    write_thread_id_.store(xe::threading::current_thread_system_id(),
                           std::memory_order_relaxed);
    RingBuffer rb(buffer_, kBufferSize);

    size_t idle_loops = 0;

    dp::sequence_t next_sequence = 0;
    dp::sequence_t last_sequence = -1;

    size_t desired_count = 1;
    while (true) {
      // We want one block to find out how many blocks we need or we know how
      // many blocks needed for at least one log line.
      auto next_range = dp::sequence_range(next_sequence, desired_count);

      claim_strategy_.wait_until_published(next_range.last(), last_sequence);

      size_t read_count = 0;
      auto available_range = next_range;
      auto available_count = available_range.size();

      rb.set_write_offset(BlockOffset(available_range.end()));

      bool terminate = false;
      for (size_t i = available_range.first(); i != available_range.end();) {
        rb.set_read_offset(BlockOffset(i));

        LogLine line;
        rb.Read(&line, sizeof(line));

        auto needed_count = BlockCount(sizeof(LogLine) + line.buffer_length);
        if (read_count + needed_count > available_count) {
          // More blocks are needed for a complete line.
          desired_count = needed_count;
          break;
        } else {
          // Enough blocks to read this log line, advance by that many.
          read_count += needed_count;
          i += needed_count;

          if (line.prefix_char) {
            // <type>> f:<frame> <thread id>
            char prefix[32];
            auto result = fmt::format_to_n(
                prefix, sizeof(prefix), "{}> f:{:07} {:08X} ", line.prefix_char,
                line.frame_number, line.thread_id);
            Write(prefix, result.size);
          }

          if (line.buffer_length) {
            // Get access to the line data - which may be split in the ring
            // buffer - and write it out in parts.
            auto line_range = rb.BeginRead(line.buffer_length);
            Write(reinterpret_cast<const char*>(line_range.first),
                  line_range.first_length);
            if (line_range.second_length) {
              Write(reinterpret_cast<const char*>(line_range.second),
                    line_range.second_length);
            }

            // Always ensure there is a newline.
            char last_char =
                line_range.second
                    ? line_range.second[line_range.second_length - 1]
                    : line_range.first[line_range.first_length - 1];
            if (last_char != '\n') {
              constexpr char suffix[1] = {'\n'};
              Write(suffix, 1);
            }

            rb.EndRead(std::move(line_range));
          } else {
            // Always ensure there is a newline.
            constexpr char suffix[1] = {'\n'};
            Write(suffix, 1);
          }

          if (line.terminate) {
            terminate = true;
            break;
          }
        }
      }

      if (terminate) {
        break;
      }

      if (read_count) {
        // Advance by the number of blocks we read.
        auto read_range = dp::sequence_range(next_sequence, read_count);
        next_sequence = read_range.end();
        last_sequence = read_range.last();
        consumed_.publish(last_sequence);

        desired_count = 1;

        if (cvars::flush_log) {
          FlushAllSinks();
        }

        idle_loops = 0;
      } else {
        if (idle_loops >= 1000) {
          // Introduce a waiting period.
          xe::threading::Sleep(std::chrono::milliseconds(50));
        } else {
          idle_loops++;
        }
      }
    }
  }

 public:
  void AppendLine(uint32_t thread_id, const char prefix_char,
                  const char* buffer_data, size_t buffer_length,
                  bool terminate = false) {
    size_t count = BlockCount(sizeof(LogLine) + buffer_length);

    auto range = claim_strategy_.claim(count);
    assert_true(range.size() == count);

    RingBuffer rb(buffer_, kBufferSize);
    rb.set_write_offset(BlockOffset(range.first()));
    rb.set_read_offset(BlockOffset(range.end()));

    LogLine line = {};
    line.buffer_length = buffer_length;
    line.thread_id = thread_id;
    line.frame_number = global_frame_number_.load(std::memory_order_relaxed);
    line.prefix_char = prefix_char;
    line.terminate = terminate;

    rb.Write(&line, sizeof(LogLine));
    if (buffer_length) {
      rb.Write(buffer_data, buffer_length);
    }

    claim_strategy_.publish(range);
  }
};

#if !XE_PLATFORM_ANDROID
// Opens the log for appending, so the xenia processes of one session can write
// to it at the same time without overwriting each other. A new session
// truncates it first.
FILE* OpenLogFile(const std::filesystem::path& path, bool truncate) {
  if (truncate) {
    if (FILE* file = xe::filesystem::OpenFile(path, "wb")) {
      fclose(file);
    }
  }
#if XE_PLATFORM_WIN32
  // With append-only access the system puts every write at the end, which the
  // C runtime's append mode doesn't do atomically across processes.
  HANDLE handle = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }
  int fd =
      _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_APPEND | _O_TEXT);
  if (fd == -1) {
    CloseHandle(handle);
    return nullptr;
  }
  FILE* file = _fdopen(fd, "at");
  if (!file) {
    // Closes the handle too.
    _close(fd);
  }
  return file;
#else
  return xe::filesystem::OpenFile(path, "at");
#endif
}
#endif  // !XE_PLATFORM_ANDROID

void InitializeLogging(const std::string_view app_name) {
  auto mem = memory::AlignedAlloc<Logger>(0x10);
  logger_ = new (mem) Logger(app_name);

#if XE_PLATFORM_ANDROID
  // TODO(Triang3l): Enable file logging, but not by default as logs may be
  // huge.
  if (cvars::log_to_logcat) {
    logger_->AddLogSink(std::make_unique<AndroidLogSink>(app_name));
  }
#else
  {
    std::filesystem::path log_path;
    if (cvars::log_file.empty()) {
      log_path = xe::filesystem::GetExecutableFolder() /
                 fmt::format("{}.log", app_name);
    } else {
      xe::filesystem::CreateParentFolder(cvars::log_file);
      log_path = cvars::log_file;
    }
    logger_->AddLogSink(std::make_unique<FileLogSink>(
        OpenLogFile(log_path, !cvars::log_append), true));
  }

  if (cvars::log_to_stdout || xe::IsUnattended()) {
    logger_->AddLogSink(std::make_unique<FileLogSink>(stdout, false));
  }
  // enable_console with stdout redirected: the console window gets its own
  // copy.
  if (FILE* console = console_echo_stream()) {
    logger_->AddLogSink(std::make_unique<FileLogSink>(console, false));
  }

  if (cvars::log_to_debugprint) {
    logger_->AddLogSink(std::make_unique<DebugPrintLogSink>());
  }
#endif  // XE_PLATFORM_ANDROID
}

void ShutdownLogging() {
  Logger* logger = logger_;
  logger_ = nullptr;

  logger->~Logger();
  memory::AlignedFree(logger);
}

void FlushLog() {
  if (!logger_) {
    return;
  }
  // Give the write thread a moment to process pending logs
  xe::threading::Sleep(std::chrono::milliseconds(10));
  // Force flush all sinks
  logger_->FlushAllSinks();
}

bool IsLogWriterThread() { return logger_ && logger_->IsWriteThread(); }

bool logging::ShouldLog(LogLevel log_level, uint32_t log_mask) {
  return static_cast<int32_t>(log_level) <= cvars::log_level &&
         (log_mask & cvars::log_mask) == 0;
}

uint32_t logging::GetFrameNumber() {
  return global_frame_number_.load(std::memory_order_relaxed);
}

void logging::IncrementFrameNumber() {
  global_frame_number_.fetch_add(1, std::memory_order_relaxed);
}

uint32_t logging::internal::GetLogLevel() { return cvars::log_level; }

std::pair<char*, size_t> logging::internal::GetThreadBuffer() {
  return {thread_log_buffer_, sizeof(thread_log_buffer_)};
}
XE_NOALIAS
void logging::internal::AppendLogLine(LogLevel log_level,
                                      const char prefix_char, size_t written) {
  if (!logger_ || !ShouldLog(log_level) || !written) {
    return;
  }
  logger_->AppendLine(xe::threading::current_thread_id(), prefix_char,
                      thread_log_buffer_, written);
}

void logging::AppendLogLine(LogLevel log_level, const char prefix_char,
                            const std::string_view str, uint32_t log_mask) {
  // Null once ShutdownLogging has begun, before a quick_exit.
  if (!logger_ || !ShouldLog(log_level, log_mask) || !str.size()) {
    return;
  }
  logger_->AppendLine(xe::threading::current_thread_id(), prefix_char,
                      str.data(), str.size());
}

void LogUIElement(const std::string_view kind, const std::string_view title,
                  const std::string_view text) {
  if (text.empty()) {
    XELOGI("UI {}: {}", kind, title);
  } else {
    XELOGI("UI {}: {}\n{}", kind, title, text);
  }
}

bool IsUnattended() { return cvars::unattended || cvars::headless; }

bool LogSimpleMessageBox(SimpleMessageBoxType type,
                         const std::string_view message) {
  std::string_view title;
  switch (type) {
    default:
    case SimpleMessageBoxType::Help:
      title = "Xenia Help";
      break;
    case SimpleMessageBoxType::Warning:
      title = "Xenia Warning";
      break;
    case SimpleMessageBoxType::Error:
      title = "Xenia Error";
      break;
  }
  LogUIElement("message box", title, message);
  return !xe::IsUnattended();
}

void ExitUnattended(int exit_code, const std::string_view reason) {
  XELOGI("Unattended: {}, exiting with code {}", reason, exit_code);
  // Waits for the writer to drain the queue, which FlushLog doesn't.
  if (logger_ && !IsLogWriterThread()) {
    ShutdownLogging();
  } else {
    FlushLog();
  }
  fflush(stdout);
  std::quick_exit(exit_code);
}

void FatalError(const std::string_view str) {
  logging::AppendLogLine(LogLevel::Error, 'x', str);

  if (xe::IsUnattended()) {
    ExitUnattended(EXIT_FAILURE, "fatal error");
  }

  if (!xe::has_console_attached()) {
    ShowSimpleMessageBox(SimpleMessageBoxType::Error, str);
  }

  ShutdownLogging();

#if XE_PLATFORM_ANDROID
  // Throw an error that can be reported to the developers via the store.
  std::abort();
#else
  // skip static destructors so they can't race with worker threads still
  // running and corrupt the heap, at_quick_exit handlers will take care
  // of necessary cleanup (e.g. /dev/shm/xenia* files on linux )
  std::quick_exit(EXIT_FAILURE);
#endif  // XE_PLATFORM_ANDROID
}

}  // namespace xe
