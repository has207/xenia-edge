/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/net/system_link.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/net/xnet_tcp.h"

namespace xe::net {
namespace {

using secure::Address;
using secure::AssociationConfig;
using secure::AssociationEvent;
using secure::Associations;
using secure::Kind;
using secure::Packet;

// XNADDR is ina, inaOnline, wPortOnline, abEnet[6], abOnline[20].
constexpr size_t kEnetOffset = 10;
// SGADDR.platform_type within abOnline.
constexpr size_t kPlatformOffset = 32;
constexpr uint8_t kXbox360Platform = 1;

constexpr size_t kKeyLimit = 8, kPeerLimit = 32;
constexpr size_t kInboxLimit = 64, kPortLimit = 128;
constexpr size_t kOutgoingFrameLimit = 64;
constexpr size_t kEthernetHeaderSize = 14;
constexpr uint64_t kRepeatWindowMs = 500;
constexpr size_t kRepeatLimit = 512;
constexpr uint16_t kFirstDynamicPort = 49152;
// Ports up to this one are reserved.
constexpr uint16_t kLastReservedPort = 1257;
constexpr uint32_t kBroadcast = 0xFFFFFFFF;
// What this console's own XNADDR maps to.
constexpr uint32_t kLoopback = 0x7F000001;
// The teardown reason xam sends when a peer's address changed.
constexpr std::array<uint8_t, 4> kAddressChanged{0x0B, 0, 0, 0};
// A QoS lookup asks a host this often for its data before giving up.
constexpr unsigned kQosAttempts = 4;
constexpr uint64_t kQosRetryMs = 400;
// First probes of pairs from other consoles wait this long for the second.
constexpr auto kQosPairWindow = std::chrono::seconds(2);
constexpr size_t kQosPairLimit = 256;
constexpr int kBacklogLimit = 32;
// A closed socket's connection gets this long to finish before a reset ends
// it.
constexpr uint64_t kOrphanMs = 60000;
// Rounds of segments a console sends itself in one go, each answering the
// last.
constexpr unsigned kLoopbackRounds = 64;

uint64_t Now() { return Clock::QueryHostUptimeMillis(); }

// Stands in for the transport once the System Link is gone.
class ClosedTransport final : public secure::PacketTransport {
 public:
  secure::Mac identity() const override { return {}; }
  uint32_t mtu() const override { return 0; }
  bool healthy() const override { return false; }
  int Send(std::span<const uint8_t> frame) override { return -1; }
  bool Receive(std::vector<uint8_t>& frame, uint32_t timeout_ms) override {
    return false;
  }
  void Wake() override {}
};

uint32_t Ina(const Address& address) {
  return uint32_t(address[0]) << 24 | uint32_t(address[1]) << 16 |
         uint32_t(address[2]) << 8 | address[3];
}

secure::Mac Enet(const Address& address) {
  secure::Mac mac;
  std::copy_n(address.begin() + kEnetOffset, mac.size(), mac.begin());
  return mac;
}

std::string MacText(const secure::Mac& mac) {
  return fmt::format("{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}", mac[0],
                     mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// System Link keys carry their type in the top three bits. Online peer keys
// share the registry but never get an association.
bool ValidKeyId(const KeyId& id, bool cross_platform) {
  return secure::SystemLinkKeyId(id, cross_platform) || (id[0] & 0xF0) == 0x80;
}

struct Datagram {
  Endpoint source;
  std::vector<uint8_t> data;
};

struct Inbox {
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::deque<Datagram> packets;
  std::function<void()> ready;
  // What every call fails with once the port is closed or the LAN is down.
  uint32_t error = 0;

  void Push(Datagram packet) {
    std::function<void()> notify;
    {
      std::lock_guard lock(mutex);
      if (error || packets.size() >= kInboxLimit) {
        return;
      }
      // Every datagram signals, as on the console, not only the first.
      notify = ready;
      packets.push_back(std::move(packet));
    }
    changed.notify_all();
    if (notify) {
      notify();
    }
  }

  void Fail(uint32_t code, bool wake) {
    std::function<void()> notify;
    {
      std::lock_guard lock(mutex);
      if (!error) {
        error = code;
      }
      packets.clear();
      notify = std::exchange(ready, nullptr);
    }
    changed.notify_all();
    if (wake && notify) {
      notify();
    }
  }

  void Retire(uint32_t address) {
    std::lock_guard lock(mutex);
    std::erase_if(packets, [&](const Datagram& packet) {
      return packet.source.address == address;
    });
  }
};

// Where a TCP connection leads.
struct Route {
  Address address{};
  KeyId id{};
  // This console's own address, where segments never leave.
  bool loopback = false;
  bool operator==(const Route&) const = default;
};

// What a stream reports, kept to tell which events happened since.
struct Readiness {
  size_t readable = 0, acceptable = 0;
  bool connected = false, writable = false, ended = false, failed = false;
  uint32_t error = 0;
  bool operator==(const Readiness&) const = default;
};

// A guest TCP socket, guarded by the System Link's mutex.
struct StreamState {
  uint16_t port = 0;
  // Holds |port| rather than sharing a listening stream's.
  bool reserved = false;
  bool listening = false;
  size_t backlog = 0;
  // A listening stream's connections that aren't accepted yet, oldest first.
  std::deque<std::shared_ptr<StreamState>> pending;

  std::optional<tcp::Connection> connection;
  Route route;
  uint16_t remote_port = 0;
  // The address the guest knows the peer by.
  uint32_t remote_address = 0;

  bool closed = false, shut_receive = false, shut_send = false;
  uint64_t closed_at = 0;
  // What calls fail with once the LAN or the peer went away or the guest
  // closed the socket.
  uint32_t error = 0;

  Readiness seen;
  std::function<void(uint32_t)> ready;
  // FD_CONNECT went out, which happens once per socket.
  bool connect_reported = false;
  std::condition_variable changed;
};

uint32_t FailureError(tcp::Failure failure) {
  switch (failure) {
    case tcp::Failure::Refused:
      return kWsaConnectionRefused;
    case tcp::Failure::Reset:
      return kWsaConnectionReset;
    case tcp::Failure::TimedOut:
      return kWsaTimedOut;
    default:
      return kWsaConnectionAborted;
  }
}

bool ConnectFailed(const StreamState& stream) {
  return stream.connection && !stream.connection->connected() &&
         (stream.connection->closed() || stream.error);
}

Readiness Observe(const StreamState& stream) {
  Readiness readiness;
  readiness.error = stream.error;
  for (const auto& pending : stream.pending) {
    if (pending->connection->connected() && !pending->connection->closed()) {
      ++readiness.acceptable;
    }
  }
  if (stream.connection) {
    const auto& connection = *stream.connection;
    readiness.readable = connection.readable();
    readiness.connected = connection.connected();
    readiness.writable = connection.connected() && !stream.shut_send &&
                         !stream.error && connection.writable();
    readiness.ended =
        connection.connected() && (connection.finished_receiving() ||
                                   connection.closed() || stream.error);
    readiness.failed = ConnectFailed(stream);
  }
  return readiness;
}

// The events that hold now, which arming a callback reports.
uint32_t Level(const Readiness& readiness) {
  return (readiness.readable ? kStreamRead : 0) |
         (readiness.acceptable ? kStreamAccept : 0) |
         (readiness.writable ? kStreamWrite : 0) |
         (readiness.ended ? kStreamClose : 0);
}

}  // namespace

uint32_t DatagramDestinationError(Endpoint destination, bool broadcast) {
  const uint32_t address = destination.address;
  if (!address || !destination.port ||
      ((address >> 24) == 0x7F && address != kLoopback)) {
    return kWsaAddressNotAvailable;
  }
  if (address == kBroadcast && !broadcast) {
    return kWsaAccessDenied;
  }
  return 0;
}

uint32_t SocketOptionValue(const void* optval, uint32_t optlen) {
  const auto* bytes = static_cast<const uint8_t*>(optval);
  if (optlen < 4) {
    return bytes[0];
  }
  return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
         uint32_t(bytes[2]) << 8 | bytes[3];
}

void StoreSocketOptionValue(void* optval, uint32_t& optlen, uint32_t value) {
  auto* bytes = static_cast<uint8_t*>(optval);
  if (optlen < 4) {
    bytes[0] = uint8_t(value);
    optlen = 1;
    return;
  }
  for (size_t i = 0; i < 4; ++i) {
    bytes[i] = uint8_t(value >> (24 - 8 * i));
  }
  optlen = 4;
}

StartupParams NormalizeStartupParams(std::span<const uint8_t> params) {
  constexpr StartupParams kDefaults{0, 0, 8, 32, 16, 16, 8, 32, 64, 2, 3, 8, 2};
  constexpr StartupParams kMinimums{0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  constexpr StartupParams kMaximums{0,   255, 232, 232, 63,  63, 255,
                                    255, 255, 255, 255, 255, 255};
  StartupParams result = kDefaults;
  // The extended struct starts with the same fields, any other size is
  // ignored.
  if (params.size() >= kStartupParamsSize &&
      (params[0] == kStartupParamsSize || params[0] == 0x4C)) {
    for (size_t i = 1; i < kStartupParamsSize; ++i) {
      if (params[i]) {
        result[i] = std::clamp(params[i], kMinimums[i], kMaximums[i]);
      }
    }
  }
  // Retail xam clears the flags that turn security off.
  result[1] &= 0xF6;
  result[0] = kStartupParamsSize;
  return result;
}

struct SystemLink::State {
  struct QosListen {
    bool enabled = false;
    std::vector<uint8_t> data;
  };
  // A lookup target still waiting for its host to answer.
  struct QosProbing {
    size_t target = 0;
    Address address{};
    KeyId id{};
    secure::RegistrationKeys keys;
    unsigned attempts = 0;
    uint64_t next_ms = 0;
  };
  struct QosLookup {
    secure::Block nonce{};
    std::vector<QosProbing> probing;
    std::function<void(size_t, const QosResult&)> done;
  };
  // A console's first probe of a pair, by prober, nonce, target and sequence.
  using QosPair = std::tuple<secure::Mac, secure::Block, uint16_t, uint8_t>;

  std::shared_ptr<secure::PacketTransport> transport;
  const Address identity;
  const Key broadcast_auth;
  // Guards everything below.
  std::mutex mutex;
  std::optional<secure::CrossPlatform> cross_platform;
  uint16_t system_link_port = secure::kSystemLinkPort;
  std::map<KeyId, Key> keys;
  std::map<uint32_t, Peer> peers;
  uint32_t next_address = 1;
  Associations associations;
  std::map<std::pair<uint16_t, bool>, std::weak_ptr<Inbox>> ports;
  uint16_t next_port = kFirstDynamicPort;
  // TCP ports that streams bound.
  std::map<uint16_t, std::weak_ptr<StreamState>> stream_ports;
  uint16_t next_stream_port = kFirstDynamicPort;
  // Listening streams and connections, which segments find their way to.
  std::vector<std::shared_ptr<StreamState>> streams;
  // Segments this console sent itself.
  std::vector<Packet> looped;
  std::map<KeyId, QosListen> qos_listens;
  std::map<uint32_t, QosLookup> qos_lookups;
  std::map<QosPair, std::chrono::steady_clock::time_point> qos_pairs;
  std::vector<secure::Frame> qos_outbound;
  // Frames of a guest with its own network stack.
  std::function<void(std::span<const uint8_t>)> frame_receiver;
  std::vector<std::vector<uint8_t>> frames_out;
  // Hashes of recently received frames with their arrival time, oldest first.
  // Only the worker touches them.
  std::deque<std::pair<uint64_t, size_t>> recent;
  bool reported_repeat = false;
  std::jthread worker;

  State(std::shared_ptr<secure::PacketTransport> io, AssociationConfig config)
      : transport(std::move(io)),
        identity(config.identity),
        broadcast_auth(config.broadcast.authentication),
        cross_platform(config.cross_platform),
        associations(WithCredential(std::move(config), keys)) {
    worker = std::jthread([this](std::stop_token stop) { Run(stop); });
  }

  // Associations only ask for a credential with mutex held.
  static AssociationConfig WithCredential(AssociationConfig config,
                                          const std::map<KeyId, Key>& keys) {
    config.credential = [&keys](const KeyId& id) -> std::optional<Key> {
      auto found = keys.find(id);
      return found != keys.end() ? std::optional(found->second) : std::nullopt;
    };
    return config;
  }

  // Only this thread sends and receives on the transport so calls that queue
  // frames wake it.
  void Run(std::stop_token stop) {
    threading::set_name("XNet System Link");
    while (!stop.stop_requested()) {
      std::vector<uint8_t> wire;
      const bool arrived = transport->Receive(wire, 25);
      // Repeats too, since a guest stack can send the same frame twice.
      if (arrived) {
        std::function<void(std::span<const uint8_t>)> receiver;
        {
          std::lock_guard lock(mutex);
          receiver = frame_receiver;
        }
        if (receiver) {
          receiver(wire);
        }
      }
      const bool received = arrived && !Repeat(wire, Now());
      std::vector<secure::Frame> outbound;
      std::vector<std::vector<uint8_t>> frames;
      std::optional<secure::CrossPlatform> addressing;
      {
        std::lock_guard lock(mutex);
        addressing = cross_platform;
        if (!transport->healthy()) {
          XELOGE("System Link packet transport lost");
          associations.Reset();
          peers.clear();
          CloseInboxes(kWsaNetworkDown);
          FailStreams(kWsaNetworkDown);
          FinishQos();
          return;
        }
        if (received) {
          if (auto frame = secure::DecodeFrame(wire, addressing)) {
            if (!ReceiveQos(*frame)) {
              associations.Receive(*frame, Now());
            }
          }
        }
        associations.Tick(Now());
        TickQos(Now());
        Deliver();
        TickStreams(Now());
        outbound = associations.TakeOutbound();
        std::move(qos_outbound.begin(), qos_outbound.end(),
                  std::back_inserter(outbound));
        qos_outbound.clear();
        frames = std::exchange(frames_out, {});
      }
      for (const auto& frame : outbound) {
        if (auto encoded = secure::EncodeFrame(frame, addressing)) {
          if (transport->Send(*encoded) != static_cast<int>(encoded->size())) {
            XELOGE("System Link packet transmission failed");
          }
        }
      }
      for (const auto& frame : frames) {
        if (transport->Send(frame) != static_cast<int>(frame.size())) {
          XELOGE("System Link packet transmission failed");
        }
      }
    }
  }

  // Copies of a frame can arrive through several subnets or a bridge. The
  // protocol never sends the same frame twice so only such copies repeat.
  bool Repeat(std::span<const uint8_t> wire, uint64_t now) {
    const size_t hash = std::hash<std::string_view>()(std::string_view(
        reinterpret_cast<const char*>(wire.data()), wire.size()));
    while (!recent.empty() && now - recent.front().first > kRepeatWindowMs) {
      recent.pop_front();
    }
    if (std::any_of(recent.begin(), recent.end(),
                    [&](const auto& entry) { return entry.second == hash; })) {
      if (!reported_repeat) {
        reported_repeat = true;
        XELOGI("System Link: dropping copies of frames that arrived twice");
      }
      return true;
    }
    if (recent.size() >= kRepeatLimit) {
      recent.pop_front();
    }
    recent.emplace_back(now, hash);
    return false;
  }

  // The rest expects mutex held.

  secure::Mac Machine(const Address& address) const {
    return secure::Machine(address, cross_platform.has_value());
  }

  bool SameMachine(const Address& a, const Address& b) const {
    return secure::SameMachine(a, b, cross_platform.has_value());
  }

  // QoS records travel outside any association.
  bool ReceiveQos(const secure::Frame& frame) {
    const auto header = secure::PeekQos(frame.payload);
    if (!header) {
      return false;
    }
    if (header->reply) {
      QosReplied(frame, *header);
    } else {
      QosProbed(frame, *header);
    }
    return true;
  }

  void QosProbed(const secure::Frame& frame, const secure::QosHeader& header) {
    auto listen = qos_listens.find(header.id);
    auto key = keys.find(header.id);
    if (listen == qos_listens.end() || key == keys.end()) {
      return;
    }
    const auto registration =
        secure::DeriveRegistration(broadcast_auth, header.id, key->second);
    const auto probe = secure::DecodeQosProbe(registration, frame.payload);
    // System Link probes name the host by its MAC.
    if (!probe || Enet(probe->address) != Enet(identity)) {
      return;
    }
    secure::QosReply reply;
    reply.nonce = probe->nonce;
    reply.target = probe->target;
    reply.sequence = probe->sequence;
    if ((probe->flags & secure::kQosProbeDataRequest) ||
        !listen->second.enabled) {
      reply.flags = secure::kQosReplyData;
      if (!listen->second.enabled) {
        reply.flags |= secure::kQosReplyDisabled;
      }
      reply.data = listen->second.data;
      SendQos(frame.source, registration, reply);
      XELOGD("System Link QoS: sent {} bytes to {}", reply.data.size(),
             MacText(frame.source));
      return;
    }
    // Both replies of a timed pair leave once its second probe arrives.
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(qos_pairs, [&](const auto& entry) {
      return now - entry.second > kQosPairWindow;
    });
    const QosPair pair{frame.source, probe->nonce, probe->target,
                       probe->sequence};
    if (!(probe->flags & secure::kQosProbeSecond)) {
      if (qos_pairs.size() < kQosPairLimit) {
        qos_pairs[pair] = now;
      }
      return;
    }
    auto first = qos_pairs.find(pair);
    if (first == qos_pairs.end()) {
      return;
    }
    const uint32_t gap = uint32_t(std::clamp<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                              first->second)
            .count(),
        1, UINT32_MAX));
    qos_pairs.erase(first);
    reply.delay = gap;
    SendQos(frame.source, registration, reply);
    reply.flags = secure::kQosReplySecond;
    if (listen->second.data.empty()) {
      reply.flags |= secure::kQosReplyNoData;
    }
    reply.delay = 0;
    reply.gap = gap;
    SendQos(frame.source, registration, reply);
    XELOGD("System Link QoS: timed probes from {}, {} us apart",
           MacText(frame.source), gap);
  }

  void SendQos(const secure::Mac& destination,
               const secure::RegistrationKeys& registration,
               const secure::QosReply& reply) {
    if (auto payload = secure::EncodeQosReply(registration, reply)) {
      qos_outbound.push_back(
          {Machine(identity), destination, false, std::move(*payload)});
    }
  }

  void QosReplied(const secure::Frame& frame, const secure::QosHeader& header) {
    for (auto lookup = qos_lookups.begin(); lookup != qos_lookups.end();
         ++lookup) {
      if (lookup->second.nonce != header.nonce) {
        continue;
      }
      auto& probing = lookup->second.probing;
      auto target = std::find_if(probing.begin(), probing.end(),
                                 [&](const QosProbing& entry) {
                                   return entry.target == header.target;
                                 });
      // Hosts answer from their own machine.
      if (target == probing.end() || Machine(target->address) != frame.source) {
        return;
      }
      auto reply = secure::DecodeQosReply(target->keys, frame.payload);
      // Only data replies answer what this console asks.
      if (!reply || !(reply->flags & secure::kQosReplyData) ||
          reply->data.size() > kQosDataLimit) {
        return;
      }
      QosResult result;
      result.contacted = true;
      result.disabled = reply->flags & secure::kQosReplyDisabled;
      result.data = std::move(reply->data);
      XELOGD("System Link QoS: {} answered with {} bytes{}",
             MacText(frame.source), result.data.size(),
             result.disabled ? ", not listening" : "");
      const size_t index = target->target;
      probing.erase(target);
      lookup->second.done(index, result);
      if (probing.empty()) {
        qos_lookups.erase(lookup);
      }
      return;
    }
  }

  void TickQos(uint64_t now) {
    for (auto lookup = qos_lookups.begin(); lookup != qos_lookups.end();) {
      auto& probing = lookup->second.probing;
      for (auto target = probing.begin(); target != probing.end();) {
        if (now < target->next_ms) {
          ++target;
          continue;
        }
        if (target->attempts == kQosAttempts) {
          XELOGD("System Link QoS: no answer from {}",
                 MacText(Enet(target->address)));
          const size_t index = target->target;
          target = probing.erase(target);
          lookup->second.done(index, {});
          continue;
        }
        secure::QosProbe probe;
        probe.nonce = lookup->second.nonce;
        probe.target = uint16_t(target->target);
        probe.sequence = uint8_t(++target->attempts);
        probe.flags = secure::kQosProbeDataRequest;
        probe.id = target->id;
        probe.address = target->address;
        qos_outbound.push_back({Machine(identity), Machine(target->address),
                                false,
                                secure::EncodeQosProbe(target->keys, probe)});
        target->next_ms = now + kQosRetryMs;
        ++target;
      }
      lookup = probing.empty() ? qos_lookups.erase(lookup) : std::next(lookup);
    }
  }

  // Ends every lookup without an answer.
  void FinishQos() {
    auto lookups = std::move(qos_lookups);
    qos_lookups.clear();
    for (auto& [tag, lookup] : lookups) {
      for (const auto& target : lookup.probing) {
        lookup.done(target.target, {});
      }
    }
  }

  QosResult LocalQos(const KeyId& id) const {
    QosResult result;
    auto listen = qos_listens.find(id);
    if (listen != qos_listens.end()) {
      result.contacted = true;
      result.disabled = !listen->second.enabled;
      result.data = listen->second.data;
    }
    return result;
  }

  // Peers are one per MAC and key like xam's associations.
  static bool SamePeer(const Address& a, const KeyId& a_id, const Address& b,
                       const KeyId& b_id) {
    return a_id == b_id && Enet(a) == Enet(b);
  }

  // |inbound| maps a peer that reached this console, which xam answers in the
  // association its title mapped whatever XNADDR it named the peer by.
  uint32_t Map(const Address& address, const KeyId& id, uint32_t& secure,
               bool inbound = false) {
    secure = 0;
    // This console's own XNADDR maps to loopback with any key.
    if (Enet(address) == Enet(identity)) {
      secure = kLoopback;
      return 0;
    }
    const uint32_t ina = Ina(address);
    // xam also refuses an XNADDR without a MAC.
    if (!keys.contains(id) || Enet(address) == secure::Mac{} || !ina ||
        ina == kBroadcast) {
      return kWsaInvalid;
    }
    auto found =
        std::find_if(peers.begin(), peers.end(), [&](const auto& entry) {
          return SamePeer(entry.second.address, entry.second.key_id, address,
                          id);
        });
    if (found != peers.end()) {
      const auto& peer = found->second;
      // A title naming a cross-platform peer by another IP or an online peer
      // by another XNADDR replaces its association.
      if (inbound || (secure::SystemLinkKeyId(id, cross_platform.has_value())
                          ? SameMachine(peer.address, address)
                          : peer.address == address)) {
        secure = found->first;
        return 0;
      }
      const Address old = peer.address;
      associations.Close(old, id, Now(), kAddressChanged, false);
      Retire(old, id);
    }
    if (peers.size() >= kPeerLimit) {
      return kWsaNoBuffers;
    }
    // 0.0.0.0/8 is reserved and cannot alias a physical LAN endpoint.
    do {
      secure = next_address++;
      if (next_address >= 0x01000000) {
        next_address = 1;
      }
    } while (peers.contains(secure));
    peers[secure] = Peer{address, id, secure};
    return 0;
  }

  void Retire(const Address& address, const KeyId& id) {
    std::erase_if(peers, [&](const auto& entry) {
      if (!SamePeer(entry.second.address, entry.second.key_id, address, id)) {
        return false;
      }
      for (auto& [port, weak] : ports) {
        if (auto inbox = weak.lock()) {
          inbox->Retire(entry.first);
        }
      }
      return true;
    });
    // Connections end with the association and nothing reaches the peer any
    // more to tell it.
    for (auto& stream : streams) {
      auto& connection = stream->connection;
      const auto& route = stream->route;
      if (!connection || route.loopback ||
          !SamePeer(route.address, route.id, address, id) ||
          connection->closed()) {
        continue;
      }
      if (!connection->connected()) {
        stream->error = kWsaHostUnreachable;
      }
      connection->Detach();
    }
  }

  void CloseInboxes(uint32_t error) {
    for (auto& [port, weak] : ports) {
      if (auto inbox = weak.lock()) {
        inbox->Fail(error, true);
      }
    }
  }

  void Deliver() {
    for (auto& event : associations.TakeEvents()) {
      const bool discovery = event.packet.kind == Kind::Discovery;
      uint32_t source = 0;
      if (discovery) {
        // Discovery comes from the virtual 0.0.0.1, so it gets no mapping.
        source = 1;
      } else if (event.type == AssociationEvent::Type::Retired) {
        Retire(event.peer, event.id);
        continue;
      } else if (Map(event.peer, event.id, source, true) ||
                 event.type == AssociationEvent::Type::Connected) {
        // Mapping on Connected shows the peer to the side that accepted.
        continue;
      }
      if (event.packet.kind == Kind::Tcp) {
        // Streams route by the XNADDR the peer was mapped by.
        auto mapped = peers.find(source);
        if (mapped != peers.end()) {
          TcpInput({mapped->second.address, event.id, false}, source,
                   event.packet, Now());
        }
        continue;
      }
      if (!discovery) {
        XELOGD(
            "System Link datagram received: vdp={}, encrypted={}, clear={}, "
            "source port={}, destination port={}",
            event.packet.kind == Kind::Vdp, event.packet.encrypted.size(),
            event.packet.clear.size(), event.packet.source_port,
            event.packet.destination_port);
      }
      Push(event.packet, source);
    }
    FlushStreams(Now());
  }

  // Hands |packet| from |source| to the ports bound to its destination.
  void Push(const Packet& packet, uint32_t source) {
    const bool discovery = packet.kind == Kind::Discovery;
    const bool vdp = packet.kind == Kind::Vdp;
    auto data = vdp ? secure::ToGuestVdp(packet) : packet.encrypted;
    if (vdp && data.empty()) {
      return;
    }
    for (auto it = ports.begin(); it != ports.end();) {
      auto inbox = it->second.lock();
      if (!inbox) {
        it = ports.erase(it);
        continue;
      }
      if (it->first.first == packet.destination_port &&
          (discovery || it->first.second == vdp)) {
        inbox->Push({{source, packet.source_port}, data});
      }
      ++it;
    }
  }

  // Port 0 picks a free one.
  uint32_t Reserve(const std::shared_ptr<StreamState>& stream, uint16_t port) {
    std::erase_if(stream_ports, [](const auto& entry) {
      auto bound = entry.second.lock();
      return !bound || bound->closed;
    });
    if (stream_ports.size() >= kPortLimit) {
      return kWsaNoBuffers;
    }
    if (!port) {
      do {
        port = next_stream_port++;
        if (!next_stream_port) {
          next_stream_port = kFirstDynamicPort;
        }
      } while (stream_ports.contains(port));
    } else if (stream_ports.contains(port)) {
      return kWsaAddressInUse;
    }
    stream->port = port;
    stream->reserved = true;
    stream_ports[port] = stream;
    return 0;
  }

  void Release(StreamState& stream) {
    if (!stream.reserved) {
      return;
    }
    stream.reserved = false;
    auto found = stream_ports.find(stream.port);
    if (found != stream_ports.end() && found->second.lock().get() == &stream) {
      stream_ports.erase(found);
    }
  }

  void SendSegment(const Route& route, const tcp::Segment& segment,
                   uint16_t source, uint16_t destination, uint64_t now) {
    auto packet = tcp::ToPacket(segment, source, destination);
    if (route.loopback) {
      looped.push_back(std::move(packet));
      return;
    }
    // A segment the association can't take is resent like a lost one.
    associations.Send(route.address, route.id, std::move(packet), now);
  }

  // Hands a segment from |route| to its connection, or to a listening stream
  // when it opens one.
  void TcpInput(const Route& route, uint32_t source, const Packet& packet,
                uint64_t now) {
    auto segment = tcp::FromPacket(packet);
    if (!segment) {
      return;
    }
    const bool syn =
        (segment->flags & (tcp::kSyn | tcp::kAck | tcp::kRst)) == tcp::kSyn;
    for (auto& stream : streams) {
      auto& connection = stream->connection;
      if (!connection || connection->closed() ||
          stream->port != packet.destination_port ||
          stream->remote_port != packet.source_port || stream->route != route) {
        continue;
      }
      // A new connection may take the ports of one waiting out TIME_WAIT, as
      // RFC 1122 allows.
      if (syn && connection->state() == tcp::State::TimeWait) {
        connection->Detach();
        break;
      }
      connection->Input(*segment, now);
      return;
    }
    if (syn) {
      for (auto& listener : streams) {
        if (!listener->listening || listener->port != packet.destination_port) {
          continue;
        }
        // The peer sends its SYN again later.
        if (listener->pending.size() >= listener->backlog) {
          return;
        }
        auto stream = std::make_shared<StreamState>();
        stream->port = listener->port;
        stream->route = route;
        stream->remote_port = packet.source_port;
        stream->remote_address = source;
        stream->connection = tcp::Connection::Answer(*segment, now);
        // FD_CONNECT is for the side that connects.
        stream->connect_reported = true;
        listener->pending.push_back(stream);
        streams.push_back(std::move(stream));
        return;
      }
    }
    if (auto reset = tcp::Refusal(*segment)) {
      XELOGD("System Link TCP: refused a segment for port {}",
             packet.destination_port);
      SendSegment(route, *reset, packet.destination_port, packet.source_port,
                  now);
    }
  }

  // Wakes waiting calls when |stream| changed and reports its events along
  // with those of |again| that still hold. A call that takes only part of
  // what an event was about reports the event again.
  void Refresh(StreamState& stream, uint32_t again = 0) {
    const Readiness now = Observe(stream);
    const Readiness& before = stream.seen;
    uint32_t events = again & Level(now);
    if (now.readable > before.readable) {
      events |= kStreamRead;
    }
    if (now.acceptable > before.acceptable) {
      events |= kStreamAccept;
    }
    if ((now.connected && !before.connected) ||
        (now.failed && !before.failed)) {
      events |= kStreamConnect;
    }
    if (now.writable && !before.writable) {
      events |= kStreamWrite;
    }
    if (now.ended && !before.ended) {
      events |= kStreamClose;
    }
    if (stream.connection && !stream.closed) {
      auto peer = [&] {
        return fmt::format("{:08X}:{}", stream.remote_address,
                           stream.remote_port);
      };
      if (now.connected && !before.connected) {
        XELOGI("System Link TCP: port {} connected to {}", stream.port, peer());
      } else if (now.failed && !before.failed) {
        XELOGI("System Link TCP: port {} failed to connect to {}, error {}",
               stream.port, peer(),
               stream.error ? stream.error
                            : FailureError(stream.connection->failure()));
      }
      if (now.ended && !before.ended) {
        XELOGI(
            "System Link TCP: connection from port {} to {} ended{}",
            stream.port, peer(),
            stream.connection->failure() == tcp::Failure::None && !stream.error
                ? ""
                : " abnormally");
      }
    }
    const bool changed = now != before;
    stream.seen = now;
    if (changed || events) {
      stream.changed.notify_all();
    }
    if (events && stream.ready) {
      stream.connect_reported |= (events & kStreamConnect) != 0;
      stream.ready(events);
    }
  }

  // Sends what the connections queued and takes in what they sent this
  // console, then reports what changed.
  void FlushStreams(uint64_t now) {
    for (auto& stream : streams) {
      auto& connection = stream->connection;
      if (!stream->closed || !connection || connection->closed()) {
        continue;
      }
      // Nobody reads a closed socket so data that still arrives resets it.
      // TIME_WAIT ends by itself.
      if (connection->readable() ||
          (now - stream->closed_at >= kOrphanMs &&
           connection->state() != tcp::State::TimeWait)) {
        connection->Abort();
      }
    }
    for (unsigned round = 0; round < kLoopbackRounds; ++round) {
      // Input can add streams so they're held while sending.
      const auto current = streams;
      for (const auto& stream : current) {
        if (!stream->connection) {
          continue;
        }
        for (const auto& segment : stream->connection->TakeOutput()) {
          SendSegment(stream->route, segment, stream->port, stream->remote_port,
                      now);
        }
      }
      if (looped.empty()) {
        break;
      }
      for (const auto& packet : std::exchange(looped, {})) {
        TcpInput({{}, {}, true}, kLoopback, packet, now);
      }
    }
    for (auto& stream : streams) {
      // An accept can't take a connection that ended first.
      std::erase_if(stream->pending, [](const auto& pending) {
        return pending->connection->closed();
      });
      Refresh(*stream);
    }
    // What a closed connection or listener could still do happened above.
    std::erase_if(streams, [](const auto& stream) {
      return stream->connection ? stream->connection->closed()
                                : !stream->listening;
    });
  }

  void TickStreams(uint64_t now) {
    for (auto& stream : streams) {
      if (stream->connection) {
        stream->connection->Tick(now);
      }
    }
    FlushStreams(now);
  }

  void FailStreams(uint32_t error) {
    auto failed = std::move(streams);
    streams.clear();
    for (auto& [port, weak] : stream_ports) {
      if (auto stream = weak.lock()) {
        failed.push_back(std::move(stream));
      }
    }
    stream_ports.clear();
    looped.clear();
    for (auto& stream : failed) {
      if (stream->connection) {
        stream->connection->Abort();
        stream->connection->TakeOutput();
      }
      stream->reserved = false;
      stream->listening = false;
      stream->pending.clear();
      if (!stream->error) {
        stream->error = error;
      }
      Refresh(*stream);
    }
  }
};

class SystemLink::Port final : public SystemLinkPort {
 public:
  Port(std::shared_ptr<State> state, std::shared_ptr<Inbox> inbox,
       uint16_t port, bool vdp)
      : state_(std::move(state)),
        inbox_(std::move(inbox)),
        port_(port),
        vdp_(vdp) {}
  ~Port() override { Close(); }

