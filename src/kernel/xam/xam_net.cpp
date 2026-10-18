/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <random>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/live/addressing.h>
#include <rex/kernel/xam/live/live.h>
#include <rex/kernel/xam/live/relay.h>
#include <rex/kernel/xam/live/upnp.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/online_hooks.h>
#include <rex/kernel/xam/net_trace.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/overlapped_receive.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

REXCVAR_DEFINE_BOOL(wwe_net_trace, false, "Kernel",
                    "Log the guest's network, XMsg and session calls (rate-limited)");

// [svr2011] Online: the game is signed in to an online service (xam_user.cpp)
// and its GameSpy web services (Community Creations) go to online_server.
// (SvR 2011's online play, off unless a game turns it on; WWE All Stars' is wwe_online.)
REXCVAR_DEFINE_BOOL(online_enabled, false, "Online",
                    "Signed in online: the ONLINE menu, Community Creations on online_server");
REXCVAR_DEFINE_STRING(online_server, "https://sho-ti.me/svr", "Online",
                      "The Community Creations server (its URL; the port relays the game's GameSpy traffic there)");
// [svr2011] The port's relay (online.cpp) on this PC: the game's GameSpy
// connections go to it (http on online_relay_http, presence on online_relay_gp).
REXCVAR_DEFINE_UINT32(online_relay_http, 0, "Online", "(set by the game: its relay's http port)");
REXCVAR_DEFINE_UINT32(online_relay_gp, 0, "Online", "(set by the game: its relay's presence port)");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {

// [svr2011] DNS answers the game's *.gamespy.com names with this loopback
// address (nothing listens there); connect() sends it on to the port's relay.
constexpr uint32_t kGameSpyAddr = 0x7F0B0001;  // 127.11.0.1

bool IsGameSpyHost(std::string host) {
  for (auto& c : host) c = char(std::tolower(uint8_t(c)));
  const std::string suffix = ".gamespy.com";
  return host == "gamespy.com" ||
         (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0);
}

// [svr2011] SVR2011_NET_TRACE=1: logs the game's network calls (the first
// 40 of each).
void NetTrace(const char* name) {
  static const bool on = std::getenv("SVR2011_NET_TRACE") != nullptr;
  if (!on) return;
  static std::mutex mutex;
  static std::unordered_map<std::string, int> counts;
  std::lock_guard<std::mutex> lock(mutex);
  if (++counts[name] <= 40) {
    REXKRNL_INFO("[svr2011] net: {}", name);
  }
}

// The host socket call's error as the guest's WSA error code.
uint32_t HostSocketError() {
#if REX_PLATFORM_WIN32
  return WSAGetLastError();
#else
  switch (errno) {
    case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case EINPROGRESS:  // (a non-blocking connect: WSAEWOULDBLOCK on Windows)
      return 10035;
    case EALREADY: return 10037;
    case ENOTSOCK: return 10038;
    case EMSGSIZE: return 10040;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    default: return 10022;  // WSAEINVAL
  }
#endif
}

OnlineHooks g_online_hooks;  // [svr2011] (online_hooks.h)

// A socket address the game gives (sendto, connect) as the real one.
void ToHost(N_XSOCKADDR_IN* sin) {
  if (!g_online_hooks.to_host || !sin || sin->sin_family != XSocket::X_AF_INET) return;
  uint32_t addr = sin->sin_addr;
  uint16_t port = sin->sin_port;
  g_online_hooks.to_host(addr, port);
  sin->sin_addr = addr;
  sin->sin_port = port;
}

// A real source address (recvfrom, accept) as the game knows it.
void ToGuest(N_XSOCKADDR_IN* sin) {
  if (!g_online_hooks.to_guest || !sin || sin->sin_family != XSocket::X_AF_INET) return;
  uint32_t addr = sin->sin_addr;
  uint16_t port = sin->sin_port;
  g_online_hooks.to_guest(addr, port);
  sin->sin_addr = addr;
  sin->sin_port = port;
}

// [svr2011] Sockets as the game sees them: small positive numbers (the
// handle less kHandleBase). Games test socket() > 0 as a signed number (SvR
// 2011's peer sockets); 0xF8xxxxxx handles are negative.
uint32_t GuestSocket(uint32_t handle) {
  return handle >= XObject::kHandleBase ? handle - XObject::kHandleBase : handle;
}
object_ref<XSocket> LookupSocket(uint32_t socket) {
  return REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket < XObject::kHandleBase ? socket + XObject::kHandleBase
                                                                                    : socket);
}

}  // namespace

