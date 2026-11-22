/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xenia/net/system_link.h"
#include "xenia/net/xnet_tcp.h"

#include "third_party/catch/include/catch.hpp"
#include "third_party/mbedtls/include/mbedtls/sha1.h"

namespace xe::net::secure::test {
namespace {

struct Wire {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::vector<uint8_t>> frames[2];
};

class TestTransport : public PacketTransport {
 public:
  TestTransport(std::shared_ptr<Wire> wire, unsigned side)
      : wire_(std::move(wire)), side_(side) {}

  Mac identity() const override {
    return {2, 17, 34, 51, 68, uint8_t(side_ + 1)};
  }
  uint32_t mtu() const override { return 1500; }
  bool healthy() const override { return healthy_; }

  void Disconnect() {
    healthy_ = false;
    wire_->changed.notify_all();
  }

  // Delivers every frame sent from now on twice.
  void Duplicate() { duplicate_ = true; }

  // Receives from now on ignore their timeout, only a frame, Wake() or
  // Disconnect() ends them.
  void Stall() { stall_ = true; }
  int stalled() const { return stalled_; }

  // Sends wait from now on until Release() so frames pile up in the System
  // Link meanwhile.
  void Hold() {
    std::lock_guard lock(wire_->mutex);
    hold_ = true;
  }
  void Release() {
    {
      std::lock_guard lock(wire_->mutex);
      hold_ = false;
    }
    wire_->changed.notify_all();
  }
  int held() const { return held_; }

  void Wake() override {
    {
      std::lock_guard lock(wire_->mutex);
      woken_ = true;
    }
    wire_->changed.notify_all();
  }

  int Send(std::span<const uint8_t> bytes) override {
    std::unique_lock lock(wire_->mutex);
    if (hold_) {
      ++held_;
      wire_->changed.wait(lock, [&] { return !hold_ || !healthy_; });
    }
    for (int i = duplicate_ ? 2 : 1; i; --i) {
      wire_->frames[1 - side_].emplace_back(bytes.begin(), bytes.end());
    }
    sent_.emplace_back(bytes.begin(), bytes.end());
    wire_->changed.notify_all();
    return static_cast<int>(bytes.size());
  }

  // Every frame this side sent.
  std::vector<std::vector<uint8_t>> Sent() const {
    std::lock_guard lock(wire_->mutex);
    return sent_;
  }

  bool SetPort(uint16_t port) override {
    std::lock_guard lock(wire_->mutex);
    ports_.push_back(port);
    return true;
  }
  // Every port SetPort moved to.
  std::vector<uint16_t> Ports() const {
    std::lock_guard lock(wire_->mutex);
    return ports_;
  }

  // Delivers |frame| to this side as if it came over the wire.
  void Inject(std::vector<uint8_t> frame) {
    {
      std::lock_guard lock(wire_->mutex);
      wire_->frames[side_].push_back(std::move(frame));
    }
    wire_->changed.notify_all();
  }

  bool Receive(std::vector<uint8_t>& bytes, uint32_t timeout_ms) override {
    std::unique_lock lock(wire_->mutex);
    auto ready = [&] {
      return !healthy_ || woken_ || !wire_->frames[side_].empty();
    };
    if (stall_) {
      ++stalled_;
      wire_->changed.wait(lock, ready);
    } else {
      wire_->changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              ready);
    }
    woken_ = false;
    if (!healthy_ || wire_->frames[side_].empty()) {
      return false;
    }
    bytes = std::move(wire_->frames[side_].front());
    wire_->frames[side_].pop_front();
    return true;
  }

 private:
  std::shared_ptr<Wire> wire_;
  unsigned side_;
  std::atomic<bool> healthy_{true};
  std::atomic<bool> duplicate_{false};
  std::atomic<bool> stall_{false};
  std::atomic<int> stalled_{0};
  std::atomic<int> held_{0};
  // Guarded by the wire's mutex.
  bool woken_ = false;
  bool hold_ = false;
  std::vector<std::vector<uint8_t>> sent_;
  std::vector<uint16_t> ports_;
};

struct Pair {
  std::shared_ptr<Wire> wire = std::make_shared<Wire>();
  KeyId id{1, 3, 5, 7, 9, 11, 13, 15};
  Key key{2, 4, 6, 8, 10, 12, 14, 16};
  ProtectionKeys broadcast;
  std::shared_ptr<TestTransport> transports[2];
  std::unique_ptr<SystemLink> a, b;

  Pair() {
    auto derived = DeriveBroadcast(key, [&](auto input, auto& digest) {
      digest = HmacSha1(key, {input});
      return true;
    });
    REQUIRE(derived);
    broadcast = *derived;
    a = Make(0, broadcast);
    b = Make(1, broadcast);
    REQUIRE(a->RegisterKey(id, key) == 0);
    REQUIRE(b->RegisterKey(id, key) == 0);
  }

  std::unique_ptr<SystemLink> Make(unsigned side,
                                   const ProtectionKeys& broadcast,
                                   const Metadata& metadata = {}) {
    auto transport = std::make_shared<TestTransport>(wire, side);
    transports[side] = transport;
    AssociationConfig association;
    association.broadcast = broadcast;
    association.metadata = metadata;
    association.mtu = transport->mtu();
    // Documentation range addresses.
    return std::make_unique<SystemLink>(0xC0000201 + side, transport,
                                        std::move(association));
  }
};

std::unique_ptr<SystemLinkPort> Bind(SystemLink& link, uint16_t port,
                                     bool vdp) {
  uint32_t error = 0;
  auto bound = link.Bind(port, vdp, error);
  CHECK(error == 0);
  return bound;
}

bool WaitFor(const std::function<bool()>& condition) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

bool Ready(SystemLinkPort& port) {
  return WaitFor([&] { return port.Readable(); });
}

std::unique_ptr<SystemLinkStream> Stream(SystemLink& link) {
  uint32_t error = 0;
  auto stream = link.CreateStream(error);
  CHECK(error == 0);
  return stream;
}

std::unique_ptr<SystemLinkStream> Listen(SystemLink& link, uint16_t port) {
  auto stream = Stream(link);
  REQUIRE(stream);
  REQUIRE(stream->Bind(port) == 0);
  REQUIRE(stream->Listen(5) == 0);
  return stream;
}

constexpr uint32_t kWait = 5000;

// Reads until |size| bytes arrived or the stream ends.
std::vector<uint8_t> ReceiveAll(SystemLinkStream& stream, size_t size) {
  std::vector<uint8_t> received;
  std::array<uint8_t, 1500> buffer;
  while (received.size() < size) {
    uint32_t error = 0;
    const int read = stream.Receive(buffer, false, kWait, error);
    if (read <= 0) {
      break;
    }
    received.insert(received.end(), buffer.begin(), buffer.begin() + read);
  }
  return received;
}

// Runs on other threads too so it asserts nothing itself.
bool SendAll(SystemLinkStream& stream, std::span<const uint8_t> data) {
  while (!data.empty()) {
    uint32_t error = 0;
    const int sent = stream.Send(data, kWait, error);
    if (sent <= 0) {
      return false;
    }
    data = data.subspan(sent);
  }
  return true;
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = uint8_t(seed + i * 13);
  }
  return bytes;
}

// XNADDR.abEnet.
constexpr size_t kEnetOffset = 10;