  uint16_t port() const override { return port_; }

  // What a blocking Winsock call returns when another thread closes its
  // socket.
  void Close() override { inbox_->Fail(kWsaInterrupted, false); }

  size_t PendingBytes() const override {
    std::lock_guard lock(inbox_->mutex);
    return inbox_->packets.empty() ? 0 : inbox_->packets.front().data.size();
  }

  bool Readable() const override {
    std::lock_guard lock(inbox_->mutex);
    return inbox_->error || !inbox_->packets.empty();
  }

  void SetReadyCallback(std::function<void()> callback) override {
    bool readable;
    {
      std::lock_guard lock(inbox_->mutex);
      inbox_->ready = callback;
      readable = inbox_->error || !inbox_->packets.empty();
    }
    if (readable && callback) {
      callback();
    }
  }

  int Send(Endpoint destination, std::span<const uint8_t> bytes,
           uint32_t& error) override {
    {
      std::lock_guard lock(inbox_->mutex);
      if (inbox_->error) {
        error = inbox_->error;
        return -1;
      }
    }
    std::lock_guard lock(state_->mutex);
    if (!state_->transport->healthy()) {
      error = kWsaNetworkDown;
      return -1;
    }
    Packet packet;
    bool sent;
    if (destination.address == kBroadcast) {
      packet.source_port = port_;
      packet.destination_port = destination.port;
      packet.encrypted.assign(bytes.begin(), bytes.end());
      sent = state_->associations.Discover(std::move(packet));
    } else {
      const bool loopback = destination.address == kLoopback;
      auto found = state_->peers.find(destination.address);
      if (!loopback && found == state_->peers.end()) {
        error = kWsaHostUnreachable;
        return -1;
      }
      if (vdp_) {
        auto converted = secure::FromGuestVdp(port_, destination.port, bytes);
        if (!converted) {
          error = kWsaInvalid;
          return -1;
        }
        packet = std::move(*converted);
      } else {
        packet.source_port = port_;
        packet.destination_port = destination.port;
        packet.encrypted.assign(bytes.begin(), bytes.end());
      }
      if (loopback) {
        // No larger than a UDP datagram can be.
        sent = bytes.size() <= kMaxDatagramSize;
        if (sent) {
          state_->Push(packet, kLoopback);
        }
      } else {
        sent = state_->associations.Send(found->second.address,
                                         found->second.key_id,
                                         std::move(packet), Now());
      }
      XELOGD(
          "System Link datagram sent: vdp={}, bytes={}, source port={}, "
          "destination port={}, accepted={}",
          vdp_, bytes.size(), port_, destination.port, sent);
    }
    if (!sent) {
      error = kWsaMessageSize;
      return -1;
    }
    state_->transport->Wake();
    return static_cast<int>(bytes.size());
  }

