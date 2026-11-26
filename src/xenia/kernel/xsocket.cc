/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsocket.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/net/xnet_tcp.h"

namespace xe {
namespace kernel {

namespace {

// Drives async_wait completions on a worker thread for the process lifetime.
class IoContextRunner {
 public:
  IoContextRunner() : work_(asio::make_work_guard(io_context_)) {
    thread_ = std::thread([this]() {
      xe::threading::set_name("Xenia Socket I/O");
      io_context_.run();
    });
  }
  ~IoContextRunner() {
    work_.reset();
    io_context_.stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  asio::io_context& get() { return io_context_; }

 private:
  asio::io_context io_context_;
  asio::executor_work_guard<asio::io_context::executor_type> work_;
  std::thread thread_;
};

}  // namespace

// Shared io_context for all sockets
static asio::io_context& GetIoContext() {
  static IoContextRunner runner;
  return runner.get();
}

namespace {

// Spreads one datagram over the guest's buffers in order.
void Scatter(std::span<const uint8_t> data,
             std::vector<std::span<uint8_t>>& buffers) {
  for (auto& buffer : buffers) {
    const size_t size = std::min(buffer.size(), data.size());
    std::memcpy(buffer.data(), data.data(), size);
    data = data.subspan(size);
  }
}

// MSG_PARTIAL, what the flags of a truncated datagram hold.
constexpr uint32_t kMsgPartial = 0x8000;

// The byte count and flags land first since a title may poll internal. A
// failure is stored as its WSA error in 0x8007xxxx form.
void CompleteOverlapped(XWSAOVERLAPPED* overlapped, uint32_t error,
                        uint32_t bytes, uint32_t flags) {
  overlapped->internal_high = bytes;
  overlapped->offset.low = flags;
  std::atomic_thread_fence(std::memory_order_release);
  overlapped->internal = X_HRESULT_FROM_WIN32(error);
}

}  // namespace

struct XSocket::OverlappedReceives {
  struct Receive {
    std::vector<std::span<uint8_t>> buffers;
    XSOCKADDR_IN* from;
    xe::be<uint32_t>* from_len;
    XWSAOVERLAPPED* overlapped;
    object_ref<XEvent> event;
  };

  explicit OverlappedReceives(net::SystemLinkPort* port) : port(port) {}

  // mutex held.
  bool Pending(const XWSAOVERLAPPED* overlapped) const {
    return std::ranges::any_of(pending, [overlapped](const Receive& receive) {
      return receive.overlapped == overlapped;
    });
  }

  // Fills the pending receives in order while datagrams are queued.
  void Complete() {
    std::vector<object_ref<XEvent>> completed;
    std::unique_lock lock(mutex);
    while (port && !pending.empty()) {
      Receive& receive = pending.front();
      size_t size = 0;
      for (const auto& buffer : receive.buffers) {
        size += buffer.size();
      }
      std::vector<uint8_t> data(size);
      net::Endpoint source;
      uint32_t error = 0;
      const int received = port->Receive(data, false, source, 0, error);
      if (received < 0 && error == net::kWsaWouldBlock) {
        break;
      }
      // A datagram too large for the buffers still fills them.
      const bool truncated = received < 0 && error == net::kWsaMessageSize;
      const bool filled = received >= 0 || truncated;
      const size_t bytes = received >= 0 ? size_t(received) : filled ? size : 0;
      if (filled) {
        Scatter({data.data(), bytes}, receive.buffers);
        if (receive.from) {
          receive.from->sin_family = AF_INET;
          receive.from->sin_addr = source.address;
          receive.from->sin_port = source.port;
          std::memset(receive.from->x_sin_zero, 0,
                      sizeof(receive.from->x_sin_zero));
        }
        if (receive.from_len) {
          *receive.from_len = sizeof(XSOCKADDR_IN);
        }
      }
      CompleteOverlapped(receive.overlapped, received >= 0 ? 0 : error,
                         static_cast<uint32_t>(bytes),
                         truncated ? kMsgPartial : 0);
      if (receive.event) {
        completed.push_back(std::move(receive.event));
      }
      pending.pop_front();
    }
    lock.unlock();
    // Waking a waiter takes scheduler locks, which must not nest in this one.
    for (auto& event : completed) {
      event->Set(0, false);
    }
  }

  // Completes the pending receives with |error| and wakes their waiters. With
  // |error| 0 the waiters are only woken.
  void Drop(uint32_t error) {
    std::deque<Receive> dropped;
    {
      std::lock_guard lock(mutex);
      dropped.swap(pending);
      // Under the lock so a receive never reads as pending once it has left
      // the queue.
      if (error) {
        for (auto& receive : dropped) {
          CompleteOverlapped(receive.overlapped, error, 0, 0);
        }
      }
    }
    for (auto& receive : dropped) {
      if (receive.event) {
        receive.event->Set(0, false);
      }
    }
  }

  // closesocket completes the pending receives as canceled since 4D5307EA
  // polls internal rather than waiting on the event.
  void Close(bool cancel) {
    {
      std::lock_guard lock(mutex);
      port = nullptr;
    }
    Drop(cancel ? uint32_t(X_WSAError::X_WSAECANCELLED) : 0);
  }

