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
#include <memory>
#include <string>
#include <vector>

class wxWebSessionSync;

namespace xe {
namespace ui {

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
