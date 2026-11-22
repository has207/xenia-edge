/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_NET_SYSTEM_LINK_H_
#define XENIA_NET_SYSTEM_LINK_H_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "xenia/net/packet_transport.h"
#include "xenia/net/xnet_association.h"

namespace xe::net {

// Winsock error codes, as the guest sees them.
inline constexpr uint32_t kWsaInterrupted = 10004;
inline constexpr uint32_t kWsaAccessDenied = 10013;
inline constexpr uint32_t kWsaInvalid = 10022;
inline constexpr uint32_t kWsaWouldBlock = 10035;
inline constexpr uint32_t kWsaAlready = 10037;
inline constexpr uint32_t kWsaMessageSize = 10040;
inline constexpr uint32_t kWsaAddressInUse = 10048;
inline constexpr uint32_t kWsaAddressNotAvailable = 10049;
inline constexpr uint32_t kWsaNetworkDown = 10050;
inline constexpr uint32_t kWsaConnectionAborted = 10053;
inline constexpr uint32_t kWsaConnectionReset = 10054;
inline constexpr uint32_t kWsaNoBuffers = 10055;
inline constexpr uint32_t kWsaIsConnected = 10056;
inline constexpr uint32_t kWsaNotConnected = 10057;
inline constexpr uint32_t kWsaShutdown = 10058;
inline constexpr uint32_t kWsaTimedOut = 10060;
inline constexpr uint32_t kWsaConnectionRefused = 10061;
inline constexpr uint32_t kWsaHostUnreachable = 10065;
inline constexpr uint32_t kWsaSystemNotReady = 10091;

// What XNetGetConnectStatus reports for an address without a peer.
inline constexpr uint32_t kConnectStatusLost = 3;

// FD_* event bits.
inline constexpr uint32_t kStreamRead = 0x01;
inline constexpr uint32_t kStreamWrite = 0x02;
inline constexpr uint32_t kStreamAccept = 0x08;
inline constexpr uint32_t kStreamConnect = 0x10;
inline constexpr uint32_t kStreamClose = 0x20;

// Host byte order.
struct Endpoint {
  uint32_t address = 0;
  uint16_t port = 0;
  bool operator==(const Endpoint&) const = default;
};

// The error sendto fails a datagram to |destination| with before it binds or
// sends anything, 0 if the send goes on. |broadcast| is SO_BROADCAST.
uint32_t DatagramDestinationError(Endpoint destination, bool broadcast);

// The value of a BOOL or DWORD socket option: its first byte when |optlen| is
// under four, otherwise a big-endian DWORD.
uint32_t SocketOptionValue(const void* optval, uint32_t optlen);
// Stores a BOOL or DWORD socket option value the same way and sets |optlen| to
// the bytes written.
void StoreSocketOptionValue(void* optval, uint32_t& optlen, uint32_t value);

// XNetStartupParams as the guest lays them out.
inline constexpr size_t kStartupParamsSize = 13;
using StartupParams = std::array<uint8_t, kStartupParamsSize>;
// What XNet runs with for the parameters a title passed, none when empty. Zero
// fields take xam's defaults and the others are clamped to its limits.
StartupParams NormalizeStartupParams(std::span<const uint8_t> params);

// XNetQosListen flags.
inline constexpr uint32_t kQosListenEnable = 0x01;
inline constexpr uint32_t kQosListenDisable = 0x02;
inline constexpr uint32_t kQosListenSetData = 0x04;
inline constexpr uint32_t kQosListenRelease = 0x10;
// The most data a host answers QoS probes with.
inline constexpr size_t kQosDataLimit = 256;

struct QosTarget {
  secure::Address address{};
  KeyId id{};
  Key key{};
};

struct QosResult {
  bool contacted = false;
  bool disabled = false;
  std::vector<uint8_t> data;
};

struct Peer {
  // The XNADDR the peer was first mapped by.
  secure::Address address{};
  KeyId key_id{};
  uint32_t secure_address = 0;
};

// A guest UDP or VDP socket bound to a System Link port. Errors are Winsock
// codes.
class SystemLinkPort {
 public:
  static constexpr uint32_t kWaitForever = UINT32_MAX;

  virtual ~SystemLinkPort() = default;
  virtual uint16_t port() const = 0;
  virtual void Close() = 0;
  // Size of the next datagram, 0 if none is queued.
  virtual size_t PendingBytes() const = 0;
  // A receive would return without waiting.
  virtual bool Readable() const = 0;
  // Runs on any thread when a datagram arrives at an empty queue or the LAN
  // goes down, and right away if already Readable(). It must not call back
  // into the System Link.
  virtual void SetReadyCallback(std::function<void()> callback) = 0;
  virtual int Send(Endpoint destination, std::span<const uint8_t> data,
                   uint32_t& error) = 0;
  // Waits up to |wait_ms| for a datagram, 0 returns right away.
  virtual int Receive(std::span<uint8_t> buffer, bool peek, Endpoint& source,
                      uint32_t wait_ms, uint32_t& error) = 0;
};

// A guest TCP socket on the System Link LAN. Errors are Winsock codes. Calls
// that take |wait_ms| wait up to that long to make progress. With 0 they fail
// with kWsaWouldBlock rather than wait.
class SystemLinkStream {
 public:
  static constexpr uint32_t kWaitForever = UINT32_MAX;

