/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xhttp_host.h"

#include <mutex>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

#include <winhttp.h>  // NOLINT(build/include_order)

namespace xe {
namespace kernel {
namespace xam {
namespace xhttp {

// Synchronous WinHTTP. XHttp is close to a subset of it, and its error codes
// are WinHTTP's, so they pass through.

static uint32_t LastError() {
  const DWORD error = GetLastError();
  if (error >= 12000 && error < 12200) {
    return error;
  }
  if (error == ERROR_INVALID_HANDLE) {
    // The request was aborted while a call was using it.
    return kErrorOperationCancelled;
  }
  return kErrorInternalError;
}

static std::wstring Widen(const std::string& text) {
  const std::u16string wide = xe::to_utf16(text);
  return std::wstring(wide.begin(), wide.end());
}

class HostSession {
 public:
  explicit HostSession(HINTERNET handle) : handle_(handle) {}
  ~HostSession() { WinHttpCloseHandle(handle_); }
  HINTERNET handle() const { return handle_; }

 private:
  HINTERNET handle_;
};

class HostConnection {
 public:
  HostConnection(std::shared_ptr<HostSession> session, HINTERNET handle,
                 bool secure)
      : session_(std::move(session)), handle_(handle), secure_(secure) {}
  ~HostConnection() { WinHttpCloseHandle(handle_); }
  HINTERNET handle() const { return handle_; }
  bool secure() const { return secure_; }

 private:
  std::shared_ptr<HostSession> session_;
  HINTERNET handle_;
  bool secure_;
};

class WinHttpRequest : public HostRequest {
 public:
  WinHttpRequest(std::shared_ptr<HostConnection> connection, HINTERNET handle)
      : connection_(std::move(connection)), handle_(handle) {}
  ~WinHttpRequest() override { Abort(); }

  uint32_t Send(const RequestOptions& options, const std::string& headers,
                std::span<const uint8_t> body, uint32_t total_length) override {
    HINTERNET request = handle();
    if (!request) {
      return kErrorOperationCancelled;
    }
    ApplyOptions(request, options);
    const std::wstring wide_headers = Widen(headers);
    if (!WinHttpSendRequest(request,
                            wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                 : wide_headers.c_str(),
                            wide_headers.empty() ? 0 : DWORD(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA
                                         : const_cast<uint8_t*>(body.data()),
                            DWORD(body.size()), total_length, 0)) {
      return LastError();
    }
    return 0;
  }

  uint32_t Write(std::span<const uint8_t> data) override {
    HINTERNET request = handle();
    DWORD written = 0;
    if (!request ||
        !WinHttpWriteData(request, data.data(), DWORD(data.size()), &written)) {
      return request ? LastError() : kErrorOperationCancelled;
    }
    return 0;
  }

  uint32_t ReceiveResponse() override {
    HINTERNET request = handle();
    if (!request || !WinHttpReceiveResponse(request, nullptr)) {
      return request ? LastError() : kErrorOperationCancelled;
    }
    return 0;
  }

  uint32_t Read(std::span<uint8_t> buffer, uint32_t* bytes_read) override {
    HINTERNET request = handle();
    DWORD read = 0;
    *bytes_read = 0;
    if (!request ||
        !WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &read)) {
      return request ? LastError() : kErrorOperationCancelled;
    }
    *bytes_read = read;
    return 0;
  }

  std::string response_headers() override {
    HINTERNET request = handle();
    if (!request) {
      return {};
    }
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size) {
      return {};
    }
    std::u16string text(size / sizeof(char16_t), u'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                             WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size,
                             WINHTTP_NO_HEADER_INDEX)) {
      return {};
    }
    text.resize(size / sizeof(char16_t));
    return xe::to_utf8(text);
  }

  uint32_t QueryAuthSchemes(uint32_t* supported, uint32_t* first,
                            uint32_t* target) override {
    HINTERNET request = handle();
    DWORD supported_schemes = 0, first_scheme = 0, auth_target = 0;
    if (!request || !WinHttpQueryAuthSchemes(request, &supported_schemes,
                                             &first_scheme, &auth_target)) {
      return request ? LastError() : kErrorOperationCancelled;
    }
    *supported = supported_schemes;
    *first = first_scheme;
    *target = auth_target;
    return 0;
  }

  uint32_t SetCredentials(uint32_t target, uint32_t scheme,
                          const std::string& user_name,
                          const std::string& password) override {
    HINTERNET request = handle();
    const std::wstring wide_user = Widen(user_name);
    const std::wstring wide_password = Widen(password);
    if (!request ||
        !WinHttpSetCredentials(request, target, scheme, wide_user.c_str(),
                               wide_password.c_str(), nullptr)) {
      return request ? LastError() : kErrorOperationCancelled;
    }
    return 0;
  }