  int Receive(std::span<uint8_t> buffer, bool peek, Endpoint& source,
              uint32_t wait_ms, uint32_t& error) override {
    std::unique_lock lock(inbox_->mutex);
    auto ready = [&] { return inbox_->error || !inbox_->packets.empty(); };
    if (wait_ms == kWaitForever) {
      inbox_->changed.wait(lock, ready);
    } else if (wait_ms) {
      inbox_->changed.wait_for(lock, std::chrono::milliseconds(wait_ms), ready);
    }
    if (inbox_->packets.empty()) {
      error = inbox_->error ? inbox_->error
              : wait_ms     ? kWsaTimedOut
                            : kWsaWouldBlock;
      return -1;
    }
    const auto& packet = inbox_->packets.front();
    source = packet.source;
    const size_t size = std::min(buffer.size(), packet.data.size());
    std::copy_n(packet.data.begin(), size, buffer.begin());
    const bool truncated = size < packet.data.size();
    if (!peek) {
      inbox_->packets.pop_front();
    }
    if (truncated) {
      error = kWsaMessageSize;
      return -1;
    }
    return static_cast<int>(size);
  }

 private:
  std::shared_ptr<State> state_;
  std::shared_ptr<Inbox> inbox_;
  uint16_t port_;
  bool vdp_;
};

class SystemLink::Stream final : public SystemLinkStream {
 public:
  Stream(std::shared_ptr<State> state, std::shared_ptr<StreamState> stream)
      : state_(std::move(state)), stream_(std::move(stream)) {}
  ~Stream() override { Close(false); }

