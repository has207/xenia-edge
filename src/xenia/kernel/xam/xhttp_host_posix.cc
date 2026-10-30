/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xhttp_host.h"

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xhttp {

// There is no host HTTP client here yet so requests fail to connect.

class HostSession {};

class HostConnection {};

class UnconnectedRequest : public HostRequest {
 public:
  uint32_t Send(const RequestOptions& options, const std::string& headers,
                std::span<const uint8_t> body, uint32_t total_length) override {
    XELOGW("XHttp: no host HTTP client on this platform");
    return X_ERROR_WINHTTP_CANNOT_CONNECT;
  }
  uint32_t Write(std::span<const uint8_t> data) override {
    return X_ERROR_WINHTTP_CANNOT_CONNECT;
  }
  uint32_t ReceiveResponse() override { return X_ERROR_WINHTTP_CANNOT_CONNECT; }
  uint32_t Read(std::span<uint8_t> buffer, uint32_t* bytes_read) override {
    *bytes_read = 0;
    return X_ERROR_WINHTTP_CANNOT_CONNECT;
  }
  std::string response_headers() override { return {}; }
  std::vector<std::string> TakeRedirects() override { return {}; }
  void Abort() override {}
};

std::shared_ptr<HostSession> OpenSession(const SessionConfig& config,
                                         uint32_t* error) {
  *error = 0;
  return std::make_shared<HostSession>();
}

std::shared_ptr<HostConnection> Connect(
    const std::shared_ptr<HostSession>& session, const std::string& server,
    uint16_t port, bool secure, uint32_t* error) {
  *error = 0;
  return std::make_shared<HostConnection>();
}

std::unique_ptr<HostRequest> OpenRequest(
    const std::shared_ptr<HostConnection>& connection,
    const RequestConfig& config, uint32_t* error) {
  *error = 0;
  return std::make_unique<UnconnectedRequest>();
}

}  // namespace xhttp
}  // namespace xam
}  // namespace kernel
}  // namespace xe
