/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XHTTP_HOST_H_
#define XENIA_KERNEL_XAM_XHTTP_HOST_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {
namespace xhttp {

// The host's HTTP client that XHttp requests go out through. Requests go
// straight to the server the title names, where the console would have
// checked the server's certificate through LIVE first.
//
// Every call blocks until it's done and returns 0 or an XHttp error code
// (12000 and up, the same values WinHTTP uses). Abort may be called from any
// thread to end a call in progress.

constexpr uint32_t kErrorTimeout = 12002;
constexpr uint32_t kErrorInternalError = 12004;
constexpr uint32_t kErrorNameNotResolved = 12007;
constexpr uint32_t kErrorOperationCancelled = 12017;
constexpr uint32_t kErrorCannotConnect = 12029;
constexpr uint32_t kErrorConnectionError = 12030;
constexpr uint32_t kErrorSecureFailure = 12175;

class HostSession;
class HostConnection;

struct SessionConfig {
  std::string user_agent;
  // Empty to use the host's own proxy settings.
  std::string proxy;
};

struct RequestConfig {
  std::string verb;
  std::string object_name;
  std::string version;
  std::string referrer;
  std::vector<std::string> accept_types;
};

struct RequestOptions {
  // 0 keeps the host default.
  uint32_t receive_timeout_ms = 0;
  // Redirects aren't followed unless a title asks for them.
  uint32_t max_redirects = 0;
  // 0 never, 1 not from https to http, 2 always, 3 not to another scheme.
  uint32_t redirect_policy = 3;
  bool keep_alive = true;
  bool ignore_certificate_errors = false;
};

class HostRequest {
 public:
  virtual ~HostRequest() = default;

  virtual uint32_t Send(const RequestOptions& options,
                        const std::string& headers,
                        std::span<const uint8_t> body,
                        uint32_t total_length) = 0;
  virtual uint32_t Write(std::span<const uint8_t> data) = 0;
  virtual uint32_t ReceiveResponse() = 0;
  // Reads up to |buffer|'s size. 0 bytes read means the response is over.
  virtual uint32_t Read(std::span<uint8_t> buffer, uint32_t* bytes_read) = 0;
  // The status line and every response header, each ending in CRLF, then a
  // blank line.
  virtual std::string response_headers() = 0;

  // Only valid once a 401 or 407 response has arrived.
  virtual uint32_t QueryAuthSchemes(uint32_t* supported, uint32_t* first,
                                    uint32_t* target) = 0;
  virtual uint32_t SetCredentials(uint32_t target, uint32_t scheme,
                                  const std::string& user_name,
                                  const std::string& password) = 0;

  virtual void Abort() = 0;
};

std::shared_ptr<HostSession> OpenSession(const SessionConfig& config,
                                         uint32_t* error);
std::shared_ptr<HostConnection> Connect(
    const std::shared_ptr<HostSession>& session, const std::string& server,
    uint16_t port, bool secure, uint32_t* error);
std::unique_ptr<HostRequest> OpenRequest(
    const std::shared_ptr<HostConnection>& connection,
    const RequestConfig& config, uint32_t* error);

}  // namespace xhttp
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XHTTP_HOST_H_