  uint16_t port() const override {
    std::lock_guard lock(state_->mutex);
    return stream_->port;
  }

  std::optional<Endpoint> peer() const override {
    std::lock_guard lock(state_->mutex);
    if (!stream_->connection || !stream_->connection->connected()) {
      return std::nullopt;
    }
    return Endpoint{stream_->remote_address, stream_->remote_port};
  }

  uint32_t Bind(uint16_t port) override {
    std::lock_guard lock(state_->mutex);
    if (stream_->error) {
      return stream_->error;
    }
    if (stream_->port) {
      return kWsaInvalid;
    }
    if (!state_->transport->healthy()) {
      return kWsaNetworkDown;
    }
    return state_->Reserve(stream_, port);
  }

  uint32_t Listen(int backlog) override {
    std::lock_guard lock(state_->mutex);
    if (stream_->error) {
      return stream_->error;
    }
    if (stream_->connection) {
      return kWsaIsConnected;
    }
    if (!stream_->port) {
      return kWsaInvalid;
    }
    stream_->backlog = size_t(std::clamp(backlog, 1, kBacklogLimit));
    if (!stream_->listening) {
      stream_->listening = true;
      state_->streams.push_back(stream_);
    }
    return 0;
  }

  std::unique_ptr<SystemLinkStream> Accept(Endpoint& peer, uint32_t wait_ms,
                                           uint32_t& error) override {
    std::unique_lock lock(state_->mutex);
    const auto deadline = Deadline(wait_ms);
    while (true) {
      if (stream_->error) {
        error = stream_->error;
        return nullptr;
      }
      if (!stream_->listening) {
        error = kWsaInvalid;
        return nullptr;
      }
      auto& pending = stream_->pending;
      auto ready = std::find_if(
          pending.begin(), pending.end(),
          [](auto& entry) { return entry->connection->connected(); });
      if (ready != pending.end()) {
        auto accepted = std::move(*ready);
        pending.erase(ready);
        peer = {accepted->remote_address, accepted->remote_port};
        state_->Refresh(*stream_, kStreamAccept);
        return std::make_unique<Stream>(state_, std::move(accepted));
      }
      if (!Wait(lock, wait_ms, deadline)) {
        error = wait_ms ? kWsaTimedOut : kWsaWouldBlock;
        return nullptr;
      }
    }
  }