  std::mutex mutex;
  // Null once the socket closes.
  net::SystemLinkPort* port;
  std::deque<Receive> pending;
};

// Translate socket options to native
// Note:
// SO_DONTLINGER = ~SO_LINGER
// SO_EXCLUSIVEADDRUSE = ~SO_REUSEADDR
// TODO: Check SO_DONTLINGER and SO_EXCLUSIVEADDRUSE usage on linux
const std::map<uint32_t, int> supported_socket_options = {
    {0x0004, SO_REUSEADDR}, {0x0020, SO_BROADCAST}, {0x0080, SO_LINGER},
    {0x1001, SO_SNDBUF},    {0x1002, SO_RCVBUF},    {0x1005, SO_SNDTIMEO},
    {0x1006, SO_RCVTIMEO},  {~0x0080u, ~SO_LINGER}, {~0x0004u, ~SO_REUSEADDR}};

// Translate socket TCP options to native
const std::map<uint32_t, int> supported_tcp_options = {{0x0001, TCP_NODELAY}};

// Translate ioctl commands to native
const std::map<uint32_t, uint32_t> supported_controls = {
    {0x8004667E, FIONBIO}, {0x4004667F, FIONREAD}};

// Translate socket levels to native
const std::map<uint32_t, int> supported_levels = {{0xFFFF, SOL_SOCKET},
                                                  {0x6, IPPROTO_TCP}};

// Winsock FD_* flags split into asio wait_read / wait_write groups.
namespace {
constexpr uint32_t kFD_READ = 1;
constexpr uint32_t kFD_WRITE = 2;
constexpr uint32_t kFD_ACCEPT = 8;
constexpr uint32_t kFD_CONNECT = 16;
constexpr uint32_t kFD_CLOSE = 32;
constexpr uint32_t kReadEvents = kFD_READ | kFD_ACCEPT | kFD_CLOSE;
constexpr uint32_t kWriteEvents = kFD_WRITE | kFD_CONNECT;
// What the console's WSAEventSelect keeps, everything but FD_OOB.
constexpr uint32_t kSelectableEvents = kReadEvents | kWriteEvents;
}  // namespace

// asio error_code -> Winsock WSAE* code. Guests look up by Winsock value;
// returning raw POSIX errno makes recoverable errors look fatal (e.g. COD4
// MP treats unrecognized recvfrom error as a hard init failure).
uint32_t AsioErrorToWSAError(const asio::error_code& ec) {
  if (!ec) {
    return 0;
  }
  if (ec == asio::error::would_block || ec == asio::error::try_again) {
    return 10035;  // WSAEWOULDBLOCK
  }
  if (ec == asio::error::in_progress) {
    return 10036;  // WSAEINPROGRESS
  }
  if (ec == asio::error::already_started) {
    return 10037;  // WSAEALREADY
  }
  if (ec == asio::error::not_socket) {
    return 10038;  // WSAENOTSOCK
  }
  if (ec == asio::error::message_size) {
    return 10040;  // WSAEMSGSIZE
  }
  if (ec == asio::error::no_protocol_option) {
    return 10042;  // WSAENOPROTOOPT
  }
  if (ec == asio::error::address_family_not_supported) {
    return 10047;  // WSAEAFNOSUPPORT
  }
  if (ec == asio::error::address_in_use) {
    return 10048;  // WSAEADDRINUSE
  }
  if (ec == asio::error::network_down) {
    return 10050;  // WSAENETDOWN
  }
  if (ec == asio::error::network_unreachable) {
    return 10051;  // WSAENETUNREACH
  }
  if (ec == asio::error::network_reset) {
    return 10052;  // WSAENETRESET
  }
  if (ec == asio::error::connection_aborted) {
    return 10053;  // WSAECONNABORTED
  }
  if (ec == asio::error::connection_reset) {
    return 10054;  // WSAECONNRESET
  }
  if (ec == asio::error::no_buffer_space) {
    return 10055;  // WSAENOBUFS
  }
  if (ec == asio::error::already_connected) {
    return 10056;  // WSAEISCONN
  }
  if (ec == asio::error::not_connected) {
    return 10057;  // WSAENOTCONN
  }
  if (ec == asio::error::shut_down) {
    return 10058;  // WSAESHUTDOWN
  }
  if (ec == asio::error::timed_out) {
    return 10060;  // WSAETIMEDOUT
  }
  if (ec == asio::error::connection_refused) {
    return 10061;  // WSAECONNREFUSED
  }
  if (ec == asio::error::host_unreachable) {
    return 10065;  // WSAEHOSTUNREACH
  }
  if (ec == asio::error::access_denied) {
    return 10013;  // WSAEACCES
  }
  if (ec == asio::error::fault) {
    return 10014;  // WSAEFAULT
  }
  if (ec == asio::error::invalid_argument) {
    return 10022;  // WSAEINVAL
  }
  if (ec == asio::error::operation_aborted) {
    return 995;  // WSA_OPERATION_ABORTED
  }
  if (ec == asio::error::interrupted) {
    return 10004;  // WSAEINTR
  }
  return static_cast<uint32_t>(ec.value());
}

// The error of the last call that failed on a host socket.
static asio::error_code HostSocketErrorCode() {
#if XE_PLATFORM_WIN32
  const int host_error = WSAGetLastError();
#else
  const int host_error = errno;
#endif  // XE_PLATFORM_WIN32
  return asio::error_code(host_error, asio::error::get_system_category());
}

// The Winsock code of the last call that failed on a host socket.
static uint32_t HostSocketError() {
  return AsioErrorToWSAError(HostSocketErrorCode());
}

// Starts a connect on a non-blocking host socket, would_block while it runs.
static asio::error_code StartHostConnect(
    uint64_t handle, const asio::ip::tcp::endpoint& endpoint) {
  if (::connect(static_cast<int>(handle), endpoint.data(),
                static_cast<socklen_t>(endpoint.size())) == 0) {
    return {};
  }
  const asio::error_code ec = HostSocketErrorCode();
  // POSIX hosts report EINPROGRESS where Winsock reports WSAEWOULDBLOCK.
  return ec == asio::error::in_progress ? asio::error::would_block : ec;
}

// How a finished connect went, by the host's SO_ERROR.
static asio::error_code HostConnectError(uint64_t handle) {
  int error = 0;
  socklen_t length = sizeof(error);
  if (getsockopt(static_cast<int>(handle), SOL_SOCKET, SO_ERROR,
                 reinterpret_cast<char*>(&error), &length) < 0) {
    return HostSocketErrorCode();
  }
  return asio::error_code(error, asio::error::get_system_category());
}

// How a connect StartHostConnect began is going, would_block while it runs.
static asio::error_code PollHostConnect(uint64_t handle) {
  fd_set write_set;
  fd_set error_set;
  FD_ZERO(&write_set);
  FD_ZERO(&error_set);
  FD_SET(handle, &write_set);
  FD_SET(handle, &error_set);
  timeval now = {0, 0};
  const int ready = select(static_cast<int>(handle) + 1, nullptr, &write_set,
                           &error_set, &now);
  if (ready < 0) {
    return HostSocketErrorCode();
  }
  return ready ? HostConnectError(handle) : asio::error::would_block;
}

// SOL_SOCKET and IPPROTO_TCP values are a big-endian word, or a byte below 4
// bytes, which the host takes in its own byte order and layout. Other levels'
// values pass as they are.
static bool HasNumericValue(uint32_t level) {
  return level == 0xFFFF || level == IPPROTO_TCP;
}

static bool IsTimeout(uint32_t level, uint32_t optname) {
  return level == 0xFFFF && (optname == 0x1005 || optname == 0x1006);
}

template <typename T>
static std::vector<char> ValueBytes(const T& value) {
  const char* bytes = reinterpret_cast<const char*>(&value);
  return {bytes, bytes + sizeof(value)};
}

// The SO_SNDBUF or SO_RCVBUF a guest gets for the size it asks for.
static uint32_t GuestBufferSize(uint32_t optname, uint32_t requested) {
  constexpr int32_t kMaxBufferSize = 63 * 1024;
  const int32_t size = int32_t(requested);
  if (size > kMaxBufferSize) {
    return kMaxBufferSize;
  }
  if (size > 0) {
    return uint32_t(size);
  }
  return optname == 0x1001 ? 1 : 0;
}

// The guest's value of an option as the host's setsockopt takes it.
static std::vector<char> HostOptionValue(uint32_t level, uint32_t optname,
                                         const void* optval, uint32_t optlen) {
  const char* guest = static_cast<const char*>(optval);
  if (!HasNumericValue(level) || !optlen) {
    return {guest, guest + optlen};
  }
  if (level == 0xFFFF && optname == 0x0080 && optlen >= 4) {
    const auto* words = static_cast<const xe::be<uint16_t>*>(optval);
    linger value = {};
    value.l_onoff = words[0];
    value.l_linger = words[1];
    return ValueBytes(value);
  }
  if (IsTimeout(level, optname)) {
    const uint32_t ms = net::SocketOptionValue(optval, optlen);
#if XE_PLATFORM_WIN32
    return ValueBytes(DWORD(ms));
#else
    timeval value = {};
    value.tv_sec = ms / 1000;
    value.tv_usec = ms % 1000 * 1000;
    return ValueBytes(value);
#endif  // XE_PLATFORM_WIN32
  }
  const uint32_t value = net::SocketOptionValue(optval, optlen);
  if (level == 0xFFFF && (optname == 0x1001 || optname == 0x1002)) {
    return ValueBytes(int(GuestBufferSize(optname, value)));
  }
  // The rest are BOOLs, which Windows reads by their first byte.
  return ValueBytes(int(value != 0));
}

// Reads a host option back into the guest's layout, false on a host error.
static bool ReadHostOption(uint64_t handle, int native_level, int native_name,
                           uint32_t level, void* optval, uint32_t* optlen) {
  if (!HasNumericValue(level) || !*optlen) {
    socklen_t length = static_cast<socklen_t>(*optlen);
    if (getsockopt(static_cast<int>(handle), native_level, native_name,
                   static_cast<char*>(optval), &length) != 0) {
      return false;
    }
    *optlen = static_cast<uint32_t>(length);
    return true;
  }
  // Zeroed first, as Windows gives some BOOLs in a single byte.
  int host = 0;
  socklen_t length = sizeof(host);
  if (getsockopt(static_cast<int>(handle), native_level, native_name,
                 reinterpret_cast<char*>(&host), &length) != 0) {
    return false;
  }
  net::StoreSocketOptionValue(optval, *optlen, uint32_t(host));
  return true;
}

XSocket::XSocket(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, asio::ip::tcp::socket socket)
    : XObject(kernel_state, kObjectType), tcp_socket_(std::move(socket)) {
  af_ = AddressFamily::X_AF_INET;
  type_ = Type::X_SOCK_STREAM;
  proto_ = Protocol::X_IPPROTO_TCP;
}

XSocket::XSocket(KernelState* kernel_state,
                 std::unique_ptr<net::SystemLinkStream> stream)
    : XObject(kernel_state, kObjectType),
      af_(AddressFamily::X_AF_INET),
      type_(Type::X_SOCK_STREAM),
      proto_(Protocol::X_IPPROTO_TCP),
      bound_(true),
      bound_port_(stream->port()),
      on_system_link_(true),
      system_link_stream_(std::move(stream)) {}

XSocket::~XSocket() {
  // Guest memory may be going away with a socket the title never closed.
  Abandon();
}

void XSocket::Abandon() {
  if (overlapped_receives_) {
    overlapped_receives_->Close(false);
  }
  Close();
}

uint64_t XSocket::native_handle() {
  if (tcp_socket_ && tcp_socket_->is_open()) {
    return static_cast<uint64_t>(tcp_socket_->native_handle());
  }
  if (udp_socket_ && udp_socket_->is_open()) {
    return static_cast<uint64_t>(udp_socket_->native_handle());
  }
  if (acceptor_ && acceptor_->is_open()) {
    return static_cast<uint64_t>(acceptor_->native_handle());
  }
  return static_cast<uint64_t>(-1);
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;
  const net::StartupParams params =
      kernel_state_->GetKernelModule<xam::XamModule>("xam.xex")
          ->xnet_startup_params();
  receive_buffer_size_ = uint32_t(params[4]) << 10;
  send_buffer_size_ = uint32_t(params[5]) << 10;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  asio::error_code ec;

  const auto link = xam::GetSystemLink(kernel_state_);
  if (type == Type::X_SOCK_STREAM && link) {
    uint32_t error = 0;
    system_link_stream_ = link->CreateStream(error);
    if (!system_link_stream_) {
      last_error_ = error;
      return X_STATUS_UNSUCCESSFUL;
    }
    on_system_link_ = true;
    return X_STATUS_SUCCESS;
  }

  if (type == Type::X_SOCK_STREAM) {
    // Use an acceptor for TCP — it supports bind, listen, and accept natively.
    // If Connect() is called later, we transition to a tcp_socket at that
    // point.
    acceptor_.emplace(GetIoContext());
    acceptor_->open(asio::ip::tcp::v4(), ec);
  } else if (type == Type::X_SOCK_DGRAM) {
    udp_socket_.emplace(GetIoContext());
    udp_socket_->open(asio::ip::udp::v4(), ec);
    on_system_link_ = link != nullptr;
  } else {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  // Lets a relaunched title listen on a port its closed connections still
  // hold in TIME_WAIT.
  if (type == Type::X_SOCK_STREAM) {
    int reuse = 1;
    setsockopt(static_cast<int>(native_handle()), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  if (overlapped_receives_) {
    overlapped_receives_->Close(true);
  }
  // Both are kept until destruction since a call on another thread may be in
  // them.
  if (system_link_port_) {
    system_link_port_->Close();
  }
  if (system_link_stream_) {
    system_link_stream_->Close(linger_on_ && !linger_seconds_);
  }

  asio::error_code ec;

  if (tcp_socket_) {
    if (tcp_socket_->is_open()) {
      tcp_socket_->shutdown(asio::socket_base::shutdown_both, ec);
      tcp_socket_->close(ec);
    }
    tcp_socket_.reset();
  }

  if (udp_socket_) {
    if (udp_socket_->is_open()) {
      udp_socket_->close(ec);
    }
    udp_socket_.reset();
  }

  if (acceptor_) {
    if (acceptor_->is_open()) {
      acceptor_->close(ec);
    }
    acceptor_.reset();
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t* optlen) {
  if (system_link_stream_) {
    return StreamGetOption(level, optname, optval_ptr, optlen);
  }
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    last_error_ = AsioErrorToWSAError(asio::error::not_socket);
    return X_STATUS_INVALID_HANDLE;
  }
  // SetOption keeps SO_BROADCAST of a System Link socket off its host socket.
  if (UsesSystemLink() && level == 0xFFFF && optname == 0x0020) {
    net::StoreSocketOptionValue(optval_ptr, *optlen, broadcast_socket_);
    return X_STATUS_SUCCESS;
  }
  // SO_ERROR is refused, a guest learns how a connect went from select's sets.
  if (level == 0xFFFF && optname == 0x1007) {
    last_error_ = AsioErrorToWSAError(asio::error::no_protocol_option);
    return X_STATUS_UNSUCCESSFUL;
  }
  if (const std::optional<uint32_t> value = GuestOption(level, optname)) {
    net::StoreSocketOptionValue(optval_ptr, *optlen, *value);
    return X_STATUS_SUCCESS;
  }
  // IPPROTO_IP's numbers aren't the host's.
  if (level == 0) {
    last_error_ = 10042;  // WSAENOPROTOOPT
    return X_STATUS_UNSUCCESSFUL;
  }

  // Map Xbox socket levels to native
  int native_level = level;
  if (supported_levels.contains(level)) {
    native_level = supported_levels.at(level);
  }

  // Map Xbox socket options to native
  int native_optname = optname;
  if (level == 0xFFFF && supported_socket_options.contains(optname)) {
    native_optname = supported_socket_options.at(optname);
  } else if (level == IPPROTO_TCP && supported_tcp_options.contains(optname)) {
    native_optname = supported_tcp_options.at(optname);
  }

  const uint64_t handle = native_handle();
  if (handle == static_cast<uint64_t>(-1)) {
    last_error_ = AsioErrorToWSAError(asio::error::not_socket);
    return X_STATUS_INVALID_HANDLE;
  }

  if (!ReadHostOption(handle, native_level, native_optname, level, optval_ptr,
                      optlen)) {
    last_error_ = HostSocketError();
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }
  if (level == 0) {
    return SetIpOption(optname, optval_ptr, optlen);
  }

  if (system_link_stream_) {
    KeepOption(level, optname, optval_ptr, optlen);
    return X_STATUS_SUCCESS;
  }

  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    last_error_ = AsioErrorToWSAError(asio::error::not_socket);
    return X_STATUS_INVALID_HANDLE;
  }

  // SO_EXCLUSIVEADDRUSE. Bind already refuses a port another socket holds.
  // Windows refuses the option next to the SO_REUSEADDR of stream sockets and
  // other hosts have no such option.
  if (level == 0xFFFF && optname == ~0x0004u) {
    return X_STATUS_SUCCESS;
  }

  // SO_BROADCAST of a System Link socket, which never sends from its host
  // socket. Hosts other than Windows refuse a one-byte BOOL.
  if (UsesSystemLink() && level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = net::SocketOptionValue(optval_ptr, optlen) != 0;
    return X_STATUS_SUCCESS;
  }

  // Map Xbox socket levels to native
  int native_level = level;
  if (supported_levels.contains(level)) {
    native_level = supported_levels.at(level);
  }

  // Map Xbox socket options to native
  int native_optname = optname;
  if (level == 0xFFFF && supported_socket_options.contains(optname)) {
    native_optname = supported_socket_options.at(optname);
  } else if (level == IPPROTO_TCP && supported_tcp_options.contains(optname)) {
    native_optname = supported_tcp_options.at(optname);
  }

  int native_handle_val = static_cast<int>(native_handle());
  if (native_handle_val == -1) {
    last_error_ = AsioErrorToWSAError(asio::error::not_socket);
    return X_STATUS_INVALID_HANDLE;
  }

  std::vector<char> value = HostOptionValue(level, optname, optval_ptr, optlen);
  int ret = setsockopt(native_handle_val, native_level, native_optname,
                       value.data(), static_cast<socklen_t>(value.size()));
  // getsockopt reports the guest's buffer sizes and a host refusing one, as
  // macOS does a receive buffer of 0, changes nothing the guest sees.
  const bool buffer_size =
      level == 0xFFFF && (optname == 0x1001 || optname == 0x1002);
  if (ret < 0 && !buffer_size) {
    last_error_ = HostSocketError();
    XELOGE("XSocket::SetOption: failed with error {:08X}", last_error_);
    return X_STATUS_UNSUCCESSFUL;
  }
  if (ret == 0 && acceptor_) {
    acceptor_options_.push_back(
        {native_level, native_optname, std::move(value)});
  }

  KeepOption(level, optname, optval_ptr, optlen);
  return X_STATUS_SUCCESS;
}

void XSocket::KeepOption(uint32_t level, uint32_t optname,
                         const void* optval_ptr, uint32_t optlen) {
  const uint32_t value = net::SocketOptionValue(optval_ptr, optlen);
  if (level == 0xFFFF && optname == 0x1005) {
    send_timeout_ms_ = value;
  } else if (level == 0xFFFF && optname == 0x1006) {
    recv_timeout_ms_ = value;
  } else if (level == 0xFFFF && (optname == 0x1001 || optname == 0x1002)) {
    (optname == 0x1001 ? send_buffer_size_ : receive_buffer_size_) =
        GuestBufferSize(optname, value);
  } else if (level == 0xFFFF && optname == 0x0080 && optlen >= 4) {
    // Two big-endian WORDs, l_onoff and l_linger.
    const auto* linger = static_cast<const xe::be<uint16_t>*>(optval_ptr);
    linger_on_ = linger[0];
    linger_seconds_ = linger[1];
  } else if (level == 0xFFFF && optname == ~0x0080u) {
    linger_on_ = !value;
  } else if (level == X_IPPROTO_TCP && optname == 0x0001) {
    no_delay_ = value != 0;
  }
}

std::optional<uint32_t> XSocket::GuestOption(uint32_t level,
                                             uint32_t optname) const {
  if (level == X_IPPROTO_TCP && optname == 0x0001) {
    return uint32_t(no_delay_);
  }
  if (level == 0 && optname == 7) {
    return ip_ttl_;
  }
  if (level == 0 && optname == 0x64) {
    return ip_dscp_;
  }
  if (level != 0xFFFF) {
    return std::nullopt;
  }
  switch (optname) {
    case 0x0080:
      return uint32_t(linger_on_) << 16 | linger_seconds_;
    case ~0x0080u:
      return uint32_t(!linger_on_);
    case 0x1001:
      return send_buffer_size_;
    case 0x1002:
      return receive_buffer_size_;
    case 0x1005:
      return send_timeout_ms_;
    case 0x1006:
      return recv_timeout_ms_;
    default:
      return std::nullopt;
  }
}

void XSocket::TakeListenerOptions(XSocket& accepted) const {
  accepted.recv_timeout_ms_ = recv_timeout_ms_;
  accepted.send_timeout_ms_ = send_timeout_ms_;
  accepted.send_buffer_size_ = send_buffer_size_;
  accepted.receive_buffer_size_ = receive_buffer_size_;
  accepted.linger_on_ = linger_on_;
  accepted.linger_seconds_ = linger_seconds_;
  accepted.no_delay_ = no_delay_;
  accepted.ip_ttl_ = ip_ttl_;
  accepted.ip_dscp_ = ip_dscp_;
}

X_STATUS XSocket::SetIpOption(uint32_t optname, const void* optval_ptr,
                              uint32_t optlen) {
  const uint32_t value = net::SocketOptionValue(optval_ptr, optlen);
  int host_name = 0;
  int host_value = 0;
  switch (optname) {
    case 7:  // IP_TTL
      if (optlen != 4 || !value || value > 0xFF) {
        last_error_ = uint32_t(X_WSAError::X_WSAEINVAL);
        return X_STATUS_UNSUCCESSFUL;
      }
      ip_ttl_ = uint8_t(value);
      host_name = IP_TTL;
      host_value = int(value);
      break;
    case 0x64:  // The DSCP, the TOS byte's top 6 bits
      if (optlen != 1 || value > 0x3F) {
        last_error_ = uint32_t(X_WSAError::X_WSAEINVAL);
        return X_STATUS_UNSUCCESSFUL;
      }
      ip_dscp_ = uint8_t(value);
      host_name = IP_TOS;
      host_value = int(value << 2);
      break;
    case 5:  // IP_ADD_MEMBERSHIP
    case 6:  // IP_DROP_MEMBERSHIP
      return SetMulticastGroup(optname == 5, value);
    default:
      last_error_ = 10042;  // WSAENOPROTOOPT
      return X_STATUS_UNSUCCESSFUL;
  }
  // A System Link stream has no host socket. A host refusing IP_TOS changes
  // nothing the guest sees.
  const uint64_t handle = native_handle();
  if (handle == static_cast<uint64_t>(-1)) {
    return X_STATUS_SUCCESS;
  }
  std::vector<char> bytes = ValueBytes(host_value);
  if (setsockopt(static_cast<int>(handle), IPPROTO_IP, host_name, bytes.data(),
                 static_cast<socklen_t>(bytes.size())) == 0 &&
      acceptor_) {
    acceptor_options_.push_back({IPPROTO_IP, host_name, std::move(bytes)});
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetMulticastGroup(bool join, uint32_t group) {
  if (type_ != Type::X_SOCK_DGRAM) {
    last_error_ = 10042;  // WSAENOPROTOOPT
    return X_STATUS_UNSUCCESSFUL;
  }
  const auto it = std::ranges::find(multicast_groups_, group);
  if (!join && it == multicast_groups_.end()) {
    last_error_ = uint32_t(X_WSAError::X_WSAEINVAL);
    return X_STATUS_UNSUCCESSFUL;
  }
  if (join && it != multicast_groups_.end()) {
    return X_STATUS_SUCCESS;
  }
  if (join) {
    multicast_groups_.push_back(group);
  } else {
    multicast_groups_.erase(it);
  }
  // The host joins too so the group's datagrams arrive.
  ip_mreq request = {};
  request.imr_multiaddr.s_addr = htonl(group);
  request.imr_interface.s_addr = htonl(INADDR_ANY);
  const uint64_t handle = native_handle();
  if (handle != static_cast<uint64_t>(-1) &&
      setsockopt(static_cast<int>(handle), IPPROTO_IP,
                 join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                 reinterpret_cast<const char*>(&request),
                 sizeof(request)) < 0) {
    XELOGW("XSocket: host multicast {} of {:08X} failed: {:08X}",
           join ? "join" : "leave", group, HostSocketError());
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::StreamGetOption(uint32_t level, uint32_t optname,
                                  void* optval_ptr, uint32_t* optlen) {
  std::optional<uint32_t> value = GuestOption(level, optname);
  if (level == 0xFFFF && optname == 0x1008) {
    value = X_SOCK_STREAM;
  } else if (level == 0xFFFF && (optname == 0x0004 || optname == 0x0008 ||
                                 optname == 0x0020 || optname == 0x0100)) {
    // SO_REUSEADDR, SO_KEEPALIVE, SO_BROADCAST and SO_OOBINLINE, which no
    // stream uses.
    value = 0;
  }
  if (!value) {
    last_error_ = 10042;  // WSAENOPROTOOPT
    return X_STATUS_UNSUCCESSFUL;
  }
  net::StoreSocketOptionValue(optval_ptr, *optlen, *value);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  if (system_link_stream_) {
    if (cmd == 0x8004667E) {
      guest_non_blocking_ = *reinterpret_cast<uint32_t*>(arg_ptr) != 0;
      return X_STATUS_SUCCESS;
    }
    if (cmd == 0x4004667F) {
      xe::store_and_swap<uint32_t>(
          arg_ptr, static_cast<uint32_t>(system_link_stream_->PendingBytes()));
      return X_STATUS_SUCCESS;
    }
    XELOGE("XSocket::IOControl: unsupported command {:08X}", cmd);
    last_error_ = 10022;  // WSAEINVAL
    return X_STATUS_INVALID_PARAMETER;
  }
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // FIONBIO - set non-blocking mode
  if (cmd == 0x8004667E) {
    uint32_t value = *reinterpret_cast<uint32_t*>(arg_ptr);
    bool non_blocking = (value != 0);

    if (acceptor_) {
      acceptor_->non_blocking(non_blocking, ec);
    } else if (tcp_socket_) {
      tcp_socket_->non_blocking(non_blocking, ec);
    } else if (udp_socket_) {
      udp_socket_->non_blocking(non_blocking, ec);
    }

    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }
    guest_non_blocking_ = non_blocking;
    return X_STATUS_SUCCESS;
  }

  // FIONREAD - get bytes available
  if (cmd == 0x4004667F) {
    if (system_link_port_) {
      xe::store_and_swap<uint32_t>(
          arg_ptr, static_cast<uint32_t>(system_link_port_->PendingBytes()));
      return X_STATUS_SUCCESS;
    }

    size_t available = 0;
    if (tcp_socket_) {
      available = tcp_socket_->available(ec);
    } else if (udp_socket_) {
      available = udp_socket_->available(ec);
    }

    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }

    xe::store_and_swap<uint32_t>(arg_ptr, static_cast<uint32_t>(available));
    return X_STATUS_SUCCESS;
  }

  XELOGE("XSocket::IOControl: unsupported command {:08X}", cmd);
  return X_STATUS_INVALID_PARAMETER;
}

void XSocket::SetHostNonBlocking(bool enable) {
  asio::error_code ec;
  if (acceptor_) {
    acceptor_->non_blocking(enable, ec);
  } else if (tcp_socket_) {
    tcp_socket_->non_blocking(enable, ec);
  } else if (udp_socket_) {
    udp_socket_->non_blocking(enable, ec);
  }
}

void XSocket::RunCooperatively(asio::error_code& ec, RetryMode mode,
                               const std::function<void()>& attempt) {
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self || guest_non_blocking_) {
    attempt();
    return;
  }

  // Enforced here because the host option only applies in blocking mode. No
  // socket timeout bounds a connect or accept.
  uint32_t timeout_ms = 0;
  if (mode == RetryMode::kReceive) {
    timeout_ms = recv_timeout_ms_;
  } else if (mode == RetryMode::kSend) {
    timeout_ms = send_timeout_ms_;
  }
  uint64_t deadline_ms =
      timeout_ms ? Clock::QueryHostUptimeMillis() + timeout_ms : 0;

  if (cooperative_io_depth_.fetch_add(1) == 0) {
    SetHostNonBlocking(true);
  }
  auto* scheduler = self->kernel_state()->guest_scheduler();
  for (bool retried = false;; retried = true) {
    attempt();
    bool retry = ec == asio::error::would_block || ec == asio::error::try_again;
    if (mode == RetryMode::kConnect) {
      // A retried connect reports success by refusing to connect twice. A
      // socket connected before the call fails with WSAEISCONN.
      if (ec == asio::error::already_connected && retried) {
        ec.clear();
        break;
      }
      retry = retry || ec == asio::error::in_progress ||
              ec == asio::error::already_started;
    }
    if (!retry) {
      break;
    }
    if (deadline_ms && Clock::QueryHostUptimeMillis() >= deadline_ms) {
      // What a blocking socket reports when its SO_*TIMEO expires.
      ec = asio::error::timed_out;
      break;
    }
    scheduler->BlockCurrentThread();
  }
  if (cooperative_io_depth_.fetch_sub(1) == 1) {
    SetHostNonBlocking(false);
  }
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  auto* addr_in = reinterpret_cast<N_XSOCKADDR_IN*>(name);
  if (system_link_stream_) {
    return StreamConnect({addr_in->sin_addr, addr_in->sin_port});
  }
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (UsesSystemLink()) {
    if (!system_link_port_ && BindSystemLink(0, 0) != X_STATUS_SUCCESS) {
      return X_STATUS_UNSUCCESSFUL;
    }
    system_link_peer_ = net::Endpoint{addr_in->sin_addr, addr_in->sin_port};
    return X_STATUS_SUCCESS;
  }

  asio::ip::address_v4 addr(addr_in->sin_addr);
  uint16_t port = addr_in->sin_port;

  asio::error_code ec;

  // asio's connect waits for the connection. A non-blocking guest gets
  // WSAEWOULDBLOCK and waits for it in select. A blocking one parks between
  // polls, as waiting would stall the other guest threads on this host thread.
  auto connect_stream = [&](const asio::ip::tcp::endpoint& endpoint) {
    const uint64_t handle = tcp_socket_->native_handle();
    if (!guest_non_blocking_) {
      bool started = false;
      RunCooperatively(ec, RetryMode::kConnect, [&]() {
        if (!XThread::GetCurrentFiberThread()) {
          tcp_socket_->connect(endpoint, ec);
        } else if (!started) {
          started = true;
          ec = StartHostConnect(handle, endpoint);
        } else {
          ec = PollHostConnect(handle);
        }
      });
      connect_state_ = ConnectState::kNone;
      return;
    }
    ec = StartHostConnect(handle, endpoint);
    if (ec == asio::error::would_block) {
      connect_state_ = ConnectState::kConnecting;
    } else if (ec != asio::error::already_started) {
      // WSAEALREADY leaves the pending connect to select.
      connect_state_ = ConnectState::kNone;
    }
  };

  if (acceptor_) {
    // Transition from acceptor to tcp_socket for client connection.
    // The acceptor was created in Initialize() before we knew whether this
    // socket would listen or connect.
    const asio::ip::tcp::endpoint bound_endpoint =
        bound_ ? acceptor_->local_endpoint(ec) : asio::ip::tcp::endpoint();
    acceptor_->close(ec);
    acceptor_.reset();

    tcp_socket_.emplace(GetIoContext());
    tcp_socket_->open(asio::ip::tcp::v4(), ec);
    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }
    // The new socket takes the FIONBIO and options the guest set on the
    // acceptor.
    if (guest_non_blocking_) {
      tcp_socket_->non_blocking(true, ec);
      if (ec) {
        last_error_ = AsioErrorToWSAError(ec);
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    const int handle = static_cast<int>(tcp_socket_->native_handle());
    for (const HostOption& option : acceptor_options_) {
      if (setsockopt(handle, option.level, option.name, option.value.data(),
                     static_cast<socklen_t>(option.value.size())) < 0) {
        XELOGW("XSocket::Connect: option {}/{} not kept: {:08X}", option.level,
               option.name, HostSocketError());
      }
    }
    acceptor_options_.clear();
    // And the address the guest bound it to.
    if (bound_) {
      tcp_socket_->bind(bound_endpoint, ec);
      if (ec) {
        last_error_ = AsioErrorToWSAError(ec);
        return X_STATUS_UNSUCCESSFUL;
      }
    }

    connect_stream(asio::ip::tcp::endpoint(addr, port));
  } else if (tcp_socket_) {
    connect_stream(asio::ip::tcp::endpoint(addr, port));
  } else if (udp_socket_) {
    asio::ip::udp::endpoint endpoint(addr, port);
    udp_socket_->connect(endpoint, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void XSocket::SettleConnect() {
  if (connect_state_ != ConnectState::kConnecting) {
    return;
  }
  connect_state_ = HostConnectError(native_handle()) ? ConnectState::kFailed
                                                     : ConnectState::kNone;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (system_link_stream_ || UsesSystemLink()) {
    return BindSystemLink(name->sin_addr, name->sin_port);
  }
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  // On Linux and Windows (when running under Wine), ports < 1024 require root
  // privileges. Remap to port + 10000 to avoid privilege issues.
  // Note: N_XSOCKADDR_IN uses xe::be<> which auto-converts to/from host endian.
  const uint16_t original_port = static_cast<uint16_t>(name->sin_port);
  if (original_port < 1024 && original_port != 0) {
    uint16_t new_port = original_port + 10000;
    name->sin_port = new_port;
    XELOGW("XSocket::Bind: port {} requires privileges, remapping to port {}",
           original_port, new_port);
  }

  asio::ip::address_v4 addr(name->sin_addr);
  uint16_t port = name->sin_port;

  // Reject duplicate active binds (Xbox 360 behaviour).  SO_REUSEADDR is set
  // on stream sockets for TIME_WAIT reclaiming but games expect a second bind
  // to the same port to fail with WSAEADDRINUSE.  Query the kernel's live
  // object table so the check is naturally correct across in-process relaunches
  // (Shutdown destroys the old object table).
  if (port != 0) {
    auto sockets =
        kernel_state()->object_table()->GetObjectsByType<XSocket>(kObjectType);
    for (auto& s : sockets) {
      if (s.get() != this && s->bound_ && s->bound_port_ == port &&
          s->type_ == type_) {
        last_error_ = 10048;  // WSAEADDRINUSE
        return X_STATUS_UNSUCCESSFUL;
      }
    }
  }

  asio::error_code ec;

  if (acceptor_) {
    asio::ip::tcp::endpoint endpoint(addr, port);
    acceptor_->bind(endpoint, ec);
  } else if (tcp_socket_) {
    asio::ip::tcp::endpoint endpoint(addr, port);
    tcp_socket_->bind(endpoint, ec);
  } else if (udp_socket_) {
    asio::ip::udp::endpoint endpoint(addr, port);
    udp_socket_->bind(endpoint, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_address_ = name->sin_addr;

  // Get the actual bound port (important when binding to port 0)
  if (acceptor_) {
    bound_port_ = acceptor_->local_endpoint(ec).port();
  } else if (tcp_socket_) {
    bound_port_ = tcp_socket_->local_endpoint(ec).port();
  } else if (udp_socket_) {
    bound_port_ = udp_socket_->local_endpoint(ec).port();
  }

  if (ec) {
    bound_port_ = port;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  if (system_link_stream_) {
    if (const uint32_t error = system_link_stream_->Listen(backlog)) {
      last_error_ = error;
      return X_STATUS_UNSUCCESSFUL;
    }
    return X_STATUS_SUCCESS;
  }
  if (!acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  acceptor_->listen(backlog, ec);
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

// Writes the accepted peer as a guest sockaddr_in or clears it without one.
// Host sockaddr_in layouts differ (macOS starts with sin_len).
static void AcceptedName(N_XSOCKADDR* name, int* name_len,
                         std::optional<net::Endpoint> peer) {
  if (name) {
    std::memset(name, 0, sizeof(*name));
  }
  if (!peer) {
    *name_len = 0;
    return;
  }
  if (name) {
    const xe::be<uint16_t> port = peer->port;
    const xe::be<uint32_t> address = peer->address;
    name->address_family = AF_INET;
    std::memcpy(name->sa_data, &port, sizeof(port));
    std::memcpy(name->sa_data + sizeof(port), &address, sizeof(address));
  }
  *name_len = sizeof(N_XSOCKADDR);
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  if (system_link_stream_) {
    return StreamAccept(name, name_len);
  }
  if (!acceptor_) {
    // WSAEOPNOTSUPP for a datagram socket, WSAEINVAL before listen().
    last_error_ = udp_socket_ ? 10045 : uint32_t(X_WSAError::X_WSAEINVAL);
    return nullptr;
  }

  asio::error_code ec;

  // On the heap since a fiber reclaimed while it waits here never unwinds and
  // a socket on its stack would stay linked into the io_context.
  auto new_socket = std::make_unique<asio::ip::tcp::socket>(GetIoContext());
  asio::ip::tcp::endpoint peer_endpoint;
  RunCooperatively(ec, RetryMode::kAccept, [&]() {
    acceptor_->accept(*new_socket, peer_endpoint, ec);
  });
  ReenableHostEvents(kFD_ACCEPT);

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    AcceptedName(name, name_len, std::nullopt);
    return nullptr;
  }
  AcceptedName(name, name_len,
               net::Endpoint{peer_endpoint.address().to_v4().to_uint(),
                             peer_endpoint.port()});

  // Create a kernel object to represent the new socket
  auto socket =
      object_ref<XSocket>(new XSocket(kernel_state_, std::move(*new_socket)));
  socket->bound_ = true;
  socket->bound_address_ = bound_address_;
  TakeListenerOptions(*socket);
  // An accepted socket starts in the listener's non-blocking mode.
  if (guest_non_blocking_) {
    socket->SetHostNonBlocking(true);
    socket->guest_non_blocking_ = true;
  }
  // It takes over the listener's WSAEventSelect too.
  std::lock_guard lock(select_mutex_);
  if (selected_event_ && selected_event_flags_) {
    std::lock_guard accepted_lock(socket->select_mutex_);
    socket->selected_event_ = selected_event_;
    socket->selected_event_flags_ = selected_event_flags_;
    socket->ArmHostReady(selected_event_flags_);
  }

  return socket;
}

int XSocket::Shutdown(int how) {
  if (system_link_stream_) {
    if (const uint32_t error = system_link_stream_->Shutdown(how)) {
      last_error_ = error;
      return -1;
    }
    return 0;
  }
  if (UsesSystemLink()) {
    // SD_RECEIVE and SD_BOTH complete the pending receive and fail every later
    // one. Sends are left alone.
    if (how != 1) {
      receive_shutdown_ = true;
      if (overlapped_receives_) {
        overlapped_receives_->Drop(net::kWsaShutdown);
      }
    }
    return 0;
  }
  if (!tcp_socket_ && !udp_socket_) {
    last_error_ = 10057;  // WSAENOTCONN
    return -1;
  }

  asio::error_code ec;
  asio::socket_base::shutdown_type shutdown_type;

  switch (how) {
    case 0:
      shutdown_type = asio::socket_base::shutdown_receive;
      break;
    case 1:
      shutdown_type = asio::socket_base::shutdown_send;
      break;
    case 2:
    default:
      shutdown_type = asio::socket_base::shutdown_both;
      break;
  }

  if (tcp_socket_) {
    tcp_socket_->shutdown(shutdown_type, ec);
  }
  // UDP sockets don't support shutdown (connectionless protocol)

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return 0;
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (system_link_stream_) {
    return SystemLinkReceive(buf, buf_len, flags, nullptr, nullptr);
  }
  if (!tcp_socket_ && !udp_socket_) {
    last_error_ = 10057;  // WSAENOTCONN
    return -1;
  }

  if (UsesSystemLink()) {
    return SystemLinkReceive(buf, buf_len, flags, nullptr, nullptr);
  }

  asio::error_code ec;
  size_t bytes_received = 0;

  if (udp_socket_) {
    RunCooperatively(ec, RetryMode::kReceive, [&]() {
      bytes_received =
          udp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
    });
  } else if (tcp_socket_) {
    RunCooperatively(ec, RetryMode::kReceive, [&]() {
      bytes_received =
          tcp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
    });
  }
  ReenableHostEvents(kFD_READ);

  // A stream peer that closed the connection reads as 0 bytes.
  if (ec == asio::error::eof) {
    return 0;
  }
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_received);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                      N_XSOCKADDR_IN* from, uint32_t* from_len) {
  if (system_link_stream_) {
    return SystemLinkReceive(buf, buf_len, flags, from, from_len);
  }
  if (!udp_socket_ && !tcp_socket_) {
    last_error_ = 10057;  // WSAENOTCONN
    return -1;
  }

  if (UsesSystemLink()) {
    return SystemLinkReceive(buf, buf_len, flags, from, from_len);
  }

  asio::error_code ec;
  size_t bytes_received = 0;

  if (udp_socket_) {
    asio::ip::udp::endpoint sender_endpoint;
    RunCooperatively(ec, RetryMode::kReceive, [&]() {
      bytes_received = udp_socket_->receive_from(asio::buffer(buf, buf_len),
                                                 sender_endpoint, 0, ec);
    });

    if (!ec && from) {
      from->sin_family = AF_INET;
      from->sin_addr = sender_endpoint.address().to_v4().to_uint();
      from->sin_port = sender_endpoint.port();
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
    }
    if (from_len) {
      *from_len = sizeof(N_XSOCKADDR_IN);
    }
  } else if (tcp_socket_) {
    RunCooperatively(ec, RetryMode::kReceive, [&]() {
      bytes_received =
          tcp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
    });
  }
  ReenableHostEvents(kFD_READ);

  // A stream peer that closed the connection reads as 0 bytes.
  if (ec == asio::error::eof) {
    return 0;
  }
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_received);
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (system_link_stream_) {
    return StreamSend(buf, buf_len);
  }
  if (!tcp_socket_ && !udp_socket_) {
    last_error_ = 10057;  // WSAENOTCONN
    return -1;
  }

  if (UsesSystemLink()) {
    return SystemLinkSend(buf, buf_len, nullptr);
  }

  asio::error_code ec;
  size_t bytes_sent = 0;

  if (udp_socket_) {
    RunCooperatively(ec, RetryMode::kSend, [&]() {
      bytes_sent = udp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
    });
  } else if (tcp_socket_) {
    RunCooperatively(ec, RetryMode::kSend, [&]() {
      bytes_sent = tcp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
    });
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    // A send that would block re-enables FD_WRITE.
    if (last_error_ == 10035) {
      ReenableHostEvents(kFD_WRITE);
    }
    return -1;
  }

  return static_cast<int>(bytes_sent);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    N_XSOCKADDR_IN* to, uint32_t to_len) {
  // A connection ignores the destination.
  if (system_link_stream_) {
    return StreamSend(buf, buf_len);
  }
  if (!udp_socket_ && !tcp_socket_) {
    last_error_ = 10057;  // WSAENOTCONN
    return -1;
  }

  if (UsesSystemLink()) {
    return SystemLinkSend(buf, buf_len, to);
  }

  asio::error_code ec;
  size_t bytes_sent = 0;

  if (udp_socket_) {
    if (to) {
      asio::ip::address_v4 addr(to->sin_addr);
      uint16_t port = to->sin_port;
      asio::ip::udp::endpoint endpoint(addr, port);

      RunCooperatively(ec, RetryMode::kSend, [&]() {
        bytes_sent = udp_socket_->send_to(asio::buffer(buf, buf_len), endpoint,
                                          flags, ec);
      });
    } else {
      // Send to connected endpoint
      RunCooperatively(ec, RetryMode::kSend, [&]() {
        bytes_sent = udp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
      });
    }
  } else if (tcp_socket_) {
    RunCooperatively(ec, RetryMode::kSend, [&]() {
      bytes_sent = tcp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
    });
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    // A send that would block re-enables FD_WRITE.
    if (last_error_ == 10035) {
      ReenableHostEvents(kFD_WRITE);
    }
    return -1;
  }

  return static_cast<int>(bytes_sent);
}

int XSocket::WSARecvFrom(std::vector<std::span<uint8_t>> buffers,
                         uint32_t flags, XSOCKADDR_IN* from,
                         xe::be<uint32_t>* from_len,
                         XWSAOVERLAPPED* overlapped) {
  if (overlapped) {
    if (!UsesSystemLink()) {
      last_error_ = 10045;  // WSAEOPNOTSUPP
      return -1;
    }
    if (!overlapped_receives_) {
      last_error_ = 10022;  // WSAEINVAL
      return -1;
    }
    object_ref<XEvent> event;
    if (overlapped->event_handle) {
      event = kernel_state()->object_table()->LookupObject<XEvent>(
          overlapped->event_handle);
    }
    auto receives = overlapped_receives_;
    {
      std::lock_guard lock(receives->mutex);
      // A socket being closed takes no new receives.
      if (!receives->port) {
        last_error_ = uint32_t(X_WSAError::X_WSAENOTSOCK);
        return -1;
      }
      if (receive_shutdown_) {
        last_error_ = net::kWsaShutdown;
        return -1;
      }
      // Ikaruga posts the same overlapped again while it is still pending.
      if (!receives->Pending(overlapped)) {
        if (event) {
          event->Reset();
        }
        overlapped->internal = X_STATUS_PENDING;
        overlapped->internal_high = 0;
        receives->pending.push_back(
            {std::move(buffers), from, from_len, overlapped, std::move(event)});
      }
    }
    // A datagram queued already won't run the ready callback again.
    asio::post(GetIoContext(), [receives]() { receives->Complete(); });
    last_error_ = uint32_t(X_WSAError::X_WSA_IO_PENDING);
    return -1;
  }

  size_t size = 0;
  for (const auto& buffer : buffers) {
    size += buffer.size();
  }
  std::vector<uint8_t> data(size);
  N_XSOCKADDR_IN native_from;
  if (from) {
    native_from = *from;
  }
  uint32_t native_from_len = sizeof(native_from);
  const int received = RecvFrom(data.data(), static_cast<uint32_t>(size), flags,
                                &native_from, &native_from_len);
  // A datagram too large for the buffers still fills them.
  const bool filled = received >= 0 || last_error_ == net::kWsaMessageSize;
  if (!filled) {
    return -1;
  }
  Scatter({data.data(), received >= 0 ? size_t(received) : size}, buffers);
  if (from) {
    from->sin_family = native_from.sin_family;
    from->sin_port = native_from.sin_port;
    from->sin_addr = native_from.sin_addr;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }
  if (from_len) {
    *from_len = native_from_len;
  }
  return received;
}

bool XSocket::IsOverlappedPending(const XWSAOVERLAPPED* overlapped) const {
  if (!overlapped_receives_) {
    return false;
  }
  std::lock_guard lock(overlapped_receives_->mutex);
  return overlapped_receives_->Pending(overlapped);
}

int XSocket::WSAEventSelect(object_ref<XEvent> event, uint32_t flags) {
  if (!tcp_socket_ && !udp_socket_ && !acceptor_ && !system_link_stream_) {
    last_error_ = uint32_t(X_WSAError::X_WSAENOTSOCK);
    return -1;
  }
  // The console drops FD_OOB and any bit it doesn't know.
  flags &= kSelectableEvents;

  asio::error_code ec;

  // WSAEventSelect implicitly sets the socket to non-blocking.
  if (acceptor_) {
    acceptor_->non_blocking(true, ec);
  } else if (tcp_socket_) {
    tcp_socket_->non_blocking(true, ec);
  } else if (udp_socket_) {
    udp_socket_->non_blocking(true, ec);
  }
  guest_non_blocking_ = true;

  std::lock_guard<std::mutex> lock(select_mutex_);

  // Cancel pending waits from any prior selection.
  if (selected_event_) {
    asio::error_code cancel_ec;
    if (acceptor_) {
      acceptor_->cancel(cancel_ec);
    } else if (tcp_socket_) {
      tcp_socket_->cancel(cancel_ec);
    } else if (udp_socket_) {
      udp_socket_->cancel(cancel_ec);
    }
  }
  ++select_generation_;
  host_read_wait_ = false;
  host_write_wait_ = false;

  selected_event_ = std::move(event);
  selected_event_flags_ = flags;
  // Arming clears the event. Readiness that already holds sets it again
  // below. Host sockets keep theirs since the I/O thread would only set it
  // again later.
  if (on_system_link_ && selected_event_ && flags) {
    selected_event_->Reset();
  }

  if (system_link()) {
    ArmSystemLinkReady();
    return 0;
  }

  ArmHostReady(flags);
  return 0;
}

void XSocket::ArmHostReady(uint32_t events) {
  if (!selected_event_) {
    return;
  }
  // Only a listener accepts, and it does nothing else.
  events &= selected_event_flags_ & (acceptor_ ? kFD_ACCEPT : ~kFD_ACCEPT);
  auto wait = [this](bool read) {
    (read ? host_read_wait_ : host_write_wait_) = true;
    // Strong refs so the socket and event outlive the wait.
    auto handler = [self = retain_object(this), ev = selected_event_,
                    generation = select_generation_,
                    read](const asio::error_code& wait_ec) {
      {
        std::lock_guard lock(self->select_mutex_);
        if (generation == self->select_generation_) {
          (read ? self->host_read_wait_ : self->host_write_wait_) = false;
        }
      }
      // A cancelled wait or a closed socket sets nothing.
      if (!wait_ec) {
        ev->Set(0, false);
      }
    };
    const auto type =
        read ? asio::socket_base::wait_read : asio::socket_base::wait_write;
    if (acceptor_) {
      acceptor_->async_wait(type, handler);
    } else if (tcp_socket_) {
      tcp_socket_->async_wait(type, handler);
    } else if (udp_socket_) {
      udp_socket_->async_wait(type, handler);
    }
  };
  if ((events & kReadEvents) && !host_read_wait_) {
    wait(true);
  }
  if ((events & kWriteEvents) && !host_write_wait_) {
    wait(false);
  }
}

void XSocket::ReenableHostEvents(uint32_t events) {
  std::lock_guard lock(select_mutex_);
  ArmHostReady(events);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port,
                          const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = static_cast<uint16_t>(len);
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push(reinterpret_cast<uint8_t*>(pkt));

  // TODO: Limit on number of incoming packets?
  return true;
}

X_STATUS XSocket::QueryName(bool peer, N_XSOCKADDR_IN* name) {
  if (on_system_link_) {
    const auto connected =
        system_link_stream_ ? system_link_stream_->peer() : system_link_peer_;
    const uint16_t port =
        system_link_stream_ ? system_link_stream_->port() : bound_port_;
    if (peer ? !connected : !port) {
      // WSAENOTCONN without a connection, WSAEINVAL before the socket binds.
      last_error_ = peer ? 10057 : 10022;
      return X_STATUS_UNSUCCESSFUL;
    }
    // getsockname reports 0.0.0.0 with the bound port.
    const net::Endpoint endpoint = peer ? *connected : net::Endpoint{0, port};
    name->sin_family = AF_INET;
    name->sin_addr = endpoint.address;
    name->sin_port = endpoint.port;
    std::memset(name->x_sin_zero, 0, sizeof(name->x_sin_zero));
    return X_STATUS_SUCCESS;
  }

  auto handle = native_handle();
  if (handle == static_cast<uint64_t>(-1)) {
    last_error_ = AsioErrorToWSAError(asio::error::not_socket);
    return X_STATUS_INVALID_HANDLE;
  }

  sockaddr_in host_name = {};
  socklen_t len = sizeof(host_name);
  auto host_address = reinterpret_cast<sockaddr*>(&host_name);
  int result = peer ? getpeername(static_cast<int>(handle), host_address, &len)
                    : getsockname(static_cast<int>(handle), host_address, &len);
  if (result == -1) {
    last_error_ = HostSocketError();
    return X_STATUS_UNSUCCESSFUL;
  }

  name->sin_family = host_name.sin_family;
  name->sin_port = ntohs(host_name.sin_port);
  name->sin_addr = peer ? ntohl(host_name.sin_addr.s_addr) : bound_address_;
  std::memset(name->x_sin_zero, 0, sizeof(name->x_sin_zero));
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetSockName(N_XSOCKADDR_IN* name) {
  return QueryName(false, name);
}

X_STATUS XSocket::GetPeerName(N_XSOCKADDR_IN* name) {
  return QueryName(true, name);
}

bool XSocket::UsesSystemLink() const { return udp_socket_ && on_system_link_; }

X_STATUS XSocket::BindSystemLink(uint32_t address, uint16_t port) {
  if (bound_) {
    last_error_ = 10022;  // WSAEINVAL
    return X_STATUS_UNSUCCESSFUL;
  }
  auto link = xam::GetSystemLink(kernel_state_);
  if (!link) {
    last_error_ = net::kWsaNetworkDown;
    return X_STATUS_UNSUCCESSFUL;
  }
  const uint32_t own = xe::load_and_swap<uint32_t>(link->identity().data());
  if (address && address != own) {
    last_error_ = 10049;  // WSAEADDRNOTAVAIL
    return X_STATUS_UNSUCCESSFUL;
  }
  if (system_link_stream_) {
    if (const uint32_t error = system_link_stream_->Bind(port)) {
      last_error_ = error;
      return X_STATUS_UNSUCCESSFUL;
    }
    bound_port_ = system_link_stream_->port();
    bound_ = true;
    return X_STATUS_SUCCESS;
  }
  uint32_t error = 0;
  auto bound = link->Bind(port, proto_ == Protocol::X_IPPROTO_VDP, error);
  if (!bound) {
    last_error_ = error;
    return X_STATUS_UNSUCCESSFUL;
  }
  system_link_port_ = std::move(bound);
  overlapped_receives_ =
      std::make_shared<OverlappedReceives>(system_link_port_.get());
  bound_port_ = system_link_port_->port();
  bound_ = true;
  std::lock_guard<std::mutex> lock(select_mutex_);
  ArmSystemLinkReady();
  return X_STATUS_SUCCESS;
}

// What RunCooperatively makes of a System Link error so it retries what is
// still under way.
static asio::error_code RetryCode(uint32_t error) {
  switch (error) {
    case 0:
      return {};
    case net::kWsaWouldBlock:
      return asio::error::would_block;
    case net::kWsaAlready:
      return asio::error::already_started;
    case net::kWsaIsConnected:
      return asio::error::already_connected;
    default:
      return asio::error::fault;
  }
}

// The error to report once RunCooperatively ends with |ec|, which is
// timed_out past SO_RCVTIMEO or SO_SNDTIMEO.
static uint32_t SystemLinkError(const asio::error_code& ec, uint32_t error) {
  return ec == asio::error::timed_out ? net::kWsaTimedOut : error;
}

int XSocket::SystemLinkReceive(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                               N_XSOCKADDR_IN* from, uint32_t* from_len) {
  if (!system_link()) {
    last_error_ = 10022;  // WSAEINVAL
    return -1;
  }
  constexpr uint32_t kMsgPeek = 2;
  const bool peek = (flags & kMsgPeek) != 0;
  std::optional<net::Endpoint> source;
  uint32_t error = 0;
  int received = -1;
  asio::error_code ec;
  RunCooperatively(ec, RetryMode::kReceive, [&]() {
    const uint32_t wait_ms = SystemLinkWait(RetryMode::kReceive);
    if (system_link_stream_) {
      received =
          system_link_stream_->Receive({buf, buf_len}, peek, wait_ms, error);
      source = system_link_stream_->peer();
    } else if (receive_shutdown_) {
      received = -1;
      error = net::kWsaShutdown;
    } else {
      net::Endpoint datagram;
      received = system_link_port_->Receive({buf, buf_len}, peek, datagram,
                                            wait_ms, error);
      source = datagram;
    }
    ec = RetryCode(received >= 0 ? 0 : error);
  });
  if (ec) {
    last_error_ = SystemLinkError(ec, error);
    return -1;
  }
  // Like the console, post FD_READ again while datagrams remain.
  if (!peek && !system_link_stream_) {
    std::lock_guard<std::mutex> lock(select_mutex_);
    if (selected_event_ && (selected_event_flags_ & kFD_READ) &&
        system_link_port_->Readable()) {
      selected_event_->Set(0, false);
    }
  }
  if (from && source) {
    from->sin_family = AF_INET;
    from->sin_addr = source->address;
    from->sin_port = source->port;
    std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
  }
  if (from_len) {
    *from_len = sizeof(N_XSOCKADDR_IN);
  }
  return received;
}

int XSocket::StreamSend(const uint8_t* buf, uint32_t buf_len) {
  uint32_t sent = 0;
  uint32_t error = 0;
  asio::error_code ec;
  RunCooperatively(ec, RetryMode::kSend, [&]() {
    do {
      const int written =
          system_link_stream_->Send({buf + sent, buf_len - sent},
                                    SystemLinkWait(RetryMode::kSend), error);
      if (written < 0) {
        ec = RetryCode(error);
        return;
      }
      sent += uint32_t(written);
      // A non-blocking send returns what fit.
    } while (sent < buf_len && !guest_non_blocking_);
    ec.clear();
  });
  if (ec) {
    last_error_ = SystemLinkError(ec, error);
    return -1;
  }
  return static_cast<int>(sent);
}

X_STATUS XSocket::StreamConnect(net::Endpoint destination) {
  uint32_t error = 0;
  asio::error_code ec;
  RunCooperatively(ec, RetryMode::kConnect, [&]() {
    error = system_link_stream_->Connect(destination,
                                         SystemLinkWait(RetryMode::kConnect));
    ec = RetryCode(error);
  });
  if (ec) {
    last_error_ = SystemLinkError(ec, error);
    return X_STATUS_UNSUCCESSFUL;
  }
  bound_port_ = system_link_stream_->port();
  bound_ = true;
  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::StreamAccept(N_XSOCKADDR* name, int* name_len) {
  net::Endpoint peer;
  std::unique_ptr<net::SystemLinkStream> accepted;
  uint32_t error = 0;
  asio::error_code ec;
  RunCooperatively(ec, RetryMode::kAccept, [&]() {
    accepted = system_link_stream_->Accept(
        peer, SystemLinkWait(RetryMode::kAccept), error);
    ec = RetryCode(accepted ? 0 : error);
  });
  if (!accepted) {
    last_error_ = SystemLinkError(ec, error);
    AcceptedName(name, name_len, std::nullopt);
    return nullptr;
  }
  AcceptedName(name, name_len, peer);
  auto socket =
      object_ref<XSocket>(new XSocket(kernel_state_, std::move(accepted)));
  TakeListenerOptions(*socket);
  // An accepted socket starts in the listener's non-blocking mode and takes
  // over its WSAEventSelect, which makes it non-blocking either way.
  socket->guest_non_blocking_ = guest_non_blocking_;
  std::lock_guard lock(select_mutex_);
  if (selected_event_ && selected_event_flags_) {
    std::lock_guard accepted_lock(socket->select_mutex_);
    socket->guest_non_blocking_ = true;
    socket->selected_event_ = selected_event_;
    socket->selected_event_flags_ = selected_event_flags_;
    socket->ArmSystemLinkReady();
  }
  return socket;
}

int XSocket::SystemLinkSend(const uint8_t* buf, uint32_t buf_len,
                            const N_XSOCKADDR_IN* to) {
  if (to) {
    if (const uint32_t error = net::DatagramDestinationError(
            {to->sin_addr, to->sin_port}, broadcast_socket_)) {
      last_error_ = error;
      return -1;
    }
  }
  // Sending binds an unbound socket.
  if (!system_link_port_ && BindSystemLink(0, 0) != X_STATUS_SUCCESS) {
    return -1;
  }
  if (!to && !system_link_peer_) {
    last_error_ = 10057;  // WSAENOTCONN
    return -1;
  }
  const net::Endpoint destination =
      to ? net::Endpoint{to->sin_addr, to->sin_port} : *system_link_peer_;
  uint32_t error = 0;
  const int sent = system_link_port_->Send(destination, {buf, buf_len}, error);
  if (sent < 0) {
    last_error_ = error;
  }
  return sent;
}

uint32_t XSocket::SystemLinkWait(RetryMode mode) const {
  // RunCooperatively retries on a fiber so only a host thread waits here.
  if (guest_non_blocking_ || XThread::GetCurrentFiberThread()) {
    return 0;
  }
  // No socket timeout bounds a connect or accept.
  const uint32_t timeout_ms = mode == RetryMode::kReceive ? recv_timeout_ms_
                              : mode == RetryMode::kSend  ? send_timeout_ms_
                                                          : 0;
  return timeout_ms ? timeout_ms : net::SystemLinkPort::kWaitForever;
}

bool XSocket::SystemLinkReadable() const {
  return system_link_stream_ ? system_link_stream_->Readable()
                             : system_link_port_->Readable();
}

bool XSocket::SystemLinkWritable() const {
  // A datagram socket can always send.
  return !system_link_stream_ || system_link_stream_->Writable();
}

bool XSocket::SystemLinkFailed() const {
  return system_link_stream_ && system_link_stream_->Failed();
}

void XSocket::ArmSystemLinkReady() {
  auto event = selected_event_;
  const uint32_t flags = event ? selected_event_flags_ : 0;
  if (system_link_stream_) {
    // The stream reports FD_* events itself.
    std::function<void(uint32_t)> ready;
    if (flags) {
      ready = [event, flags](uint32_t events) {
        if (events & flags) {
          event->Set(0, false);
        }
      };
    }
    system_link_stream_->SetReadyCallback(std::move(ready));
    return;
  }
  system_link_port_->SetReadyCallback(
      [event, flags, receives = overlapped_receives_]() {
        if (flags & kReadEvents) {
          event->Set(0, false);
        }
        // Receiving here would call back into the System Link.
        asio::post(GetIoContext(), [receives]() { receives->Complete(); });
      });
  // A datagram socket can always send.
  if (flags & kWriteEvents) {
    event->Set(0, false);
  }
}

uint32_t XSocket::GetLastWSAError() const { return last_error_; }

}  // namespace kernel
}  // namespace xe
