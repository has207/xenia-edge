/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Asio must be included before Windows headers to avoid macro conflicts
#include <asio.hpp>

#include "xenia/kernel/xam/xam_system_link.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_crypt.h"
#include "xenia/kernel/xboxkrnl/xecrypt_sha.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/net/host_interfaces.h"
#include "xenia/net/packet_transport.h"

DEFINE_string(systemlink_transport, "tunnel",
              "How System Link traffic travels. \"tunnel\" sends it straight "
              "to other Xenia instances on this LAN and is the better choice "
              "for Xenia-to-Xenia play. \"kai\" goes through an XLink Kai "
              "engine, which reaches Xbox 360 consoles and players over the "
              "internet. \"off\" turns System Link off for every title. While "
              "systemlink_cross_platform is on, titles with cross-platform "
              "System Link use the local network in place of the tunnel or "
              "Kai.",
              "Network");
DEFINE_bool(systemlink_cross_platform, false,
            "Lets Shadowrun, Lost Planet Colonies, Aliens: Colonial Marines "
            "and Universe at War: Earth Assault play System Link with Xbox "
            "360 consoles and Windows PCs on the local network. While it is "
            "on, these titles only play on the local network, not over XLink "
            "Kai or the tunnel. Turn it off to play them over Kai or the "
            "tunnel. Other Xenia instances need the same setting to join.",
            "Network");
DEFINE_string(systemlink_cross_platform_interface, "auto",
              "Network interface that cross-platform System Link uses. "
              "\"auto\" picks the one with the default route.",
              "Network");
DEFINE_string(systemlink_kai_engine, "127.0.0.1",
              "IPv4 address of the XLink Kai engine. An engine on another "
              "machine needs \"Allow remote DDS connections\" turned on in its "
              "settings.",
              "Network");
DEFINE_int32(systemlink_tunnel_port, 36020,
             "UDP port of the System Link tunnel, the same on every instance.",
             "Network");
DEFINE_string(systemlink_tunnel_address, "255.255.255.255",
              "Where the System Link tunnel sends: the LAN broadcast address "
              "or the IPv4 address of one other instance.",
              "Network");