  uint32_t Connect(Endpoint destination, uint32_t wait_ms) override {
    std::unique_lock lock(state_->mutex);
    if (stream_->error) {
      return stream_->error;
    }
    if (stream_->listening) {
      return kWsaInvalid;
    }
    if (!stream_->connection) {
      if (!state_->transport->healthy()) {
        return kWsaNetworkDown;
      }
      Route route;
      if (destination.address == kLoopback) {
        route.loopback = true;
      } else {
        auto found = state_->peers.find(destination.address);
        if (found == state_->peers.end()) {
          return kWsaHostUnreachable;
        }
        route = {found->second.address, found->second.key_id, false};
      }
      if (!destination.port) {
        return kWsaAddressNotAvailable;
      }
      if (!stream_->port) {
        if (const uint32_t error = state_->Reserve(stream_, 0)) {
          return error;
        }
      }
      XELOGI("System Link TCP: port {} connecting to {:08X}:{}", stream_->port,
             destination.address, destination.port);
      // A socket closed earlier may still be finishing a connection on these
      // ports, which this one replaces.
      for (auto& other : state_->streams) {
        auto& old = other->connection;
        if (other->closed && old && !old->closed() &&
            other->port == stream_->port &&
            other->remote_port == destination.port && other->route == route) {
          old->Abort();
        }
      }
      stream_->route = route;
      stream_->remote_port = destination.port;
      stream_->remote_address = destination.address;
      stream_->connection = tcp::Connection::Open(Now());
      state_->streams.push_back(stream_);
      state_->FlushStreams(Now());
      state_->transport->Wake();
      if (!wait_ms) {
        return kWsaWouldBlock;
      }
    } else if (stream_->connection->connected()) {
      return kWsaIsConnected;
    } else if (stream_->connection->connecting() && !wait_ms) {
      return kWsaAlready;
    }
    const auto deadline = Deadline(wait_ms);
    while (stream_->connection->connecting() && !stream_->error) {
      if (!Wait(lock, wait_ms, deadline)) {
        return kWsaTimedOut;
      }
    }
    if (stream_->error) {
      return stream_->error;
    }
    return stream_->connection->connected()
               ? 0
               : FailureError(stream_->connection->failure());
  }