void SetOnlineHooks(OnlineHooks hooks) { g_online_hooks = std::move(hooks); }
const OnlineHooks& online_hooks() { return g_online_hooks; }

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
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
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
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
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

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  NetTrace("NetDll_XNetStartup");
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  NetTrace("NetDll_XNetCleanup");
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  NetTrace("NetDll_XNetGetOpt");
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  NetTrace("NetDll_XNetRandom");
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
  NetTrace("NetDll_WSAStartup");
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  NetTrace("NetDll_WSACleanup");
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  NetTrace("NetDll_WSAGetLastError");
  return XThread::GetLastError();
}

// --wwe_online_ip: a socket that sends or connects to a peer on this PC without a bind
// leaves from this copy's address, not the default one.
void BindToOnlineAddress(XSocket* socket, uint32_t destination) {
  if (!live::Enabled() || socket->bound()) {
    return;
  }
  const uint32_t address = live::ImplicitBindAddress(destination, live::BindAddress());
  if (!address) {
    return;
  }
  N_XSOCKADDR_IN name;
  name.sin_family = AF_INET;
  name.sin_port = 0;
  name.sin_addr = address;
  socket->Bind(&name, sizeof(sockaddr_in));
}

namespace overlapped {

// What a later attempt of a guest WSARecvFrom needs.
struct Receive {
  uint32_t socket;
  uint32_t buffers_ptr;
  uint32_t buffer_count;
  uint32_t from_ptr;
  uint32_t from_len_ptr;
};

net::OverlappedReceives<Receive> receives;

// One datagram from the host socket into the guest buffers, or -1 when none is waiting.
int TryReceive(const Receive& r) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(r.socket);
  if (!socket) {
    return -1;
  }
#if REX_PLATFORM_WIN32
  const auto native = static_cast<SOCKET>(socket->native_handle());
#else
  const auto native = static_cast<int>(socket->native_handle());
#endif
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(native, &readable);
  timeval no_wait{0, 0};
  if (select(int(native + 1), &readable, nullptr, nullptr, &no_wait) <= 0) {
    return -1;
  }
  // Room for any UDP datagram, so a large one is never cut short here.
  static thread_local uint8_t data[65536];
  N_XSOCKADDR_IN from;
  uint32_t from_len = sizeof(from);
  const int received = socket->RecvFrom(data, sizeof(data), 0, &from, &from_len);
  if (received < 0) {
    return -1;
  }
  auto* memory = REX_KERNEL_MEMORY();
  auto* buffers = memory->TranslateVirtual<XWSABUF*>(r.buffers_ptr);
  uint32_t copied = 0;
  for (uint32_t i = 0; i < r.buffer_count && copied < uint32_t(received); ++i) {
    const uint32_t n = std::min<uint32_t>(buffers[i].len, uint32_t(received) - copied);
    std::memcpy(memory->TranslateVirtual(buffers[i].buf_ptr), data + copied, n);
    copied += n;
  }
  if (r.from_ptr) {
    auto* out = memory->TranslateVirtual<XSOCKADDR_IN*>(r.from_ptr);
    out->sin_family = from.sin_family;
    // A peer's mapped router port reaches the game as its game port.
    out->sin_port = live::upnp::Peers().Game(from.sin_addr, from.sin_port);
    out->sin_addr = from.sin_addr;
    std::memset(out->x_sin_zero, 0, sizeof(out->x_sin_zero));
  }
  if (r.from_len_ptr) {
    memory::store_and_swap<uint32_t>(memory->TranslateVirtual(r.from_len_ptr), 16);
  }
  return int(copied);
}

// The OVERLAPPED of a finished operation: status, size, and its event signalled.
void Complete(uint32_t overlapped_ptr, uint32_t bytes) {
  auto* overlapped = REX_KERNEL_MEMORY()->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_ptr);
  overlapped->internal = 0;
  overlapped->internal_high = bytes;
  if (overlapped->event_handle) {
    xboxkrnl::xeNtSetEvent(overlapped->event_handle, nullptr);
  }
}

}  // namespace overlapped