std::vector<uint8_t> Hex(std::string_view text) {
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i + 1 < text.size(); i += 2) {
    bytes.push_back(
        uint8_t(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
  }
  return bytes;
}

constexpr uint32_t kBroadcastAddress = 0xFFFFFFFF;
constexpr uint32_t kSubnetMask = 0xFFFFFF00;
// 10.0.1.12, on the console's subnet.
constexpr uint32_t kHostAddress = 0x0A00010C;
// 10.0.1.172.
constexpr uint32_t kConsoleAddress = 0x0A0001AC;

// The console at kConsoleAddress looking for Lost Planet Colonies games, as
// captured on its LAN with IpMachine() of its address as the source MAC, and
// the datagram the title broadcast.
constexpr std::string_view kConsoleSearch =
    "ffffffffffff02000a0001ac080045000061ee7300004011806d0a0001acffffffff0c02"
    "0c02004dc580870000007d88ecfd11e853b9b1c5e19d1c64f96311fb2c27af4f0ff946e3"
    "ca9a79c8e4444ac5f1a2a009c450de074343030000000097442005446853f13e1831f7e2"
    "132a1d";
constexpr std::string_view kConsoleSearchData =
    "000C41FF00000000FFFFFFFF82B600FFFFFFFFFF82AEF8884C60545329C0D9B500000000";
// Its next search, with a nonce of its own.
constexpr std::string_view kConsoleSearchAgain =
    "ffffffffffff02000a0001ac080045000061ee7600004011806a0a0001acffffffff0c02"
    "0c02004dad278700000086a20c272fc9ac9895d4b1f3611bc3b7c46b22b54dc83384c722"
    "7c8b3ffed0570eb0164309d42a82de074343030000000097442005693a4c2d2e53416f64"
    "bc6e61";
constexpr std::string_view kConsoleSearchAgainData =
    "000C41FF00000000FFFFFFFF82B600FFFFFFFFFF82AEF888865DBE9F1AE51EA400000000";
// The title's LAN key encrypted with key 0x43, as the console computed it.
constexpr Key kColoniesEncryptedLanKey{0xCF, 0x04, 0x4B, 0xED, 0x8C, 0xC1,
                                       0xFF, 0x2E, 0x00, 0x70, 0x77, 0xA9,
                                       0xB7, 0x05, 0xDE, 0xD6};

// A frame as another sender would have sent it. The UDP checksum is optional
// so it is left out.
std::vector<uint8_t> Readdressed(std::vector<uint8_t> frame, uint32_t source,
                                 uint32_t destination,
                                 uint16_t source_port = kSystemLinkPort,
                                 uint16_t destination_port = kSystemLinkPort) {
  auto ip = std::span(frame).subspan(14, 20);
  for (size_t i = 0; i < 4; ++i) {
    ip[12 + i] = uint8_t(source >> (24 - 8 * i));
    ip[16 + i] = uint8_t(destination >> (24 - 8 * i));
  }
  ip[10] = ip[11] = 0;
  uint32_t sum = 0;
  for (size_t i = 0; i < ip.size(); i += 2) {
    sum += uint32_t(ip[i]) << 8 | ip[i + 1];
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  ip[10] = uint8_t(~sum >> 8);
  ip[11] = uint8_t(~sum);
  auto udp = std::span(frame).subspan(34, 8);
  udp[0] = uint8_t(source_port >> 8);
  udp[1] = uint8_t(source_port);
  udp[2] = uint8_t(destination_port >> 8);
  udp[3] = uint8_t(destination_port);
  udp[6] = udp[7] = 0;
  return frame;
}

struct FrameAddresses {
  uint32_t source = 0, destination = 0;
  uint16_t source_port = 0, destination_port = 0;
  bool operator==(const FrameAddresses&) const = default;
};

FrameAddresses AddressesOf(std::span<const uint8_t> frame) {
  auto word = [&](size_t offset) {
    return uint16_t(frame[offset] << 8 | frame[offset + 1]);
  };
  return {uint32_t(word(26)) << 16 | word(28),
          uint32_t(word(30)) << 16 | word(32), word(34), word(36)};
}

// System Links of a title with cross-platform System Link on 10.0.1.0/24.
struct CrossPlatformPair {
  std::shared_ptr<Wire> wire = std::make_shared<Wire>();
  KeyId id{0x41, 3, 5, 7, 9, 11, 13, 15};
  Key key{2, 4, 6, 8, 10, 12, 14, 16};
  std::shared_ptr<TestTransport> transports[2];
  std::unique_ptr<SystemLink> a, b;

  CrossPlatformPair() {
    a = Make(0);
    b = Make(1);
    REQUIRE(a->RegisterKey(id, key) == 0);
    REQUIRE(b->RegisterKey(id, key) == 0);
  }

  std::unique_ptr<SystemLink> Make(unsigned side) {
    auto derived =
        DeriveBroadcast(kColoniesEncryptedLanKey, [](auto input, auto& digest) {
          return !mbedtls_sha1(input.data(), input.size(), digest.data());
        });
    REQUIRE(derived);
    auto transport = std::make_shared<TestTransport>(wire, side);
    transports[side] = transport;
    AssociationConfig association;
    association.broadcast = *derived;
    association.mtu = transport->mtu();
    association.cross_platform = CrossPlatform{0, kSubnetMask};
    return std::make_unique<SystemLink>(kHostAddress + side, transport,
                                        std::move(association));
  }
};

struct QosResults {
  std::mutex mutex;
  std::map<size_t, QosResult> results;
  // Assertions only run on the test thread.
  size_t repeats = 0;

  ~QosResults() { CHECK(repeats == 0); }

  std::function<void(size_t, const QosResult&)> Done() {
    return [this](size_t target, const QosResult& result) {
      std::lock_guard lock(mutex);
      if (!results.emplace(target, result).second) {
        ++repeats;
      }
    };
  }
  size_t Count() {
    std::lock_guard lock(mutex);
    return results.size();
  }
  QosResult Get(size_t target) {
    std::lock_guard lock(mutex);
    return results[target];
  }
};

}  // namespace

TEST_CASE("System Link ports deliver broadcasts and secure datagrams",
          "[net][system_link]") {
  for (bool reverse : {false, true}) {
    Pair pair;
    auto& sender = reverse ? *pair.b : *pair.a;
    auto& receiver = reverse ? *pair.a : *pair.b;
    auto tx = Bind(sender, 1001, false);
    auto rx = Bind(receiver, 1001, false);
    REQUIRE(tx);
    REQUIRE(rx);
    uint32_t error = 0;
    CHECK_FALSE(receiver.Bind(1001, false, error));
    CHECK(error == kWsaAddressInUse);
    CHECK_FALSE(rx->Readable());
    std::atomic<int> wakeups{0};
    rx->SetReadyCallback([&] { ++wakeups; });
    CHECK(wakeups.load() == 0);

    const std::vector<uint8_t> data{0, 1, 2, 3, 4, 5, 6};
    const int data_size = static_cast<int>(data.size());
    REQUIRE(tx->Send({0xFFFFFFFF, 1001}, data, error) == data_size);
    REQUIRE(WaitFor([&] { return wakeups.load() == 1; }));
    CHECK(rx->Readable());
    CHECK(rx->PendingBytes() == data.size());
    std::array<uint8_t, 32> buffer{};
    Endpoint source;
    REQUIRE(rx->Receive(buffer, true, source, 0, error) == data_size);
    CHECK(source == Endpoint{1, 1001});
    CHECK(rx->Readable());
    CHECK(rx->Receive(std::span(buffer).first(2), true, source, 0, error) ==
          -1);
    CHECK(error == kWsaMessageSize);
    CHECK(rx->PendingBytes() == data.size());
    REQUIRE(rx->Receive(buffer, false, source, 0, error) == data_size);
    CHECK_FALSE(rx->Readable());
    CHECK(rx->Receive(buffer, false, source, 0, error) == -1);
    CHECK(error == kWsaWouldBlock);
    CHECK(rx->Receive(buffer, false, source, 10, error) == -1);
    CHECK(error == kWsaTimedOut);
    rx->SetReadyCallback(nullptr);

    uint32_t address;
    REQUIRE(sender.Map(receiver.identity(), pair.id, address) == 0);
    REQUIRE(sender.Connect(address) == 0);
    REQUIRE(tx->Send({address, 1001}, data, error) == data_size);
    REQUIRE(Ready(*rx));
    REQUIRE(rx->Receive(buffer, false, source, 0, error) == data_size);
    CHECK(std::equal(data.begin(), data.end(), buffer.begin()));
    CHECK(sender.ConnectStatus(address) == 2);
    REQUIRE(receiver.peers().size() == 1);
    Address peer;
    KeyId id;
    REQUIRE(receiver.Reverse(source.address, peer, id) == 0);
    CHECK(peer == sender.identity());
    CHECK(id == pair.id);

    REQUIRE(rx->Send({source.address, 1001}, data, error) == data_size);
    REQUIRE(Ready(*tx));
    CHECK(tx->PendingBytes() == data.size());
    // Unmapping retires the association and drops what it queued.
    REQUIRE(sender.Unmap(address) == 0);
    CHECK(tx->PendingBytes() == 0);
    CHECK_FALSE(tx->Readable());
    CHECK(sender.ConnectStatus(address) == 3);
    tx->Close();
    rx->Close();
  }
}

TEST_CASE("System Link VDP ports keep the guest prefix", "[net][system_link]") {
  Pair pair;
  auto tx = Bind(*pair.a, 1000, true);
  auto rx = Bind(*pair.b, 1000, true);
  REQUIRE(tx);
  REQUIRE(rx);
  uint32_t address;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  const std::vector<uint8_t> vdp{0, 3, 10, 20, 30, 40, 50};
  uint32_t error = 0;
  REQUIRE(tx->Send({address, 1000}, vdp, error) ==
          static_cast<int>(vdp.size()));
  REQUIRE(Ready(*rx));
  std::array<uint8_t, 32> buffer{};
  Endpoint source;
  REQUIRE(rx->Receive(buffer, false, source, 0, error) ==
          static_cast<int>(vdp.size()));
  CHECK(std::equal(vdp.begin(), vdp.end(), buffer.begin()));
  // The prefix claims more encrypted bytes than follow it.
  const std::vector<uint8_t> short_vdp{0, 9, 1, 2};
  CHECK(tx->Send({address, 1000}, short_vdp, error) == -1);
  CHECK(error == kWsaInvalid);
}

TEST_CASE("System Link ports pick a free port and unblock on close",
          "[net][system_link]") {
  Pair pair;
  auto auto_bound = Bind(*pair.a, 0, false);
  REQUIRE(auto_bound);
  CHECK(auto_bound->port() >= 49152);
  auto rx = Bind(*pair.b, 1000, false);
  REQUIRE(rx);
  uint32_t error = 0;
  auto blocked = std::async(std::launch::async, [&] {
    uint8_t byte;
    Endpoint source;
    return rx->Receive(std::span(&byte, 1), false, source,
                       SystemLinkPort::kWaitForever, error);
  });
  // Lets the receive start waiting, the result is the same either way.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  rx->Close();
  REQUIRE(blocked.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  CHECK(blocked.get() == -1);
  CHECK(error == kWsaInterrupted);
  // A closed port frees its number.
  CHECK(Bind(*pair.b, 1000, false));
}

TEST_CASE("System Link releases its transport while ports outlive it",
          "[net][system_link]") {
  Pair pair;
  auto port = Bind(*pair.a, 1000, false);
  auto listener = Listen(*pair.a, 1003);
  auto stream = Stream(*pair.a);
  std::weak_ptr<TestTransport> transport = pair.transports[0];
  pair.transports[0].reset();
  pair.a.reset();
  CHECK(transport.expired());
  // They fail as if the LAN went away.
  const uint8_t byte = 0;
  uint32_t error = 0;
  CHECK(port->Send({0xFFFFFFFF, 1000}, std::span(&byte, 1), error) == -1);
  CHECK(error == kWsaNetworkDown);
  CHECK(stream->Bind(1004) == kWsaNetworkDown);
  listener->Close(false);
}

TEST_CASE("System Link delivers a frame that arrives twice once",
          "[net][system_link]") {
  Pair pair;
  pair.transports[0]->Duplicate();
  auto tx = Bind(*pair.a, 1001, false);
  auto rx = Bind(*pair.b, 1001, false);
  REQUIRE(tx);
  REQUIRE(rx);
  const std::array<uint8_t, 3> data{1, 3, 5};
  uint32_t error = 0;
  REQUIRE(tx->Send({0xFFFFFFFF, 1001}, data, error) == 3);
  std::array<uint8_t, 16> buffer{};
  Endpoint source;
  REQUIRE(rx->Receive(buffer, false, source, 2000, error) == 3);
  CHECK(rx->Receive(buffer, false, source, 200, error) == -1);
  CHECK(error == kWsaTimedOut);
}

TEST_CASE("System Link sends without waiting for a frame to arrive",
          "[net][system_link]") {
  Pair pair;
  auto tx = Bind(*pair.a, 1001, false);
  auto rx = Bind(*pair.b, 1001, false);
  REQUIRE(tx);
  REQUIRE(rx);
  // The sender's receive now ends only on a frame or a wake.
  pair.transports[0]->Stall();
  CHECK(WaitFor([&] { return pair.transports[0]->stalled() > 0; }));
  const std::vector<uint8_t> data{1, 2, 3};
  uint32_t error = 0;
  CHECK(tx->Send({0xFFFFFFFF, 1001}, data, error) == 3);
  CHECK(Ready(*rx));
  pair.transports[0]->Disconnect();
}

// An original Xbox asks who has 10.0.0.3, which isn't XNet at all.
const std::vector<uint8_t> kArpRequest{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x50, 0xF2, 1, 2,
    3,    0x08, 0x06, 0,    1,    0x08, 0,    6,    4,    0, 1,
    0x00, 0x50, 0xF2, 1,    2,    3,    10,   0,    0,    2, 0,
    0,    0,    0,    0,    0,    10,   0,    0,    3};

TEST_CASE("System Link hands a frame receiver every frame that arrives",
          "[net][system_link]") {
  Pair pair;
  std::mutex mutex;
  std::vector<std::vector<uint8_t>> received;
  pair.a->SetFrameReceiver([&](std::span<const uint8_t> frame) {
    std::lock_guard lock(mutex);
    received.emplace_back(frame.begin(), frame.end());
  });
  auto count = [&] {
    std::lock_guard lock(mutex);
    return received.size();
  };
  auto rx = Bind(*pair.a, 1001, false);
  auto tx = Bind(*pair.b, 1001, false);
  REQUIRE(rx);
  REQUIRE(tx);
  pair.transports[0]->Inject(kArpRequest);
  pair.transports[0]->Inject(kArpRequest);
  const std::vector<uint8_t> data{1, 2, 3};
  uint32_t error = 0;
  REQUIRE(tx->Send({0xFFFFFFFF, 1001}, data, error) == 3);
  // The System Link still reads the frames it gets.
  REQUIRE(Ready(*rx));
  REQUIRE(WaitFor([&] { return count() == 3; }));
  {
    std::lock_guard lock(mutex);
    CHECK(received[0] == kArpRequest);
    CHECK(received[1] == kArpRequest);
    CHECK(received[2] == pair.transports[1]->Sent().back());
  }

  pair.a->SetFrameReceiver(nullptr);
  std::array<uint8_t, 16> buffer{};
  Endpoint source;
  REQUIRE(rx->Receive(buffer, false, source, 0, error) == 3);
  pair.transports[0]->Inject(kArpRequest);
  REQUIRE(tx->Send({0xFFFFFFFF, 1001}, data, error) == 3);
  REQUIRE(Ready(*rx));
  CHECK(count() == 3);
}

TEST_CASE("System Link releases its frame receiver while ports outlive it",
          "[net][system_link]") {
  Pair pair;
  auto port = Bind(*pair.a, 1000, false);
  auto token = std::make_shared<int>();
  std::weak_ptr<int> receiver = token;
  pair.a->SetFrameReceiver([token](std::span<const uint8_t>) {});
  token.reset();
  pair.a.reset();
  CHECK(receiver.expired());
}

TEST_CASE("System Link sends the frames it is handed as they are",
          "[net][system_link]") {
  Pair pair;
  // The receive now ends only on a frame or a wake.
  pair.transports[0]->Stall();
  REQUIRE(WaitFor([&] { return pair.transports[0]->stalled() > 0; }));
  CHECK(pair.a->SendFrame(kArpRequest) == 0);
  CHECK(WaitFor([&] {
    auto sent = pair.transports[0]->Sent();
    return !sent.empty() && sent.back() == kArpRequest;
  }));
  // Shorter than an Ethernet header, longer than the MTU allows.
  CHECK(pair.a->SendFrame(std::span(kArpRequest).first(13)) == kWsaNoBuffers);
  CHECK(pair.a->SendFrame(std::vector<uint8_t>(14 + 1501)) == kWsaNoBuffers);
  CHECK(pair.a->SendFrame(std::vector<uint8_t>(14 + 1500)) == 0);
  pair.transports[0]->Disconnect();
}

TEST_CASE("System Link refuses frames once too many wait to go out",
          "[net][system_link]") {
  Pair pair;
  pair.transports[0]->Hold();
  bool refused = false;
  for (int i = 0; i < 1000 && !refused; ++i) {
    refused = pair.a->SendFrame(kArpRequest) == kWsaNoBuffers;
  }
  CHECK(refused);
  pair.transports[0]->Release();
}

TEST_CASE("System Link refuses frames once the LAN is down",
          "[net][system_link]") {
  Pair pair;
  pair.transports[0]->Disconnect();
  CHECK(pair.a->SendFrame(kArpRequest) == kWsaNetworkDown);
}

TEST_CASE("System Link QoS lookups bring back the data a host listens with",
          "[net][system_link][qos]") {
  Pair pair;
  const std::vector<uint8_t> data{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2,
                                  3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4};
  CHECK(pair.a->QosListen(KeyId{2}, kQosListenEnable, {}) == kWsaInvalid);
  REQUIRE(pair.a->QosListen(pair.id, kQosListenEnable | kQosListenSetData,
                            data) == 0);
  const QosTarget host{pair.a->identity(), pair.id, pair.key};
  QosResults results;
  pair.b->QosLookup(1, {host, host}, results.Done());
  REQUIRE(WaitFor([&] { return results.Count() == 2; }));
  for (size_t target : {0, 1}) {
    const auto result = results.Get(target);
    CHECK(result.contacted);
    CHECK_FALSE(result.disabled);
    CHECK(result.data == data);
  }

  // A host that stopped listening still answers with the disabled flag.
  REQUIRE(pair.a->QosListen(pair.id, kQosListenDisable, {}) == 0);
  QosResults disabled;
  pair.b->QosLookup(2, {host}, disabled.Done());
  REQUIRE(WaitFor([&] { return disabled.Count() == 1; }));
  CHECK(disabled.Get(0).contacted);
  CHECK(disabled.Get(0).disabled);
  CHECK(disabled.Get(0).data == data);
}

TEST_CASE("System Link QoS lookups give up on hosts that do not answer",
          "[net][system_link][qos]") {
  Pair pair;
  auto absent = pair.a->identity();
  absent[kEnetOffset + 5] ^= 0x40;
  QosResults results;
  // Neither a host that never listened nor an online key gets an answer.
  KeyId online = pair.id;
  online[0] |= 0x80;
  pair.b->QosLookup(1,
                    {{pair.a->identity(), pair.id, pair.key},
                     {absent, pair.id, pair.key},
                     {pair.a->identity(), online, pair.key}},
                    results.Done());
  REQUIRE(WaitFor([&] { return results.Count() == 3; }));
  for (size_t target : {0, 1, 2}) {
    CHECK_FALSE(results.Get(target).contacted);
  }
}

TEST_CASE("System Link answers its own QoS lookups",
          "[net][system_link][qos]") {
  Pair pair;
  const std::vector<uint8_t> data{1, 2, 3};
  REQUIRE(pair.a->QosListen(pair.id, kQosListenEnable | kQosListenSetData,
                            data) == 0);
  QosResults results;
  pair.a->QosLookup(1, {{pair.a->identity(), pair.id, pair.key}},
                    results.Done());
  REQUIRE(results.Count() == 1);
  CHECK(results.Get(0).contacted);
  CHECK(results.Get(0).data == data);

  // Releasing the listener leaves nothing to answer with.
  REQUIRE(pair.a->QosListen(pair.id, kQosListenRelease, {}) == 0);
  QosResults released;
  pair.a->QosLookup(2, {{pair.a->identity(), pair.id, pair.key}},
                    released.Done());
  REQUIRE(released.Count() == 1);
  CHECK_FALSE(released.Get(0).contacted);
}

TEST_CASE("System Link stops a canceled QoS lookup",
          "[net][system_link][qos]") {
  Pair pair;
  auto absent = pair.a->identity();
  absent[kEnetOffset + 5] ^= 0x40;
  QosResults results;
  pair.b->QosLookup(1, {{absent, pair.id, pair.key}}, results.Done());
  pair.b->CancelQos(1);
  pair.b->CancelQos(2);
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(results.Count() == 0);
}

TEST_CASE("System Link ends pending QoS lookups when the LAN goes away",
          "[net][system_link][qos]") {
  for (bool reset : {true, false}) {
    Pair pair;
    QosResults results;
    // The host never listens so the lookup keeps asking.
    pair.a->QosLookup(1, {{pair.b->identity(), pair.id, pair.key}},
                      results.Done());
    CHECK(results.Count() == 0);
    if (reset) {
      pair.a->Reset();
    } else {
      pair.transports[0]->Disconnect();
    }
    REQUIRE(WaitFor([&] { return results.Count() == 1; }));
    CHECK_FALSE(results.Get(0).contacted);
  }
}

TEST_CASE("System Link answers the QoS probe pairs of a console",
          "[net][system_link][qos]") {
  Pair pair;
  // The test probes in place of the second console.
  pair.b.reset();
  const std::vector<uint8_t> data{4, 5, 6, 7, 8};
  REQUIRE(pair.a->QosListen(pair.id, kQosListenEnable | kQosListenSetData,
                            data) == 0);
  const auto keys =
      DeriveRegistration(pair.broadcast.authentication, pair.id, pair.key);
  const Mac host = pair.transports[0]->identity();
  const Mac prober = pair.transports[1]->identity();
  auto send = [&](const QosProbe& probe) {
    auto wire = EncodeFrame({prober, host, false, EncodeQosProbe(keys, probe)});
    REQUIRE(wire);
    {
      std::lock_guard lock(pair.wire->mutex);
      pair.wire->frames[0].push_back(std::move(*wire));
    }
    pair.wire->changed.notify_all();
  };
  auto receive = [&](uint32_t wait_ms) -> std::optional<QosReply> {
    std::unique_lock lock(pair.wire->mutex);
    if (!pair.wire->changed.wait_for(
            lock, std::chrono::milliseconds(wait_ms),
            [&] { return !pair.wire->frames[1].empty(); })) {
      return std::nullopt;
    }
    auto wire = std::move(pair.wire->frames[1].front());
    pair.wire->frames[1].pop_front();
    auto frame = DecodeFrame(wire);
    REQUIRE(frame);
    CHECK(frame->source == host);
    CHECK(frame->destination == prober);
    return DecodeQosReply(keys, frame->payload);
  };

  QosProbe probe;
  probe.nonce = {1, 2, 3, 4, 5, 6, 7, 8};
  probe.target = 5;
  probe.sequence = 1;
  probe.id = pair.id;
  probe.address = pair.a->identity();
  send(probe);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  probe.flags = kQosProbeSecond;
  send(probe);
  const auto first = receive(5000);
  REQUIRE(first);
  CHECK(first->nonce == probe.nonce);
  CHECK(first->target == 5);
  CHECK(first->sequence == 1);
  CHECK(first->flags == 0);
  CHECK(first->delay >= 5000);
  const auto second = receive(5000);
  REQUIRE(second);
  CHECK(second->sequence == 1);
  // The prober asks for the data next.
  CHECK(second->flags == kQosReplySecond);
  CHECK(second->gap >= 5000);
  CHECK(second->data.empty());

  // A second probe without its first goes unanswered.
  probe.sequence = 2;
  send(probe);
  CHECK_FALSE(receive(300));

  probe.sequence = 3;
  probe.flags = kQosProbeDataRequest;
  send(probe);
  const auto reply = receive(5000);
  REQUIRE(reply);
  CHECK(reply->sequence == 3);
  CHECK(reply->flags == kQosReplyData);
  CHECK(reply->data == data);

  // Without data the prober is done after the pair.
  REQUIRE(pair.a->QosListen(pair.id, kQosListenSetData, {}) == 0);
  probe.sequence = 4;
  probe.flags = 0;
  send(probe);
  probe.flags = kQosProbeSecond;
  send(probe);
  REQUIRE(receive(5000));
  const auto last = receive(5000);
  REQUIRE(last);
  CHECK(last->flags == (kQosReplySecond | kQosReplyNoData));

  // Probes name the host they are for.
  probe.sequence = 5;
  probe.flags = kQosProbeDataRequest;
  probe.address[kEnetOffset] ^= 0x40;
  send(probe);
  CHECK_FALSE(receive(300));
}

TEST_CASE("System Link registry checks key ids", "[net][system_link]") {
  Pair pair;
  Key key;
  auto id = pair.id;
  CHECK(pair.a->RegisterKey(id, pair.key) == kWsaAlready);
  id[0] = 0x1F;
  REQUIRE(pair.a->RegisterKey(id, pair.key) == 0);
  CHECK(pair.a->GetKey(id, key));
  CHECK(key == pair.key);
  REQUIRE(pair.a->UnregisterKey(id) == 0);
  CHECK_FALSE(pair.a->GetKey(id, key));
  CHECK(pair.a->UnregisterKey(id) == kWsaInvalid);
  id[0] = 0x20;
  CHECK(pair.a->RegisterKey(id, pair.key) == kWsaInvalid);
}

TEST_CASE("System Link unmaps a peer that never connected",
          "[net][system_link]") {
  Pair pair;
  uint32_t address;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  REQUIRE(pair.a->Unmap(address) == 0);
  Address peer;
  KeyId id;
  CHECK(pair.a->Reverse(address, peer, id) == kWsaInvalid);
  CHECK(pair.a->peers().empty());
  CHECK(pair.a->Unmap(address) == kWsaInvalid);
}

TEST_CASE("System Link reports addresses it has not mapped as lost",
          "[net][system_link]") {
  Pair pair;
  // 4D5307EA polls the broadcast address and waits while it reads idle.
  CHECK(pair.a->ConnectStatus(0xFFFFFFFF) == 3);
  CHECK(pair.a->ConnectStatus(0) == 3);
  uint32_t self;
  REQUIRE(pair.a->Map(pair.a->identity(), pair.id, self) == 0);
  CHECK(pair.a->ConnectStatus(self) == 3);
  uint32_t address;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  CHECK(pair.a->ConnectStatus(address) == 0);
  REQUIRE(pair.a->Unmap(address) == 0);
  CHECK(pair.a->ConnectStatus(address) == 3);
}

TEST_CASE("System Link keeps a peer through a simultaneous connect",
          "[net][system_link]") {
  Pair pair;
  uint32_t to_b, to_a;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, to_b) == 0);
  REQUIRE(pair.b->Map(pair.a->identity(), pair.id, to_a) == 0);
  // Both key exchange requests are on their way before either arrives.
  pair.transports[0]->Hold();
  pair.transports[1]->Hold();
  CHECK(pair.a->Connect(to_b) == 0);
  CHECK(pair.b->Connect(to_a) == 0);
  CHECK(WaitFor([&] {
    return pair.transports[0]->held() > 0 && pair.transports[1]->held() > 0;
  }));
  pair.transports[0]->Release();
  pair.transports[1]->Release();
  CHECK(WaitFor([&] {
    return pair.a->ConnectStatus(to_b) == 2 && pair.b->ConnectStatus(to_a) == 2;
  }));
  Address peer;
  KeyId id;
  CHECK(pair.a->Reverse(to_b, peer, id) == 0);
  CHECK(pair.b->Reverse(to_a, peer, id) == 0);
  CHECK(peer == pair.a->identity());
}