  int Send(std::span<const uint8_t> data, uint32_t wait_ms,
           uint32_t& error) override {
    std::unique_lock lock(state_->mutex);
    const auto deadline = Deadline(wait_ms);
    while (true) {
      error = Usable();
      if (error) {
        return -1;
      }
      auto& connection = *stream_->connection;
      if (stream_->shut_send) {
        error = kWsaShutdown;
        return -1;
      }
      if (connection.closed()) {
        error = FailureError(connection.failure());
        return -1;
      }
      if (data.empty()) {
        return 0;
      }
      if (const size_t written = connection.Write(data, Now())) {
        state_->FlushStreams(Now());
        state_->Refresh(*stream_);
        state_->transport->Wake();
        return static_cast<int>(written);
      }
      if (!Wait(lock, wait_ms, deadline)) {
        error = wait_ms ? kWsaTimedOut : kWsaWouldBlock;
        return -1;
      }
    }
  }

  int Receive(std::span<uint8_t> buffer, bool peek, uint32_t wait_ms,
              uint32_t& error) override {
    std::unique_lock lock(state_->mutex);
    const auto deadline = Deadline(wait_ms);
    while (true) {
      error = Usable();
      if (error) {
        return -1;
      }
      auto& connection = *stream_->connection;
      if (stream_->shut_receive) {
        error = kWsaShutdown;
        return -1;
      }
      // A reset ends the stream even after the peer's FIN.
      if (connection.failure() != tcp::Failure::None) {
        error = FailureError(connection.failure());
        return -1;
      }
      if (connection.readable()) {
        const size_t read = connection.Read(buffer, peek, Now());
        state_->FlushStreams(Now());
        state_->Refresh(*stream_, kStreamRead);
        state_->transport->Wake();
        return static_cast<int>(read);
      }
      if (connection.finished_receiving() || buffer.empty()) {
        return 0;
      }
      if (connection.closed()) {
        error = kWsaConnectionAborted;
        return -1;
      }
      if (!Wait(lock, wait_ms, deadline)) {
        error = wait_ms ? kWsaTimedOut : kWsaWouldBlock;
        return -1;
      }
    }
  }

  uint32_t Shutdown(int how) override {
    std::lock_guard lock(state_->mutex);
    if (const uint32_t error = Usable()) {
      return error;
    }
    if (how < 0 || how > 2) {
      return kWsaInvalid;
    }
    if (how != 1) {
      stream_->shut_receive = true;
    }
    if (how != 0 && !stream_->shut_send) {
      stream_->shut_send = true;
      stream_->connection->Finish(Now());
      state_->FlushStreams(Now());
      state_->Refresh(*stream_);
      state_->transport->Wake();
    }
    // Calls waiting on this stream now fail.
    stream_->changed.notify_all();
    return 0;
  }

