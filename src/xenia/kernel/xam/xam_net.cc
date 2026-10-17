/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xam/xam_system_link.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xsocket.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

#ifdef XE_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#endif
#include <winsock2.h>  // NOLINT(build/include_order)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace xe {
namespace kernel {
namespace xam {

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                   // IP address (zero if not static/DHCP)
  in_addr inaOnline;             // Online IP address (zero if not online)
  xe::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];             // Ethernet MAC address
  uint8_t abOnline[20];          // Online identification
} XNADDR;
static_assert_size(XNADDR, sizeof(net::secure::Address));

struct XNDNS {
  xe::be<int32_t> status;
  xe::be<uint32_t> cina;
  in_addr aina[8];
};
static_assert_size(XNDNS, 0x28);

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  xe::be<uint16_t> probes_xmit;
  xe::be<uint16_t> probes_recv;
  xe::be<uint16_t> data_len;
  xe::be<uint32_t> data_ptr;
  xe::be<uint16_t> rtt_min_in_msecs;
  xe::be<uint16_t> rtt_med_in_msecs;
  xe::be<uint32_t> up_bits_per_sec;
  xe::be<uint32_t> down_bits_per_sec;
};
static_assert_size(XNQOSINFO, 0x18);

constexpr uint8_t XNET_XNQOSINFO_COMPLETE = 0x01;
constexpr uint8_t XNET_XNQOSINFO_TARGET_CONTACTED = 0x02;
constexpr uint8_t XNET_XNQOSINFO_TARGET_DISABLED = 0x04;
constexpr uint8_t XNET_XNQOSINFO_DATA_RECEIVED = 0x08;

struct XNQOS {
  xe::be<uint32_t> count;
  xe::be<uint32_t> count_pending;
  XNQOSINFO info[1];
};

struct Xsockaddr_t {
  xe::be<uint16_t> sa_family;
  char sa_data[14];
};
static_assert_size(XNQOS, 0x20);

struct X_WSADATA {
  xe::be<uint16_t> version;
  xe::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  xe::be<uint16_t> max_sockets;
  xe::be<uint16_t> max_udpdg;
  xe::be<uint32_t> vendor_info_ptr;
};
static_assert_size(X_WSADATA, 0x190);

struct XAUTH_SETTINGS {
  xe::be<uint32_t> SizeOfStruct;
  xe::be<uint32_t> Flags;
  xe::be<uint32_t> title_buffer;
};
static_assert_size(XAUTH_SETTINGS, 0xC);

struct XWSABUF {
  xe::be<uint32_t> len;
  xe::be<uint32_t> buf_ptr;
};

#pragma pack(push, 4)
struct X_PERFORMANCE_COUNTERS {
  xe::be<uint32_t> unk1;  // 0x0, sz:0x4
  xe::be<uint32_t> unk2;  // 0x4, sz:0x4
  xe::be<uint32_t> unk3;  // 0x8, sz:0x4
  xe::be<uint64_t>
      unk4;  // 0xC, sz:0x8, set to 0 by NetDll_XHttpResetPerfCounters
  xe::be<uint64_t>
      unk5;  // 0x14, sz:0x8, set to 0 by NetDll_XHttpResetPerfCounters
};
static_assert_size(X_PERFORMANCE_COUNTERS, 0x1C);
#pragma pack(pop)

struct X_HTTP_OBJECT {
  xe::be<uint32_t> caller_type;  // 0x0, sz:0x4, 1 - title
  xe::be<uint32_t> refcount;     // 0x4, sz:0x4
  xe::be<uint32_t> unk2;         // 0x8, sz:0x4
  xe::be<uint32_t>
      unk3;  // 0xc, sz:0x4, set to 0 by xeNetDll_XHttpGetPerfCounters &
             // NetDll_XHttpResetPerfCounters
  X_PERFORMANCE_COUNTERS perf;  // 0x10, sz:0x1c
};
static_assert_size(X_HTTP_OBJECT, 0x2C);

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = xe::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = xe::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      xe::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      xe::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      xe::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};
static_assert_size(XNetStartupParams, 0xD);

XNetStartupParams xnet_startup_params = {0};

dword_result_t NetDll_XNetStartup_entry(dword_t caller,
                                        pointer_t<XNetStartupParams> params) {
  XELOGI("NetDll_XNetStartup: caller={}, params={:08X}", caller.value(),
         params.guest_address());

  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  // Gives the transport time to come up before the title looks for peers.
  GetSystemLink(kernel_state());

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetStartup, kNetworking, kImplemented);

// https://github.com/jogolden/testdev/blob/master/xkelib/syssock.h#L46
dword_result_t NetDll_XNetStartupEx_entry(dword_t caller,
                                          pointer_t<XNetStartupParams> params,
                                          dword_t versionReq) {
  // versionReq
  // MW3, Ghosts: 0x20501400

  return NetDll_XNetStartup_entry(caller, params);
}
DECLARE_XAM_EXPORT1(NetDll_XNetStartupEx, kNetworking, kImplemented);

dword_result_t NetDll_XNetCleanup_entry(dword_t caller, lpvoid_t params) {
  if (auto link = GetSystemLink(kernel_state())) {
    link->Reset();
  }

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetCleanup, kNetworking, kImplemented);

dword_result_t XNetLogonGetMachineID_entry(lpqword_t machine_id_ptr) {
  // See Netplay
  *machine_id_ptr = 0;
  return 0x80151802;  // X_ERROR_LOGON_NOT_LOGGED_ON;
}
DECLARE_XAM_EXPORT1(XNetLogonGetMachineID, kNetworking, kStub);

dword_result_t NetDll_XNetGetOpt_entry(dword_t one, dword_t option_id,
                                       lpvoid_t buffer_ptr,
                                       lpdword_t buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return uint32_t(X_WSAError::X_WSAEMSGSIZE);
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      XELOGE("NetDll_XNetGetOpt: option {} unimplemented",
             static_cast<uint32_t>(option_id));
      return uint32_t(X_WSAError::X_WSAEINVAL);
  }
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetOpt, kNetworking, kSketchy);

dword_result_t NetDll_XNetRandom_entry(dword_t caller, lpvoid_t buffer_ptr,
                                       dword_t length) {
  uint8_t* buffer_data_ptr = buffer_ptr.as<uint8_t*>();

  if (buffer_data_ptr == nullptr || length == 0) {
    return X_ERROR_SUCCESS;
  }

  std::random_device rnd;
  std::mt19937_64 gen(rnd());
  std::uniform_int_distribution<int> dist(0,
                                          std::numeric_limits<uint8_t>::max());

  std::generate(buffer_data_ptr, buffer_data_ptr + length,
                [&]() { return static_cast<uint8_t>(dist(gen)); });

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetRandom, kNetworking, kImplemented);