TEST_CASE("System Link maps the XNADDRs of one machine to one address",
          "[net][system_link]") {
  Pair pair;
  uint32_t address, again;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  auto garbled = pair.b->identity();
  std::copy_n("192.0.2.", 8, garbled.begin());
  REQUIRE(pair.a->Map(garbled, pair.id, again) == 0);
  CHECK(again == address);
  Address peer;
  KeyId id;
  REQUIRE(pair.a->Reverse(again, peer, id) == 0);
  CHECK(peer == pair.b->identity());
  auto other = pair.b->identity();
  other[15] ^= 1;
  REQUIRE(pair.a->Map(other, pair.id, again) == 0);
  CHECK(again != address);
  // xam refuses an XNADDR without a MAC.
  auto unnamed = pair.b->identity();
  std::fill_n(unnamed.begin() + kEnetOffset, 6, 0);
  CHECK(pair.a->Map(unnamed, pair.id, again) == kWsaInvalid);
}

TEST_CASE("System Link maps an online peer whose XNADDR changed anew",
          "[net][system_link]") {
  Pair pair;
  KeyId online = pair.id;
  online[0] |= 0x80;
  REQUIRE(pair.a->RegisterKey(online, pair.key) == 0);
  uint32_t address, again;
  REQUIRE(pair.a->Map(pair.b->identity(), online, address) == 0);
  REQUIRE(pair.a->Map(pair.b->identity(), online, again) == 0);
  CHECK(again == address);
  auto changed = pair.b->identity();
  changed[4] ^= 1;
  REQUIRE(pair.a->Map(changed, online, again) == 0);
  CHECK(again != address);
  Address peer;
  KeyId id;
  CHECK(pair.a->Reverse(address, peer, id) == kWsaInvalid);
  REQUIRE(pair.a->Reverse(again, peer, id) == 0);
  CHECK(peer == changed);
  CHECK(pair.a->peers().size() == 1);
}