  void Close(bool abort) override {
    std::lock_guard lock(state_->mutex);
    if (stream_->closed) {
      return;
    }
    stream_->closed = true;
    stream_->closed_at = Now();
    stream_->ready = nullptr;
    // Connections nobody accepted are refused.
    for (auto& pending : stream_->pending) {
      pending->connection->Abort();
    }
    stream_->pending.clear();
    stream_->listening = false;
    if (auto& connection = stream_->connection) {
      // Closing with data left to read resets the connection.
      if (abort || connection->readable() || connection->connecting()) {
        connection->Abort();
      } else {
        connection->Finish(Now());
      }
    }
    state_->Release(*stream_);
    if (!stream_->error) {
      stream_->error = kWsaInterrupted;
    }
    stream_->changed.notify_all();
    state_->FlushStreams(Now());
    state_->transport->Wake();
  }

  size_t PendingBytes() const override {
    std::lock_guard lock(state_->mutex);
    return stream_->connection ? stream_->connection->readable() : 0;
  }

  bool Readable() const override {
    std::lock_guard lock(state_->mutex);
    const Readiness readiness = Observe(*stream_);
    return readiness.readable || readiness.acceptable || readiness.ended ||
           readiness.error || stream_->shut_receive;
  }

  bool Writable() const override {
    std::lock_guard lock(state_->mutex);
    return Observe(*stream_).writable;
  }

  bool Failed() const override {
    std::lock_guard lock(state_->mutex);
    return ConnectFailed(*stream_);
  }

  void SetReadyCallback(std::function<void(uint32_t)> callback) override {
    std::lock_guard lock(state_->mutex);
    stream_->ready = std::move(callback);
    if (!stream_->ready) {
      return;
    }
    const Readiness readiness = Observe(*stream_);
    uint32_t events = Level(readiness);
    // How a connect ended is reported once, even to a late arming.
    if ((readiness.connected || readiness.failed) &&
        !stream_->connect_reported) {
      events |= kStreamConnect;
      stream_->connect_reported = true;
    }
    if (events) {
      stream_->ready(events);
    }
  }

 private:
  using Clock = std::chrono::steady_clock;

  static std::optional<Clock::time_point> Deadline(uint32_t wait_ms) {
    if (wait_ms == kWaitForever) {
      return std::nullopt;
    }
    return Clock::now() + std::chrono::milliseconds(wait_ms);
  }

  // Waits for the stream to change. False once no time is left, which the
  // caller only finds after one more look at the stream.
  bool Wait(std::unique_lock<std::mutex>& lock, uint32_t wait_ms,
            const std::optional<Clock::time_point>& deadline) {
    if (!wait_ms) {
      return false;
    }
    if (!deadline) {
      stream_->changed.wait(lock);
      return true;
    }
    if (Clock::now() >= *deadline) {
      return false;
    }
    stream_->changed.wait_until(lock, *deadline);
    return true;
  }

  // What sending and receiving fail with before looking at the data.
  uint32_t Usable() const {
    if (stream_->error) {
      return stream_->error;
    }
    if (!stream_->connection || !stream_->connection->connected()) {
      return ConnectFailed(*stream_)
                 ? FailureError(stream_->connection->failure())
                 : kWsaNotConnected;
    }
    return 0;
  }

