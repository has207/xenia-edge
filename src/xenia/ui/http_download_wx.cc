/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/http_download.h"

#include <wx/app.h>
#include <wx/stream.h>
#include <wx/webrequest.h>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_LINUX
#include <cstdlib>
#include <filesystem>
#include <string>

#include <curl/curl.h>
#endif

namespace xe {
namespace ui {

#if XE_PLATFORM_LINUX
// The bundled curl looks for root certificates only where the distribution it
// was built on keeps them.
static std::string FindRootCertificates() {
  static constexpr const char* kBundles[] = {
      "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch
      "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
      "/etc/ssl/ca-bundle.pem",              // openSUSE
  };
  std::error_code ec;
  for (const char* bundle : kBundles) {
    if (std::filesystem::exists(bundle, ec)) {
      return bundle;
    }
  }
  return {};
}

// Takes the variables curl honors first, which wx applies only to a session's
// first request.
static void SetRootCertificates(CURL* curl) {
  if (const char* dir = std::getenv("SSL_CERT_DIR")) {
    curl_easy_setopt(curl, CURLOPT_CAPATH, dir);
  }
  const char* file = std::getenv("CURL_CA_BUNDLE");
  if (!file) {
    file = std::getenv("SSL_CERT_FILE");
  }
  static const std::string found = FindRootCertificates();
  if (!file && !found.empty()) {
    file = found.c_str();
  }
  if (file) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, file);
  }
}
#endif

HttpDownloader::HttpDownloader() {
  // A session reads the wx app's name, which console tools like the trace
  // dumps don't have.
  if (!wxTheApp) {
    return;
  }
  session_ = std::make_unique<wxWebSessionSync>(wxWebSessionSync::New());
  if (session_->IsOpened()) {
    session_->AddCommonHeader("User-Agent", "xenia-edge");
  }
}

HttpDownloader::~HttpDownloader() = default;

bool HttpDownloader::DownloadToMemory(const std::string& url,
                                      std::vector<uint8_t>& out) {
  out.clear();
  if (!session_ || !session_->IsOpened()) {
    XELOGE("Download: no HTTP session");
    return false;
  }
  wxWebRequestSync request = session_->CreateRequest(wxString::FromUTF8(url));
  if (!request.IsOk()) {
    XELOGE("Download: failed to create the request");
    return false;
  }
#if XE_PLATFORM_LINUX
  SetRootCertificates(reinterpret_cast<CURL*>(request.GetNativeHandle()));
#endif
  // Under curl the data timeout caps the whole transfer, not each read.
  request.SetTimeouts(15000, 60000);
  const wxWebRequestSync::Result result = request.Execute();
  if (!result) {
    XELOGE("Download: {}", result.error.utf8_string());
    return false;
  }
  // Execute() succeeds on any status below 400.
  const wxWebResponse response = request.GetResponse();
  if (response.GetStatus() != 200) {
    XELOGE("Download: HTTP status {}", response.GetStatus());
    return false;
  }
  wxInputStream* stream = response.GetStream();
  const wxFileOffset length = stream ? stream->GetLength() : wxInvalidOffset;
  if (length == wxInvalidOffset) {
    XELOGE("Download: no response body");
    return false;
  }
  out.resize(size_t(length));
  if (!out.empty() && !stream->ReadAll(out.data(), out.size())) {
    XELOGE("Download: failed to read the response body");
    out.clear();
    return false;
  }
  return true;
}

}  // namespace ui
}  // namespace xe