TEST_CASE("System Link maps a peer that started over anew",
          "[net][system_link]") {
  Pair pair;
  uint32_t to_b, to_a;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, to_b) == 0);
  REQUIRE(pair.a->Connect(to_b) == 0);
  REQUIRE(WaitFor([&] { return pair.a->ConnectStatus(to_b) == 2; }));
  pair.b.reset();
  pair.b = pair.Make(1, pair.broadcast);
  REQUIRE(pair.b->RegisterKey(pair.id, pair.key) == 0);
  REQUIRE(pair.b->Map(pair.a->identity(), pair.id, to_a) == 0);
  REQUIRE(pair.b->Connect(to_a) == 0);
  REQUIRE(WaitFor([&] { return pair.b->ConnectStatus(to_a) == 2; }));
  CHECK(WaitFor([&] { return pair.a->ConnectStatus(to_b) == 3; }));
  // The new association maps once it carries the peer's first packet.
  std::vector<Peer> peers;
  CHECK(WaitFor([&] {
    peers = pair.a->peers();
    return !peers.empty();
  }));
  REQUIRE(peers.size() == 1);
  CHECK(peers[0].secure_address != to_b);
  CHECK(pair.a->ConnectStatus(peers[0].secure_address) == 2);
}

TEST_CASE(
    "System Link keeps a peer named by a garbled XNADDR through a "
    "simultaneous connect",
    "[net][system_link]") {
  for (bool reverse : {false, true}) {
    Pair pair;
    auto& namer = reverse ? *pair.b : *pair.a;
    auto& named = reverse ? *pair.a : *pair.b;
    auto garbled = named.identity();
    std::copy_n("192.0.2.", 8, garbled.begin());
    uint32_t to_named, to_namer;
    REQUIRE(namer.Map(garbled, pair.id, to_named) == 0);
    REQUIRE(named.Map(namer.identity(), pair.id, to_namer) == 0);
    pair.transports[0]->Hold();
    pair.transports[1]->Hold();
    CHECK(namer.Connect(to_named) == 0);
    CHECK(named.Connect(to_namer) == 0);
    CHECK(WaitFor([&] {
      return pair.transports[0]->held() > 0 && pair.transports[1]->held() > 0;
    }));
    pair.transports[0]->Release();
    pair.transports[1]->Release();
    CHECK(WaitFor([&] {
      return namer.ConnectStatus(to_named) == 2 &&
             named.ConnectStatus(to_namer) == 2;
    }));
    auto namer_port = Bind(namer, 1001, false);
    auto named_port = Bind(named, 1001, false);
    REQUIRE(namer_port);
    REQUIRE(named_port);
    const std::vector<uint8_t> data{1, 2, 3};
    uint32_t error = 0;
    REQUIRE(named_port->Send({to_namer, 1001}, data, error) == 3);
    REQUIRE(Ready(*namer_port));
    std::array<uint8_t, 8> buffer;
    Endpoint source;
    REQUIRE(namer_port->Receive(buffer, false, source, 0, error) == 3);
    CHECK(source == Endpoint{to_named, 1001});
    CHECK(namer.peers().size() == 1);
  }
}

TEST_CASE("System Link loops datagrams to its own address back",
          "[net][system_link]") {
  Pair pair;
  // The key is not looked at.
  KeyId unregistered = pair.id;
  unregistered[7] ^= 1;
  uint32_t address = 0;
  REQUIRE(pair.a->Map(pair.a->identity(), unregistered, address) == 0);
  CHECK(address == 0x7F000001);
  CHECK(pair.a->peers().empty());
  Address own;
  KeyId id;
  REQUIRE(pair.a->Reverse(address, own, id) == 0);
  CHECK(own == pair.a->identity());
  CHECK(id == KeyId{});
  // There is no association to connect or drop.
  CHECK(pair.a->Connect(address) == kWsaInvalid);
  CHECK(pair.a->Unmap(address) == kWsaInvalid);

  for (bool vdp : {false, true}) {
    auto tx = Bind(*pair.a, 1004, vdp);
    auto rx = Bind(*pair.a, 1005, vdp);
    auto other_kind = Bind(*pair.a, 1005, !vdp);
    auto remote = Bind(*pair.b, 1005, vdp);
    REQUIRE(tx);
    REQUIRE(rx);
    REQUIRE(other_kind);
    REQUIRE(remote);
    const std::vector<uint8_t> data{0, 2, 7, 8, 9};
    const int data_size = static_cast<int>(data.size());
    uint32_t error = 0;
    REQUIRE(tx->Send({address, 1005}, data, error) == data_size);
    // A host joins its own session through the port it sends from.
    REQUIRE(tx->Send({address, 1004}, data, error) == data_size);
    std::array<uint8_t, 16> buffer{};
    Endpoint source;
    REQUIRE(rx->Receive(buffer, false, source, 0, error) == data_size);
    CHECK(std::equal(data.begin(), data.end(), buffer.begin()));
    CHECK(source == Endpoint{address, 1004});
    buffer = {};
    REQUIRE(tx->Receive(buffer, false, source, 0, error) == data_size);
    CHECK(std::equal(data.begin(), data.end(), buffer.begin()));
    CHECK(source == Endpoint{address, 1004});
    CHECK_FALSE(other_kind->Readable());
    CHECK(remote->Receive(buffer, false, source, 100, error) == -1);
    CHECK(error == kWsaTimedOut);
    if (!vdp) {
      const std::vector<uint8_t> oversized(kMaxDatagramSize + 1);
      CHECK(tx->Send({address, 1005}, oversized, error) == -1);
      CHECK(error == kWsaMessageSize);
      CHECK_FALSE(rx->Readable());
    }
  }
  CHECK(pair.a->peers().empty());
}