  std::shared_ptr<State> state_;
  std::shared_ptr<StreamState> stream_;
};

SystemLink::SystemLink(uint32_t address,
                       std::shared_ptr<secure::PacketTransport> transport,
                       AssociationConfig association) {
  for (size_t i = 0; i < 4; ++i) {
    identity_[i] = uint8_t(address >> (24 - 8 * i));
  }
  const auto mac = transport->identity();
  std::copy(mac.begin(), mac.end(), identity_.begin() + kEnetOffset);
  // Offline, but SGADDR still names the console platform.
  identity_[kPlatformOffset] = kXbox360Platform;
  association.identity = identity_;
  if (association.cross_platform) {
    association.cross_platform->address = address;
  }
  state_ =
      std::make_shared<State>(std::move(transport), std::move(association));
}

SystemLink::~SystemLink() {
  // Bound ports keep the state alive, but nothing may run it any more.
  state_->worker.request_stop();
  state_->transport->Wake();
  state_->worker.join();
  std::shared_ptr<secure::PacketTransport> transport;
  std::function<void(std::span<const uint8_t>)> receiver;
  {
    // Lookups end without writing results once the System Link is gone.
    std::lock_guard lock(state_->mutex);
    state_->qos_lookups.clear();
    // The transport closes and the frame receiver goes even while ports and
    // streams keep the state alive.
    transport =
        std::exchange(state_->transport, std::make_shared<ClosedTransport>());
    receiver = std::exchange(state_->frame_receiver, nullptr);
  }
  Reset();
}

uint64_t SystemLink::MachineId(const Address& address) {
  uint64_t id = 0;
  for (size_t i = 0; i < 6; ++i) {
    id = (id << 8) | address[kEnetOffset + i];
  }
  return 0xFA00000000000000ull | id;
}

void SystemLink::CreateKey(KeyId& id, Key& key, bool cross_platform) {
  Random(id);
  // XNKID_SYSTEM_LINK.
  id[0] = (id[0] & 0x0F) | secure::SystemLinkKeyType(cross_platform);
  Random(key);
}

uint32_t SystemLink::RegisterKey(const KeyId& id, const Key& key) {
  std::lock_guard lock(state_->mutex);
  if (!ValidKeyId(id, state_->cross_platform.has_value())) {
    return kWsaInvalid;
  }
  if (state_->keys.contains(id)) {
    return kWsaAlready;
  }
  if (state_->keys.size() >= kKeyLimit) {
    return kWsaNoBuffers;
  }
  state_->keys.emplace(id, key);
  return 0;
}

uint32_t SystemLink::UnregisterKey(const KeyId& id) {
  std::lock_guard lock(state_->mutex);
  if (!state_->keys.erase(id)) {
    return kWsaInvalid;
  }
  state_->qos_listens.erase(id);
  // Retiring the associations also drops the datagrams they queued.
  state_->associations.Unregister(id, Now());
  state_->transport->Wake();
  state_->Deliver();
  std::erase_if(state_->peers,
                [&](const auto& entry) { return entry.second.key_id == id; });
  return 0;
}

bool SystemLink::GetKey(const KeyId& id, Key& key) const {
  key = {};
  std::lock_guard lock(state_->mutex);
  auto found = state_->keys.find(id);
  if (found == state_->keys.end()) {
    return false;
  }
  key = found->second;
  return true;
}

uint32_t SystemLink::Map(const Address& address, const KeyId& id,
                         uint32_t& secure) {
  std::lock_guard lock(state_->mutex);
  return state_->Map(address, id, secure);
}

uint32_t SystemLink::Reverse(uint32_t secure, Address& address,
                             KeyId& id) const {
  address = {};
  id = {};
  if (secure == kLoopback) {
    address = state_->identity;
    return 0;
  }
  std::lock_guard lock(state_->mutex);
  auto found = state_->peers.find(secure);
  if (found == state_->peers.end()) {
    return kWsaInvalid;
  }
  address = found->second.address;
  id = found->second.key_id;
  return 0;
}

uint32_t SystemLink::Unmap(uint32_t secure) {
  std::lock_guard lock(state_->mutex);
  auto found = state_->peers.find(secure);
  if (found == state_->peers.end()) {
    return kWsaInvalid;
  }
  const Peer peer = found->second;
  // Retiring the association removes the mapping and drops what it queued, a
  // peer that never connected has neither.
  state_->associations.Close(peer.address, peer.key_id, Now());
  state_->transport->Wake();
  state_->Deliver();
  state_->peers.erase(secure);
  return 0;
}

uint32_t SystemLink::Connect(uint32_t secure) {
  std::lock_guard lock(state_->mutex);
  auto found = state_->peers.find(secure);
  if (found == state_->peers.end()) {
    return kWsaInvalid;
  }
  if (!state_->transport->healthy()) {
    return kWsaNetworkDown;
  }
  if (!state_->associations.Connect(found->second.address, found->second.key_id,
                                    Now())) {
    return kWsaNoBuffers;
  }
  state_->transport->Wake();
  return 0;
}

uint32_t SystemLink::ConnectStatus(uint32_t secure) const {
  std::lock_guard lock(state_->mutex);
  auto found = state_->peers.find(secure);
  // Only a mapped peer on a live LAN has a status. Every other address reads
  // as lost, loopback and the broadcast address included.
  if (found == state_->peers.end() || !state_->transport->healthy()) {
    return kConnectStatusLost;
  }
  return static_cast<uint32_t>(
      state_->associations.Status(found->second.address, found->second.key_id));
}

std::vector<Peer> SystemLink::peers() const {
  std::lock_guard lock(state_->mutex);
  std::vector<Peer> peers;
  for (const auto& [secure, peer] : state_->peers) {
    peers.push_back(peer);
  }
  return peers;
}

uint32_t SystemLink::QosListen(const KeyId& id, uint32_t flags,
                               std::span<const uint8_t> data) {
  std::lock_guard lock(state_->mutex);
  if (!state_->keys.contains(id)) {
    return kWsaInvalid;
  }
  if (flags & kQosListenRelease) {
    state_->qos_listens.erase(id);
    return 0;
  }
  auto& listen = state_->qos_listens[id];
  if (flags & kQosListenSetData) {
    listen.data.assign(data.begin(),
                       data.begin() + std::min(data.size(), kQosDataLimit));
  }
  if (flags & (kQosListenEnable | kQosListenDisable)) {
    listen.enabled = flags & kQosListenEnable;
  }
  return 0;
}

void SystemLink::QosLookup(
    uint32_t tag, std::vector<QosTarget> targets,
    std::function<void(size_t target, const QosResult& result)> done) {
  std::lock_guard lock(state_->mutex);
  State::QosLookup lookup;
  Random(lookup.nonce);
  const auto mac = Enet(state_->identity);
  for (size_t i = 0; i < targets.size(); ++i) {
    const auto& target = targets[i];
    const auto host = Enet(target.address);
    if (host == mac) {
      done(i, state_->LocalQos(target.id));
      continue;
    }
    // Only System Link keys cross the LAN.
    // Replies name targets in 16 bits.
    const auto& cross_platform = state_->cross_platform;
    if (!secure::SystemLinkKeyId(target.id, cross_platform.has_value()) ||
        (cross_platform &&
         !secure::SubnetPeer(target.address, *cross_platform)) ||
        host == secure::Mac{} || i > 0xFFFF || !state_->transport->healthy()) {
      done(i, {});
      continue;
    }
    lookup.probing.push_back(
        {i, target.address, target.id,
         secure::DeriveRegistration(state_->broadcast_auth, target.id,
                                    target.key)});
  }
  if (lookup.probing.empty()) {
    return;
  }
  lookup.done = std::move(done);
  state_->qos_lookups[tag] = std::move(lookup);
  state_->transport->Wake();
}

uint32_t SystemLink::SetSystemLinkPort(uint16_t port) {
  const auto resolved = ResolveSystemLinkPort(port);
  if (!resolved) {
    return kWsaAddressInUse;
  }
  std::lock_guard lock(state_->mutex);
  if (auto& cross_platform = state_->cross_platform) {
    if (*resolved != cross_platform->port) {
      if (!state_->transport->SetPort(*resolved)) {
        return kWsaAddressInUse;
      }
      XELOGI("System Link: moved to UDP port {}", *resolved);
    }
    cross_platform->port = *resolved;
  }
  state_->system_link_port = *resolved;
  return 0;
}

uint16_t SystemLink::system_link_port() const {
  std::lock_guard lock(state_->mutex);
  return state_->system_link_port;
}

std::optional<uint16_t> SystemLink::ResolveSystemLinkPort(uint16_t port) {
  if (!port) {
    return secure::kSystemLinkPort;
  }
  if (port <= kLastReservedPort) {
    return std::nullopt;
  }
  return port;
}

bool SystemLink::cross_platform() const {
  std::lock_guard lock(state_->mutex);
  return state_->cross_platform.has_value();
}

uint32_t SystemLink::BroadcastVersionStatus(bool reset) {
  std::lock_guard lock(state_->mutex);
  return state_->associations.BroadcastVersionStatus(reset);
}

void SystemLink::CancelQos(uint32_t tag) {
  std::lock_guard lock(state_->mutex);
  state_->qos_lookups.erase(tag);
}

std::unique_ptr<SystemLinkPort> SystemLink::Bind(uint16_t port, bool vdp,
                                                 uint32_t& error) {
  std::lock_guard lock(state_->mutex);
  if (!state_->transport->healthy()) {
    error = kWsaNetworkDown;
    return nullptr;
  }
  std::erase_if(state_->ports, [](const auto& entry) {
    auto inbox = entry.second.lock();
    if (!inbox) {
      return true;
    }
    std::lock_guard lock(inbox->mutex);
    return inbox->error != 0;
  });
  if (state_->ports.size() >= kPortLimit) {
    error = kWsaNoBuffers;
    return nullptr;
  }
  if (!port) {
    do {
      port = state_->next_port++;
      if (!state_->next_port) {
        state_->next_port = kFirstDynamicPort;
      }
    } while (state_->ports.contains({port, vdp}));
  } else if (state_->ports.contains({port, vdp})) {
    error = kWsaAddressInUse;
    return nullptr;
  }
  auto inbox = std::make_shared<Inbox>();
  state_->ports[{port, vdp}] = inbox;
  return std::make_unique<Port>(state_, std::move(inbox), port, vdp);
}

std::unique_ptr<SystemLinkStream> SystemLink::CreateStream(uint32_t& error) {
  std::lock_guard lock(state_->mutex);
  if (!state_->transport->healthy()) {
    error = kWsaNetworkDown;
    return nullptr;
  }
  return std::make_unique<Stream>(state_, std::make_shared<StreamState>());
}

void SystemLink::SetFrameReceiver(
    std::function<void(std::span<const uint8_t>)> receive) {
  // The old receiver goes once the lock is released.
  std::lock_guard lock(state_->mutex);
  std::swap(state_->frame_receiver, receive);
}

uint32_t SystemLink::SendFrame(std::span<const uint8_t> frame) {
  std::lock_guard lock(state_->mutex);
  if (!state_->transport->healthy()) {
    return kWsaNetworkDown;
  }
  if (frame.size() < kEthernetHeaderSize ||
      frame.size() > kEthernetHeaderSize + state_->transport->mtu() ||
      state_->frames_out.size() >= kOutgoingFrameLimit) {
    return kWsaNoBuffers;
  }
  state_->frames_out.emplace_back(frame.begin(), frame.end());
  state_->transport->Wake();
  return 0;
}

void SystemLink::Reset() {
  std::lock_guard lock(state_->mutex);
  state_->associations.Reset();
  state_->keys.clear();
  state_->peers.clear();
  state_->qos_listens.clear();
  state_->FinishQos();
  state_->qos_pairs.clear();
  state_->qos_outbound.clear();
  state_->CloseInboxes(kWsaNetworkDown);
  state_->FailStreams(kWsaNetworkDown);
}

}  // namespace xe::net
