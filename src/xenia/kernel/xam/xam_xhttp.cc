/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_xhttp.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xhttp_host.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

DEFINE_bool(xhttp_ignore_certificate_errors, false,
            "Lets titles' https requests through to servers whose certificate "
            "the host doesn't trust, such as an expired one.",
            "Network");

namespace xe {
namespace kernel {
namespace xam {

// XHttp is an always-asynchronous WinHTTP. Requests go to the server the
// title names through the host's HTTP client. Each request has a host thread
// that makes its calls in order. What they produce waits for the session's
// XHttpDoWork, which writes it to guest memory and runs the status callbacks
// on the title's thread. A closing handle's callback runs from
// XHttpCloseHandle.

// XHttpOpen requires the async flag. XHttpConnect's secure flag picks 443.
constexpr uint32_t kXHttpFlagAsync = 0x10000000;
constexpr uint32_t kXHttpFlagSecure = 0x00800000;
constexpr uint32_t kXHttpConnectFlags = 0x00C00003;
constexpr uint32_t kXHttpAccessTypeNamedProxy = 3;

// Status codes, which double as notification flag bits.
constexpr uint32_t kXHttpStatusHandleClosing = 0x00000800;
constexpr uint32_t kXHttpStatusRedirect = 0x00004000;
constexpr uint32_t kXHttpStatusHeadersAvailable = 0x00020000;
constexpr uint32_t kXHttpStatusReadComplete = 0x00080000;
constexpr uint32_t kXHttpStatusWriteComplete = 0x00100000;
constexpr uint32_t kXHttpStatusRequestError = 0x00200000;
constexpr uint32_t kXHttpStatusSendRequestComplete = 0x00400000;

// The API a request error reports as failing.
constexpr uint32_t kXHttpApiReceiveResponse = 1;
constexpr uint32_t kXHttpApiReadData = 3;
constexpr uint32_t kXHttpApiWriteData = 4;
constexpr uint32_t kXHttpApiSendRequest = 5;

constexpr uint32_t kXHttpOptionCallback = 1;
// Request options, readable only until the request is sent.
constexpr uint32_t kXHttpOptionReceiveTimeout = 6;
constexpr uint32_t kXHttpOptionDisableFeature = 63;
constexpr uint32_t kXHttpOptionRedirectPolicy = 88;
constexpr uint32_t kXHttpOptionMaxRedirects = 89;
constexpr uint32_t kXHttpOptionMaxContinues = 90;
// Whether the request's connection can carry another, read-only.
constexpr uint32_t kXHttpOptionConnectionReusable = 22;
// A value of the title's own on any handle.
constexpr uint32_t kXHttpOptionUserValue = 103;
// The one option a null handle takes, which does nothing.
constexpr uint32_t kXHttpOptionNothing = 104;

constexpr uint32_t kXHttpDisableKeepAlive = 0x00000008;
constexpr uint32_t kXHttpDisableFeatures = 0x00000018;
constexpr uint32_t kXHttpRedirectPolicyLast = 3;
// Setting a redirect policy allows this many redirects when none were.
constexpr uint32_t kXHttpDefaultMaxRedirects = 10;

constexpr uint32_t kXHttpQueryHost = 15;
constexpr uint32_t kXHttpQueryRawHeadersCrlf = 22;
constexpr uint32_t kXHttpQueryStatusCode = 65534;
constexpr uint32_t kXHttpQueryCustom = 65535;
constexpr uint32_t kXHttpQueryFlagRequestHeaders = 0x80000000;
constexpr uint32_t kXHttpQueryFlagSystemTime = 0x40000000;
constexpr uint32_t kXHttpQueryFlagNumber = 0x20000000;
constexpr uint32_t kXHttpQueryFlagFileTime = 0x10000000;
constexpr uint32_t kXHttpQueryFlagNumber64 = 0x08000000;

// The headers a query level names, by level.
constexpr const char* kXHttpQueryHeaderNames[] = {
    "Accept",
    "Accept-Charset",
    "Accept-Encoding",
    "Accept-Language",
    "Accept-Ranges",
    "Allow",
    "Cache-Control",
    "Connection",
    "Content-Language",
    "Content-Length",
    "Content-Transfer-Encoding",
    "Content-Type",
    "Date",
    "Expires",
    "Ext",
    "Host",
    "If-Match",
    "If-Modified-Since",
    "If-None-Match",
    "If-Range",
    "If-Unmodified-Since",
    "Last-Modified",
    nullptr,  // Every header.
    "Man",
    "MIME-Version",
    "MX",
    "NT",
    "NTS",
    "Range",
    "Referer",
    "Server",
    "SEQ",
    "SID",
    "ST",
    "Timeout",
    "Transfer-Encoding",
    "Unless-Modified-Since",
    "User-Agent",
    "USN",
    "X-Delay",
    "X-DelayFlags",
    "X-Err",
    "Content-Range",
};

// The smallest buffer a title can open a request in.
constexpr uint32_t kXHttpRequestMemoryMin = 0x5D0;
// Basic credentials are "user:password" with its terminator.
constexpr size_t kXHttpBasicCredentialsMax = 0x100;

enum class XHttpType : uint32_t { kRequest = 1, kConnect = 2, kSession = 3 };

// Each handle is a small guest block whose address the title holds. +0 holds
// the type, +4 the handle itself (what a closing notification points at), +8
// the result a request error points at and +0x10 the count a write completion
// points at.
constexpr uint32_t kXHttpBlockSize = 0x20;

// Runs a request's host calls one after another on a thread of its own so
// the blocking host client never holds up the title. The thread only lives
// while there's work, waiting a moment for the next call before it ends.
class XHttpWorker : public std::enable_shared_from_this<XHttpWorker> {
 public:
  using Job = std::function<void(xhttp::HostRequest&)>;

  explicit XHttpWorker(std::unique_ptr<xhttp::HostRequest> request)
      : request_(std::move(request)) {}

  void Post(Job job) {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    jobs_.push_back(std::move(job));
    if (running_) {
      cv_.notify_one();
      return;
    }
    running_ = true;
    std::thread([self = shared_from_this()] { self->Run(); }).detach();
  }

  // Ends the call in progress and drops the rest.
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      jobs_.clear();
    }
    request_->Abort();
    cv_.notify_one();
  }

 private:
  void Run() {
    std::unique_lock lock(mutex_);
    while (cv_.wait_for(lock, std::chrono::seconds(1), [this] {
      return stopping_ || !jobs_.empty();
    }) && !stopping_) {
      Job job = std::move(jobs_.front());
      jobs_.pop_front();
      lock.unlock();
      job(*request_);
      lock.lock();
    }
    running_ = false;
  }

