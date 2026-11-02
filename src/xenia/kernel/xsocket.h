/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSOCKET_H_
#define XENIA_KERNEL_XSOCKET_H_

// Asio must be included before Windows headers to avoid macro conflicts
#include <asio.hpp>

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xobject.h"
#include "xenia/net/system_link.h"

namespace xe {
namespace kernel {

class XEvent;

enum class X_WSAError : uint32_t {
  X_WSA_INVALID_PARAMETER = 0x0057,
  X_WSA_IO_INCOMPLETE = 0x03E4,
  X_WSA_IO_PENDING = 0x03E5,
  X_WSAEFAULT = 0x271E,
  X_WSAEINVAL = 0x2726,
  X_WSAENOTSOCK = 0x2736,
  X_WSAEMSGSIZE = 0x2738,
  X_WSAENOTEMPTY = 0x2752,
  X_WSANOTINITIALISED = 0x276D,
  X_WSAECANCELLED = 0x2777,
  X_WSAHOST_NOT_FOUND = 0x2AF9,
};

struct XSOCKADDR {
  xe::be<uint16_t> address_family;
  char sa_data[14];
};
static_assert_size(XSOCKADDR, 0x10);

struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, xe::countof(sa_data));
    return *this;
  }

  uint16_t address_family;
  char sa_data[14];
};

struct XSOCKADDR_IN {
  xe::be<uint16_t> sin_family;

  // Always big-endian!
  xe::be<uint16_t> sin_port;
  xe::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};
static_assert_size(XSOCKADDR_IN, 0x10);

// Xenia native sockaddr_in
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

  uint16_t sin_family;
  xe::be<uint16_t> sin_port;
  xe::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

struct XWSAOVERLAPPED {
  xe::be<uint32_t> internal;       // STATUS_PENDING, then 0 or 0x8007xxxx
  xe::be<uint32_t> internal_high;  // Bytes transferred
  union {
    struct {
      xe::be<uint32_t> low;
      xe::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    xe::be<uint32_t> pointer;
  };
  xe::be<uint32_t> event_handle;
};

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  // Note: These enum values use X_ prefix to avoid conflicts with Windows
  // macros
  enum AddressFamily {
    X_AF_INET = 2,
  };

  enum Type {
    X_SOCK_STREAM = 1,
    X_SOCK_DGRAM = 2,
  };

  enum Protocol {
    X_IPPROTO_TCP = 6,
    X_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    X_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  // Returns the native socket handle for use with select() etc.
  // Returns -1 if socket is not initialized.
  uint64_t native_handle();
  uint16_t bound_port() const { return bound_port_; }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();
  // Closes without completing pending overlapped receives, whose memory may be
  // gone.
  void Abandon();