TEST_CASE("System Link datagrams refuse destinations sendto rejects",
          "[net][system_link]") {
  CHECK(DatagramDestinationError({0, 1000}, true) == kWsaAddressNotAvailable);
  CHECK(DatagramDestinationError({1, 0}, true) == kWsaAddressNotAvailable);
  CHECK(DatagramDestinationError({0x7F000002, 1000}, true) ==
        kWsaAddressNotAvailable);
  CHECK(DatagramDestinationError({0x7F000001, 1000}, false) == 0);
  CHECK(DatagramDestinationError({0xFFFFFFFF, 1000}, false) ==
        kWsaAccessDenied);
  CHECK(DatagramDestinationError({0xFFFFFFFF, 1000}, true) == 0);
  // Secure addresses and unmapped ones fail later if at all.
  CHECK(DatagramDestinationError({1, 1000}, false) == 0);
  CHECK(DatagramDestinationError({0xA9FE0488, 1000}, false) == 0);
}

TEST_CASE("System Link socket options read a short value as one byte",
          "[net][system_link]") {
  const uint8_t dword[] = {0x00, 0x00, 0x03, 0xE8};
  CHECK(SocketOptionValue(dword, 4) == 1000);
  // A one-byte BOOL and the stack bytes after it.
  const uint8_t bool_byte[] = {0x01, 0xCD, 0xCD, 0xCD};
  CHECK(SocketOptionValue(bool_byte, 1) == 1);
  CHECK(SocketOptionValue(bool_byte, 3) == 1);
  const uint8_t false_byte[] = {0x00, 0xCD, 0xCD, 0xCD};
  CHECK(SocketOptionValue(false_byte, 1) == 0);
  // Virtual-On Force sets SO_BROADCAST that way before it broadcasts.
  CHECK(DatagramDestinationError({0xFFFFFFFF, 1002},
                                 SocketOptionValue(bool_byte, 1) != 0) == 0);
}

TEST_CASE("XNet startup parameters take xam's defaults and limits",
          "[net][system_link]") {
  const StartupParams defaults{13, 0, 8, 32, 16, 16, 8, 32, 64, 2, 3, 8, 2};
  // Lost Planet 2 sets the size and leaves the rest to XNet.
  const uint8_t unset[13] = {13};
  CHECK(NormalizeStartupParams(unset) == defaults);
  CHECK(NormalizeStartupParams({}) == defaults);
  const uint8_t unknown_size[13] = {12, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};
  CHECK(NormalizeStartupParams(unknown_size) == defaults);
  const uint8_t given[13] = {13, 0x0F, 255, 1, 100, 63, 255, 1, 16, 9, 9, 9, 9};
  CHECK(NormalizeStartupParams(given) ==
        StartupParams{13, 0x06, 232, 1, 63, 63, 255, 1, 16, 9, 9, 9, 9});
  std::array<uint8_t, 0x4C> extended{0x4C};
  extended[8] = 16;
  auto from_extended = defaults;
  from_extended[8] = 16;
  CHECK(NormalizeStartupParams(extended) == from_extended);
}

TEST_CASE("System Link reports broadcasts of other builds of the title",
          "[net][system_link]") {
  Pair pair;
  pair.a.reset();
  pair.b.reset();
  // An older build that also broadcasts with another LAN key.
  auto older_keys = pair.broadcast;
  older_keys.authentication[0] ^= 1;
  pair.a =
      pair.Make(0, older_keys, MakeMetadata(0x434307ED, 3, 2, 0, 17559, 0));
  pair.b = pair.Make(1, pair.broadcast,
                     MakeMetadata(0x434307ED, 0x103, 2, 0, 17559, 0));
  auto tx = Bind(*pair.a, 1001, false);
  auto rx = Bind(*pair.b, 1001, false);
  REQUIRE(tx);
  REQUIRE(rx);
  CHECK(pair.b->BroadcastVersionStatus(false) == 0);
  const std::vector<uint8_t> data{0, 1, 2, 3};
  uint32_t error = 0;
  REQUIRE(tx->Send({0xFFFFFFFF, 1001}, data, error) == int(data.size()));
  REQUIRE(WaitFor([&] { return pair.b->BroadcastVersionStatus(false) != 0; }));
  CHECK(pair.b->BroadcastVersionStatus(true) == kBroadcastVersionOlder);
  CHECK(pair.b->BroadcastVersionStatus(false) == 0);
  CHECK_FALSE(rx->Readable());
}

TEST_CASE("System Link socket options store a short value as one byte",
          "[net][system_link]") {
  std::array<uint8_t, 4> dword;
  dword.fill(0xCD);
  uint32_t optlen = 4;
  StoreSocketOptionValue(dword.data(), optlen, 1000);
  CHECK(optlen == 4);
  CHECK(dword == std::array<uint8_t, 4>{0x00, 0x00, 0x03, 0xE8});
  std::array<uint8_t, 4> bool_byte;
  bool_byte.fill(0xCD);
  optlen = 1;
  StoreSocketOptionValue(bool_byte.data(), optlen, 1);
  CHECK(optlen == 1);
  CHECK(bool_byte == std::array<uint8_t, 4>{0x01, 0xCD, 0xCD, 0xCD});
}

TEST_CASE("System Link unregistering a key retires its peers",
          "[net][system_link]") {
  Pair pair;
  auto tx = Bind(*pair.a, 1001, false);
  auto rx = Bind(*pair.b, 1001, false);
  REQUIRE(tx);
  REQUIRE(rx);
  uint32_t address;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  const std::array<uint8_t, 3> data{1, 3, 5};
  uint32_t error = 0;
  REQUIRE(tx->Send({address, 1001}, data, error) == 3);
  REQUIRE(Ready(*rx));
  REQUIRE(pair.b->peers().size() == 1);
  REQUIRE(pair.b->UnregisterKey(pair.id) == 0);
  CHECK(pair.b->peers().empty());
  CHECK(rx->PendingBytes() == 0);
  CHECK_FALSE(rx->Readable());
}

TEST_CASE(
    "System Link transport loss retires peers and wakes receives but keeps "
    "keys",
    "[net][system_link]") {
  Pair pair;
  auto tx = Bind(*pair.a, 1001, false);
  auto rx = Bind(*pair.b, 1001, false);
  REQUIRE(tx);
  REQUIRE(rx);
  uint32_t address;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  const std::array<uint8_t, 3> data{1, 3, 5};
  uint32_t error = 0;
  REQUIRE(tx->Send({address, 1001}, data, error) == 3);
  REQUIRE(Ready(*rx));
  std::promise<void> woken;
  tx->SetReadyCallback([&] { woken.set_value(); });
  uint32_t blocked_error = 0;
  auto blocked = std::async(std::launch::async, [&] {
    uint8_t byte;
    Endpoint source;
    return tx->Receive(std::span(&byte, 1), false, source,
                       SystemLinkPort::kWaitForever, blocked_error);
  });
  pair.transports[0]->Disconnect();
  REQUIRE(blocked.wait_for(std::chrono::seconds(5)) ==
          std::future_status::ready);
  CHECK(blocked.get() == -1);
  CHECK(blocked_error == kWsaNetworkDown);
  CHECK(woken.get_future().wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready);
  CHECK(pair.a->peers().empty());
  CHECK(pair.a->ConnectStatus(address) == 3);
  // A peer mapped once the LAN is down has no status either.
  uint32_t again;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, again) == 0);
  CHECK(pair.a->ConnectStatus(again) == 3);
  CHECK(tx->Send({address, 1001}, data, error) == -1);
  CHECK(error == kWsaNetworkDown);
  CHECK_FALSE(pair.a->Bind(1002, false, error));
  CHECK(error == kWsaNetworkDown);
  Key key;
  CHECK(pair.a->GetKey(pair.id, key));
  CHECK(key == pair.key);
}

TEST_CASE("System Link streams connect, carry data and close",
          "[net][system_link][tcp]") {
  for (bool reverse : {false, true}) {
    Pair pair;
    auto& client_link = reverse ? *pair.b : *pair.a;
    auto& host_link = reverse ? *pair.a : *pair.b;
    auto listener = Listen(host_link, 1003);
    uint32_t host;
    REQUIRE(client_link.Map(host_link.identity(), pair.id, host) == 0);
    auto client = Stream(client_link);
    REQUIRE(client);
    CHECK_FALSE(client->peer());
    REQUIRE(client->Connect({host, 1003}, kWait) == 0);
    CHECK(client->port() >= 49152);
    CHECK(client->peer() == Endpoint{host, 1003});
    CHECK(client->Connect({host, 1003}, kWait) == kWsaIsConnected);

    Endpoint source;
    uint32_t error = 0;
    auto accepted = listener->Accept(source, kWait, error);
    REQUIRE(accepted);
    CHECK(source.port == client->port());
    CHECK(accepted->port() == 1003);
    CHECK(accepted->peer() == source);
    Address peer;
    KeyId id;
    REQUIRE(host_link.Reverse(source.address, peer, id) == 0);
    CHECK(peer == client_link.identity());

    const auto request = Pattern(5000, 1);
    REQUIRE(SendAll(*client, request));
    CHECK(ReceiveAll(*accepted, request.size()) == request);
    const auto reply = Pattern(7, 3);
    REQUIRE(SendAll(*accepted, reply));
    CHECK(ReceiveAll(*client, reply.size()) == reply);

    // The client finishes sending, the host still answers.
    REQUIRE(client->Shutdown(1) == 0);
    std::array<uint8_t, 16> buffer;
    CHECK(accepted->Receive(buffer, false, kWait, error) == 0);
    CHECK(client->Send(reply, 0, error) == -1);
    CHECK(error == kWsaShutdown);
    REQUIRE(SendAll(*accepted, reply));
    CHECK(ReceiveAll(*client, reply.size()) == reply);
    accepted->Close(false);
    CHECK(client->Receive(buffer, false, kWait, error) == 0);
    CHECK(accepted->Receive(buffer, false, 0, error) == -1);
    CHECK(error == kWsaInterrupted);
  }
}

TEST_CASE("System Link streams carry more than a window",
          "[net][system_link][tcp]") {
  Pair pair;
  auto listener = Listen(*pair.b, 28001);
  uint32_t host;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, host) == 0);
  auto client = Stream(*pair.a);
  REQUIRE(client->Connect({host, 28001}, kWait) == 0);
  Endpoint source;
  uint32_t error = 0;
  auto accepted = listener->Accept(source, kWait, error);
  REQUIRE(accepted);
  const auto data = Pattern(300000, 7);
  auto sending =
      std::async(std::launch::async, [&] { return SendAll(*client, data); });
  CHECK(ReceiveAll(*accepted, data.size()) == data);
  CHECK(sending.get());
}

TEST_CASE("System Link streams refuse ports nobody listens on",
          "[net][system_link][tcp]") {
  Pair pair;
  uint32_t host;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, host) == 0);
  auto client = Stream(*pair.a);
  CHECK(client->Connect({host, 1004}, kWait) == kWsaConnectionRefused);
  CHECK(client->Failed());
  uint32_t error = 0;
  std::array<uint8_t, 4> buffer;
  CHECK(client->Receive(buffer, false, 0, error) == -1);
  CHECK(error == kWsaConnectionRefused);

  // Nor does an address that maps to no peer lead anywhere.
  auto lost = Stream(*pair.a);
  CHECK(lost->Connect({0x00000777, 1003}, kWait) == kWsaHostUnreachable);
  CHECK(lost->Connect({host, 0}, kWait) == kWsaAddressNotAvailable);
}