  std::unique_ptr<xhttp::HostRequest> request_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool running_ = false;
  bool stopping_ = false;
};

struct XHttpHandle {
  XHttpType type;
  // Tells this handle from a later one given the same block and a request's
  // send from the one before it.
  uint64_t serial = 0;
  // 0 once the parent has closed. A handle stays open when its parent closes.
  uint32_t parent = 0;
  // 0 once the session has closed, which leaves nothing to deliver to.
  uint32_t session = 0;
  uint32_t callback = 0;
  uint32_t notify_flags = 0;
  uint32_t user_value = 0;

  std::shared_ptr<xhttp::HostSession> host_session;

  // A connect handle's server and its one open request. A request keeps its
  // connect's server too.
  std::shared_ptr<xhttp::HostConnection> host_connection;
  std::string server;
  uint16_t port = 0;
  bool secure = false;
  uint32_t open_request = 0;
  // Connect options with no use here, kept as set.
  std::unordered_map<uint32_t, uint32_t> connect_options;

  uint32_t receive_timeout = 180000;
  uint32_t disabled_features = 0;
  uint32_t redirect_policy = kXHttpRedirectPolicyLast;
  uint32_t max_redirects = 0;
  uint32_t max_continues = 10;
  xhttp::Credentials credentials;

  std::shared_ptr<XHttpWorker> worker;
  // Its connect has closed and taken the connection with it.
  bool orphaned = false;
  std::string verb;
  std::string object_name;
  uint32_t context = 0;
  std::string request_headers;
  std::string response_headers;
  // How far the last send has got. It sends, completes, can be written to
  // until the body is all there, receives a response and is read one read at
  // a time.
  bool sent = false;
  bool send_complete = false;
  bool receiving = false;
  bool headers_available = false;
  bool read_pending = false;
  bool write_pending = false;
  uint32_t upload_remaining = 0;
};

struct XHttpNotification {
  uint32_t handle;
  uint64_t serial;
  uint32_t status;
  uint32_t info = 0;
  uint32_t info_length = 0;
  // Written to guest memory just before the callback runs.
  uint32_t api = 0;
  uint32_t error = 0;
  // Bytes sent or written.
  uint32_t bytes = 0;
  std::vector<uint8_t> data;
  // Response headers or where a redirect went.
  std::string text;
  // Only for a closing handle, which is gone by the time it's delivered.
  uint32_t callback = 0;
  uint32_t context = 0;
};

struct XHttpState {
  ~XHttpState() {
    for (auto& [handle, entry] : handles) {
      if (entry.worker) {
        entry.worker->Stop();
      }
    }
  }

  std::mutex mutex;
  std::unordered_map<uint32_t, XHttpHandle> handles;
  // What each session's next XHttpDoWork delivers.
  std::unordered_map<uint32_t, std::deque<XHttpNotification>> pending;
  uint64_t next_serial = 1;
  // What passes through XHttp: the title's headers and data, the response
  // headers and body.
  uint64_t bytes_received = 0;
  uint64_t bytes_sent = 0;
};

std::shared_ptr<XHttpState> CreateXHttpState() {
  return std::make_shared<XHttpState>();
}

static std::shared_ptr<XHttpState> GetXHttpState() {
  return kernel_state()->GetKernelModule<XamModule>("xam.xex")->xhttp_state();
}

static std::string GuestString(const char* text, size_t max_length = 0x1000) {
  return text ? std::string(text, strnlen(text, max_length)) : std::string();
}

static bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
         });
}

static bool StartsWithIgnoreCase(std::string_view text,
                                 std::string_view prefix) {
  return text.size() >= prefix.size() &&
         EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

// Looks up a handle of |type|. Sets the error for a missing one or another
// type.
static XHttpHandle* FindXHttpHandle(XHttpState& state, uint32_t handle,
                                    XHttpType type) {
  auto it = state.handles.find(handle);
  if (it == state.handles.end() || it->second.type != type) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    return nullptr;
  }
  return &it->second;
}