// A receive that finds no datagram stays pending (WSA_IO_PENDING) when it has an
// OVERLAPPED; WSAGetOverlappedResult then polls the host socket for it.
u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket_handle, u32 buffers_ptr, u32 buffer_count,
                             mapped_u32 num_bytes_recv, mapped_u32 flags_ptr, u32 from_ptr,
                             u32 from_len_ptr, u32 overlapped_ptr, u32 completion_routine) {
  if (!REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle)) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  using Status = decltype(overlapped::receives)::Status;
  const overlapped::Receive request{socket_handle, buffers_ptr, buffer_count, from_ptr,
                                    from_len_ptr};
  const auto outcome = overlapped::receives.Start(overlapped_ptr, request, overlapped::TryReceive);
  switch (outcome.status) {
    case Status::kCompleted:
      if (num_bytes_recv) {
        *num_bytes_recv = uint32_t(outcome.bytes);
      }
      if (flags_ptr) {
        *flags_ptr = 0;
      }
      if (overlapped_ptr) {
        overlapped::Complete(overlapped_ptr, uint32_t(outcome.bytes));
      }
      XThread::SetLastError(0);
      return 0;
    case Status::kPending: {
      auto* overlapped =
          REX_KERNEL_MEMORY()->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_ptr);
      overlapped->internal = X_STATUS_PENDING;
      if (overlapped->event_handle) {
        xboxkrnl::xeNtClearEvent(overlapped->event_handle);
      }
      XThread::SetLastError(net::kWsaIoPending);
      return -1;
    }
    default:
      XThread::SetLastError(net::kWsaEWouldBlock);
      return -1;
  }
}

u32 NetDll_WSAGetOverlappedResult_entry(u32 caller, u32 socket_handle, u32 overlapped_ptr,
                                        mapped_u32 bytes_transferred, u32 wait,
                                        mapped_u32 flags_ptr) {
  if (!overlapped_ptr) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return 0;
  }
  using Status = decltype(overlapped::receives)::Status;
  // A wait gives up after 5 s (a closed socket never delivers), as an incomplete result.
  const auto outcome = overlapped::receives.Poll(
      overlapped_ptr, wait ? std::chrono::seconds(5) : std::chrono::seconds(0),
      overlapped::TryReceive, [] { return std::chrono::steady_clock::now(); },
      [] { rex::thread::Sleep(std::chrono::milliseconds(1)); });
  if (outcome.status == Status::kIncomplete) {
    XThread::SetLastError(net::kWsaIoIncomplete);
    return 0;
  }
  if (outcome.status == Status::kCompleted) {
    overlapped::Complete(overlapped_ptr, uint32_t(outcome.bytes));
  }
  // Completed now, or earlier (a receive that completed at once, or a send).
  auto* overlapped = REX_KERNEL_MEMORY()->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_ptr);
  if (bytes_transferred) {
    *bytes_transferred = overlapped->internal_high;
  }
  if (flags_ptr) {
    *flags_ptr = 0;
  }
  return 1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!completion_routine);

  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
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
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  BindToOnlineAddress(socket.get(), native_to.sin_addr);
  native_to.sin_port = live::upnp::Peers().External(native_to.sin_addr, native_to.sin_port);
  if (!live::relay::DropsDirect(native_to.sin_addr, native_to.sin_port)) {
    socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);
  }

  if (num_bytes_sent) {
    *num_bytes_sent = combined_buffer_size;
  }
  // Sends complete at once.
  if (overlapped) {
    overlapped::Complete(overlapped.guest_address(), combined_buffer_size);
  }
  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  NetTrace("NetDll_WSAWaitForMultipleEvents");
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