TEST_CASE("System Link streams report readiness as Winsock events",
          "[net][system_link][tcp]") {
  Pair pair;
  auto listener = Listen(*pair.b, 1003);
  std::atomic<uint32_t> listener_events{0};
  listener->SetReadyCallback(
      [&](uint32_t events) { listener_events |= events; });
  CHECK(listener_events.load() == 0);
  CHECK_FALSE(listener->Readable());
  Endpoint source;
  uint32_t error = 0;
  CHECK_FALSE(listener->Accept(source, 0, error));
  CHECK(error == kWsaWouldBlock);

  uint32_t host;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, host) == 0);
  auto client = Stream(*pair.a);
  std::atomic<uint32_t> client_events{0};
  client->SetReadyCallback([&](uint32_t events) { client_events |= events; });
  // The connect stays under way while its key exchange is held back.
  pair.transports[0]->Hold();
  CHECK(client->Connect({host, 1003}, 0) == kWsaWouldBlock);
  CHECK(client->Connect({host, 1003}, 0) == kWsaAlready);
  CHECK_FALSE(client->Writable());
  std::array<uint8_t, 8> buffer;
  CHECK(client->Send(buffer, 0, error) == -1);
  CHECK(error == kWsaNotConnected);
  pair.transports[0]->Release();
  REQUIRE(WaitFor([&] {
    return (client_events.load() & (kStreamConnect | kStreamWrite)) ==
           (kStreamConnect | kStreamWrite);
  }));
  CHECK(client->Writable());
  CHECK(client->Connect({host, 1003}, 0) == kWsaIsConnected);
  REQUIRE(WaitFor([&] { return listener_events.load() & kStreamAccept; }));
  CHECK(listener->Readable());
  auto accepted = listener->Accept(source, 0, error);
  REQUIRE(accepted);
  CHECK_FALSE(listener->Readable());

  std::atomic<uint32_t> host_events{0};
  accepted->SetReadyCallback([&](uint32_t events) { host_events |= events; });
  // Arming reports what holds already.
  CHECK(host_events.load() == kStreamWrite);
  host_events = 0;
  CHECK(accepted->Receive(buffer, false, 0, error) == -1);
  CHECK(error == kWsaWouldBlock);
  const auto data = Pattern(6, 2);
  REQUIRE(SendAll(*client, data));
  REQUIRE(WaitFor([&] { return host_events.load() & kStreamRead; }));
  CHECK(accepted->PendingBytes() == data.size());
  host_events = 0;
  // A read that leaves data behind reports it again.
  CHECK(accepted->Receive(std::span(buffer).first(2), true, 0, error) == 2);
  CHECK(accepted->Receive(std::span(buffer).first(2), false, 0, error) == 2);
  CHECK(buffer[0] == data[0]);
  CHECK(host_events.load() == kStreamRead);
  host_events = 0;
  CHECK(accepted->Receive(buffer, false, 0, error) == 4);
  CHECK(host_events.load() == 0);
  CHECK_FALSE(accepted->Readable());

  client->Close(false);
  REQUIRE(WaitFor([&] { return host_events.load() & kStreamClose; }));
  CHECK(accepted->Readable());
  CHECK(accepted->Receive(buffer, false, 0, error) == 0);
}

TEST_CASE("System Link streams loop back to this console",
          "[net][system_link][tcp]") {
  Pair pair;
  auto listener = Listen(*pair.a, 1003);
  uint32_t self;
  REQUIRE(pair.a->Map(pair.a->identity(), pair.id, self) == 0);
  auto client = Stream(*pair.a);
  REQUIRE(client->Connect({self, 1003}, kWait) == 0);
  Endpoint source;
  uint32_t error = 0;
  auto accepted = listener->Accept(source, kWait, error);
  REQUIRE(accepted);
  CHECK(source == Endpoint{self, client->port()});
  const auto data = Pattern(100000, 9);
  auto sending =
      std::async(std::launch::async, [&] { return SendAll(*client, data); });
  CHECK(ReceiveAll(*accepted, data.size()) == data);
  CHECK(sending.get());
  client->Close(false);
  std::array<uint8_t, 4> buffer;
  CHECK(accepted->Receive(buffer, false, kWait, error) == 0);
  CHECK(pair.a->peers().empty());
}

TEST_CASE("System Link stream ports are their own", "[net][system_link][tcp]") {
  Pair pair;
  auto first = Stream(*pair.a);
  REQUIRE(first->Bind(1003) == 0);
  CHECK(first->Bind(1004) == kWsaInvalid);
  auto second = Stream(*pair.a);
  CHECK(second->Bind(1003) == kWsaAddressInUse);
  // UDP and VDP ports are apart from TCP ones.
  CHECK(Bind(*pair.a, 1003, false));
  auto dynamic = Stream(*pair.a);
  REQUIRE(dynamic->Bind(0) == 0);
  CHECK(dynamic->port() >= 49152);
  CHECK(second->Listen(1) == kWsaInvalid);
  first->Close(false);
  CHECK(second->Bind(1003) == 0);
}