namespace xe {
namespace kernel {
namespace xam {
namespace {

// An Ethernet header and a 1500 byte payload.
constexpr uint32_t kEthernetFrameLimit = 1514;
// Frames that arrived and wait for the title's callback.
constexpr size_t kReceiveQueueLimit = 256;

// Titles with cross-platform System Link derive their broadcast keys from
// their LAN key AES-CBC encrypted with key 0x43 and a zero IV, which only a
// console can compute.
struct CrossPlatformKey {
  net::Key lan_key;
  net::Key encrypted;
};
constexpr CrossPlatformKey kCrossPlatformKeys[] = {
    // Lost Planet Colonies.
    {{0xBF, 0xB2, 0x96, 0x96, 0x23, 0x9F, 0x88, 0xBF, 0x5F, 0x20, 0x0E, 0x47,
      0x96, 0x66, 0x47, 0xC6},
     {0xCF, 0x04, 0x4B, 0xED, 0x8C, 0xC1, 0xFF, 0x2E, 0x00, 0x70, 0x77, 0xA9,
      0xB7, 0x05, 0xDE, 0xD6}},
    // Shadowrun and Aliens: Colonial Marines.
    {{0xFE, 0xEF, 0xF0, 0x0D, 0x01, 0x02, 0x03, 0x04, 0x50, 0x60, 0x71, 0x72,
      0x8A, 0x8D, 0xA9, 0x9A},
     {0x85, 0x02, 0x0A, 0x99, 0x03, 0x67, 0x63, 0x69, 0x90, 0x09, 0x14, 0xC8,
      0xD8, 0x2C, 0x1B, 0x12}},
};

std::string KeyText(const net::Key& key) {
  std::string text;
  for (uint8_t byte : key) {
    text += fmt::format("{:02X}", byte);
  }
  return text;
}

std::optional<net::secure::ProtectionKeys> DeriveBroadcast(
    const net::Key& lan_key, bool cross_platform) {
  if (cross_platform) {
    const auto known = std::ranges::find(kCrossPlatformKeys, lan_key,
                                         &CrossPlatformKey::lan_key);
    if (known != std::end(kCrossPlatformKeys)) {
      return net::secure::DeriveBroadcast(
          known->encrypted,
          [](std::span<const uint8_t> input, net::secure::Sha1Digest& digest) {
            xboxkrnl::XeCryptSha(input.data(), uint32_t(input.size()), nullptr,
                                 0, nullptr, 0, digest.data(),
                                 uint32_t(digest.size()));
            return true;
          });
    }
    XELOGE(
        "System Link: LAN key {} has no cross-platform key so consoles won't "
        "see this title. On a modded console, call XeKeysAesCbc (xboxkrnl "
        "ordinal 587) with key 0x43, this LAN key as the 16-byte input, an "
        "output buffer, 16 zero bytes as the IV and 1 to encrypt, then add the "
        "LAN key and the output to kCrossPlatformKeys.",
        KeyText(lan_key));
  }
  return net::secure::DeriveBroadcast(
      lan_key,
      [](std::span<const uint8_t> input, net::secure::Sha1Digest& digest) {
        return xboxkrnl::XeKeysHmacSha(
            0x19, input.data(), uint32_t(input.size()), nullptr, 0, nullptr, 0,
            digest.data(), uint32_t(digest.size()));
      });
}

std::optional<uint32_t> ParseAddress(const std::string& text) {
  asio::error_code ec;
  const auto address = asio::ip::make_address_v4(text, ec);
  if (ec) {
    XELOGE("System Link: \"{}\" is not an IPv4 address", text);
    return std::nullopt;
  }
  return address.to_uint();
}

std::unique_ptr<net::secure::PacketTransport> OpenTransport(
    const net::secure::Mac& mac) {
  const std::string& name = cvars::systemlink_transport;
  std::string error;
  std::unique_ptr<net::secure::PacketTransport> transport;
  if (name == "kai") {
    const auto engine = ParseAddress(cvars::systemlink_kai_engine);
    if (!engine) {
      return nullptr;
    }
    transport = net::secure::OpenKaiTransport(
        mac, {*engine, net::secure::kKaiEngine.port}, error);
  } else if (name == "tunnel") {
    const auto destination = ParseAddress(cvars::systemlink_tunnel_address);
    const int32_t port = cvars::systemlink_tunnel_port;
    if (!destination) {
      return nullptr;
    }
    if (port <= 0 || port > 0xFFFF) {
      XELOGE("System Link: tunnel port {} is out of range", port);
      return nullptr;
    }
    transport = net::secure::OpenUdpTunnelTransport(
        mac, uint16_t(port), {{*destination, uint16_t(port)}}, error);
  } else {
    XELOGE("System Link: unknown transport \"{}\"", name);
    return nullptr;
  }
  if (!transport) {
    XELOGE("System Link: {}", error);
  }
  return transport;
}

// Titles with cross-platform System Link play on the host network as a
// machine of the interface's subnet.
std::unique_ptr<net::secure::PacketTransport> OpenCrossPlatformTransport(
    net::secure::CrossPlatform& cross_platform, uint16_t port) {
  const std::string& name = cvars::systemlink_cross_platform_interface;
  const bool automatic = name.empty() || name == "auto";
  const auto host = net::FindHostInterface(automatic ? "" : name);
  if (!host) {
    std::string names;
    for (const auto& known : net::HostInterfaceNames()) {
      names += fmt::format("{}\"{}\"", names.empty() ? "" : ", ", known);
    }
    if (names.empty()) {
      XELOGE("System Link: no network interface that can broadcast is up");
    } else if (automatic) {
      XELOGE(
          "System Link: the default route doesn't lead through an interface "
          "that can broadcast. Set systemlink_cross_platform_interface to one "
          "of {}.",
          names);
    } else {
      XELOGE(
          "System Link: no interface named \"{}\" is up. Set "
          "systemlink_cross_platform_interface to one of {}.",
          name, names);
    }
    return nullptr;
  }
  // Peers come from the subnet so a /31, /32 or /0 has none.
  if (!host->mask || host->mask >= 0xFFFFFFFE) {
    XELOGE("System Link: interface \"{}\" is a /{} with no room for peers",
           host->name, std::popcount(host->mask));
    return nullptr;
  }
  std::string error;
  auto transport = net::secure::OpenHostTransport(*host, port, error);
  if (!transport) {
    XELOGE("System Link: {}", error);
    return nullptr;
  }
  const auto& mac = host->mac;
  XELOGI(
      "System Link: cross-platform on \"{}\", {}/{}, UDP port {}, MAC "
      "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
      host->name, asio::ip::address_v4(host->address).to_string(),
      std::popcount(host->mask), port, mac[0], mac[1], mac[2], mac[3], mac[4],
      mac[5]);
  cross_platform = {host->address, host->mask, port};
  return transport;
}

}  // namespace

bool SystemLinkEnabled() { return cvars::systemlink_transport != "off"; }

bool CrossPlatformTitle(KernelState* kernel_state) {
  auto module = kernel_state->GetExecutableModule();
  if (!module) {
    return false;
  }
  uint32_t system_flags = 0;
  module->GetOptHeader<uint32_t>(XEX_HEADER_SYSTEM_FLAGS, &system_flags);
  return system_flags & XEX_SYSTEM_CROSS_PLATFORM_SYSTEM_LINK;
}

bool CrossPlatformSystemLink(KernelState* kernel_state) {
  return cvars::systemlink_cross_platform && CrossPlatformTitle(kernel_state);
}

std::unique_ptr<net::SystemLink> CreateSystemLink(KernelState* kernel_state,
                                                  uint16_t port) {
  if (!SystemLinkEnabled()) {
    return nullptr;
  }
  auto module = kernel_state->GetExecutableModule();
  xex2_opt_lan_key* lan_key = nullptr;
  const xex2_opt_execution_info* execution =
      module ? module->xex_module()->opt_execution_info() : nullptr;
  if (!execution ||
      !module->xex_module()->GetOptHeader(XEX_HEADER_LAN_KEY, &lan_key) ||
      !lan_key) {
    XELOGE("System Link: the title has no LAN key");
    return nullptr;
  }
  net::Key key;
  std::copy_n(lan_key->key, key.size(), key.begin());
  const bool cross_platform = CrossPlatformSystemLink(kernel_state);
  if (!cross_platform && CrossPlatformTitle(kernel_state)) {
    XELOGI(
        "System Link: the title has cross-platform System Link. Turn on "
        "systemlink_cross_platform to play it with consoles and PCs.");
  }
  auto broadcast = DeriveBroadcast(key, cross_platform);
  if (!broadcast) {
    XELOGE("System Link: no platform key to derive the LAN keys");
    return nullptr;
  }

  net::secure::AssociationConfig association;
  std::unique_ptr<net::secure::PacketTransport> transport;
  if (cross_platform) {
    association.cross_platform.emplace();
    transport = OpenCrossPlatformTransport(*association.cross_platform, port);
  } else {
    transport = OpenTransport(kernel_state->xconfig()->mac_address());
  }
  if (!transport) {
    return nullptr;
  }
  const auto mac = transport->identity();

  association.broadcast = *broadcast;
  association.freshness_time = [] { return Clock::QueryGuestSystemTime(); };
  const auto* version = kernel_state->GetKernelVersion();
  association.metadata = net::secure::MakeMetadata(
      execution->title_id, execution->version_value, version->major,
      version->minor, version->build, uint8_t(version->qfe));
  association.mtu = transport->mtu();
  uint32_t address;
  if (association.cross_platform) {
    address = association.cross_platform->address;
  } else {
    // A console without DHCP picks an RFC 3927 address. Peers only display
    // it since frames go by MAC.
    address = 0xA9FE0000 | (uint32_t(1 + mac[4] % 254) << 8) | mac[5];
    XELOGI(
        "System Link: {} transport, MAC "
        "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
        cvars::systemlink_transport, mac[0], mac[1], mac[2], mac[3], mac[4],
        mac[5]);
  }
  auto link = std::make_unique<net::SystemLink>(address, std::move(transport),
                                                std::move(association));
  link->SetSystemLinkPort(port);
  return link;
}

std::shared_ptr<net::SystemLink> GetSystemLink(KernelState* kernel_state) {
  if (!SystemLinkEnabled()) {
    return nullptr;
  }
  return kernel_state->GetKernelModule<XamModule>("xam.xex")->system_link();
}

struct EthernetIntercept::Queue {
  std::mutex mutex;
  // The XNCALLER that set the callback, 0 without one.
  uint32_t caller = 0;
  uint32_t receive = 0;
  uint32_t context = 0;
  std::deque<std::vector<uint8_t>> frames;
  object_ref<XEvent> arrived;
};

EthernetIntercept::EthernetIntercept(KernelState* kernel_state)
    : kernel_state_(kernel_state), queue_(std::make_shared<Queue>()) {}

EthernetIntercept::~EthernetIntercept() {
  {
    // The System Link can outlive this and still hand frames over.
    std::lock_guard lock(queue_->mutex);
    queue_->receive = 0;
    queue_->frames.clear();
  }
  if (buffer_) {
    kernel_state_->memory()->SystemHeapFree(buffer_);
  }
}

uint32_t EthernetIntercept::SetCallbacks(net::SystemLink& link, uint32_t caller,
                                         uint32_t receive, uint32_t context) {
  std::lock_guard lock(queue_->mutex);
  if (!receive) {
    if (queue_->caller != caller) {
      return net::kWsaSystemNotReady;
    }
    queue_->caller = queue_->receive = queue_->context = 0;
    queue_->frames.clear();
    link.SetFrameReceiver(nullptr);
    return 0;
  }
  if (queue_->caller) {
    return net::kWsaAlready;
  }
  if (!thread_ && !Start()) {
    return net::kWsaNoBuffers;
  }
  queue_->caller = caller;
  queue_->receive = receive;
  queue_->context = context;
  link.SetFrameReceiver([queue = queue_](std::span<const uint8_t> frame) {
    {
      std::lock_guard lock(queue->mutex);
      // A full receive ring drops what arrives.
      if (!queue->receive || frame.size() > kEthernetFrameLimit ||
          queue->frames.size() >= kReceiveQueueLimit) {
        return;
      }
      queue->frames.emplace_back(frame.begin(), frame.end());
    }
    queue->arrived->Set(0, false);
  });
  return 0;
}

bool EthernetIntercept::Start() {
  buffer_ = kernel_state_->memory()->SystemHeapAlloc(kEthernetFrameLimit);
  if (!buffer_) {
    return false;
  }
  queue_->arrived = object_ref<XEvent>(new XEvent(kernel_state_, true));
  queue_->arrived->Initialize(false, false);
  thread_ = object_ref<XHostThread>(new XHostThread(
      kernel_state_, 128 * 1024, 0,
      [kernel_state = kernel_state_, queue = queue_, buffer = buffer_]() {
        Deliver(kernel_state, *queue, buffer);
        return 0;
      },
      kernel_state_->GetSystemProcess(), true));
  thread_->set_name("XNet Ethernet Intercept");
  if (XFAILED(thread_->Create())) {
    thread_.reset();
    kernel_state_->memory()->SystemHeapFree(buffer_);
    buffer_ = 0;
    return false;
  }
  // At the top priority, as xam's receive DPC interrupts what runs below
  // DISPATCH_LEVEL.
  thread_->SetPriority(31);
  return true;
}

void EthernetIntercept::Deliver(KernelState* kernel_state, Queue& queue,
                                uint32_t buffer) {
  auto thread_state = XThread::GetCurrentThread()->thread_state();
  auto context = thread_state->context();
  while (true) {
    queue.arrived->Wait(0, 0, 0, nullptr);
    DPCImpersonationScope dpc_scope{};
    kernel_state->BeginDPCImpersonation(context, dpc_scope);
    while (true) {
      std::vector<uint8_t> frame;
      uint32_t receive, receive_context;
      {
        std::lock_guard lock(queue.mutex);
        if (queue.frames.empty()) {
          break;
        }
        frame = std::move(queue.frames.front());
        queue.frames.pop_front();
        receive = queue.receive;
        receive_context = queue.context;
      }
      std::memcpy(kernel_state->memory()->TranslateVirtual(buffer),
                  frame.data(), frame.size());
      uint64_t args[] = {receive_context, buffer, frame.size()};
      kernel_state->processor()->Execute(thread_state, receive, args,
                                         std::size(args));
    }
    kernel_state->EndDPCImpersonation(context, dpc_scope);
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