  void Abort() override {
    std::lock_guard lock(mutex_);
    if (handle_) {
      // Ends a call blocked on the handle in another thread.
      WinHttpCloseHandle(handle_);
      handle_ = nullptr;
    }
  }

 private:
  HINTERNET handle() {
    std::lock_guard lock(mutex_);
    return handle_;
  }

  void ApplyOptions(HINTERNET request, const RequestOptions& options) {
    DWORD redirect_policy;
    switch (options.redirect_policy) {
      case 0:
        redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        break;
      case 2:
        redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        break;
      default:
        // WinHTTP has no policy against every change of scheme, so stop the
        // one that loses security.
        redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        break;
    }
    if (!options.max_redirects) {
      redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    }
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
                     sizeof(redirect_policy));
    if (options.max_redirects) {
      DWORD max_redirects = options.max_redirects;
      WinHttpSetOption(request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                       &max_redirects, sizeof(max_redirects));
    }
    // Titles keep their own cookies.
    DWORD disable = WINHTTP_DISABLE_COOKIES;
    if (!options.keep_alive) {
      disable |= WINHTTP_DISABLE_KEEP_ALIVE;
    }
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable,
                     sizeof(disable));
    if (options.receive_timeout_ms) {
      DWORD timeout = options.receive_timeout_ms;
      WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout,
                       sizeof(timeout));
    }
    if (options.ignore_certificate_errors && connection_->secure()) {
      DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                    SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
      WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags,
                       sizeof(flags));
    }
  }

  std::shared_ptr<HostConnection> connection_;
  std::mutex mutex_;
  HINTERNET handle_;
};

std::shared_ptr<HostSession> OpenSession(const SessionConfig& config,
                                         uint32_t* error) {
  const std::wstring user_agent = Widen(config.user_agent);
  HINTERNET handle;
  if (config.proxy.empty()) {
    handle =
        WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!handle) {
      // Older hosts only know the registry's proxy.
      handle =
          WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
  } else {
    const std::wstring proxy = Widen(config.proxy);
    handle = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                         proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!handle) {
    *error = LastError();
    return nullptr;
  }
  // Servers today won't talk to the TLS 1.0 the console speaks.
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  if (!WinHttpSetOption(handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                        sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                     sizeof(protocols));
  }
  *error = 0;
  return std::make_shared<HostSession>(handle);
}

std::shared_ptr<HostConnection> Connect(
    const std::shared_ptr<HostSession>& session, const std::string& server,
    uint16_t port, bool secure, uint32_t* error) {
  const std::wstring wide_server = Widen(server);
  HINTERNET handle =
      WinHttpConnect(session->handle(), wide_server.c_str(), port, 0);
  if (!handle) {
    *error = LastError();
    return nullptr;
  }
  *error = 0;
  return std::make_shared<HostConnection>(session, handle, secure);
}

std::unique_ptr<HostRequest> OpenRequest(
    const std::shared_ptr<HostConnection>& connection,
    const RequestConfig& config, uint32_t* error) {
  const std::wstring verb = Widen(config.verb.empty() ? "GET" : config.verb);
  const std::wstring object_name = Widen(config.object_name);
  const std::wstring version = Widen(config.version);
  const std::wstring referrer = Widen(config.referrer);
  std::vector<std::wstring> accept_types;
  for (const std::string& type : config.accept_types) {
    accept_types.push_back(Widen(type));
  }
  std::vector<LPCWSTR> accept_type_ptrs;
  for (const std::wstring& type : accept_types) {
    accept_type_ptrs.push_back(type.c_str());
  }
  accept_type_ptrs.push_back(nullptr);

  // The title already escaped its object name, so it goes out as given.
  DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY;
  if (connection->secure()) {
    flags |= WINHTTP_FLAG_SECURE;
  }
  HINTERNET handle = WinHttpOpenRequest(
      connection->handle(), verb.c_str(), object_name.c_str(),
      version.empty() ? nullptr : version.c_str(),
      referrer.empty() ? WINHTTP_NO_REFERER : referrer.c_str(),
      accept_types.empty() ? WINHTTP_DEFAULT_ACCEPT_TYPES
                           : accept_type_ptrs.data(),
      flags);
  if (!handle) {
    *error = LastError();
    return nullptr;
  }
  *error = 0;
  return std::make_unique<WinHttpRequest>(connection, handle);
}

}  // namespace xhttp
}  // namespace xam
}  // namespace kernel
}  // namespace xe