  X_STATUS GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                     uint32_t* optlen);
  X_STATUS SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                     uint32_t optlen);
  X_STATUS IOControl(uint32_t cmd, uint8_t* arg_ptr);

  X_STATUS Connect(N_XSOCKADDR* name, int name_len);
  // A non-blocking connect select hasn't seen finish, and one it saw fail. The
  // console reports a failed one in select's write and error sets alike.
  bool connecting() const {
    return connect_state_ == ConnectState::kConnecting;
  }
  bool connect_failed() const {
    return connect_state_ == ConnectState::kFailed;
  }
  // Settles a connect select found ready, by the host's SO_ERROR.
  void SettleConnect();
  X_STATUS Bind(N_XSOCKADDR_IN* name, int name_len);
  X_STATUS Listen(int backlog);
  X_STATUS GetSockName(N_XSOCKADDR_IN* name);
  X_STATUS GetPeerName(N_XSOCKADDR_IN* name);
  object_ref<XSocket> Accept(N_XSOCKADDR* name, int* name_len);
  int Shutdown(int how);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
               N_XSOCKADDR_IN* from, uint32_t* from_len);
  int SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
             uint32_t to_len);
  // Receives one datagram across |buffers|. An |overlapped| receive (System
  // Link only) pends until the I/O thread completes it.
  int WSARecvFrom(std::vector<std::span<uint8_t>> buffers, uint32_t flags,
                  XSOCKADDR_IN* from, xe::be<uint32_t>* from_len,
                  XWSAOVERLAPPED* overlapped);
  bool IsOverlappedPending(const XWSAOVERLAPPED* overlapped) const;

  // Associates the socket with an XEvent signaled on readiness for any of the
  // requested Winsock FD_* flags. flags == 0 detaches.
  int WSAEventSelect(object_ref<XEvent> event, uint32_t flags);

  // On the System Link LAN so there is no host handle to select on.
  bool system_link() const {
    return system_link_port_ != nullptr || system_link_stream_ != nullptr;
  }
  bool SystemLinkReadable() const;
  bool SystemLinkWritable() const;
  // A connect failed.
  bool SystemLinkFailed() const;

  uint32_t GetLastWSAError() const;

  struct packet {
    // These values are in network byte order.
    xe::be<uint16_t> src_port;
    xe::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf,
                   size_t len);

 private:
  // Private constructor for accepted sockets
  XSocket(KernelState* kernel_state, asio::ip::tcp::socket socket);
  XSocket(KernelState* kernel_state,
          std::unique_ptr<net::SystemLinkStream> stream);

  // Retry shape for RunCooperatively. A connect reports progress through error
  // codes a transfer never sees. Receive and send each pick a guest timeout
  // while connect and accept wait without one.
  enum class RetryMode { kReceive, kSend, kConnect, kAccept };

  // Runs |attempt| so the guest still sees a blocking call while the host
  // thread stays free. On a fiber the socket goes non-blocking and we park
  // between attempts, otherwise |attempt| just runs once.
  void RunCooperatively(asio::error_code& ec, RetryMode mode,
                        const std::function<void()>& attempt);
  void SetHostNonBlocking(bool enable);

  // UDP and VDP sockets bind on the System Link LAN when the title runs it.
  // TCP sockets of such a title are System Link streams from the start.
  bool UsesSystemLink() const;
  X_STATUS BindSystemLink(uint32_t address, uint16_t port);
  int SystemLinkReceive(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                        N_XSOCKADDR_IN* from, uint32_t* from_len);
  int SystemLinkSend(const uint8_t* buf, uint32_t buf_len,
                     const N_XSOCKADDR_IN* to);
  // A blocking send returns once all of |buf| is queued.
  int StreamSend(const uint8_t* buf, uint32_t buf_len);
  X_STATUS StreamConnect(net::Endpoint destination);
  object_ref<XSocket> StreamAccept(N_XSOCKADDR* name, int* name_len);
  X_STATUS StreamGetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                           uint32_t* optlen);
  // How long a System Link call waits on this thread.
  uint32_t SystemLinkWait(RetryMode mode) const;
  // SO_RCVTIMEO and SO_SNDTIMEO, which the retry loop enforces itself.
  void KeepTimeout(uint32_t level, uint32_t optname, const void* optval_ptr,
                   uint32_t optlen);
  // Hooks the WSAEventSelect event and the overlapped receives up to the
  // System Link port or stream. select_mutex_ held.
  void ArmSystemLinkReady();

  // Reads the local or remote address into xenia's own layout. Some hosts lay
  // sockaddr_in out differently.
  X_STATUS QueryName(bool peer, N_XSOCKADDR_IN* name);

  // Socket storage - either TCP or UDP
  std::optional<asio::ip::tcp::socket> tcp_socket_;
  std::optional<asio::ip::udp::socket> udp_socket_;

  // Acceptor for listening TCP sockets (created when Listen() is called)
  std::optional<asio::ip::tcp::acceptor> acceptor_;
  // Host options the guest set on the acceptor, which Connect sets again on the
  // TCP socket it opens in the acceptor's place.
  struct HostOption {
    int level;
    int name;
    std::vector<char> value;
  };
  std::vector<HostOption> acceptor_options_;

  enum class ConnectState { kNone, kConnecting, kFailed };
  std::atomic<ConnectState> connect_state_{ConnectState::kNone};

  AddressFamily af_;    // Address family
  Type type_;           // Type (DGRAM/Stream/etc)
  Protocol proto_;      // Protocol (TCP/UDP/etc)
  bool secure_ = true;  // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?
  uint16_t bound_port_ = 0;

  bool broadcast_socket_ = false;

  // The guest's own FIONBIO/WSAEventSelect choice, tracked separately from the
  // socket's live flag because RunCooperatively toggles that one itself.
  bool guest_non_blocking_ = false;
  // Cooperative operations in flight, so the last one out restores blocking.
  std::atomic<uint32_t> cooperative_io_depth_{0};
  // SO_RCVTIMEO / SO_SNDTIMEO in ms, 0 for none. Enforced by RunCooperatively,
  // since the host option has no effect on a non-blocking socket.
  uint32_t recv_timeout_ms_ = 0;
  uint32_t send_timeout_ms_ = 0;

  // Last error code for this socket
  mutable uint32_t last_error_ = 0;

  std::unique_ptr<xe::threading::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;

  std::mutex select_mutex_;
  object_ref<XEvent> selected_event_;
  uint32_t selected_event_flags_ = 0;

  // A socket of a title running System Link.
  bool on_system_link_ = false;
  std::unique_ptr<net::SystemLinkPort> system_link_port_;
  std::unique_ptr<net::SystemLinkStream> system_link_stream_;
  // SO_LINGER with a zero timeout so closing resets the connection.
  bool linger_abort_ = false;
  // Set by shutdown(SD_RECEIVE or SD_BOTH). Receives fail with WSAESHUTDOWN.
  std::atomic<bool> receive_shutdown_{false};
  // Overlapped receives, shared with the port's ready callback that completes
  // them on the I/O thread.
  struct OverlappedReceives;
  std::shared_ptr<OverlappedReceives> overlapped_receives_;
  // Set by connect(), where send() goes.
  std::optional<net::Endpoint> system_link_peer_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSOCKET_H_