  virtual ~SystemLinkStream() = default;
  // 0 until bound.
  virtual uint16_t port() const = 0;
  // Where a connection leads.
  virtual std::optional<Endpoint> peer() const = 0;
  // Port 0 picks a free one.
  virtual uint32_t Bind(uint16_t port) = 0;
  virtual uint32_t Listen(int backlog) = 0;
  virtual std::unique_ptr<SystemLinkStream> Accept(Endpoint& peer,
                                                   uint32_t wait_ms,
                                                   uint32_t& error) = 0;
  // Binds an unbound stream first. A connect still under way fails with
  // kWsaAlready when called again and one that finished fails with
  // kWsaIsConnected.
  virtual uint32_t Connect(Endpoint destination, uint32_t wait_ms) = 0;
  // Returns how much of |data| was queued.
  virtual int Send(std::span<const uint8_t> data, uint32_t wait_ms,
                   uint32_t& error) = 0;
  // Returns 0 once the peer finished sending.
  virtual int Receive(std::span<uint8_t> buffer, bool peek, uint32_t wait_ms,
                      uint32_t& error) = 0;
  // Winsock's SD_RECEIVE, SD_SEND or SD_BOTH.
  virtual uint32_t Shutdown(int how) = 0;
  // The connection finishes in the background, or ends with a reset when
  // |abort| is set or data was left unread. Calls on other threads return
  // kWsaInterrupted.
  virtual void Close(bool abort) = 0;
  virtual size_t PendingBytes() const = 0;
  // What select() reports: data, the peer's end or a connection to accept;
  // room to send on a connection; a connect that failed.
  virtual bool Readable() const = 0;
  virtual bool Writable() const = 0;
  virtual bool Failed() const = 0;
  // Runs with the kStream* events that happened and right away with those
  // that already hold. It must not call back into the System Link.
  virtual void SetReadyCallback(
      std::function<void(uint32_t events)> callback) = 0;
};

// One console on the System Link LAN: its keys, the peers it mapped to secure
// addresses and the secure associations with them, run over a packet
// transport. Methods are thread safe. Secure addresses are opaque handles in
// 0.0.0.0/8 that the guest passes to socket calls in place of a peer's IP.
// This console's own XNADDR maps to 127.0.0.1.
class SystemLink {
 public:
  // Fills in |association|'s identity and credential.
  SystemLink(uint32_t address,
             std::shared_ptr<secure::PacketTransport> transport,
             secure::AssociationConfig association);
  ~SystemLink();

  // This console's XNADDR.
  const secure::Address& identity() const { return identity_; }
  static uint64_t MachineId(const secure::Address& address);

  static void CreateKey(KeyId& id, Key& key, bool cross_platform);
  uint32_t RegisterKey(const KeyId& id, const Key& key);
  uint32_t UnregisterKey(const KeyId& id);
  bool GetKey(const KeyId& id, Key& key) const;

  uint32_t Map(const secure::Address& address, const KeyId& id,
               uint32_t& secure);
  uint32_t Reverse(uint32_t secure, secure::Address& address, KeyId& id) const;
  uint32_t Unmap(uint32_t secure);
  uint32_t Connect(uint32_t secure);
  uint32_t ConnectStatus(uint32_t secure) const;
  std::vector<Peer> peers() const;

  // Sets what this console answers QoS probes for a registered key with.
  uint32_t QosListen(const KeyId& id, uint32_t flags,
                     std::span<const uint8_t> data);
  // Asks the host of each target for the data it listens with. |done| runs
  // once per target with the System Link locked and must not call back into
  // it. It runs before this returns for a target that isn't probed, such as
  // this console's own address, otherwise on the worker. |tag| must not name a
  // lookup still running.
  void QosLookup(
      uint32_t tag, std::vector<QosTarget> targets,
      std::function<void(size_t target, const QosResult& result)> done);
  // No |done| of the lookup runs after this returns.
  void CancelQos(uint32_t tag);

  // Moves cross-platform frames to UDP |port|, 3074 for 0. Other System Links
  // only remember it.
  uint32_t SetSystemLinkPort(uint16_t port);
  uint16_t system_link_port() const;
  // The port SetSystemLinkPort moves to, none for a reserved one.
  static std::optional<uint16_t> ResolveSystemLinkPort(uint16_t port);
  bool cross_platform() const;

  // Whether broadcasts came from older or newer builds of this title, as
  // secure::kBroadcastVersion* bits, cleared by |reset|.
  uint32_t BroadcastVersionStatus(bool reset);

  // Port 0 picks a free one.
  std::unique_ptr<SystemLinkPort> Bind(uint16_t port, bool vdp,
                                       uint32_t& error);
  // An unbound TCP socket. TCP ports are apart from UDP and VDP ones.
  std::unique_ptr<SystemLinkStream> CreateStream(uint32_t& error);
  // Hands a guest that runs its own network stack, such as an emulated
  // console, every Ethernet frame that arrives, before this System Link reads
  // it. |receive| runs on the worker, maybe once more after it is replaced,
  // and must not call back into the System Link. None stops it.
  void SetFrameReceiver(std::function<void(std::span<const uint8_t>)> receive);
  // Queues an Ethernet frame to go out as it is. Fails with kWsaNetworkDown
  // once the LAN is down and with kWsaNoBuffers when the frame doesn't fit
  // the MTU or too many wait.
  uint32_t SendFrame(std::span<const uint8_t> frame);
  // Drops every key, peer, association and QoS listener, ends every QoS lookup
  // and closes every bound port and stream.
  void Reset();

 private:
  struct State;
  class Port;
  class Stream;

  secure::Address identity_{};
  // Shared with the bound ports and streams, which can outlive this.
  std::shared_ptr<State> state_;
};

}  // namespace xe::net

#endif  // XENIA_NET_SYSTEM_LINK_H_
