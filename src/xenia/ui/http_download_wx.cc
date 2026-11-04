/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/http_download.h"

#include <chrono>
#include <cstdio>

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/stream.h>
#include <wx/utils.h>
#include <wx/webrequest.h>

#include "xenia/base/filesystem.h"
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

// Sets up a request's curl handle. Downloads run on several threads at once and
// signals are process-wide, so curl mustn't use them for SIGPIPE or DNS
// timeouts.
static void ConfigureCurl(CURL* curl) {
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  SetRootCertificates(curl);
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
  ConfigureCurl(reinterpret_cast<CURL*>(request.GetNativeHandle()));
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

DownloadResult DownloadFile(
    const std::string& url, const std::filesystem::path& path,
    const std::function<bool(uint64_t received, uint64_t size)>& progress) {
  wxEventLoopBase* loop = wxEventLoopBase::GetActive();
  if (!wxTheApp || !loop) {
    XELOGE("Download: no event loop");
    return DownloadResult::kFailed;
  }
  wxWebSession session = wxWebSession::New();
  if (!session.IsOpened()) {
    XELOGE("Download: no HTTP session");
    return DownloadResult::kFailed;
  }
  session.AddCommonHeader("User-Agent", "xenia-edge");
  // Declared before the request so it outlives the events sent to it.
  wxEvtHandler handler;
  wxWebRequest request =
      session.CreateRequest(&handler, wxString::FromUTF8(url));
  if (!request.IsOk()) {
    XELOGE("Download: failed to create the request");
    return DownloadResult::kFailed;
  }
#if XE_PLATFORM_LINUX
  ConfigureCurl(reinterpret_cast<CURL*>(request.GetNativeHandle()));
#endif
  // Only connecting is timed: under curl a data timeout caps the whole
  // transfer.
  request.SetTimeouts(15000, wxWebRequest::Timeout_Default);
  request.SetStorage(wxWebRequest::Storage_None);

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    XELOGE("Download: failed to create {}",
           xe::strip_pii(xe::path_to_utf8(path)));
    return DownloadResult::kWriteFailed;
  }
  uint64_t received = 0;
  uint64_t size = 0;
  bool write_failed = false;
  handler.Bind(wxEVT_WEBREQUEST_DATA, [&](wxWebRequestEvent& event) {
    if (write_failed) {
      return;
    }
    if (std::fwrite(event.GetDataBuffer(), 1, event.GetDataSize(), file) !=
        event.GetDataSize()) {
      XELOGE("Download: failed to write {}",
             xe::strip_pii(xe::path_to_utf8(path)));
      write_failed = true;
      return;
    }
    received += event.GetDataSize();
    const wxFileOffset length = event.GetResponse().GetContentLength();
    size = length > 0 ? uint64_t(length) : 0;
  });
  wxWebRequest::State state = wxWebRequest::State_Idle;
  handler.Bind(wxEVT_WEBREQUEST_STATE, [&](wxWebRequestEvent& event) {
    state = event.GetState();
    if (state == wxWebRequest::State_Failed) {
      XELOGE("Download: {}", event.GetErrorDescription().utf8_string());
    } else if (state == wxWebRequest::State_Unauthorized) {
      XELOGE("Download: the server asks for credentials");
    }
  });
  // An authentication request ends it too, since nothing gives credentials.
  const auto ended = [&state]() {
    return state == wxWebRequest::State_Completed ||
           state == wxWebRequest::State_Failed ||
           state == wxWebRequest::State_Cancelled ||
           state == wxWebRequest::State_Unauthorized;
  };
  request.Start();

  // Ends a transfer that stops sending, which curl doesn't time out by itself.
  constexpr auto kStallTimeout = std::chrono::seconds(30);
  auto last_data_time = std::chrono::steady_clock::now();
  uint64_t last_received = 0;
  // Set once the request is to stop, after which it still ends with a state
  // event.
  bool stopping = false;
  bool cancelled = false;
  while (!ended()) {
    if (!stopping) {
      const auto now = std::chrono::steady_clock::now();
      if (received != last_received) {
        last_received = received;
        last_data_time = now;
      }
      if (write_failed) {
        stopping = true;
      } else if (now - last_data_time > kStallTimeout) {
        XELOGE("Download: no data for {} seconds", kStallTimeout.count());
        stopping = true;
      } else if (!progress(received, size)) {
        cancelled = true;
        stopping = true;
      }
      if (stopping) {
        // progress() can let curl finish the transfer, and cancelling a
        // request whose end is still queued releases it twice.
        wxTheApp->ProcessPendingEvents();
        if (!ended()) {
          request.Cancel();
        }
      }
    }
    // Runs native events, such as curl's sockets. The request's own events are
    // wx pending events, which a yield that leaves out user input skips.
    loop->YieldFor(wxEVT_CATEGORY_ALL & ~wxEVT_CATEGORY_USER_INPUT);
    wxTheApp->ProcessPendingEvents();
    wxMilliSleep(20);
  }
  if (std::fclose(file) != 0 && !write_failed) {
    XELOGE("Download: failed to write {}",
           xe::strip_pii(xe::path_to_utf8(path)));
    write_failed = true;
  }

  DownloadResult result = DownloadResult::kFailed;
  if (cancelled) {
    result = DownloadResult::kCancelled;
  } else if (write_failed) {
    result = DownloadResult::kWriteFailed;
  } else if (state == wxWebRequest::State_Completed) {
    const int status = request.GetResponse().GetStatus();
    if (status == 200) {
      result = DownloadResult::kSuccess;
    } else {
      XELOGE("Download: HTTP status {}", status);
    }
  }
  if (result != DownloadResult::kSuccess) {
    std::filesystem::remove(path, ec);
  }
  return result;
}

}  // namespace ui
}  // namespace xe