TEST_CASE("System Link streams wake calls when closed or the LAN goes away",
          "[net][system_link][tcp]") {
  for (bool lan : {false, true}) {
    Pair pair;
    auto listener = Listen(*pair.b, 1003);
    uint32_t host;
    REQUIRE(pair.a->Map(pair.b->identity(), pair.id, host) == 0);
    auto client = Stream(*pair.a);
    REQUIRE(client->Connect({host, 1003}, kWait) == 0);
    uint32_t error = 0;
    auto blocked = std::async(std::launch::async, [&] {
      uint8_t byte;
      return client->Receive(std::span(&byte, 1), false,
                             SystemLinkStream::kWaitForever, error);
    });
    Endpoint source;
    uint32_t accept_error = 0;
    // Kept open, as closing it would end the client's receive.
    auto accepted = listener->Accept(source, kWait, accept_error);
    REQUIRE(accepted);
    auto accepting = std::async(std::launch::async, [&] {
      return listener->Accept(source, SystemLinkStream::kWaitForever,
                              accept_error);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (lan) {
      pair.transports[0]->Disconnect();
      pair.transports[1]->Disconnect();
    } else {
      client->Close(false);
      listener->Close(false);
    }
    REQUIRE(blocked.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    CHECK(blocked.get() == -1);
    CHECK(error == (lan ? kWsaNetworkDown : kWsaInterrupted));
    REQUIRE(accepting.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready);
    CHECK_FALSE(accepting.get());
    CHECK(accept_error == (lan ? kWsaNetworkDown : kWsaInterrupted));
  }
}

TEST_CASE("System Link streams end when the peer goes away",
          "[net][system_link][tcp]") {
  Pair pair;
  auto listener = Listen(*pair.b, 1003);
  uint32_t host;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, host) == 0);
  auto client = Stream(*pair.a);
  REQUIRE(client->Connect({host, 1003}, kWait) == 0);
  Endpoint source;
  uint32_t error = 0;
  auto accepted = listener->Accept(source, kWait, error);
  REQUIRE(accepted);
  std::atomic<uint32_t> events{0};
  accepted->SetReadyCallback([&](uint32_t happened) { events |= happened; });
  REQUIRE(pair.b->UnregisterKey(pair.id) == 0);
  CHECK(events.load() & kStreamClose);
  std::array<uint8_t, 4> buffer;
  CHECK(accepted->Receive(buffer, false, 0, error) == -1);
  CHECK(error == kWsaConnectionReset);
  CHECK(accepted->Send(buffer, 0, error) == -1);
  CHECK(error == kWsaConnectionReset);
}

TEST_CASE(
    "System Link streams reach a peer mapped by a garbled XNADDR that "
    "connected first",
    "[net][system_link][tcp]") {
  Pair pair;
  auto garbled = pair.b->identity();
  std::copy_n("192.0.2.", 8, garbled.begin());
  uint32_t host, to_a;
  REQUIRE(pair.a->Map(garbled, pair.id, host) == 0);
  REQUIRE(pair.b->Map(pair.a->identity(), pair.id, to_a) == 0);
  REQUIRE(pair.b->Connect(to_a) == 0);
  REQUIRE(WaitFor([&] { return pair.a->ConnectStatus(host) == 2; }));
  auto listener = Listen(*pair.b, 1003);
  auto client = Stream(*pair.a);
  REQUIRE(client->Connect({host, 1003}, kWait) == 0);
  Endpoint source;
  uint32_t error = 0;
  auto accepted = listener->Accept(source, kWait, error);
  REQUIRE(accepted);
  const auto reply = Pattern(7, 3);
  REQUIRE(SendAll(*accepted, reply));
  CHECK(ReceiveAll(*client, reply.size()) == reply);
  std::atomic<uint32_t> events{0};
  client->SetReadyCallback([&](uint32_t happened) { events |= happened; });
  REQUIRE(pair.b->Unmap(to_a) == 0);
  CHECK(WaitFor([&] { return pair.a->peers().empty(); }));
  CHECK(WaitFor([&] { return events.load() & kStreamClose; }));
}

// A connected stream on each side, the client on a and the host on b.
struct StreamPair {
  Pair pair;
  std::unique_ptr<SystemLinkStream> listener, client, accepted;
  uint32_t host = 0;
  Endpoint source;

  explicit StreamPair(uint16_t port = 0) {
    listener = Listen(*pair.b, 1003);
    REQUIRE(pair.a->Map(pair.b->identity(), pair.id, host) == 0);
    client = Stream(*pair.a);
    if (port) {
      REQUIRE(client->Bind(port) == 0);
    }
    REQUIRE(client->Connect({host, 1003}, kWait) == 0);
    uint32_t error = 0;
    accepted = listener->Accept(source, kWait, error);
    REQUIRE(accepted);
  }
};

TEST_CASE("System Link datagrams still leave while a stream bursts",
          "[net][system_link][tcp]") {
  StreamPair streams;
  auto& pair = streams.pair;
  // Warm up until a burst fills the peer's whole window.
  const auto warmup = Pattern(300000, 1);
  auto sending = std::async(std::launch::async,
                            [&] { return SendAll(*streams.client, warmup); });
  CHECK(ReceiveAll(*streams.accepted, warmup.size()) == warmup);
  CHECK(sending.get());
  auto tx = Bind(*pair.a, 1001, false);
  REQUIRE(tx);
  const std::array<uint8_t, 4> datagram{1, 2, 3, 4};
  uint32_t error = 0;
  // Lets the last acknowledgments open the whole window.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // The worker sends nothing more for now.
  pair.transports[0]->Hold();
  CHECK(tx->Send({streams.host, 1001}, datagram, error) == 4);
  CHECK(WaitFor([&] { return pair.transports[0]->held() > 0; }));
  const auto burst = Pattern(tcp::kBufferSize, 2);
  CHECK(streams.client->Send(burst, 0, error) > 0);
  int failed = 0;
  for (int i = 0; i < 20; ++i) {
    failed += tx->Send({streams.host, 1001}, datagram, error) < 0;
  }
  CHECK(failed == 0);
  pair.transports[0]->Release();
}

TEST_CASE("System Link streams reconnect from the port of a closed one",
          "[net][system_link][tcp]") {
  StreamPair streams(1500);
  // The host keeps its end open after the client is done.
  streams.client->Close(false);
  std::array<uint8_t, 4> buffer;
  uint32_t error = 0;
  REQUIRE(streams.accepted->Receive(buffer, false, kWait, error) == 0);
  auto again = Stream(*streams.pair.a);
  REQUIRE(again->Bind(1500) == 0);
  CHECK(again->Connect({streams.host, 1003}, 3000) == 0);
  Endpoint source;
  auto second = streams.listener->Accept(source, 3000, error);
  REQUIRE(second);
  CHECK(source.port == 1500);
}

TEST_CASE("System Link streams keep data when their ports are taken again",
          "[net][system_link][tcp]") {
  StreamPair streams(1500);
  // The host finishes first and waits out TIME_WAIT with data unread.
  REQUIRE(streams.accepted->Shutdown(1) == 0);
  std::array<uint8_t, 8> buffer;
  uint32_t error = 0;
  REQUIRE(streams.client->Receive(buffer, false, kWait, error) == 0);
  const auto data = Pattern(5, 4);
  REQUIRE(SendAll(*streams.client, data));
  streams.client->Close(false);
  REQUIRE(WaitFor([&] { return streams.accepted->PendingBytes() == 5; }));
  // TIME_WAIT starts once the client's FIN arrives after its data.
  std::atomic<bool> finished{false};
  streams.accepted->SetReadyCallback([&](uint32_t events) {
    if (events & kStreamClose) {
      finished = true;
    }
  });
  REQUIRE(WaitFor([&] { return finished.load(); }));
  streams.accepted->SetReadyCallback(nullptr);
  auto again = Stream(*streams.pair.a);
  REQUIRE(again->Bind(1500) == 0);
  REQUIRE(again->Connect({streams.host, 1003}, kWait) == 0);
  CHECK(ReceiveAll(*streams.accepted, data.size()) == data);
}

TEST_CASE("System Link streams report a finished connect when armed",
          "[net][system_link][tcp]") {
  StreamPair streams;
  uint32_t events = 0;
  streams.client->SetReadyCallback([&](uint32_t ready) { events |= ready; });
  CHECK(events == (kStreamConnect | kStreamWrite));
  // Only once.
  events = 0;
  streams.client->SetReadyCallback([&](uint32_t ready) { events |= ready; });
  CHECK(events == kStreamWrite);
  streams.client->SetReadyCallback(nullptr);
}

TEST_CASE("System Link connects end when their peer is dropped",
          "[net][system_link][tcp]") {
  for (bool unregister : {false, true}) {
    Pair pair;
    // Nobody answers for this address so the connect stays under way.
    auto absent = pair.b->identity();
    absent[kEnetOffset + 5] ^= 0x40;
    uint32_t address;
    REQUIRE(pair.a->Map(absent, pair.id, address) == 0);
    auto client = Stream(*pair.a);
    REQUIRE(client->Connect({address, 1003}, 0) == kWsaWouldBlock);
    REQUIRE(client->Connect({address, 1003}, 0) == kWsaAlready);
    if (unregister) {
      REQUIRE(pair.a->UnregisterKey(pair.id) == 0);
    } else {
      REQUIRE(pair.a->Unmap(address) == 0);
    }
    CHECK(client->Connect({address, 1003}, 0) == kWsaHostUnreachable);
    CHECK(client->Failed());
  }
}

TEST_CASE("System Link streams keep what arrived before the peer left",
          "[net][system_link][tcp]") {
  StreamPair streams;
  std::atomic<uint32_t> events{0};
  streams.client->SetReadyCallback(
      [&](uint32_t happened) { events |= happened; });
  const auto data = Pattern(100, 5);
  REQUIRE(SendAll(*streams.accepted, data));
  streams.accepted->Close(false);
  REQUIRE(WaitFor([&] { return events.load() & kStreamClose; }));
  REQUIRE(streams.pair.b->Unmap(streams.source.address) == 0);
  // The association is gone before the client read anything.
  REQUIRE(WaitFor([&] { return streams.pair.a->peers().empty(); }));
  CHECK(ReceiveAll(*streams.client, data.size()) == data);
  std::array<uint8_t, 4> buffer;
  uint32_t error = 0;
  CHECK(streams.client->Receive(buffer, false, 0, error) == 0);
}

TEST_CASE("System Link stream shutdowns wake blocked calls",
          "[net][system_link][tcp]") {
  StreamPair streams;
  uint32_t error = 0;
  auto blocked = std::async(std::launch::async, [&] {
    uint8_t byte;
    return streams.client->Receive(std::span(&byte, 1), false,
                                   SystemLinkStream::kWaitForever, error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE(streams.client->Shutdown(0) == 0);
  const bool woke =
      blocked.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  CHECK(woke);
  // Ends the receive either way.
  streams.client->Close(false);
  CHECK(blocked.get() == -1);
  if (woke) {
    CHECK(error == kWsaShutdown);
  }
}

TEST_CASE("System Link streams reset data sent to a closed socket",
          "[net][system_link][tcp]") {
  StreamPair streams;
  streams.client->Close(false);
  std::array<uint8_t, 16> buffer;
  uint32_t error = 0;
  REQUIRE(streams.accepted->Receive(buffer, false, kWait, error) == 0);
  // Nobody reads on the client side any more.
  REQUIRE(
      WaitFor([&] { return streams.accepted->Send(buffer, 0, error) < 0; }));
  CHECK(error == kWsaConnectionReset);
}

TEST_CASE("System Link hears a console's cross-platform search",
          "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  auto rx = Bind(*pair.a, 1000, false);
  REQUIRE(rx);
  pair.transports[0]->Inject(Hex(kConsoleSearch));
  REQUIRE(Ready(*rx));
  const auto expected = Hex(kConsoleSearchData);
  std::array<uint8_t, 64> buffer{};
  Endpoint source;
  uint32_t error = 0;
  REQUIRE(rx->Receive(buffer, false, source, 0, error) ==
          static_cast<int>(expected.size()));
  CHECK(std::equal(expected.begin(), expected.end(), buffer.begin()));
  // Broadcasts come from 0.0.0.1.
  CHECK(source == Endpoint{1, 1000});
}

TEST_CASE("System Link drops cross-platform frames that aren't for it",
          "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  auto rx = Bind(*pair.a, 1000, false);
  REQUIRE(rx);
  const auto search = Hex(kConsoleSearch);
  pair.transports[0]->Inject(search);
  for (const auto& frame : {
           // Another subnet.
           Readdressed(search, 0x0A0002AC, kBroadcastAddress),
           // The address of titles without cross-platform System Link.
           Readdressed(search, 1, kBroadcastAddress),
           // This console's own address.
           Readdressed(search, kHostAddress, kBroadcastAddress),
           // The subnet's broadcast address.
           Readdressed(search, kConsoleAddress, 0x0A0001FF),
           // Another machine.
           Readdressed(search, kConsoleAddress, 0x0A000163),
           // Another port at either end.
           Readdressed(search, kConsoleAddress, kBroadcastAddress, 3075),
           Readdressed(search, kConsoleAddress, kBroadcastAddress,
                       kSystemLinkPort, 3075),
       }) {
    pair.transports[0]->Inject(frame);
  }
  // Frames arrive in order so the console's next search comes after them all.
  pair.transports[0]->Inject(Hex(kConsoleSearchAgain));
  std::array<uint8_t, 64> buffer{};
  Endpoint source;
  uint32_t error = 0;
  for (const auto data : {kConsoleSearchData, kConsoleSearchAgainData}) {
    REQUIRE(Ready(*rx));
    const auto expected = Hex(data);
    REQUIRE(rx->Receive(buffer, false, source, 0, error) ==
            static_cast<int>(expected.size()));
    CHECK(std::equal(expected.begin(), expected.end(), buffer.begin()));
  }
  CHECK(rx->Receive(buffer, false, source, 0, error) == -1);
  CHECK(error == kWsaWouldBlock);
}

TEST_CASE("Cross-platform frames come only from peers of their subnet",
          "[net][system_link][cross_platform]") {
  const auto search = Hex(kConsoleSearch);
  const auto decodes = [&](uint32_t source, uint32_t destination,
                           uint32_t own = kHostAddress,
                           uint32_t mask = kSubnetMask) {
    return DecodeFrame(Readdressed(search, source, destination),
                       CrossPlatform{own, mask, kSystemLinkPort})
        .has_value();
  };
  CHECK(decodes(kConsoleAddress, kBroadcastAddress));
  CHECK(decodes(kConsoleAddress, kHostAddress));
  // This console's own address, the subnet's broadcast address, another
  // subnet, the address of titles without cross-platform System Link and
  // loopback.
  CHECK_FALSE(decodes(kHostAddress, kBroadcastAddress));
  CHECK_FALSE(decodes(0x0A0001FF, kBroadcastAddress));
  CHECK_FALSE(decodes(0x0A0002AC, kBroadcastAddress));
  CHECK_FALSE(decodes(1, kBroadcastAddress));
  CHECK_FALSE(decodes(0x7F000002, kBroadcastAddress, 0x7F000001, 0xFF000000));
  // Unicast to another machine or to the subnet's broadcast address.
  CHECK_FALSE(decodes(kConsoleAddress, 0x0A000163));
  CHECK_FALSE(decodes(kConsoleAddress, 0x0A0001FF));
  // Other titles only take frames from 0.0.0.1.
  CHECK(DecodeFrame(Readdressed(search, 1, kBroadcastAddress)).has_value());
  CHECK_FALSE(
      DecodeFrame(Readdressed(search, kConsoleAddress, kBroadcastAddress))
          .has_value());
}

TEST_CASE("System Link ignores cross-platform frames otherwise",
          "[net][system_link][cross_platform]") {
  Pair pair;
  auto rx = Bind(*pair.a, 1000, false);
  auto tx = Bind(*pair.b, 1000, false);
  REQUIRE(rx);
  REQUIRE(tx);
  pair.transports[0]->Inject(Hex(kConsoleSearch));
  const std::vector<uint8_t> data{1, 2, 3};
  uint32_t error = 0;
  REQUIRE(tx->Send({kBroadcastAddress, 1000}, data, error) == 3);
  REQUIRE(Ready(*rx));
  std::array<uint8_t, 64> buffer{};
  Endpoint source;
  CHECK(rx->Receive(buffer, false, source, 0, error) == 3);
  CHECK(rx->Receive(buffer, false, source, 0, error) == -1);
  CHECK(error == kWsaWouldBlock);
}

TEST_CASE("Cross-platform System Links talk over their subnet's addresses",
          "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  auto tx = Bind(*pair.a, 1001, false);
  auto rx = Bind(*pair.b, 1001, false);
  REQUIRE(tx);
  REQUIRE(rx);
  const std::vector<uint8_t> data{0, 1, 2, 3, 4, 5, 6};
  const int data_size = static_cast<int>(data.size());
  uint32_t error = 0;
  REQUIRE(tx->Send({kBroadcastAddress, 1001}, data, error) == data_size);
  REQUIRE(Ready(*rx));
  std::array<uint8_t, 32> buffer{};
  Endpoint source;
  REQUIRE(rx->Receive(buffer, false, source, 0, error) == data_size);
  CHECK(source == Endpoint{1, 1001});

  uint32_t address;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  REQUIRE(pair.a->Connect(address) == 0);
  REQUIRE(tx->Send({address, 1001}, data, error) == data_size);
  REQUIRE(Ready(*rx));
  REQUIRE(rx->Receive(buffer, false, source, 0, error) == data_size);
  CHECK(std::equal(data.begin(), data.end(), buffer.begin()));
  // The title only ever sees secure addresses.
  CHECK(source.address >> 24 == 0);

  const auto sent = pair.transports[0]->Sent();
  REQUIRE(sent.size() >= 2);
  CHECK(AddressesOf(sent.front()) ==
        FrameAddresses{kHostAddress, kBroadcastAddress, kSystemLinkPort,
                       kSystemLinkPort});
  for (size_t i = 1; i < sent.size(); ++i) {
    CHECK(AddressesOf(sent[i]) == FrameAddresses{kHostAddress, kHostAddress + 1,
                                                 kSystemLinkPort,
                                                 kSystemLinkPort});
  }
}

TEST_CASE(
    "Cross-platform System Link maps a peer that moved to another IP anew",
    "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  uint32_t address, to_a, moved_address;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  REQUIRE(pair.a->Connect(address) == 0);
  REQUIRE(WaitFor([&] { return pair.a->ConnectStatus(address) == 2; }));
  REQUIRE(pair.b->Map(pair.a->identity(), pair.id, to_a) == 0);
  REQUIRE(WaitFor([&] { return pair.b->ConnectStatus(to_a) == 2; }));
  auto rx = Bind(*pair.a, 1001, false);
  auto tx = Bind(*pair.b, 1001, false);
  const std::vector<uint8_t> data{1, 2, 3};
  uint32_t error = 0;
  REQUIRE(tx->Send({to_a, 1001}, data, error) == 3);
  REQUIRE(Ready(*rx));
  auto moved = pair.b->identity();
  moved[3] += 10;
  REQUIRE(pair.a->Map(moved, pair.id, moved_address) == 0);
  CHECK(moved_address != address);
  // What arrived from the old address goes with it.
  CHECK_FALSE(rx->Readable());
  Address peer;
  KeyId id;
  CHECK(pair.a->Reverse(address, peer, id) == kWsaInvalid);
  CHECK(pair.a->peers().size() == 1);
  // The old association ends with a teardown.
  CHECK(WaitFor([&] { return pair.b->ConnectStatus(to_a) == 3; }));
  CHECK(pair.a->Reverse(moved_address, peer, id) == 0);
}

TEST_CASE(
    "Cross-platform System Link keeps the address its title mapped for a peer "
    "that connects from another IP",
    "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  auto stale = pair.b->identity();
  stale[3] += 10;
  uint32_t to_b, to_a;
  REQUIRE(pair.a->Map(stale, pair.id, to_b) == 0);
  REQUIRE(pair.b->Map(pair.a->identity(), pair.id, to_a) == 0);
  REQUIRE(pair.b->Connect(to_a) == 0);
  CHECK(WaitFor([&] { return pair.a->ConnectStatus(to_b) == 2; }));
  auto rx = Bind(*pair.b, 1001, false);
  auto tx = Bind(*pair.a, 1001, false);
  const std::vector<uint8_t> data{1, 2, 3};
  uint32_t error = 0;
  REQUIRE(tx->Send({to_b, 1001}, data, error) == 3);
  REQUIRE(Ready(*rx));
  std::array<uint8_t, 8> buffer;
  Endpoint source;
  REQUIRE(rx->Receive(buffer, false, source, 0, error) == 3);
  CHECK(source == Endpoint{to_a, 1001});
  CHECK(pair.b->ConnectStatus(to_a) == 2);
  CHECK(pair.a->peers().size() == 1);
  auto listener = Listen(*pair.b, 1003);
  auto client = Stream(*pair.a);
  REQUIRE(client->Connect({to_b, 1003}, kWait) == 0);
  auto accepted = listener->Accept(source, kWait, error);
  REQUIRE(accepted);
  std::atomic<uint32_t> events{0};
  client->SetReadyCallback([&](uint32_t happened) { events |= happened; });
  // The peer's teardown ends what the title mapped.
  REQUIRE(pair.b->Unmap(to_a) == 0);
  CHECK(WaitFor([&] { return pair.a->peers().empty(); }));
  CHECK(WaitFor([&] { return events.load() & kStreamClose; }));
}

TEST_CASE(
    "Cross-platform System Link maps a peer anew each time its IP changes back",
    "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  uint32_t address, stale_address, again;
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, address) == 0);
  REQUIRE(pair.a->Connect(address) == 0);
  REQUIRE(WaitFor([&] { return pair.a->ConnectStatus(address) == 2; }));
  auto stale = pair.b->identity();
  stale[3] += 10;
  REQUIRE(pair.a->Map(stale, pair.id, stale_address) == 0);
  REQUIRE(pair.a->Map(pair.b->identity(), pair.id, again) == 0);
  CHECK(again != address);
  REQUIRE(pair.a->Connect(again) == 0);
  CHECK(WaitFor([&] { return pair.a->ConnectStatus(again) == 2; }));
}

TEST_CASE("Cross-platform System Link only reaches peers of its subnet",
          "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  // Another subnet, the subnet's broadcast address and loopback.
  std::vector<QosTarget> outside;
  for (uint32_t ina : {0x08080808u, 0x0A0001FFu, 0x7F000001u}) {
    QosTarget target{pair.b->identity(), pair.id, pair.key};
    for (size_t i = 0; i < 4; ++i) {
      target.address[i] = uint8_t(ina >> (24 - 8 * i));
    }
    outside.push_back(target);
  }
  for (const auto& target : outside) {
    uint32_t address;
    REQUIRE(pair.a->Map(target.address, pair.id, address) == 0);
    CHECK(pair.a->Connect(address) != 0);
  }
  QosResults results;
  pair.a->QosLookup(1, outside, results.Done());
  // A target that isn't probed is done before the lookup returns.
  CHECK(results.Count() == outside.size());
  pair.a->CancelQos(1);
  CHECK(pair.transports[0]->Sent().empty());
}

TEST_CASE("Cross-platform System Link QoS lookups reach a host of the subnet",
          "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  const std::vector<uint8_t> data{9, 8, 7, 6};
  REQUIRE(pair.a->QosListen(pair.id, kQosListenEnable | kQosListenSetData,
                            data) == 0);
  QosResults results;
  pair.b->QosLookup(1, {{pair.a->identity(), pair.id, pair.key}},
                    results.Done());
  REQUIRE(WaitFor([&] { return results.Count() == 1; }));
  CHECK(results.Get(0).contacted);
  CHECK(results.Get(0).data == data);
}

TEST_CASE("Cross-platform System Link keeps to its key type",
          "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  KeyId id = pair.id;
  id[0] = 0x01;
  CHECK(pair.a->RegisterKey(id, pair.key) == kWsaInvalid);
  id[0] = 0x4F;
  CHECK(pair.a->RegisterKey(id, pair.key) == 0);
  // Online keys share the registry.
  id[0] = 0x81;
  CHECK(pair.a->RegisterKey(id, pair.key) == 0);
  Pair normal;
  id[0] = 0x41;
  CHECK(normal.a->RegisterKey(id, normal.key) == kWsaInvalid);

  Key key;
  SystemLink::CreateKey(id, key, true);
  CHECK((id[0] & 0xE0) == 0x40);
  SystemLink::CreateKey(id, key, false);
  CHECK((id[0] & 0xE0) == 0);
}

TEST_CASE("Cross-platform System Link moves to the port the title sets",
          "[net][system_link][cross_platform]") {
  CrossPlatformPair pair;
  CHECK(pair.a->system_link_port() == kSystemLinkPort);
  CHECK(pair.a->SetSystemLinkPort(1257) == kWsaAddressInUse);
  CHECK(pair.a->system_link_port() == kSystemLinkPort);
  REQUIRE(pair.a->SetSystemLinkPort(14000) == 0);
  CHECK(pair.a->system_link_port() == 14000);

  auto rx = Bind(*pair.a, 1000, false);
  REQUIRE(rx);
  const auto search = Hex(kConsoleSearch);
  pair.transports[0]->Inject(search);
  pair.transports[0]->Inject(
      Readdressed(search, kConsoleAddress, kBroadcastAddress, 14000, 14000));
  REQUIRE(Ready(*rx));
  std::array<uint8_t, 64> buffer{};
  Endpoint source;
  uint32_t error = 0;
  CHECK(rx->Receive(buffer, false, source, 0, error) ==
        static_cast<int>(Hex(kConsoleSearchData).size()));
  CHECK(rx->Receive(buffer, false, source, 0, error) == -1);

  const std::vector<uint8_t> data{1, 2, 3};
  REQUIRE(rx->Send({kBroadcastAddress, 1000}, data, error) == 3);
  REQUIRE(WaitFor([&] { return !pair.transports[0]->Sent().empty(); }));
  CHECK(AddressesOf(pair.transports[0]->Sent().back()) ==
        FrameAddresses{kHostAddress, kBroadcastAddress, 14000, 14000});
  REQUIRE(pair.a->SetSystemLinkPort(0) == 0);
  CHECK(pair.a->system_link_port() == kSystemLinkPort);
}

TEST_CASE("System Link only remembers the port for other titles",
          "[net][system_link][cross_platform]") {
  Pair pair;
  REQUIRE(pair.a->SetSystemLinkPort(14000) == 0);
  CHECK(pair.a->system_link_port() == 14000);
  CHECK(pair.transports[0]->Ports().empty());
  auto tx = Bind(*pair.a, 1000, false);
  auto rx = Bind(*pair.b, 1000, false);
  REQUIRE(tx);
  REQUIRE(rx);
  const std::vector<uint8_t> data{1, 2, 3};
  uint32_t error = 0;
  REQUIRE(tx->Send({kBroadcastAddress, 1000}, data, error) == 3);
  REQUIRE(Ready(*rx));
  const auto sent = pair.transports[0]->Sent();
  REQUIRE(!sent.empty());
  CHECK(AddressesOf(sent.back()) ==
        FrameAddresses{1, kBroadcastAddress, kSystemLinkPort, kSystemLinkPort});
}

}  // namespace xe::net::secure::test
