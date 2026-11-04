/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_HTTP_DOWNLOAD_H_
#define XENIA_UI_HTTP_DOWNLOAD_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxWebSessionSync;

namespace xe {
namespace ui {

enum class DownloadResult {
  kSuccess,
  kCancelled,
  kFailed,
  // The file couldn't be created or written.
  kWriteFailed,
};

// Downloads |url| into |path| on the UI thread, which keeps handling events
// meanwhile. |path| is removed unless the server answers 200 and the whole
// file arrives. |progress| gets the bytes received out of the size, 0 when the
// server doesn't give one, and cancels by returning false.
DownloadResult DownloadFile(
    const std::string& url, const std::filesystem::path& path,
    const std::function<bool(uint64_t received, uint64_t size)>& progress);

// Blocking HTTP downloads through wxWebRequest, for one thread at a time.
// Under curl, create and destroy every downloader on the same thread.
class HttpDownloader {
 public:
  HttpDownloader();
  ~HttpDownloader();

  // GETs |url| into |out|, which is left empty unless the server answers 200.
  bool DownloadToMemory(const std::string& url, std::vector<uint8_t>& out);

 private:
  std::unique_ptr<wxWebSessionSync> session_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_HTTP_DOWNLOAD_H_