// Creates a handle under |parent|, whose callback it inherits.
static uint32_t CreateXHttpHandle(XHttpState& state, XHttpType type,
                                  uint32_t parent) {
  const uint32_t block = kernel_memory()->SystemHeapAlloc(kXHttpBlockSize);
  if (!block) {
    XThread::SetLastError(X_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  uint8_t* data = kernel_memory()->TranslateVirtual<uint8_t*>(block);
  std::memset(data, 0, kXHttpBlockSize);
  xe::store_and_swap<uint32_t>(data + 0, uint32_t(type));
  xe::store_and_swap<uint32_t>(data + 4, block);
  XHttpHandle handle;
  handle.type = type;
  handle.serial = state.next_serial++;
  handle.parent = parent;
  handle.session = block;
  if (parent) {
    const XHttpHandle& owner = state.handles.at(parent);
    handle.session = owner.session;
    handle.callback = owner.callback;
    handle.notify_flags = owner.notify_flags;
  }
  state.handles.emplace(block, std::move(handle));
  return block;
}

static XHttpNotification MakeXHttpNotification(uint32_t handle, uint64_t serial,
                                               uint32_t status) {
  XHttpNotification notification;
  notification.handle = handle;
  notification.serial = serial;
  notification.status = status;
  return notification;
}

// Closes |handle| and returns its closing notification if it has a callback
// for one. What's under it stays open: a request whose connect closes loses
// its connection and nothing under a closed session is delivered any more.
static std::optional<XHttpNotification> CloseXHttpHandle(XHttpState& state,
                                                         uint32_t handle) {
  auto it = state.handles.find(handle);
  XHttpHandle entry = std::move(it->second);
  state.handles.erase(it);
  if (entry.worker) {
    entry.worker->Stop();
  }
  for (auto& [child, child_entry] : state.handles) {
    if (child_entry.parent == handle) {
      child_entry.parent = 0;
      if (child_entry.type == XHttpType::kRequest) {
        child_entry.orphaned = true;
        child_entry.worker->Stop();
      }
    }
    if (entry.type == XHttpType::kSession && child_entry.session == handle) {
      child_entry.session = 0;
    }
  }
  if (entry.type == XHttpType::kSession) {
    state.pending.erase(handle);
  } else if (auto queue = state.pending.find(entry.session);
             queue != state.pending.end()) {
    std::erase_if(queue->second, [&](const XHttpNotification& notification) {
      return notification.handle == handle;
    });
  }
  if (entry.type == XHttpType::kRequest) {
    if (auto connect = state.handles.find(entry.parent);
        connect != state.handles.end() &&
        connect->second.open_request == handle) {
      connect->second.open_request = 0;
    }
  }
  if (!entry.callback || !(entry.notify_flags & kXHttpStatusHandleClosing)) {
    kernel_memory()->SystemHeapFree(handle);
    return std::nullopt;
  }
  XHttpNotification notification =
      MakeXHttpNotification(handle, entry.serial, kXHttpStatusHandleClosing);
  notification.info = handle + 4;
  notification.info_length = 4;
  notification.callback = entry.callback;
  // Only a request has a context, from its send.
  notification.context = entry.context;
  return notification;
}

// From a request's host thread: queues |notification| for its session's next
// XHttpDoWork, unless the request has been closed or sent again since.
static void QueueXHttpNotification(const std::weak_ptr<XHttpState>& weak_state,
                                   XHttpNotification notification) {
  auto state = weak_state.lock();
  if (!state) {
    return;
  }
  std::lock_guard lock(state->mutex);
  auto it = state->handles.find(notification.handle);
  if (it == state->handles.end() || it->second.serial != notification.serial ||
      !it->second.session) {
    return;
  }
  state->pending[it->second.session].push_back(std::move(notification));
}

static void QueueXHttpError(const std::weak_ptr<XHttpState>& weak_state,
                            uint32_t request, uint64_t serial, uint32_t api,
                            uint32_t error) {
  XHttpNotification notification =
      MakeXHttpNotification(request, serial, kXHttpStatusRequestError);
  notification.info = request + 8;
  notification.info_length = 8;
  notification.api = api;
  notification.error = error;
  QueueXHttpNotification(weak_state, std::move(notification));
}

// Moves each request along with what its notifications carry, writes that to
// guest memory and calls the title's status callbacks, without the lock held
// since they may call back in. A closing handle's block is freed once its
// callback returns.
static void RunXHttpCallbacks(XHttpState& state,
                              std::vector<XHttpNotification>& notifications) {
  auto thread = XThread::GetCurrentThread();
  for (auto& notification : notifications) {
    const bool closing = notification.status == kXHttpStatusHandleClosing;
    uint32_t callback = notification.callback;
    uint32_t context = notification.context;
    uint32_t info = notification.info;
    uint32_t info_length = notification.info_length;
    // A redirect's URL, in guest memory while the callback runs.
    uint32_t url = 0;
    if (!closing) {
      std::lock_guard lock(state.mutex);
      // An earlier callback may have closed this handle.
      auto it = state.handles.find(notification.handle);
      if (it == state.handles.end() ||
          it->second.serial != notification.serial) {
        continue;
      }
      XHttpHandle& entry = it->second;
      uint8_t* block =
          kernel_memory()->TranslateVirtual<uint8_t*>(notification.handle);
      switch (notification.status) {
        case kXHttpStatusSendRequestComplete:
          entry.send_complete = true;
          state.bytes_sent += notification.bytes;
          break;
        case kXHttpStatusHeadersAvailable:
          state.bytes_received += notification.text.size();
          entry.response_headers = std::move(notification.text);
          entry.headers_available = true;
          break;
        case kXHttpStatusReadComplete:
          entry.read_pending = false;
          state.bytes_received += notification.data.size();
          if (notification.data.empty()) {
            // The end of the response.
            info = 0;
          } else {
            std::memcpy(kernel_memory()->TranslateVirtual<uint8_t*>(info),
                        notification.data.data(), notification.data.size());
          }
          break;
        case kXHttpStatusWriteComplete:
          entry.write_pending = false;
          state.bytes_sent += notification.bytes;
          xe::store_and_swap<uint32_t>(block + 0x10, notification.bytes);
          break;
        case kXHttpStatusRequestError:
          if (notification.api == kXHttpApiReadData) {
            entry.read_pending = false;
          } else if (notification.api == kXHttpApiWriteData) {
            entry.write_pending = false;
          }
          xe::store_and_swap<uint32_t>(block + 8, notification.api);
          xe::store_and_swap<uint32_t>(block + 12, notification.error);
          break;
      }
      if (!entry.callback || !(entry.notify_flags & notification.status)) {
        continue;
      }
      callback = entry.callback;
      context = entry.context;
      if (notification.status == kXHttpStatusRedirect) {
        url = kernel_memory()->SystemHeapAlloc(
            uint32_t(notification.text.size() + 1));
        if (!url) {
          continue;
        }
        std::memcpy(kernel_memory()->TranslateVirtual<char*>(url),
                    notification.text.c_str(), notification.text.size() + 1);
        info = url;
        info_length = uint32_t(notification.text.size());
      }
    }
    if (thread && callback) {
      uint64_t args[] = {notification.handle, context, notification.status,
                         info, info_length};
      kernel_state()->processor()->Execute(thread->thread_state(), callback,
                                           args, std::size(args));
    }
    if (url) {
      kernel_memory()->SystemHeapFree(url);
    }
    if (closing) {
      kernel_memory()->SystemHeapFree(notification.handle);
    }
  }
}

static xhttp::RequestOptions GetXHttpRequestOptions(const XHttpHandle& entry) {
  xhttp::RequestOptions options;
  options.receive_timeout_ms = entry.receive_timeout;
  options.max_redirects = entry.max_redirects;
  options.redirect_policy = entry.redirect_policy;
  options.keep_alive = !(entry.disabled_features & kXHttpDisableKeepAlive);
  options.ignore_certificate_errors = cvars::xhttp_ignore_certificate_errors;
  options.credentials = entry.credentials;
  return options;
}

dword_result_t NetDll_XHttpOpen_entry(dword_t caller, lpstring_t user_agent,
                                      dword_t access_type,
                                      lpstring_t proxy_name,
                                      lpstring_t proxy_bypass, dword_t flags) {
  if ((access_type != 0 && access_type != 1 && access_type != 3) ||
      proxy_bypass || (flags & ~kXHttpFlagAsync) ||
      !(flags & kXHttpFlagAsync)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  xhttp::SessionConfig config;
  config.user_agent = GuestString(user_agent);
  if (access_type == kXHttpAccessTypeNamedProxy) {
    config.proxy = GuestString(proxy_name);
  }
  uint32_t error = 0;
  auto host_session = xhttp::OpenSession(config, &error);
  if (!host_session) {
    XThread::SetLastError(error);
    return 0;
  }
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  const uint32_t session = CreateXHttpHandle(*state, XHttpType::kSession, 0);
  if (session) {
    state->handles.at(session).host_session = std::move(host_session);
    XThread::SetLastError(X_ERROR_SUCCESS);
  }
  return session;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpen, kNetworking, kImplemented);

dword_result_t NetDll_XHttpConnect_entry(dword_t caller, dword_t session,
                                         lpvoid_t server_name_ptr, word_t port,
                                         dword_t flags) {
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* owner = FindXHttpHandle(*state, session, XHttpType::kSession);
  if (!owner) {
    return 0;
  }
  const char* server_name = server_name_ptr.as<const char*>();
  const size_t length = server_name ? strnlen(server_name, 0x100) : 0;
  if (!length || length >= 0x100 || (flags & ~kXHttpConnectFlags)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  const bool secure = (flags & kXHttpFlagSecure) != 0;
  const std::string server(server_name, length);
  const uint16_t server_port =
      port.value() ? port.value() : uint16_t(secure ? 443 : 80);
  uint32_t error = 0;
  auto host_connection =
      xhttp::Connect(owner->host_session, server, server_port, secure, &error);
  if (!host_connection) {
    XThread::SetLastError(error);
    return 0;
  }
  const uint32_t connect =
      CreateXHttpHandle(*state, XHttpType::kConnect, session);
  if (!connect) {
    return 0;
  }
  XHttpHandle& entry = state->handles.at(connect);
  entry.host_connection = std::move(host_connection);
  entry.server = server;
  entry.port = server_port;
  entry.secure = secure;
  XThread::SetLastError(X_ERROR_SUCCESS);
  return connect;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpConnect, kNetworking, kImplemented);

// A connection carries one request at a time. A title's own request buffer is
// only checked for size since nothing is kept in it.
static uint32_t OpenXHttpRequest(uint32_t connect, const char* verb,
                                 const char* object_name, const char* version,
                                 const char* referrer, uint32_t accept_types,
                                 uint32_t memory, uint32_t memory_size) {
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* owner = FindXHttpHandle(*state, connect, XHttpType::kConnect);
  if (!owner) {
    return 0;
  }
  if (owner->open_request) {
    XThread::SetLastError(X_ERROR_ALREADY_EXISTS);
    return 0;
  }
  if (!object_name || (!memory && memory_size) ||
      (memory && memory_size < kXHttpRequestMemoryMin)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (*object_name != '/') {
    XThread::SetLastError(X_ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    return 0;
  }

  xhttp::RequestConfig config;
  config.verb = verb && *verb ? GuestString(verb) : "GET";
  config.object_name = GuestString(object_name, 0x10000);
  config.version = GuestString(version);
  config.referrer = GuestString(referrer);
  // A null-terminated array of type strings.
  for (uint32_t i = 0; accept_types && i < 64; ++i) {
    const uint32_t type = xe::load_and_swap<uint32_t>(
        kernel_memory()->TranslateVirtual(accept_types + i * 4));
    if (!type) {
      break;
    }
    config.accept_types.push_back(
        GuestString(kernel_memory()->TranslateVirtual<const char*>(type)));
  }
  uint32_t error = 0;
  auto host_request =
      xhttp::OpenRequest(owner->host_connection, config, &error);
  if (!host_request) {
    XThread::SetLastError(error);
    return 0;
  }

  const uint32_t request =
      CreateXHttpHandle(*state, XHttpType::kRequest, connect);
  if (!request) {
    return 0;
  }
  XHttpHandle& owner_entry = state->handles.at(connect);
  XHttpHandle& entry = state->handles.at(request);
  entry.worker = std::make_shared<XHttpWorker>(std::move(host_request));
  entry.server = owner_entry.server;
  entry.port = owner_entry.port;
  entry.secure = owner_entry.secure;
  entry.verb = config.verb;
  entry.object_name = config.object_name;
  owner_entry.open_request = request;
  XThread::SetLastError(X_ERROR_SUCCESS);
  return request;
}

dword_result_t NetDll_XHttpOpenRequest_entry(
    dword_t caller, dword_t connect, lpstring_t verb, lpstring_t object_name,
    lpstring_t version, lpstring_t referrer, lpvoid_t accept_types,
    dword_t flags) {
  return OpenXHttpRequest(connect, verb, object_name, version, referrer,
                          accept_types.guest_address(), 0, 0);
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequest, kNetworking, kImplemented);

dword_result_t NetDll_XHttpOpenRequestUsingMemory_entry(
    dword_t caller, dword_t connect, lpstring_t verb, lpstring_t object_name,
    lpstring_t version, lpstring_t referrer, lpvoid_t accept_types,
    lpvoid_t memory, dword_t memory_size, dword_t flags) {
  return OpenXHttpRequest(connect, verb, object_name, version, referrer,
                          accept_types.guest_address(), memory.guest_address(),
                          memory_size);
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpenRequestUsingMemory, kNetworking,
                    kImplemented);

dword_result_t NetDll_XHttpCloseHandle_entry(dword_t caller, dword_t handle) {
  auto state = GetXHttpState();
  std::vector<XHttpNotification> closing;
  {
    std::lock_guard lock(state->mutex);
    if (!state->handles.contains(handle)) {
      XThread::SetLastError(X_ERROR_INVALID_HANDLE);
      return 0;
    }
    if (auto notification = CloseXHttpHandle(*state, handle)) {
      closing.push_back(std::move(*notification));
    }
  }
  RunXHttpCallbacks(*state, closing);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCloseHandle, kNetworking, kImplemented);

// Returns the callback it replaces. Handles opened later under this one start
// with the new callback.
dword_result_t NetDll_XHttpSetStatusCallback_entry(dword_t caller,
                                                   dword_t handle,
                                                   dword_t callback,
                                                   dword_t notify_flags,
                                                   dword_t reserved) {
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  auto it = state->handles.find(handle);
  if (it == state->handles.end()) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  const uint32_t previous = it->second.callback;
  it->second.callback = callback;
  it->second.notify_flags = notify_flags;
  XThread::SetLastError(X_ERROR_SUCCESS);
  return previous;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetStatusCallback, kNetworking, kImplemented);

// Delivers the session's pending notifications. With none, a nonzero wait
// gives up a little time so a title polling in a loop doesn't spin.
dword_result_t NetDll_XHttpDoWork_entry(dword_t caller, dword_t session,
                                        dword_t max_wait_ms) {
  auto state = GetXHttpState();
  std::vector<XHttpNotification> notifications;
  {
    std::lock_guard lock(state->mutex);
    if (!FindXHttpHandle(*state, session, XHttpType::kSession)) {
      return 0;
    }
    if (auto it = state->pending.find(session); it != state->pending.end()) {
      notifications.assign(std::make_move_iterator(it->second.begin()),
                           std::make_move_iterator(it->second.end()));
      state->pending.erase(it);
    }
  }
  if (notifications.empty() && max_wait_ms) {
    if (auto thread = XThread::GetCurrentThread()) {
      const uint32_t wait_ms = std::min<uint32_t>(max_wait_ms, 10);
      thread->Delay(0, 0, uint64_t(-int64_t(wait_ms) * 10000));
    }
  }
  RunXHttpCallbacks(*state, notifications);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpDoWork, kNetworking, kImplemented);

// Sending again starts the request over, which is how a title answers an
// authentication challenge. What the earlier send had on its way is dropped.
dword_result_t NetDll_XHttpSendRequest_entry(
    dword_t caller, dword_t request, lpvoid_t headers, dword_t headers_length,
    lpvoid_t optional, dword_t optional_length, dword_t total_length,
    dword_t context) {
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* entry = FindXHttpHandle(*state, request, XHttpType::kRequest);
  if (!entry) {
    return 0;
  }
  if ((!total_length && (optional || optional_length)) ||
      (optional_length && !optional)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (entry->orphaned) {
    XThread::SetLastError(X_ERROR_WINHTTP_CONNECTION_ERROR);
    return 0;
  }
  std::string header_text;
  if (headers) {
    const char* text = headers.as<const char*>();
    header_text = headers_length == uint32_t(-1)
                      ? GuestString(text, 0x10000)
                      : std::string(text, strnlen(text, headers_length));
  }
  std::vector<uint8_t> body;
  if (optional && optional_length) {
    const uint8_t* data = optional.as<const uint8_t*>();
    body.assign(data, data + optional_length);
  }
  entry->serial = state->next_serial++;
  entry->sent = true;
  entry->send_complete = false;
  entry->receiving = false;
  entry->headers_available = false;
  entry->read_pending = false;
  entry->write_pending = false;
  entry->upload_remaining =
      total_length > optional_length ? total_length - optional_length : 0;
  entry->context = context;
  entry->request_headers = header_text;
  entry->response_headers.clear();

  XELOGI("XHttp: {} {}://{}:{}{}", entry->verb,
         entry->secure ? "https" : "http", entry->server, entry->port,
         entry->object_name);
  const xhttp::RequestOptions options = GetXHttpRequestOptions(*entry);
  const std::weak_ptr<XHttpState> weak_state = state;
  const uint64_t serial = entry->serial;
  const uint32_t request_handle = request;
  const uint32_t total =
      std::max<uint32_t>(total_length, uint32_t(body.size()));
  const uint32_t sent_bytes = uint32_t(header_text.size() + body.size());
  entry->worker->Post([=, body = std::move(body)](xhttp::HostRequest& host) {
    const uint32_t error = host.Send(options, header_text, body, total);
    if (error) {
      QueueXHttpError(weak_state, request_handle, serial, kXHttpApiSendRequest,
                      error);
      return;
    }
    XHttpNotification notification = MakeXHttpNotification(
        request_handle, serial, kXHttpStatusSendRequestComplete);
    notification.bytes = sent_bytes;
    QueueXHttpNotification(weak_state, std::move(notification));
  });
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSendRequest, kNetworking, kImplemented);

// The rest of the body SendRequest left out, one write at a time once the send
// has completed.
dword_result_t NetDll_XHttpWriteData_entry(dword_t caller, dword_t request,
                                           lpvoid_t buffer, dword_t length,
                                           lpdword_t bytes_written_ptr) {
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* entry = FindXHttpHandle(*state, request, XHttpType::kRequest);
  if (!entry) {
    return 0;
  }
  if (!entry->send_complete || !entry->upload_remaining || entry->receiving ||
      entry->headers_available || entry->write_pending) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    return 0;
  }
  if (length > entry->upload_remaining) {
    XThread::SetLastError(X_ERROR_BUFFER_OVERFLOW);
    return 0;
  }
  if (!buffer || !length) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (entry->orphaned) {
    XThread::SetLastError(X_ERROR_WINHTTP_CONNECTION_ERROR);
    return 0;
  }
  entry->upload_remaining -= length;
  entry->write_pending = true;
  const uint8_t* data = buffer.as<const uint8_t*>();
  std::vector<uint8_t> copy(data, data + length);
  const std::weak_ptr<XHttpState> weak_state = state;
  const uint64_t serial = entry->serial;
  const uint32_t request_handle = request;
  entry->worker->Post([=, copy = std::move(copy)](xhttp::HostRequest& host) {
    const uint32_t error = host.Write(copy);
    if (error) {
      QueueXHttpError(weak_state, request_handle, serial, kXHttpApiWriteData,
                      error);
      return;
    }
    XHttpNotification notification = MakeXHttpNotification(
        request_handle, serial, kXHttpStatusWriteComplete);
    notification.info = request_handle + 0x10;
    notification.info_length = 4;
    notification.bytes = uint32_t(copy.size());
    QueueXHttpNotification(weak_state, std::move(notification));
  });
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpWriteData, kNetworking, kImplemented);

// Each redirect followed on the way is reported before the headers.
dword_result_t NetDll_XHttpReceiveResponse_entry(dword_t caller,
                                                 dword_t request,
                                                 lpvoid_t reserved) {
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* entry = FindXHttpHandle(*state, request, XHttpType::kRequest);
  if (!entry) {
    return 0;
  }
  if (!entry->send_complete || entry->receiving || entry->headers_available) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    return 0;
  }
  if (entry->orphaned) {
    XThread::SetLastError(X_ERROR_WINHTTP_CONNECTION_ERROR);
    return 0;
  }
  entry->receiving = true;
  const std::weak_ptr<XHttpState> weak_state = state;
  const uint64_t serial = entry->serial;
  const uint32_t request_handle = request;
  entry->worker->Post([=](xhttp::HostRequest& host) {
    const uint32_t error = host.ReceiveResponse();
    for (std::string& url : host.TakeRedirects()) {
      XELOGI("XHttp: redirected to {}", url);
      XHttpNotification notification =
          MakeXHttpNotification(request_handle, serial, kXHttpStatusRedirect);
      notification.text = std::move(url);
      QueueXHttpNotification(weak_state, std::move(notification));
    }
    if (error) {
      QueueXHttpError(weak_state, request_handle, serial,
                      kXHttpApiReceiveResponse, error);
      return;
    }
    XHttpNotification notification = MakeXHttpNotification(
        request_handle, serial, kXHttpStatusHeadersAvailable);
    notification.text = host.response_headers();
    XELOGI("XHttp: {}", std::string_view(notification.text)
                            .substr(0, notification.text.find('\r')));
    QueueXHttpNotification(weak_state, std::move(notification));
  });
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReceiveResponse, kNetworking, kImplemented);

// The count read arrives with the read-complete notification so the last
// argument goes unused. The data lands in the title's buffer when that
// notification is delivered.
dword_result_t NetDll_XHttpReadData_entry(dword_t caller, dword_t request,
                                          lpvoid_t buffer, dword_t length,
                                          lpdword_t bytes_read_ptr) {
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* entry = FindXHttpHandle(*state, request, XHttpType::kRequest);
  if (!entry) {
    return 0;
  }
  if (!buffer && length) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  if (entry->upload_remaining || !entry->headers_available ||
      entry->read_pending) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    return 0;
  }
  if (entry->orphaned) {
    XThread::SetLastError(X_ERROR_WINHTTP_CONNECTION_ERROR);
    return 0;
  }
  entry->read_pending = true;
  const std::weak_ptr<XHttpState> weak_state = state;
  const uint64_t serial = entry->serial;
  const uint32_t request_handle = request;
  const uint32_t read_length = length;
  const uint32_t guest_buffer = buffer.guest_address();
  entry->worker->Post([=](xhttp::HostRequest& host) {
    std::vector<uint8_t> data(read_length);
    uint32_t bytes_read = 0;
    const uint32_t error = host.Read(data, &bytes_read);
    if (error) {
      QueueXHttpError(weak_state, request_handle, serial, kXHttpApiReadData,
                      error);
      return;
    }
    data.resize(bytes_read);
    XHttpNotification notification =
        MakeXHttpNotification(request_handle, serial, kXHttpStatusReadComplete);
    notification.info = guest_buffer;
    notification.info_length = bytes_read;
    notification.data = std::move(data);
    QueueXHttpNotification(weak_state, std::move(notification));
  });
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpReadData, kNetworking, kImplemented);

// Finds the |index|th header called |name|. Response headers start with the
// status line.
static std::optional<std::string_view> FindXHttpHeader(std::string_view text,
                                                       std::string_view name,
                                                       bool has_status_line,
                                                       uint32_t index) {
  bool first = true;
  while (!text.empty()) {
    const size_t end = text.find("\r\n");
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view()
                                         : text.substr(end + 2);
    if (std::exchange(first, false) && has_status_line) {
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    std::string_view key = line.substr(0, colon);
    while (!key.empty() && key.back() == ' ') {
      key.remove_suffix(1);
    }
    if (!EqualsIgnoreCase(key, name)) {
      continue;
    }
    if (index--) {
      continue;
    }
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    return value;
  }
  return std::nullopt;
}

// The status code of the status line that starts |headers|, such as
// "HTTP/1.1 200 OK".
static uint32_t XHttpStatusCode(std::string_view headers) {
  const std::string_view status_line = headers.substr(0, headers.find("\r\n"));
  const size_t space = status_line.find(' ');
  if (space == std::string_view::npos) {
    return 0;
  }
  return uint32_t(std::strtoul(
      std::string(status_line.substr(space + 1, 3)).c_str(), nullptr, 10));
}

// A 401 response's first WWW-Authenticate header.
static std::optional<std::string_view> FindXHttpChallenge(
    const XHttpHandle& entry) {
  if (!entry.headers_available ||
      XHttpStatusCode(entry.response_headers) != 401) {
    return std::nullopt;
  }
  return FindXHttpHeader(entry.response_headers, "WWW-Authenticate", true, 0);
}

// Parses an HTTP date such as "Sun, 06 Nov 1994 08:49:37 GMT" into the guest's
// 16 byte SYSTEMTIME.
static bool ParseXHttpDate(std::string_view text, uint8_t* out) {
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed",
                                "Thu", "Fri", "Sat"};
  char day_name[4] = {}, month_name[4] = {};
  int day = 0, year = 0, hour = 0, minute = 0, second = 0;
  const std::string value(text);
  if (std::sscanf(value.c_str(), "%3s, %d %3s %d %d:%d:%d", day_name, &day,
                  month_name, &year, &hour, &minute, &second) != 7) {
    return false;
  }
  int month = 0, weekday = 0;
  while (month < 12 && std::strcmp(kMonths[month], month_name)) {
    ++month;
  }
  while (weekday < 7 && std::strncmp(kDays[weekday], day_name, 3)) {
    ++weekday;
  }
  if (month == 12 || weekday == 7) {
    return false;
  }
  const uint16_t fields[] = {
      uint16_t(year), uint16_t(month + 1), uint16_t(weekday), uint16_t(day),
      uint16_t(hour), uint16_t(minute),    uint16_t(second),  0};
  for (size_t i = 0; i < std::size(fields); ++i) {
    xe::store_and_swap<uint16_t>(out + i * 2, fields[i]);
  }
  return true;
}

// A header the table names has one value, its first. Other headers are picked
// by index, which is never written back. Of the request's own headers only the
// server they go to can be read by level.
dword_result_t NetDll_XHttpQueryHeaders_entry(dword_t caller, dword_t request,
                                              dword_t info_level,
                                              lpstring_t name, lpvoid_t buffer,
                                              lpdword_t buffer_length_ptr,
                                              lpdword_t index_ptr) {
  const uint32_t level = info_level & 0xFFFF;
  const uint32_t modifiers = info_level & 0xFFFF0000;
  const uint32_t format =
      modifiers & (kXHttpQueryFlagSystemTime | kXHttpQueryFlagNumber |
                   kXHttpQueryFlagFileTime | kXHttpQueryFlagNumber64);
  if ((level == kXHttpQueryCustom) != static_cast<bool>(name) ||
      std::popcount(format) > 1 || !buffer_length_ptr ||
      (*buffer_length_ptr && !buffer)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* entry = FindXHttpHandle(*state, request, XHttpType::kRequest);
  if (!entry) {
    return 0;
  }
  const uint32_t index = index_ptr ? uint32_t(*index_ptr) : 0;
  const std::string_view header_name =
      name ? std::string_view(name) : std::string_view();
  uint8_t* out = buffer.as<uint8_t*>();

  std::optional<std::string_view> value;
  bool raw = false;
  if ((modifiers & kXHttpQueryFlagRequestHeaders) &&
      level != kXHttpQueryRawHeadersCrlf) {
    if (level == kXHttpQueryHost ||
        (level == kXHttpQueryCustom && EqualsIgnoreCase(header_name, "Host"))) {
      value = entry->server;
    } else if (level == kXHttpQueryCustom) {
      value =
          FindXHttpHeader(entry->request_headers, header_name, false, index);
    } else {
      XThread::SetLastError(X_ERROR_WINHTTP_INVALID_OPTION);
      return 0;
    }
  } else {
    if (!entry->headers_available) {
      XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
      return 0;
    }
    const std::string_view text = entry->response_headers;
    if (level == kXHttpQueryRawHeadersCrlf) {
      value = text;
      raw = true;
    } else if (level == kXHttpQueryStatusCode) {
      // Only ever a 32-bit number.
      if (format != kXHttpQueryFlagNumber) {
        XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
        return 0;
      }
      if (*buffer_length_ptr < 4) {
        *buffer_length_ptr = 4;
        XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
        return 0;
      }
      xe::store_and_swap<uint32_t>(out, XHttpStatusCode(text));
      *buffer_length_ptr = 4;
      XThread::SetLastError(X_ERROR_SUCCESS);
      return 1;
    } else if (level == kXHttpQueryCustom) {
      value = FindXHttpHeader(text, header_name, true, index);
    } else if (level < std::size(kXHttpQueryHeaderNames)) {
      value = FindXHttpHeader(text, kXHttpQueryHeaderNames[level], true, 0);
    } else {
      XThread::SetLastError(X_ERROR_WINHTTP_INVALID_OPTION);
      return 0;
    }
  }
  if (!value) {
    XThread::SetLastError(X_ERROR_WINHTTP_HEADER_NOT_FOUND);
    return 0;
  }

  if (format == kXHttpQueryFlagNumber) {
    // A number fills the caller's buffer as a 64-bit value when it can hold
    // one; npctrl keeps a response's remaining length in 64 bits.
    if (*buffer_length_ptr < 4) {
      *buffer_length_ptr = 4;
      XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    const uint64_t number =
        std::strtoull(std::string(*value).c_str(), nullptr, 10);
    if (*buffer_length_ptr >= 8) {
      xe::store_and_swap<uint64_t>(out, number);
      *buffer_length_ptr = 8;
    } else {
      xe::store_and_swap<uint32_t>(out, uint32_t(number));
      *buffer_length_ptr = 4;
    }
  } else if (format == kXHttpQueryFlagSystemTime) {
    if (*buffer_length_ptr < 16) {
      *buffer_length_ptr = 16;
      XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    if (!ParseXHttpDate(*value, out)) {
      XThread::SetLastError(X_ERROR_WINHTTP_HEADER_NOT_FOUND);
      return 0;
    }
    *buffer_length_ptr = 16;
  } else if (format == kXHttpQueryFlagFileTime) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  } else {
    // Text, whose length counts the terminator except for the raw headers.
    const uint32_t needed = uint32_t(value->size()) + 1;
    if (*buffer_length_ptr < needed) {
      *buffer_length_ptr = needed;
      XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
      return 0;
    }
    std::memcpy(out, value->data(), value->size());
    out[value->size()] = 0;
    *buffer_length_ptr = raw ? needed - 1 : needed;
  }
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryHeaders, kNetworking, kImplemented);

// What the server asked for in its 401 response: Basic or Digest, or a scheme
// that succeeds without saying which.
dword_result_t NetDll_XHttpQueryAuthSchemes_entry(
    dword_t caller, dword_t request, lpdword_t supported_schemes_ptr,
    lpdword_t first_scheme_ptr, lpdword_t auth_target_ptr) {
  if (!supported_schemes_ptr || !first_scheme_ptr || !auth_target_ptr) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* entry = FindXHttpHandle(*state, request, XHttpType::kRequest);
  if (!entry) {
    return 0;
  }
  const auto challenge = FindXHttpChallenge(*entry);
  if (!challenge) {
    XThread::SetLastError(X_ERROR_WINHTTP_INTERNAL_ERROR);
    return 0;
  }
  if (StartsWithIgnoreCase(*challenge, "Basic")) {
    *supported_schemes_ptr = 1;
    *first_scheme_ptr = 1;
  } else if (StartsWithIgnoreCase(*challenge, "Digest ")) {
    *supported_schemes_ptr = 8;
    *first_scheme_ptr = 8;
  }
  // Always the server.
  *auth_target_ptr = 0;
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryAuthSchemes, kNetworking, kImplemented);

// Scheme 1 is Basic and any other Digest, which answers the 401 challenge the
// request got. The credentials go out with the next send. Redirects are
// turned off either way.
dword_result_t NetDll_XHttpSetCredentials_entry(
    dword_t caller, dword_t request, dword_t auth_targets, dword_t auth_scheme,
    lpstring_t user_name, lpstring_t password, lpvoid_t auth_params) {
  if (!user_name) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  XHttpHandle* entry = FindXHttpHandle(*state, request, XHttpType::kRequest);
  if (!entry) {
    return 0;
  }
  xhttp::Credentials credentials;
  credentials.scheme = auth_scheme == 1 ? 1 : 8;
  credentials.user_name = GuestString(user_name);
  credentials.password = GuestString(password);
  const bool usable =
      auth_scheme == 1
          ? credentials.user_name.size() + credentials.password.size() + 2 <=
                kXHttpBasicCredentialsMax
          : FindXHttpChallenge(*entry).has_value();
  entry->max_redirects = 0;
  if (!usable) {
    XThread::SetLastError(X_ERROR_WINHTTP_INTERNAL_ERROR);
    return 0;
  }
  entry->credentials = std::move(credentials);
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetCredentials, kNetworking, kImplemented);

// The callback and the title's own value can be read on any handle, the rest
// on one type each. Every value is 4 bytes.
dword_result_t NetDll_XHttpQueryOption_entry(dword_t caller, dword_t handle,
                                             dword_t option, lpvoid_t buffer,
                                             lpdword_t buffer_length_ptr) {
  if (!buffer_length_ptr || (*buffer_length_ptr && !buffer)) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  std::optional<XHttpType> type;
  bool before_send = false;
  switch (option) {
    case kXHttpOptionCallback:
    case kXHttpOptionUserValue:
      break;
    case kXHttpOptionReceiveTimeout:
    case kXHttpOptionDisableFeature:
    case kXHttpOptionRedirectPolicy:
    case kXHttpOptionMaxRedirects:
    case kXHttpOptionMaxContinues:
      type = XHttpType::kRequest;
      before_send = true;
      break;
    // Request flags that are never set here.
    case kXHttpOptionConnectionReusable:
    case 23:
    case 106:
      type = XHttpType::kRequest;
      break;
    // Connect options. 100 needs a connection made, which isn't shown here.
    case 100:
    case 101:
      type = XHttpType::kConnect;
      break;
    default:
      XThread::SetLastError(X_ERROR_WINHTTP_INVALID_OPTION);
      return 0;
  }
  if (!handle) {
    XThread::SetLastError(X_ERROR_INVALID_HANDLE);
    return 0;
  }
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  auto it = state->handles.find(handle);
  if (it == state->handles.end() || (type && it->second.type != *type)) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  const XHttpHandle& entry = it->second;
  if ((before_send && entry.sent) || option == 100) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_STATE);
    return 0;
  }
  if (*buffer_length_ptr < 4) {
    *buffer_length_ptr = 4;
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  uint32_t value = 0;
  switch (option) {
    case kXHttpOptionCallback:
      value = entry.callback;
      break;
    case kXHttpOptionUserValue:
      value = entry.user_value;
      break;
    case kXHttpOptionReceiveTimeout:
      value = entry.receive_timeout;
      break;
    case kXHttpOptionDisableFeature:
      value = entry.disabled_features;
      break;
    case kXHttpOptionRedirectPolicy:
      value = entry.max_redirects ? entry.redirect_policy : 0;
      break;
    case kXHttpOptionMaxRedirects:
      value = entry.max_redirects;
      break;
    case kXHttpOptionMaxContinues:
      value = entry.max_continues;
      break;
    case 101:
      if (auto stored = entry.connect_options.find(option);
          stored != entry.connect_options.end()) {
        value = stored->second;
      }
      break;
  }
  xe::store_and_swap<uint32_t>(buffer.as<uint8_t*>(), value);
  *buffer_length_ptr = 4;
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpQueryOption, kNetworking, kImplemented);

// Request options take effect when the request is sent.
dword_result_t NetDll_XHttpSetOption_entry(dword_t caller, dword_t handle,
                                           dword_t option, lpvoid_t buffer,
                                           dword_t buffer_length) {
  std::optional<XHttpType> type;
  bool sized = true;
  switch (option) {
    case kXHttpOptionNothing:
      XThread::SetLastError(X_ERROR_SUCCESS);
      return 1;
    case 100:
      XThread::SetLastError(X_ERROR_WINHTTP_OPTION_NOT_SETTABLE);
      return 0;
    case kXHttpOptionCallback:
    case kXHttpOptionUserValue:
      break;
    case kXHttpOptionReceiveTimeout:
    case kXHttpOptionDisableFeature:
    case kXHttpOptionRedirectPolicy:
    case kXHttpOptionMaxRedirects:
    case kXHttpOptionMaxContinues:
      type = XHttpType::kRequest;
      break;
    // Connect and session settings with no use here.
    case 12:
    case 101:
      type = XHttpType::kConnect;
      break;
    case 110:
      type = XHttpType::kSession;
      break;
    case 105:
    case 108:
    case 113:
    case 114:
      type = XHttpType::kSession;
      sized = false;
      break;
    default:
      XThread::SetLastError(X_ERROR_WINHTTP_INVALID_OPTION);
      return 0;
  }
  if (!handle) {
    XThread::SetLastError(X_ERROR_INVALID_HANDLE);
    return 0;
  }
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  auto it = state->handles.find(handle);
  if (it == state->handles.end() || (type && it->second.type != *type)) {
    XThread::SetLastError(X_ERROR_WINHTTP_INCORRECT_HANDLE_TYPE);
    return 0;
  }
  XHttpHandle& entry = it->second;
  if (option == 114) {
    // Only for a console in data center mode.
    XThread::SetLastError(X_ERROR_WINHTTP_INTERNAL_ERROR);
    return 0;
  }
  if (!sized) {
    XThread::SetLastError(X_ERROR_SUCCESS);
    return 1;
  }
  if (buffer_length != 4 || !buffer) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  const uint32_t value = xe::load_and_swap<uint32_t>(buffer);
  switch (option) {
    case kXHttpOptionCallback:
      entry.callback = value;
      entry.notify_flags = 0xFFFFFFFF;
      break;
    case kXHttpOptionUserValue:
      entry.user_value = value;
      break;
    case kXHttpOptionReceiveTimeout:
      entry.receive_timeout = value;
      break;
    case kXHttpOptionDisableFeature:
      if (value & ~kXHttpDisableFeatures) {
        XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
        return 0;
      }
      entry.disabled_features |= value;
      break;
    case kXHttpOptionRedirectPolicy:
      // No policy goes with no redirects and a policy allows some.
      if (value > kXHttpRedirectPolicyLast || (!value && entry.max_redirects)) {
        XThread::SetLastError(X_ERROR_WINHTTP_INVALID_OPTION);
        return 0;
      }
      if (value) {
        entry.redirect_policy = value;
        if (!entry.max_redirects) {
          entry.max_redirects = kXHttpDefaultMaxRedirects;
        }
      }
      break;
    case kXHttpOptionMaxRedirects:
      entry.max_redirects = value;
      break;
    case kXHttpOptionMaxContinues:
      entry.max_continues = value;
      break;
    case 12:
    case 101:
      entry.connect_options[option] = value;
      break;
  }
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpSetOption, kNetworking, kImplemented);

// Leaves the last error as it was when it succeeds.
dword_result_t NetDll_XHttpGetPerfCounters_entry(
    dword_t caller, pointer_t<X_PERFORMANCE_COUNTERS> counters) {
  if (!counters) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  auto state = GetXHttpState();
  std::lock_guard lock(state->mutex);
  uint32_t requests = 0, connections = 0, sessions = 0;
  for (const auto& [handle, entry] : state->handles) {
    switch (entry.type) {
      case XHttpType::kRequest:
        ++requests;
        break;
      case XHttpType::kConnect:
        ++connections;
        break;
      case XHttpType::kSession:
        ++sessions;
        break;
    }
  }
  counters->requests = requests;
  counters->connections = connections;
  counters->sessions = sessions;
  counters->bytes_received = state->bytes_received;
  counters->bytes_sent = state->bytes_sent;
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpGetPerfCounters, kNetworking, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(XHttp);