u32 NetDll_WSACreateEvent_entry() {
  NetTrace("NetDll_WSACreateEvent");
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  NetTrace("NetDll_WSACloseEvent");
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  NetTrace("NetDll_WSAResetEvent");
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  NetTrace("NetDll_WSASetEvent");
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

// Keys, addresses and QoS for Player Match peers, after the Xenia netplay fork's
// xam_net.cc (Copyright 2026 Xenia Canary, BSD license). Players reach each other directly
// at the addresses the game server sees, so there is no security association to set up:
// a key is only remembered for XNetInAddrToXnAddr.
namespace peers {
std::mutex mutex;
uint64_t registered_key = 0;  // the last XNKID XNetRegisterKey was given

uint64_t LoadKey(uint32_t guest_xnkid) {
  return guest_xnkid ? memory::load_and_swap<uint64_t>(REX_KERNEL_MEMORY()->TranslateVirtual(guest_xnkid))
                     : 0;
}

// Peers' records from the server, by XUID and by MAC, so each is asked for once; MACs the
// server doesn't know are remembered too.
std::map<uint64_t, live::PlayerRecord> known;
std::set<std::array<uint8_t, 6>> unknown_macs;

std::optional<live::PlayerRecord> PeerByMac(const uint8_t* mac) {
  {
    std::lock_guard lock(mutex);
    for (const auto& [xuid, record] : known) {
      if (std::memcmp(record.mac.data(), mac, record.mac.size()) == 0) {
        return record;
      }
    }
  }
  std::array<uint8_t, 6> key;
  std::memcpy(key.data(), mac, key.size());
  {
    std::lock_guard lock(mutex);
    if (unknown_macs.count(key)) {
      return std::nullopt;
    }
  }
  const auto record = live::FindPlayerByMac(key);
  std::lock_guard lock(mutex);
  if (record) {
    known[record->xuid] = *record;
  } else {
    unknown_macs.insert(key);
  }
  return record;
}

std::optional<live::PlayerRecord> PeerByXuid(uint64_t xuid) {
  {
    std::lock_guard lock(mutex);
    if (const auto found = known.find(xuid); found != known.end()) {
      return found->second;
    }
  }
  const auto record = live::FindPlayerByXuid(xuid);
  if (record) {
    std::lock_guard lock(mutex);
    known[xuid] = *record;
  }
  return record;
}
}  // namespace peers

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  // Signed in to Live: the address the game server sees this copy at, and the install's MAC
  // (after the Xenia netplay fork's XLiveAPI::IpGetConsoleXnAddr, BSD license).
  if (live::SignedIn()) {
    addr_ptr.Zero();
    addr_ptr->ina.s_addr = htonl(live::LocalAddress());
    addr_ptr->inaOnline.s_addr = htonl(live::OnlineAddress());
    addr_ptr->wPortOnline = 36000;
    const auto mac = live::Mac();
    std::memcpy(addr_ptr->abEnet, mac.data(), mac.size());
    addr_ptr->abOnline[16] = 1;  // SGADDR platform type: Xbox 360
    return XnAddrStatus::XNET_GET_XNADDR_ETHERNET | XnAddrStatus::XNET_GET_XNADDR_STATIC |
           XnAddrStatus::XNET_GET_XNADDR_GATEWAY | XnAddrStatus::XNET_GET_XNADDR_DNS |
           XnAddrStatus::XNET_GET_XNADDR_ONLINE;
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

  if (REXCVAR_GET(online_enabled)) {
    // [svr2011] connected to the online service
    return XnAddrStatus::XNET_GET_XNADDR_STATIC | XnAddrStatus::XNET_GET_XNADDR_GATEWAY |
           XnAddrStatus::XNET_GET_XNADDR_DNS | XnAddrStatus::XNET_GET_XNADDR_ONLINE;
  }
  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  NetTrace("NetDll_XNetGetDebugXnAddr");
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  NetTrace("NetDll_XNetXnAddrToMachineId");
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  if (live::Enabled()) {
    rex::string::copy_truncating(
        string_out,
        fmt::format("{}.{}.{}.{}", in_addr >> 24, (in_addr >> 16) & 0xFF, (in_addr >> 8) & 0xFF,
                    in_addr & 0xFF),
        string_size);
    return;
  }
  rex::string::copy_truncating(string_out, "666.666.666.666", string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  // Signed in to Live, another player is reached at the address in its XNADDR that suits
  // (live::PeerAddress), and this copy at its own (after the fork's XNetXnAddrToInAddr).
  if (!live::SignedIn() || !xn_addr || !in_addr) {
    return 1;
  }
  const auto mac = live::Mac();
  uint32_t address = live::PeerAddress(ntohl(xn_addr->ina.s_addr),
                                       ntohl(xn_addr->inaOnline.s_addr), live::OnlineAddress());
  if (std::memcmp(xn_addr->abEnet, mac.data(), mac.size()) == 0) {
    address = live::LocalAddress();
  } else if (const auto peer = peers::PeerByMac(xn_addr->abEnet)) {
    // Reached at its public address: through the router ports it mapped, if any.
    if (live::ParseIPv4(peer->host_address) == address) {
      live::upnp::Peers().Remember(address, peer->mapped_ports);
    }
    // Direct when the peer answers a direct probe, else its stand-in address, where the
    // relay transport listens for it (live::relay::AddressFor).
    if (live::relay::Configured()) {
      address = live::relay::AddressFor(peer->xuid, address);
    }
  }
  memory::store_and_swap<uint32_t>(in_addr.host_address(), address);
  return 0;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  // Signed in to Live: this copy's XNADDR, or another player's from the server
  // (players/find gives its MAC); the key is the one XNetRegisterKey was given.
  if (!live::SignedIn() || !xn_addr) {
    return 1;
  }
  xn_addr.Zero();
  if (in_addr == live::LocalAddress() || in_addr == live::OnlineAddress() ||
      in_addr == INADDR_LOOPBACK) {
    NetDll_XNetGetTitleXnAddr_entry(caller, xn_addr);
  } else {
    uint32_t local = in_addr, online = in_addr;
    const auto relayed_xuid = live::relay::PeerAt(in_addr);
    const auto player =
        relayed_xuid ? peers::PeerByXuid(*relayed_xuid) : live::FindPlayer(in_addr);
    if (player) {
      online = live::ParseIPv4(player->host_address).value_or(in_addr);
      local = live::ParseIPv4(player->local_address).value_or(online);
      std::memcpy(xn_addr->abEnet, player->mac.data(), player->mac.size());
      if (in_addr == online) {
        live::upnp::Peers().Remember(in_addr, player->mapped_ports);
      }
    }
    xn_addr->ina.s_addr = htonl(local);
    xn_addr->inaOnline.s_addr = htonl(online);
    xn_addr->wPortOnline = 36000;
    xn_addr->abOnline[16] = 1;  // SGADDR platform type: Xbox 360
  }
  if (xid) {
    std::lock_guard lock(peers::mutex);
    memory::store_and_swap<uint64_t>(xid.host_address(), peers::registered_key);
  }
  return 0;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  NetTrace("NetDll_XNetSetSystemLinkPort");
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  NetTrace("NetDll_XNetGetEthernetLinkStatus");
  if (REXCVAR_GET(online_enabled)) {
    // [svr2011] a cable plugged in
    return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE | XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
           XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
  }
  return 0;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  NetTrace("NetDll_XNetDnsLookup");
  // [svr2011] Online: the GameSpy names answer with kGameSpyAddr (connect()
  // sends it on to the port's relay); other names don't resolve (the game has
  // no other servers).
  const std::string name = host ? std::string(host.value()) : std::string();
  const bool gamespy = REXCVAR_GET(online_enabled) && IsGameSpyHost(name);
  REXKRNL_INFO("[svr2011] online: DNS lookup {} -> {}", name, gamespy ? "the relay" : "not found");
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    std::memset(dns, 0, sizeof(XNDNS));
    if (gamespy) {
      dns->status = 0;
      dns->cina = 1;
      dns->aina[0].s_addr = htonl(kGameSpyAddr);
    } else {
      dns->status = 1;  // non-zero = error
    }
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  NetTrace("NetDll_XNetDnsRelease");
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  NetTrace("NetDll_XNetQosServiceLookup");
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    if (REXCVAR_GET(online_enabled)) {
      // [svr2011] the online service answered (complete, contacted): a
      // good connection
      std::memset(&qos->info[0], 0, sizeof(XNQOSINFO));
      qos->count = 1;
      qos->info[0].flags = 0x01 | 0x02;  // XNET_XNQOSINFO_COMPLETE | TARGET_CONTACTED
      qos->info[0].probes_xmit = 4;
      qos->info[0].probes_recv = 4;
      qos->info[0].rtt_min_in_msecs = 20;
      qos->info[0].rtt_med_in_msecs = 25;
      qos->info[0].up_bits_per_sec = 10000000;
      qos->info[0].down_bits_per_sec = 50000000;
    }
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  NetTrace("NetDll_XNetQosRelease");
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  // [svr2011] the hosts' QoS data (XNetQosLookup) too
  for (uint32_t i = 0; i < std::min<uint32_t>(qos->count, 64); ++i) {
    if (qos->info[i].data_ptr) REX_KERNEL_MEMORY()->SystemHeapFree(qos->info[i].data_ptr);
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  NetTrace("NetDll_inet_addr");
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

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  NetTrace("NetDll_socket");
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();
    REXKRNL_WARN("[svr2011] net: socket({}, {}, {}) failed: {}", uint32_t(af), uint32_t(type), uint32_t(protocol),
                 HostSocketError());

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return GuestSocket(socket->handle());
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  overlapped::receives.Forget(socket_handle);
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  NetTrace("NetDll_shutdown");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    XThread::SetLastError(HostSocketError());
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  NetTrace("NetDll_setsockopt");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  if (XFAILED(status)) {
    XThread::SetLastError(HostSocketError());  // [svr2011]
    REXKRNL_WARN("[svr2011] net: setsockopt({:X}, {:X}) failed: {}", uint32_t(level), uint32_t(optname),
                 HostSocketError());
    return -1;
  }
  return 0;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  NetTrace("NetDll_ioctlsocket");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
#if REX_PLATFORM_WIN32
    REXKRNL_WARN("[svr2011] net: ioctlsocket({:X}) failed: {}", uint32_t(cmd), HostSocketError());
#else
    const int host_errno = errno;
    REXKRNL_WARN("[svr2011] net: ioctlsocket({:X}) failed: {} (errno {})", uint32_t(cmd), HostSocketError(),
                 host_errno);
#endif
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  NetTrace("NetDll_bind");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  // --wwe_online_ip: each copy on one PC keeps its peer ports on its own address.
  if (live::Enabled()) {
    const auto target = live::PlanBind(native_name.sin_addr, native_name.sin_port,
                                       live::BindAddress());
    native_name.sin_addr = target.address;
    native_name.sin_port = target.port;
  }
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
#if REX_PLATFORM_WIN32
    REXKRNL_WARN("[svr2011] net: bind port {} failed: {}", uint16_t(native_name.sin_port), HostSocketError());
#else
    const int host_errno = errno;
    REXKRNL_WARN("[svr2011] net: bind port {} failed: {} (errno {})", uint16_t(native_name.sin_port),
                 HostSocketError(), host_errno);
#endif
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  NetTrace("NetDll_connect");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  // sa_data: the port, then the IPv4 address, both big-endian.
  const auto* bytes = reinterpret_cast<const uint8_t*>(native_name.sa_data);
  BindToOnlineAddress(socket.get(), uint32_t(bytes[2]) << 24 | uint32_t(bytes[3]) << 16 |
                                        uint32_t(bytes[4]) << 8 | bytes[5]);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    const uint32_t error = HostSocketError();
    XThread::SetLastError(error);
    static uint32_t logged = 0;
    if (error != 10035 && logged++ < 16) {  // (WSAEWOULDBLOCK: a non-blocking connect under way)
#if REX_PLATFORM_WIN32
      REXKRNL_WARN("[svr2011] net: connect failed: {}", error);
#else
      REXKRNL_WARN("[svr2011] net: connect failed: {} (errno {})", error, errno);
#endif
    }
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  NetTrace("NetDll_listen");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  NetTrace("NetDll_accept");
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    ToGuest(reinterpret_cast<N_XSOCKADDR_IN*>(&native_addr));  // [svr2011]
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return GuestSocket(new_socket->handle());
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = LookupSocket(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = GuestSocket(socket->handle());
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  NetTrace("NetDll_select");
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  // [svr2011] POSIX select needs the highest descriptor + 1 (Windows
  // ignores nfds; the game passes 0)
  int native_nfds = nfds;
#if !REX_PLATFORM_WIN32
  native_nfds = 0;
  for (host_set* set : {&host_readfds, &host_writefds, &host_exceptfds}) {
    for (uint32_t i = 0; i < set->count; ++i) {
      native_nfds = std::max(native_nfds, int(set->sockets[i]->native_handle()) + 1);
    }
  }
#endif
  int ret = select(native_nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
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

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  NetTrace("NetDll_recv");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (ret == -1) {
    XThread::SetLastError(HostSocketError());  // [svr2011] (WSAEWOULDBLOCK ...)
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  NetTrace("NetDll_recvfrom");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (ret >= 0) ToGuest(&native_from);  // [svr2011]
  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = live::upnp::Peers().Game(native_from.sin_addr, native_from.sin_port);
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
// TODO: Better way of getting the error code
    XThread::SetLastError(HostSocketError());
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  NetTrace("NetDll_send");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  int ret = socket->Send(buf_ptr, buf_len, flags);
  if (ret == -1) {
    XThread::SetLastError(HostSocketError());  // [svr2011]
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  NetTrace("NetDll_sendto");
  auto socket = LookupSocket(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  BindToOnlineAddress(socket.get(), native_to.sin_addr);
  native_to.sin_port = live::upnp::Peers().External(native_to.sin_addr, native_to.sin_port);
  if (live::relay::DropsDirect(native_to.sin_addr, native_to.sin_port)) {
    return buf_len;  // as if sent
  }
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  NetTrace("NetDll___WSAFDIsSet");
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  NetTrace("NetDll_WSASetLastError");
  XThread::SetLastError(error_code);
}

// Signed in to Live, an LSP server (the game server's Rendez-Vous) is reached directly,
// without a security gateway: its secure address is its real address and connecting
// succeeds at once. Otherwise these fail as the stubs did (r3 = 1).
u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 ina, u32 service_id, mapped_u32 out) {
  if (!live::SignedIn()) {
    return 1;
  }
  if (out) {
    *out = ina;
  }
  return 0;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 ina) {
  return live::SignedIn() ? 0 : 1;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 ina) {
  constexpr uint32_t kConnected = 2;  // XNET_CONNECT_STATUS_CONNECTED
  return live::SignedIn() ? kConnected : 1;
}


// (caller, XNKID* id, XNKEY* key): a system-link style ID and the fork's constant key.
u32 NetDll_XNetCreateKey_entry(u32 caller, u32 xnkid_ptr, u32 xnkey_ptr) {
  if (!live::Enabled()) {
    return 1;
  }
  auto* memory = REX_KERNEL_MEMORY();
  std::random_device random;
  const uint64_t id = ((uint64_t(random()) << 32) | random()) & 0x00FFFFFFFFFFFFFFull;
  if (xnkid_ptr) {
    memory::store_and_swap<uint64_t>(memory->TranslateVirtual(xnkid_ptr), id);
  }
  if (xnkey_ptr) {
    uint8_t* key = memory->TranslateVirtual(xnkey_ptr);
    for (int i = 0; i < 16; ++i) {
      key[i] = uint8_t(i);
    }
  }
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, u32 xnkid_ptr, u32 xnkey_ptr) {
  if (!live::Enabled()) {
    return 1;
  }
  std::lock_guard lock(peers::mutex);
  peers::registered_key = peers::LoadKey(xnkid_ptr);
  REXKRNL_INFO("[online] XNetRegisterKey {:016X}", peers::registered_key);
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, u32 xnkid_ptr) {
  if (!live::Enabled()) {
    return 1;
  }
  std::lock_guard lock(peers::mutex);
  if (peers::LoadKey(xnkid_ptr) == peers::registered_key) {
    peers::registered_key = 0;
  }
  return 0;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 ina) {
  return live::Enabled() ? 0 : 1;
}

// XNQOS: count, count_pending, then one 24-byte XNQOSINFO per target.
namespace qos {
constexpr uint8_t kComplete = 0x01, kTargetContacted = 0x02, kDataReceived = 0x08;
constexpr uint32_t kListenSetData = 0x04;
constexpr uint32_t kHeader = 8, kInfo = 24;
}  // namespace qos

// (caller, XNKID* id, data, size, bits per second, flags): with LISTEN_SET_DATA the host's
// QoS data goes to the server, for searchers' XNetQosLookup.
u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  if (!live::SignedIn()) {
    return X_ERROR_FUNCTION_FAILED;
  }
  if ((flags & qos::kListenSetData) && data && data_size && id) {
    const uint64_t session_id = peers::LoadKey(id.guest_address());
    live::PostQosData(session_id, std::string(reinterpret_cast<const char*>(data.host_address()),
                                              data_size));
  }
  return 0;
}

// (caller, count, XNADDR** addresses, XNKID** ids, XNKEY** keys, gateways, gateway addresses,
// service IDs, probes, bits per second, flags, event, XNQOS** out): every target answers at
// once with the data its host listed and a 10 ms round trip.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 count, u32 addresses_ptr, u32 ids_ptr,
                               u32 keys_ptr, u32 num_gateways, u32 gateways_ptr,
                               u32 service_ids_ptr, u32 probes, u32 bits_per_second,
                               u32 flags, u32 event_handle, mapped_u32 qos_out) {
  if (!qos_out || !live::SignedIn()) {
    return 0x271D;  // WSAEACCES
  }
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t targets = count + num_gateways;
  std::vector<std::string> data(targets);
  for (uint32_t i = 0; i < count && ids_ptr; ++i) {
    const uint32_t id_ptr = memory::load_and_swap<uint32_t>(memory->TranslateVirtual(ids_ptr + i * 4));
    data[i] = live::GetQosData(peers::LoadKey(id_ptr));
  }
  uint32_t size = qos::kHeader + targets * qos::kInfo;
  for (const auto& d : data) {
    size += uint32_t((d.size() + 7) & ~size_t(7));
  }
  const uint32_t qos_ptr = memory->SystemHeapAlloc(size);
  uint8_t* q = memory->TranslateVirtual(qos_ptr);
  std::memset(q, 0, size);
  uint32_t tail = qos::kHeader + targets * qos::kInfo;
  for (uint32_t i = 0; i < targets; ++i) {
    uint8_t* info = q + qos::kHeader + i * qos::kInfo;
    uint8_t info_flags = qos::kComplete | qos::kTargetContacted;
    if (!data[i].empty()) {
      info_flags |= qos::kDataReceived;
      memory::store_and_swap<uint16_t>(info + 6, uint16_t(data[i].size()));
      memory::store_and_swap<uint32_t>(info + 8, qos_ptr + tail);
      std::memcpy(q + tail, data[i].data(), data[i].size());
      tail += uint32_t((data[i].size() + 7) & ~size_t(7));
    }
    info[0] = info_flags;
    memory::store_and_swap<uint16_t>(info + 2, uint16_t(probes));  // probes sent
    memory::store_and_swap<uint16_t>(info + 4, uint16_t(probes));  // probes received
    memory::store_and_swap<uint16_t>(info + 12, 10);               // rtt min (ms)
    memory::store_and_swap<uint16_t>(info + 14, 10);               // rtt median (ms)
    memory::store_and_swap<uint32_t>(info + 16, 5u << 20);         // up bits/s
    memory::store_and_swap<uint32_t>(info + 20, 5u << 20);         // down bits/s
  }
  memory::store_and_swap<uint32_t>(q, targets);
  memory::store_and_swap<uint32_t>(q + 4, 0);
  *qos_out = qos_ptr;
  if (event_handle) {
    xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  }
  return 0;
}

// (caller, socket, sockaddr_in*, int* length)
u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 name_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  if (!name) {
    XThread::SetLastError(0x271E);  // WSAEFAULT
    return -1;
  }
  sockaddr_in native{};
#if REX_PLATFORM_WIN32
  int native_len = sizeof(native);
  const int result = getsockname(static_cast<SOCKET>(socket->native_handle()),
                                 reinterpret_cast<sockaddr*>(&native), &native_len);
#else
  socklen_t native_len = sizeof(native);
  const int result = getsockname(static_cast<int>(socket->native_handle()),
                                 reinterpret_cast<sockaddr*>(&native), &native_len);
#endif
  if (result != 0) {
    XThread::SetLastError(0x2726);  // WSAEINVAL
    return -1;
  }
  name->sin_family = native.sin_family;
  name->sin_port = ntohs(native.sin_port);
  name->sin_addr = ntohl(native.sin_addr.s_addr);
  std::memset(name->x_sin_zero, 0, sizeof(name->x_sin_zero));
  if (name_len) {
    *name_len = 16;
  }
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_TRACE_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSAGetOverlappedResult,
                 rex::kernel::xam::NetDll_WSAGetOverlappedResult_entry)
REX_TRACE_EXPORT_SOCKADDR(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry, 9)
REX_TRACE_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents, rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetXnAddrToMachineId, rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetSetSystemLinkPort, rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus, rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_TRACE_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_TRACE_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_TRACE_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_TRACE_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_TRACE_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_TRACE_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_TRACE_EXPORT_SOCKADDR(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry, 5)
REX_TRACE_EXPORT_SOCKADDR(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry, 5)
REX_TRACE_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_TRACE_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_TRACE_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_TRACE_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_TRACE_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_TRACE_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_TRACE_EXPORT_SOCKADDR(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry, 8)
REX_TRACE_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_TRACE_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetServerToInAddr,
                 rex::kernel::xam::NetDll_XNetServerToInAddr_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetGetConnectStatus,
                 rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetUnregisterKey,
                 rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetUnregisterInAddr,
                 rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_TRACE_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_TRACE_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)

REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_TRACE_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_TRACE_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_TRACE_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_TRACE_EXPORT_STUB(__imp__NetDll_WSARecv);
REX_TRACE_EXPORT_STUB(__imp__NetDll_WSASend);
REX_TRACE_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_TRACE_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_TRACE_EXPORT_STUB(__imp__NetDll_getpeername);
REX_TRACE_EXPORT_STUB(__imp__NetDll_getsockopt);