dword_result_t NetDll_WSAStartup_entry(dword_t caller, word_t version,
                                       pointer_t<X_WSADATA> data_ptr) {
  // TODO(benvanik): abstraction layer needed.
  XELOGI("NetDll_WSAStartup: version={:04X}, data_ptr={:08X}", version.value(),
         data_ptr.guest_address());

  int ret = 0;

#ifdef XE_PLATFORM_WIN32
  WSADATA wsaData = {};

  ret = WSAStartup(version, &wsaData);
#endif

  if (data_ptr) {
    auto data_out = kernel_state()->memory()->TranslateVirtual(data_ptr);

#ifdef XE_PLATFORM_WIN32
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
#else
    data_ptr->version = version.value();
    data_ptr->version_high = 0x0202;
#endif

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    // vendor_info_ptr is at offset 0x18A (after max_udpdg at 0x188)
    uint32_t vendor_ptr = xe::load_and_swap<uint32_t>(data_out + 0x18A);
    xe::store_and_swap<uint32_t>(data_out + 0x18A, vendor_ptr);
  }

  // DEBUG
  /*
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_WSAStartup, kNetworking, kImplemented);

dword_result_t NetDll_WSAStartupEx_entry(dword_t caller, word_t version,
                                         pointer_t<X_WSADATA> data_ptr,
                                         dword_t versionReq) {
  return NetDll_WSAStartup_entry(caller, version, data_ptr);
}
DECLARE_XAM_EXPORT1(NetDll_WSAStartupEx, kNetworking, kImplemented);

dword_result_t NetDll_WSACleanup_entry(dword_t caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_WSACleanup, kNetworking, kImplemented);

// Instead of using dedicated storage for WSA error like on OS.
// Xbox shares space between normal error codes and WSA errors.
// This under the hood returns directly value received from RtlGetLastError.
dword_result_t NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}
DECLARE_XAM_EXPORT1(NetDll_WSAGetLastError, kNetworking, kImplemented);

dword_result_t NetDll_WSARecvFrom_entry(
    dword_t caller, dword_t socket_handle, pointer_t<XWSABUF> buffers_ptr,
    dword_t buffer_count, lpdword_t num_bytes_recv, lpdword_t flags_ptr,
    pointer_t<XSOCKADDR_IN> from_ptr, lpdword_t from_len_ptr,
    pointer_t<XWSAOVERLAPPED> overlapped_ptr, lpvoid_t completion_routine_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }
  if (!buffers_ptr || !buffer_count) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAEINVAL));
    return -1;
  }

  std::vector<std::span<uint8_t>> buffers;
  for (uint32_t i = 0; i < buffer_count; ++i) {
    buffers.emplace_back(
        kernel_memory()->TranslateVirtual<uint8_t*>(buffers_ptr[i].buf_ptr),
        static_cast<uint32_t>(buffers_ptr[i].len));
  }
  const int received =
      socket->WSARecvFrom(std::move(buffers), flags_ptr ? flags_ptr.value() : 0,
                          from_ptr, from_len_ptr, overlapped_ptr);
  if (received < 0) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }
  if (num_bytes_recv) {
    *num_bytes_recv = static_cast<uint32_t>(received);
  }
  if (flags_ptr) {
    *flags_ptr = 0;
  }
  return 0;
}
DECLARE_XAM_EXPORT2(NetDll_WSARecvFrom, kNetworking, kImplemented,
                    kHighFrequency);

dword_result_t NetDll_WSAGetOverlappedResult_entry(
    dword_t caller, dword_t socket_handle,
    pointer_t<XWSAOVERLAPPED> overlapped_ptr, lpdword_t bytes_ptr, dword_t wait,
    lpdword_t flags_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return 0;
  }
  if (!overlapped_ptr) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAEINVAL));
    return 0;
  }

  while (socket->IsOverlappedPending(overlapped_ptr)) {
    auto event = overlapped_ptr->event_handle
                     ? kernel_state()->object_table()->LookupObject<XEvent>(
                           overlapped_ptr->event_handle)
                     : object_ref<XEvent>();
    if (!wait || !event) {
      XThread::SetLastError(uint32_t(X_WSAError::X_WSA_IO_INCOMPLETE));
      return 0;
    }
    event->Wait(0, 0, false, nullptr);
  }

  // An overlapped this socket never took still reads as in progress.
  if (overlapped_ptr->internal == X_STATUS_PENDING) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSA_IO_INCOMPLETE));
    return 0;
  }
  // internal holds the WSA error of a failed operation as 0x8007xxxx. The
  // count and flags are left unwritten then.
  if (int32_t(overlapped_ptr->internal) < 0) {
    XThread::SetLastError(
        xboxkrnl::xeRtlNtStatusToDosError(overlapped_ptr->internal));
    return 0;
  }
  if (bytes_ptr) {
    *bytes_ptr = overlapped_ptr->internal_high;
  }
  if (flags_ptr) {
    *flags_ptr = overlapped_ptr->offset.low;
  }
  return 1;
}
DECLARE_XAM_EXPORT2(NetDll_WSAGetOverlappedResult, kNetworking, kImplemented,
                    kHighFrequency);

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
dword_result_t NetDll_WSASendTo_entry(
    dword_t caller, dword_t socket_handle, pointer_t<XWSABUF> buffers,
    dword_t num_buffers, lpdword_t num_bytes_sent, dword_t flags,
    pointer_t<XSOCKADDR_IN> to_ptr, dword_t to_len,
    pointer_t<XWSAOVERLAPPED> overlapped, lpvoid_t completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                kernel_memory()->TranslateVirtual(buffers[i].buf_ptr),
                buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  // A NULL destination sends to the connected peer.
  N_XSOCKADDR_IN native_to;
  N_XSOCKADDR_IN* to = nullptr;
  if (to_ptr) {
    native_to = *to_ptr;
    to = &native_to;
  }
  int ret = socket->SendTo(combined_buffer_mem.data(), combined_buffer_size,
                           flags, to, to_len);
  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }
  if (num_bytes_sent) {
    *num_bytes_sent = static_cast<uint32_t>(ret);
  }

  // TODO: Instantly complete overlapped

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_WSASendTo, kNetworking, kImplemented);

dword_result_t NetDll_WSAWaitForMultipleEvents_entry(dword_t num_events,
                                                     lpdword_t events,
                                                     dword_t wait_all,
                                                     dword_t timeout,
                                                     dword_t alertable) {
  if (num_events > 64) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSA_INVALID_PARAMETER));
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(
        num_events, events,
        wait_all ? X_KWAIT_REASON::WaitAll : X_KWAIT_REASON::WaitAny, 1,
        alertable, timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  // The signaled event's index, WSA_WAIT_TIMEOUT or WSA_WAIT_IO_COMPLETION.
  return result;
}
DECLARE_XAM_EXPORT2(NetDll_WSAWaitForMultipleEvents, kNetworking, kImplemented,
                    kBlocking);

dword_result_t NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(kernel_state());
  ev->Initialize(true, false);
  return ev->handle();
}
DECLARE_XAM_EXPORT1(NetDll_WSACreateEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSACloseEvent_entry(dword_t event_handle) {
  X_STATUS result = kernel_state()->object_table()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSACloseEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSAResetEvent_entry(dword_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSAResetEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSASetEvent_entry(dword_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSASetEvent, kNetworking, kImplemented);

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static constexpr uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static constexpr uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static constexpr uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static constexpr uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static constexpr uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static constexpr uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static constexpr uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static constexpr uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static constexpr uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

dword_result_t NetDll_XNetGetTitleXnAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> addr_ptr) {
  if (auto link = GetSystemLink(kernel_state())) {
    std::memcpy(addr_ptr, link->identity().data(), sizeof(XNADDR));
    return XnAddrStatus::XNET_GET_XNADDR_ETHERNET |
           XnAddrStatus::XNET_GET_XNADDR_STATIC;
  }

  // Just return a loopback address atm.
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);

  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetTitleXnAddr, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetDebugXnAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetDebugXnAddr, kNetworking, kStub);

dword_result_t NetDll_XNetXnAddrToMachineId_entry(dword_t caller,
                                                  pointer_t<XNADDR> addr_ptr,
                                                  lpqword_t id_ptr) {
  if (GetSystemLink(kernel_state())) {
    if (!addr_ptr || !id_ptr) {
      return 10014;  // WSAEFAULT
    }
    net::secure::Address address;
    std::memcpy(address.data(), addr_ptr, address.size());
    *id_ptr = net::SystemLink::MachineId(address);
    return 0;
  }

  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XNetXnAddrToMachineId, kNetworking, kImplemented);

dword_result_t XNetLogonGetTitleID_entry(dword_t caller, lpvoid_t params) {
  return kernel_state()->title_id();
}
DECLARE_XAM_EXPORT1(XNetLogonGetTitleID, kNetworking, kImplemented);

void NetDll_XNetInAddrToString_entry(dword_t caller, dword_t in_addr,
                                     lpstring_t string_out,
                                     dword_t string_size) {
  strncpy(string_out, "666.666.666.666", string_size);
}
DECLARE_XAM_EXPORT1(NetDll_XNetInAddrToString, kNetworking, kStub);

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
dword_result_t NetDll_XNetXnAddrToInAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> xn_addr,
                                               lpvoid_t xid, lpvoid_t in_addr) {
  auto link = GetSystemLink(kernel_state());
  if (!link) {
    return 1;
  }
  if (!xn_addr || !xid || !in_addr) {
    return 10014;  // WSAEFAULT
  }
  net::secure::Address address;
  net::KeyId id;
  std::memcpy(address.data(), xn_addr, address.size());
  std::memcpy(id.data(), xid, id.size());
  uint32_t secure = 0;
  const uint32_t result = link->Map(address, id, secure);
  if (!result) {
    xe::store_and_swap<uint32_t>(in_addr, secure);
  }
  return result;
}
DECLARE_XAM_EXPORT1(NetDll_XNetXnAddrToInAddr, kNetworking, kImplemented);

// Does the reverse of the above.
dword_result_t NetDll_XNetInAddrToXnAddr_entry(dword_t caller, dword_t in_addr,
                                               pointer_t<XNADDR> xn_addr,
                                               lpvoid_t xid) {
  auto link = GetSystemLink(kernel_state());
  if (!link) {
    return 1;
  }
  net::secure::Address address;
  net::KeyId id;
  const uint32_t result = link->Reverse(in_addr, address, id);
  if (!result) {
    if (xn_addr) {
      std::memcpy(xn_addr, address.data(), address.size());
    }
    if (xid) {
      std::memcpy(xid, id.data(), id.size());
    }
  }
  return result;
}
DECLARE_XAM_EXPORT1(NetDll_XNetInAddrToXnAddr, kNetworking, kImplemented);

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
dword_result_t NetDll_XNetSetSystemLinkPort_entry(dword_t caller,
                                                  dword_t port) {
  return GetSystemLink(kernel_state()) ? 0 : 1;
}
DECLARE_XAM_EXPORT1(NetDll_XNetSetSystemLinkPort, kNetworking, kImplemented);

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static constexpr uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static constexpr uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static constexpr uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static constexpr uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static constexpr uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

dword_result_t NetDll_XNetGetEthernetLinkStatus_entry(dword_t caller) {
  return SystemLinkEnabled() ? XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE : 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetEthernetLinkStatus, kNetworking,
                    kImplemented);

// Fills |dns| with the IPv4 addresses |host| resolves to.
static void ResolveDnsHost(const std::string& host, XNDNS* dns) {
  asio::error_code ec;
  asio::io_context io;
  asio::ip::tcp::resolver resolver(io);
  auto results = resolver.resolve(host, "", ec);
  if (ec) {
    return;
  }
  for (const auto& entry : results) {
    if (dns->cina >= xe::countof(dns->aina)) {
      break;
    }
    auto address = entry.endpoint().address();
    if (address.is_v4()) {
      dns->aina[dns->cina].s_addr = htonl(address.to_v4().to_uint());
      dns->cina = dns->cina + 1;
    }
  }
  if (dns->cina) {
    dns->status = 0;
  }
}

dword_result_t NetDll_XNetDnsLookup_entry(dword_t caller, lpstring_t host,
                                          dword_t event_handle,
                                          lpdword_t pdns) {
  if (pdns) {
    XNDNS result = {};
    result.status = int32_t(X_WSAError::X_WSAHOST_NOT_FOUND);
    if (host) {
      const std::string name(host.value());
      // A lookup blocks for as long as the resolver takes.
      kernel_state()->RunBlockingIo(
          [&name, &result]() {
            // Nothing may throw out of here, as the wait ends only once this
            // returns.
            try {
              ResolveDnsHost(name, &result);
            } catch (...) {
            }
          },
          GuestScheduler::BlockingCallClass::kConcurrent);
      XELOGD("XNetDnsLookup({}) = {} addresses", name, uint32_t(result.cina));
    }
    auto dns_guest = kernel_memory()->SystemHeapAlloc(sizeof(XNDNS));
    std::memcpy(kernel_memory()->TranslateVirtual(dns_guest), &result,
                sizeof(XNDNS));
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev =
        kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetDnsLookup, kNetworking, kImplemented);

dword_result_t NetDll_XNetDnsRelease_entry(dword_t caller,
                                           pointer_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  kernel_memory()->SystemHeapFree(dns.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetDnsRelease, kNetworking, kStub);

dword_result_t NetDll_XNetQosServiceLookup_entry(dword_t caller, dword_t flags,
                                                 dword_t event_handle,
                                                 lpdword_t pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = kernel_memory()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = kernel_memory()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev =
        kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosServiceLookup, kNetworking, kStub);

dword_result_t NetDll_XNetQosLookup_entry(
    dword_t caller, dword_t xnaddr_count, lpdword_t xnaddrs, lpdword_t xnkids,
    lpdword_t xnkeys, dword_t gateway_count, lpvoid_t gateways,
    lpvoid_t service_ids, dword_t probe_count, dword_t bits_per_sec,
    dword_t flags, dword_t event_handle, lpdword_t qos_ptr) {
  auto link = GetSystemLink(kernel_state());
  if (!link) {
    return X_ERROR_FUNCTION_FAILED;
  }
  // Replies name targets in 16 bits.
  if (!qos_ptr || xnaddr_count > 0xFFFF || gateway_count > 0xFFFF ||
      (xnaddr_count && (!xnaddrs || !xnkids || !xnkeys))) {
    return net::kWsaInvalid;
  }
  auto memory = kernel_memory();
  const xe::be<uint32_t>* addresses = xnaddrs;
  const xe::be<uint32_t>* ids = xnkids;
  const xe::be<uint32_t>* keys = xnkeys;
  std::vector<net::QosTarget> targets(xnaddr_count);
  for (uint32_t i = 0; i < xnaddr_count; ++i) {
    if (!addresses[i] || !ids[i] || !keys[i]) {
      return net::kWsaInvalid;
    }
    auto& target = targets[i];
    std::memcpy(target.address.data(), memory->TranslateVirtual(addresses[i]),
                target.address.size());
    std::memcpy(target.id.data(), memory->TranslateVirtual(ids[i]),
                target.id.size());
    std::memcpy(target.key.data(), memory->TranslateVirtual(keys[i]),
                target.key.size());
  }
  const uint32_t count = xnaddr_count + gateway_count;
  // Each host's data follows the entries so XNetQosRelease frees it too.
  const uint32_t data_offset =
      offsetof(XNQOS, info) + sizeof(XNQOSINFO) * std::max(count, 1u);
  const uint32_t size =
      data_offset + xnaddr_count * uint32_t(net::kQosDataLimit);
  const uint32_t qos_guest = memory->SystemHeapAlloc(size);
  if (!qos_guest) {
    return net::kWsaNoBuffers;
  }
  auto qos = memory->TranslateVirtual<XNQOS*>(qos_guest);
  std::memset(qos, 0, size);
  qos->count = count;
  qos->count_pending = uint32_t(xnaddr_count);
  // A System Link LAN has no Live servers to probe.
  for (uint32_t i = xnaddr_count; i < count; ++i) {
    qos->info[i].flags = XNET_XNQOSINFO_COMPLETE;
  }
  *qos_ptr = qos_guest;
  object_ref<XEvent> event;
  if (event_handle) {
    event = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
  }
  if (!xnaddr_count) {
    if (event) {
      event->Set(0, false);
    }
    return 0;
  }
  const uint16_t probes = uint16_t(probe_count);
  auto complete = [memory, qos_guest, data_offset, probes, event](
                      size_t target, const net::QosResult& result) {
    auto qos = memory->TranslateVirtual<XNQOS*>(qos_guest);
    auto& info = qos->info[target];
    uint8_t info_flags = XNET_XNQOSINFO_COMPLETE;
    info.probes_xmit = probes;
    if (result.contacted) {
      info_flags |= XNET_XNQOSINFO_TARGET_CONTACTED;
      if (result.disabled) {
        info_flags |= XNET_XNQOSINFO_TARGET_DISABLED;
      }
      // LAN hosts are reported close and fast since the path is not timed.
      info.probes_recv = probes;
      info.rtt_min_in_msecs = 1;
      info.rtt_med_in_msecs = 1;
      info.up_bits_per_sec = 100000000;
      info.down_bits_per_sec = 100000000;
      if (!result.data.empty()) {
        const uint32_t data_guest =
            qos_guest + data_offset +
            uint32_t(target) * uint32_t(net::kQosDataLimit);
        std::memcpy(memory->TranslateVirtual(data_guest), result.data.data(),
                    result.data.size());
        info.data_ptr = data_guest;
        info.data_len = uint16_t(result.data.size());
        info_flags |= XNET_XNQOSINFO_DATA_RECEIVED;
      }
    }
    info.flags = info_flags;
    // Titles poll the pending count, which drops only once the entry is done.
    std::atomic_thread_fence(std::memory_order_release);
    qos->count_pending = qos->count_pending - 1;
    if (event) {
      event->Set(0, false);
    }
  };
  link->QosLookup(qos_guest, std::move(targets), std::move(complete));
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosLookup, kNetworking, kImplemented);

dword_result_t NetDll_XNetQosRelease_entry(dword_t caller,
                                           pointer_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  if (auto link = GetSystemLink(kernel_state())) {
    link->CancelQos(qos.guest_address());
  }
  kernel_memory()->SystemHeapFree(qos.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosRelease, kNetworking, kImplemented);

dword_result_t NetDll_XNetQosListen_entry(dword_t caller, lpvoid_t id,
                                          lpvoid_t data, dword_t data_size,
                                          dword_t bits_per_sec, dword_t flags) {
  auto link = GetSystemLink(kernel_state());
  if (!link) {
    return X_ERROR_FUNCTION_FAILED;
  }
  if (!id) {
    return net::kWsaInvalid;
  }
  net::KeyId key_id;
  std::memcpy(key_id.data(), id.as<const uint8_t*>(), key_id.size());
  std::span<const uint8_t> bytes;
  if ((flags & net::kQosListenSetData) && data) {
    bytes = {data.as<const uint8_t*>(),
             std::min<size_t>(data_size, net::kQosDataLimit)};
  }
  return link->QosListen(key_id, flags, bytes);
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosListen, kNetworking, kImplemented);

dword_result_t NetDll_inet_addr_entry(lpstring_t addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return xe::byte_swap(addr);
}
DECLARE_XAM_EXPORT1(NetDll_inet_addr, kNetworking, kImplemented);

dword_result_t XampXAuthStartup_entry(pointer_t<XAUTH_SETTINGS> setttings) {
  // TODO: save setttings for use later, save current count of setttings, use
  // ExRegisterTitleTerminateNotification with priority = 0x6c000001 and
  // notification_routine set to some xam function and create true. call again
  // with create false and set registor values to zero after. also checks a pre
  // set overlapped if result = X_ERROR_IO_PENDING and if true call
  // XMsgCancelIORequest for that overlap with wait true
  if (setttings->SizeOfStruct != 8) {
    return 0x80158401;
  }

  return 0x80158406;
}
DECLARE_XAM_EXPORT1(XampXAuthStartup, kNetworking, kStub);

void XampXAuthShutdown_entry(lpdword_t xauth_count_ptr) {
  // reduce xauth_count by 1 and return current count. if xauth_count = 0 then
  // check a pre set overlapped if result = X_ERROR_IO_PENDING and if true call
  // XMsgCancelIORequest for that overlap with wait true, next call
  // ExRegisterTitleTerminateNotification with same registor as XampXAuthStartup
  // with create set false
  *xauth_count_ptr = 0;
}
DECLARE_XAM_EXPORT1(XampXAuthShutdown, kNetworking, kStub);

dword_result_t XampXAuthGetTitleBuffer_entry() {
  // returns title buffer set by XampXAuthStartup, non-zero causes crash
  return 0;
}
DECLARE_XAM_EXPORT1(XampXAuthGetTitleBuffer, kNetworking, kStub);

dword_result_t NetDll_XHttpStartup_entry(dword_t caller, dword_t reserved,
                                         dword_t reserved_ptr,
                                         const ppc_context_t& ctx) {
  // All created X_HTTP_OBJECTs' pointers are saved into a global array where
  // other http functions use the xnc to recall them from. ex)
  // X_HTTP_OBJECT_PTR_ARRAY[xnc]
  // if caller == 2 use XamAllocEx(0x20b00000, 0x5000000, sizeof(X_HTTP_OBJECT),
  // &object); if it returns error use XThread::SetLastError(0xE). Use
  // xboxkrnl::xeAllocatePoolTypeWithTag if caller != 2 with tag = 0x48545450,
  // and pool_selector = 1 Increase global count of existing xhttp objects when
  // made. Sets new X_HTTP_OBJECT caller_type = caller, refcount = 1, unk3 = 0,
  // & zeros object->perf calls NetDll_WSAStartup here where if the error
  // returned is 0x2726, then it calls NetDll_WSACleanup and remove the
  // X_HTTP_OBJECT from array and call XamFree or ExFreePool to remove it from
  // memory. Use the returned error in SetLastError

  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpStartup, kNetworking, kStub);

dword_result_t NetDll_XHttpShutdown_entry(dword_t caller) {
  // NetDll_XHttpStartup creates nothing to release.
  XThread::SetLastError(X_ERROR_SUCCESS);
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpShutdown, kNetworking, kStub);

dword_result_t NetDll_XHttpOpen_entry(dword_t caller, lpstring_t user_agent,
                                      dword_t access_type,
                                      lpstring_t proxy_name,
                                      lpstring_t proxy_bypass, dword_t flags) {
  // retrieves xhttp directly from array X_HTTP_OBJECT_PTR_ARRAY[xnc]
  if ((access_type != 0 && access_type != 1 && access_type != 3) ||
      proxy_bypass) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
  }

  XThread::SetLastError(X_ERROR_NOT_SUPPORTED);  // Until properly implemented
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpOpen, kNetworking, kStub);

dword_result_t NetDll_socket_entry(dword_t caller, dword_t af, dword_t type,
                                   dword_t protocol) {
  XELOGI("NetDll_socket: af={}, type={}, protocol={}", af.value(), type.value(),
         protocol.value());

  XSocket* socket = new XSocket(kernel_state());
  X_STATUS result = socket->Initialize(XSocket::AddressFamily((uint32_t)af),
                                       XSocket::Type((uint32_t)type),
                                       XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    XThread::SetLastError(socket->GetLastWSAError());
    XELOGE("NetDll_socket: failed with error {:08X}",
           socket->GetLastWSAError());
    return -1;
  }

  return socket->handle();
}
DECLARE_XAM_EXPORT1(NetDll_socket, kNetworking, kImplemented);

dword_result_t NetDll_closesocket_entry(dword_t caller, dword_t socket_handle) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_closesocket, kNetworking, kImplemented);

int_result_t NetDll_shutdown_entry(dword_t caller, dword_t socket_handle,
                                   int_t how) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_shutdown, kNetworking, kImplemented);

dword_result_t NetDll_setsockopt_entry(dword_t caller, dword_t socket_handle,
                                       dword_t level, dword_t optname,
                                       lpvoid_t optval_ptr, dword_t optlen) {
  XELOGI("NetDll_setsockopt: socket={:08X}, level={:04X}, optname={:04X}",
         socket_handle.value(), level.value(), optname.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  int result = XSUCCEEDED(status) ? 0 : -1;
  XELOGI("NetDll_setsockopt: {}, returning {}",
         XSUCCEEDED(status) ? "success" : "failed", result);
  return result;
}
DECLARE_XAM_EXPORT1(NetDll_setsockopt, kNetworking, kImplemented);

dword_result_t NetDll_getsockopt_entry(dword_t caller, dword_t socket_handle,
                                       dword_t level, dword_t optname,
                                       lpvoid_t optval_ptr, lpdword_t optlen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  if (!optval_ptr || !optlen) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAEFAULT));
    return -1;
  }
  uint32_t native_len = *optlen;
  X_STATUS status = socket->GetOption(level, optname, optval_ptr, &native_len);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }
  *optlen = native_len;
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_getsockopt, kNetworking, kImplemented);

dword_result_t NetDll_ioctlsocket_entry(dword_t caller, dword_t socket_handle,
                                        dword_t cmd, lpvoid_t arg_ptr) {
  XELOGI("NetDll_ioctlsocket: socket={:08X}, cmd={:08X}", socket_handle.value(),
         cmd.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    XELOGE("NetDll_ioctlsocket: failed with error {:08X}",
           socket->GetLastWSAError());
    return -1;
  }

  // TODO
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_ioctlsocket, kNetworking, kImplemented);

dword_result_t NetDll_bind_entry(dword_t caller, dword_t socket_handle,
                                 pointer_t<XSOCKADDR_IN> name,
                                 dword_t namelen) {
  XELOGI("NetDll_bind: socket={:08X}, namelen={}", socket_handle.value(),
         namelen.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  XELOGI(
      "NetDll_bind: calling socket->Bind, family={}, port={:04X}, addr={:08X}",
      native_name.sin_family, uint16_t(native_name.sin_port),
      uint32_t(native_name.sin_addr));
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    XELOGE("NetDll_bind: failed with error {:08X}", socket->GetLastWSAError());
    return -1;
  }

  XELOGI("NetDll_bind: success, port={}", socket->bound_port());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_bind, kNetworking, kImplemented);

dword_result_t NetDll_connect_entry(dword_t caller, dword_t socket_handle,
                                    pointer_t<XSOCKADDR> name,
                                    dword_t namelen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_connect, kNetworking, kImplemented);

dword_result_t NetDll_listen_entry(dword_t caller, dword_t socket_handle,
                                   int_t backlog) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_listen, kNetworking, kImplemented);

dword_result_t NetDll_accept_entry(dword_t caller, dword_t socket_handle,
                                   pointer_t<XSOCKADDR> addr_ptr,
                                   lpdword_t addrlen_ptr) {
  // The address is optional but one asked for needs room.
  if (addr_ptr && (!addrlen_ptr || *addrlen_ptr < sizeof(XSOCKADDR))) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAEFAULT));
    return -1;
  }

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  N_XSOCKADDR native_addr;
  int native_len = sizeof(native_addr);
  auto new_socket =
      socket->Accept(addr_ptr ? &native_addr : nullptr, &native_len);
  if (new_socket) {
    if (addr_ptr) {
      addr_ptr->address_family = native_addr.address_family;
      std::memcpy(addr_ptr->sa_data, native_addr.sa_data,
                  sizeof(native_addr.sa_data));
      *addrlen_ptr = native_len;
    }
    return new_socket->handle();
  }
  XThread::SetLastError(socket->GetLastWSAError());
  return -1;
}
DECLARE_XAM_EXPORT1(NetDll_accept, kNetworking, kImplemented);

struct x_fd_set {
  xe::be<uint32_t> fd_count;
  xe::be<uint32_t> fd_array[64];
};

// Which select set a host_set is, since System Link sockets answer for
// themselves.
enum class SelectSet { kRead, kWrite, kExcept };

struct host_set {
  explicit host_set(SelectSet which) : set(which) {}

  SelectSet set;
  uint32_t count = 0;
  object_ref<XSocket> sockets[64];

  // Returns false, like Winsock with WSAENOTSOCK, on a handle that is not a
  // socket. A set holds at most 64 handles.
  bool Load(const x_fd_set* guest_set) {
    this->count = std::min<uint32_t>(guest_set->fd_count, 64);
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket =
          kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
      if (!socket) {
        this->count = i;
        return false;
      }
      this->sockets[i] = socket;
    }
    return true;
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  // Returns how many host handles it set.
  uint32_t Store(fd_set* native_set) {
    FD_ZERO(native_set);
    max_fd = -1;
    uint32_t stored = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      if (this->sockets[i]->system_link()) {
        continue;
      }
      // Another guest thread can close a socket while select waits.
      const auto handle = this->sockets[i]->native_handle();
      if (handle == static_cast<uint64_t>(-1)) {
        continue;
      }
#ifndef XE_PLATFORM_WIN32
      // A POSIX fd_set is a bitmap of FD_SETSIZE descriptors.
      if (handle >= static_cast<uint64_t>(FD_SETSIZE)) {
        XELOGE("NetDll_select: host socket {} is past FD_SETSIZE", handle);
        continue;
      }
#endif
      FD_SET(handle, native_set);
      max_fd = std::max(max_fd, static_cast<int64_t>(handle));
      ++stored;
    }
    return stored;
  }

  // The highest host handle the last Store set, or -1.
  int64_t max_fd = -1;

  bool HasSystemLink() const {
    return std::any_of(
        this->sockets, this->sockets + this->count,
        [](const auto& socket) { return socket->system_link(); });
  }

  // A System Link socket answers for itself. Only a stream's failed connect is
  // an exception.
  bool SystemLinkReady(const XSocket& socket) const {
    switch (this->set) {
      case SelectSet::kRead:
        return socket.SystemLinkReadable();
      case SelectSet::kWrite:
        return socket.SystemLinkWritable();
      default:
        return socket.SystemLinkFailed();
    }
  }

  uint32_t CountSystemLinkReady() const {
    uint32_t ready = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      if (this->sockets[i]->system_link() &&
          SystemLinkReady(*this->sockets[i])) {
        ++ready;
      }
    }
    return ready;
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      bool ready;
      if (socket->system_link()) {
        ready = SystemLinkReady(*socket);
      } else {
        const auto handle = socket->native_handle();
        ready =
            handle != static_cast<uint64_t>(-1) && FD_ISSET(handle, native_set);
      }
      if (ready) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

int_result_t NetDll_select_entry(dword_t caller, dword_t nfds,
                                 pointer_t<x_fd_set> readfds,
                                 pointer_t<x_fd_set> writefds,
                                 pointer_t<x_fd_set> exceptfds,
                                 lpvoid_t timeout_ptr) {
  XELOGD("NetDll_select: nfds={}, timeout={:08X}", nfds.value(),
         timeout_ptr.guest_address());

  host_set host_readfds(SelectSet::kRead);
  fd_set native_readfds = {0};
  host_set host_writefds(SelectSet::kWrite);
  fd_set native_writefds = {0};
  host_set host_exceptfds(SelectSet::kExcept);
  fd_set native_exceptfds = {0};
  if ((readfds && !host_readfds.Load(readfds)) ||
      (writefds && !host_writefds.Load(writefds)) ||
      (exceptfds && !host_exceptfds.Load(exceptfds))) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }
  uint32_t native_count = 0;
  // Winsock ignores nfds, so titles pass anything. POSIX select only looks
  // at descriptors below it, so give it the highest host one plus one.
  int native_nfds = 0;
  // A select with nothing ready empties the sets, so this refills them too.
  auto store_native = [&]() {
    native_count = host_readfds.Store(&native_readfds) +
                   host_writefds.Store(&native_writefds) +
                   host_exceptfds.Store(&native_exceptfds);
    native_nfds =
        static_cast<int>(std::max({host_readfds.max_fd, host_writefds.max_fd,
                                   host_exceptfds.max_fd}) +
                         1);
  };
  store_native();
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    Clock::ScaleGuestDurationTimeval(
        reinterpret_cast<int32_t*>(&timeout.tv_sec),
        reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  const bool offload = GuestScheduler::CurrentThreadOffloadsBlockingCalls();
  const bool system_link = host_readfds.HasSystemLink() ||
                           host_writefds.HasSystemLink() ||
                           host_exceptfds.HasSystemLink();
  int ret;
  if (!offload && !system_link) {
    ret = select(native_nfds, readfds ? &native_readfds : nullptr,
                 writefds ? &native_writefds : nullptr,
                 exceptfds ? &native_exceptfds : nullptr, timeout_in);
  } else {
    // A blocking select would stall the other guest threads on this host
    // thread and System Link sockets have no host handle to wait on. Poll
    // instead and wait between polls until the timeout.
    auto* scheduler = kernel_state()->guest_scheduler();
    uint64_t deadline_ms = 0;
    if (timeout_in) {
      // Rounded up so it never waits less than asked.
      deadline_ms = Clock::QueryHostUptimeMillis() +
                    uint64_t(timeout.tv_sec) * 1000 +
                    (timeout.tv_usec + 999) / 1000;
    }
    while (true) {
      ret = 0;
      // Windows fails a select with no handles at all.
      if (native_count || !system_link) {
        timeval poll_timeout = {0, 0};
        ret = select(native_nfds, readfds ? &native_readfds : nullptr,
                     writefds ? &native_writefds : nullptr,
                     exceptfds ? &native_exceptfds : nullptr, &poll_timeout);
      }
      if (ret >= 0) {
        ret += host_readfds.CountSystemLinkReady() +
               host_writefds.CountSystemLinkReady() +
               host_exceptfds.CountSystemLinkReady();
      }
      if (ret != 0 ||
          (timeout_in && Clock::QueryHostUptimeMillis() >= deadline_ms)) {
        break;
      }
      store_native();
      if (offload) {
        scheduler->BlockCurrentThread(deadline_ms);
      } else {
        xe::threading::Sleep(std::chrono::milliseconds(1));
      }
    }
  }
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_select, kNetworking, kImplemented);

dword_result_t NetDll_recv_entry(dword_t caller, dword_t socket_handle,
                                 lpvoid_t buf_ptr, dword_t buf_len,
                                 dword_t flags) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  const int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_recv, kNetworking, kImplemented);

dword_result_t NetDll_recvfrom_entry(dword_t caller, dword_t socket_handle,
                                     lpvoid_t buf_ptr, dword_t buf_len,
                                     dword_t flags,
                                     pointer_t<XSOCKADDR_IN> from_ptr,
                                     lpdword_t fromlen_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret = socket->RecvFrom(buf_ptr, buf_len, flags, &native_from,
                             fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
  } else if (ret > 0) {
    // Only log when we actually receive data
    XELOGD("NetDll_recvfrom: socket={:08X}, received {} bytes",
           socket_handle.value(), ret);
  }

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_recvfrom, kNetworking, kImplemented);

dword_result_t NetDll_send_entry(dword_t caller, dword_t socket_handle,
                                 lpvoid_t buf_ptr, dword_t buf_len,
                                 dword_t flags) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  const int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_send, kNetworking, kImplemented);

dword_result_t NetDll_sendto_entry(dword_t caller, dword_t socket_handle,
                                   lpvoid_t buf_ptr, dword_t buf_len,
                                   dword_t flags,
                                   pointer_t<XSOCKADDR_IN> to_ptr,
                                   dword_t to_len) {
  XELOGD("NetDll_sendto: socket={:08X}, len={}", socket_handle.value(),
         buf_len.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  // A NULL destination sends to the connected peer.
  N_XSOCKADDR_IN native_to;
  N_XSOCKADDR_IN* to = nullptr;
  if (to_ptr) {
    native_to = *to_ptr;
    to = &native_to;
  }
  int ret = socket->SendTo(buf_ptr, buf_len, flags, to, to_len);
  if (ret == -1) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_sendto, kNetworking, kImplemented);

dword_result_t NetDll_WSAEventSelect_entry(dword_t caller,
                                           dword_t socket_handle,
                                           dword_t event_handle,
                                           dword_t network_events) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }

  object_ref<XEvent> ev;
  if (event_handle) {
    ev = kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    if (!ev) {
      XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
      return -1;
    }
  }

  int ret = socket->WSAEventSelect(std::move(ev), network_events);
  if (ret < 0) {
    XThread::SetLastError(socket->GetLastWSAError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_WSAEventSelect, kNetworking, kImplemented);

dword_result_t NetDll___WSAFDIsSet_entry(dword_t socket_handle,
                                         pointer_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count =
      std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll___WSAFDIsSet, kNetworking, kImplemented);

void NetDll_WSASetLastError_entry(dword_t error_code) {
  XThread::SetLastError(error_code);
}
DECLARE_XAM_EXPORT1(NetDll_WSASetLastError, kNetworking, kImplemented);

// getsockname and getpeername, which write the guest's sockaddr_in.
static int GetSocketName(uint32_t socket_handle, bool peer,
                         pointer_t<XSOCKADDR_IN> name_ptr, lpdword_t len_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAENOTSOCK));
    return -1;
  }
  if (!name_ptr || !len_ptr || *len_ptr < sizeof(XSOCKADDR_IN)) {
    XThread::SetLastError(uint32_t(X_WSAError::X_WSAEFAULT));
    return -1;
  }

  N_XSOCKADDR_IN name;
  X_STATUS status =
      peer ? socket->GetPeerName(&name) : socket->GetSockName(&name);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->GetLastWSAError());
    return -1;
  }

  name_ptr->sin_family = name.sin_family;
  name_ptr->sin_port = name.sin_port;
  name_ptr->sin_addr = name.sin_addr;
  std::memset(name_ptr->x_sin_zero, 0, sizeof(name_ptr->x_sin_zero));
  *len_ptr = sizeof(XSOCKADDR_IN);
  return 0;
}

dword_result_t NetDll_getsockname_entry(dword_t caller, dword_t socket_handle,
                                        pointer_t<XSOCKADDR_IN> name_ptr,
                                        lpdword_t len_ptr) {
  return GetSocketName(socket_handle, false, name_ptr, len_ptr);
}
DECLARE_XAM_EXPORT1(NetDll_getsockname, kNetworking, kImplemented);

dword_result_t NetDll_getpeername_entry(dword_t caller, dword_t socket_handle,
                                        pointer_t<XSOCKADDR_IN> name_ptr,
                                        lpdword_t len_ptr) {
  return GetSocketName(socket_handle, true, name_ptr, len_ptr);
}
DECLARE_XAM_EXPORT1(NetDll_getpeername, kNetworking, kImplemented);

dword_result_t NetDll_XNetCreateKey_entry(dword_t caller, lpdword_t key_id,
                                          lpdword_t exchange_key) {
  if (!GetSystemLink(kernel_state())) {
    kernel_memory()->Fill(key_id.guest_address(), 8, 0xBE);
    kernel_memory()->Fill(exchange_key.guest_address(), 16, 0xBE);
    return 0;
  }
  if (!key_id || !exchange_key) {
    return 10014;  // WSAEFAULT
  }
  net::KeyId id;
  net::Key key;
  net::SystemLink::CreateKey(id, key);
  std::memcpy(key_id, id.data(), id.size());
  std::memcpy(exchange_key, key.data(), key.size());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetCreateKey, kNetworking, kImplemented);

dword_result_t NetDll_XNetRegisterKey_entry(dword_t caller, lpdword_t key_id,
                                            lpdword_t exchange_key) {
  auto link = GetSystemLink(kernel_state());
  if (!link) {
    return 0;
  }
  if (!key_id || !exchange_key) {
    return 10014;  // WSAEFAULT
  }
  net::KeyId id;
  net::Key key;
  std::memcpy(id.data(), key_id, id.size());
  std::memcpy(key.data(), exchange_key, key.size());
  return link->RegisterKey(id, key);
}
DECLARE_XAM_EXPORT1(NetDll_XNetRegisterKey, kNetworking, kImplemented);

dword_result_t NetDll_XNetUnregisterKey_entry(dword_t caller,
                                              lpdword_t key_id) {
  auto link = GetSystemLink(kernel_state());
  if (!link) {
    return 0;
  }
  if (!key_id) {
    return 10014;  // WSAEFAULT
  }
  net::KeyId id;
  std::memcpy(id.data(), key_id, id.size());
  return link->UnregisterKey(id);
}
DECLARE_XAM_EXPORT1(NetDll_XNetUnregisterKey, kNetworking, kImplemented);

dword_result_t NetDll_XNetConnect_entry(dword_t caller, dword_t in_addr) {
  auto link = GetSystemLink(kernel_state());
  return link ? link->Connect(in_addr) : net::kWsaNetworkDown;
}
DECLARE_XAM_EXPORT1(NetDll_XNetConnect, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetConnectStatus_entry(dword_t caller,
                                                 dword_t in_addr) {
  auto link = GetSystemLink(kernel_state());
  return link ? link->ConnectStatus(in_addr) : net::kConnectStatusLost;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetConnectStatus, kNetworking, kImplemented);

dword_result_t NetDll_XNetUnregisterInAddr_entry(dword_t caller,
                                                 dword_t in_addr) {
  auto link = GetSystemLink(kernel_state());
  return link ? link->Unmap(in_addr) : net::kWsaNetworkDown;
}
DECLARE_XAM_EXPORT1(NetDll_XNetUnregisterInAddr, kNetworking, kImplemented);

// URL_COMPONENTSA with 32-bit guest pointers.
struct X_URL_COMPONENTS {
  xe::be<uint32_t> struct_size;
  xe::be<uint32_t> scheme_ptr;
  xe::be<uint32_t> scheme_length;
  xe::be<uint32_t> scheme;
  xe::be<uint32_t> host_name_ptr;
  xe::be<uint32_t> host_name_length;
  xe::be<uint16_t> port;
  xe::be<uint16_t> padding;
  xe::be<uint32_t> user_name_ptr;
  xe::be<uint32_t> user_name_length;
  xe::be<uint32_t> password_ptr;
  xe::be<uint32_t> password_length;
  xe::be<uint32_t> url_path_ptr;
  xe::be<uint32_t> url_path_length;
  xe::be<uint32_t> extra_info_ptr;
  xe::be<uint32_t> extra_info_length;
};
static_assert_size(X_URL_COMPONENTS, 0x3C);

constexpr uint32_t X_ICU_DECODE = 0x10000000;
constexpr uint32_t X_ICU_ESCAPE = 0x80000000;
constexpr uint32_t X_ERROR_WINHTTP_INVALID_URL = 12005;
constexpr uint32_t X_ERROR_WINHTTP_UNRECOGNIZED_SCHEME = 12006;

enum X_INTERNET_SCHEME : uint32_t {
  X_INTERNET_SCHEME_HTTP = 1,
  X_INTERNET_SCHEME_HTTPS = 2,
};

dword_result_t NetDll_XHttpCrackUrl_entry(dword_t caller, lpvoid_t url_ptr,
                                          dword_t url_length, dword_t flags,
                                          pointer_t<X_URL_COMPONENTS> parts) {
  // TODO(has207): ICU_DECODE and ICU_ESCAPE are accepted but not applied.
  if (!url_ptr || !parts || parts->struct_size != sizeof(X_URL_COMPONENTS) ||
      (flags & ~(X_ICU_DECODE | X_ICU_ESCAPE))) {
    XThread::SetLastError(X_ERROR_INVALID_PARAMETER);
    return 0;
  }
  // A length still stops at the first NUL.
  const char* url = url_ptr.as<const char*>();
  const std::string_view text(
      url, url_length ? strnlen(url, url_length) : std::strlen(url));

  const size_t scheme_end = text.find(':');
  if (scheme_end == std::string_view::npos) {
    XThread::SetLastError(X_ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    return 0;
  }
  std::string scheme(text.substr(0, scheme_end));
  for (char& c : scheme) {
    c = char(std::tolower(uint8_t(c)));
  }
  uint32_t scheme_id;
  uint16_t port;
  if (scheme == "http") {
    scheme_id = X_INTERNET_SCHEME_HTTP;
    port = 80;
  } else if (scheme == "https") {
    scheme_id = X_INTERNET_SCHEME_HTTPS;
    port = 443;
  } else {
    XThread::SetLastError(X_ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
    return 0;
  }
  if (text.substr(scheme_end + 1, 2) != "//") {
    XThread::SetLastError(X_ERROR_WINHTTP_INVALID_URL);
    return 0;
  }

  // Offsets into text of each component.
  const size_t authority_begin = scheme_end + 3;
  size_t authority_end = text.find_first_of("/?#", authority_begin);
  if (authority_end == std::string_view::npos) {
    authority_end = text.size();
  }
  size_t host_begin = authority_begin;
  size_t user_begin = 0, user_size = 0, password_begin = 0, password_size = 0;
  bool has_user = false, has_password = false;
  const size_t at = text.rfind('@', authority_end - 1);
  if (at != std::string_view::npos && at >= authority_begin) {
    has_user = true;
    user_begin = authority_begin;
    const size_t colon = text.find(':', authority_begin);
    if (colon != std::string_view::npos && colon < at) {
      user_size = colon - authority_begin;
      has_password = true;
      password_begin = colon + 1;
      password_size = at - password_begin;
    } else {
      user_size = at - authority_begin;
    }
    host_begin = at + 1;
  }
  size_t host_end = authority_end;
  // An IPv6 literal's colons are inside its brackets.
  size_t port_search_begin = host_begin;
  if (host_begin < authority_end && text[host_begin] == '[') {
    const size_t bracket = text.find(']', host_begin);
    port_search_begin = bracket < authority_end ? bracket + 1 : authority_end;
  }
  const size_t port_colon = text.rfind(':', authority_end - 1);
  if (port_colon != std::string_view::npos && port_colon >= port_search_begin) {
    const std::string_view digits =
        text.substr(port_colon + 1, authority_end - port_colon - 1);
    if (!digits.empty()) {
      uint32_t value = 0;
      for (char c : digits) {
        if (c < '0' || c > '9') {
          XThread::SetLastError(X_ERROR_WINHTTP_INVALID_URL);
          return 0;
        }
        value = value * 10 + uint32_t(c - '0');
        if (value > 0xFFFF) {
          XThread::SetLastError(X_ERROR_WINHTTP_INVALID_URL);
          return 0;
        }
      }
      port = uint16_t(value);
    }
    host_end = port_colon;
  }
  if (host_end == host_begin) {
    XThread::SetLastError(X_ERROR_WINHTTP_INVALID_URL);
    return 0;
  }
  size_t extra_begin = text.find_first_of("?#", authority_end);
  if (extra_begin == std::string_view::npos) {
    extra_begin = text.size();
  }

  // Per component: a null pointer with a nonzero length gets a pointer into the
  // URL. A buffer gets a terminated copy. A zero length skips it.
  bool insufficient = false;
  auto set = [&](xe::be<uint32_t>& ptr, xe::be<uint32_t>& length, bool present,
                 size_t offset, size_t size) {
    if (!length) {
      return;
    }
    if (!ptr) {
      ptr = present ? url_ptr.guest_address() + uint32_t(offset) : 0;
      length = present ? uint32_t(size) : 0;
      return;
    }
    if (length <= size) {
      length = uint32_t(size + 1);
      insufficient = true;
      return;
    }
    char* out = kernel_memory()->TranslateVirtual<char*>(ptr);
    std::memcpy(out, url + offset, size);
    out[size] = '\0';
    length = uint32_t(size);
  };
  set(parts->scheme_ptr, parts->scheme_length, true, 0, scheme_end);
  set(parts->host_name_ptr, parts->host_name_length, true, host_begin,
      host_end - host_begin);
  set(parts->user_name_ptr, parts->user_name_length, has_user, user_begin,
      user_size);
  set(parts->password_ptr, parts->password_length, has_password, password_begin,
      password_size);
  set(parts->url_path_ptr, parts->url_path_length, true, authority_end,
      extra_begin - authority_end);
  set(parts->extra_info_ptr, parts->extra_info_length, true, extra_begin,
      text.size() - extra_begin);
  parts->scheme = scheme_id;
  parts->port = port;
  if (insufficient) {
    XThread::SetLastError(X_ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_XHttpCrackUrl, kNetworking, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Net);
