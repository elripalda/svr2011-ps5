/**
 * @file        relay.cpp
 * @brief       The peer transport through the game server's relay (see relay.h).
 */

#include <rex/kernel/xam/live/relay.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/kernel/xam/live/addressing.h>
#include <rex/kernel/xam/live/upnp.h>
#include <rex/logging.h>
#include <rex/thread.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#include <winsock2.h>  // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(wwe_online_force_relay, false, "Kernel",
                    "Online play (developer): send every peer's traffic through the game "
                    "server's relay");
REXCVAR_DEFINE_BOOL(wwe_online_drop_direct, false, "Kernel",
                    "Online play (developer): drop direct peer traffic, so peers fall back "
                    "to the relay");

namespace rex::kernel::xam::live::relay {

namespace {

#if REX_PLATFORM_WIN32
using Native = SOCKET;
constexpr Native kNoSocket = INVALID_SOCKET;
using AddressLength = int;
void CloseNative(Native s) {
  closesocket(s);
}
#else
using Native = int;
constexpr Native kNoSocket = -1;
using AddressLength = socklen_t;
void CloseNative(Native s) {
  close(s);
}
#endif

using Clock = std::chrono::steady_clock;
constexpr auto kHelloUntilWelcome = std::chrono::seconds(1);
constexpr auto kKeepAlive = std::chrono::seconds(10);
constexpr auto kPingInterval = std::chrono::seconds(2);
constexpr auto kReportInterval = std::chrono::seconds(10);
// A direct probe: this many tries this far apart, then the peer goes through the relay.
constexpr int kProbeTries = 6;
constexpr auto kProbeEvery = std::chrono::milliseconds(100);

sockaddr_in Endpoint(uint32_t address, uint16_t port) {
  sockaddr_in out{};
  out.sin_family = AF_INET;
  out.sin_addr.s_addr = htonl(address);
  out.sin_port = htons(port);
  return out;
}

std::string AddressText(uint32_t address) {
  return fmt::format("{}.{}.{}.{}", address >> 24, (address >> 16) & 0xFF,
                     (address >> 8) & 0xFF, address & 0xFF);
}

// A UDP socket bound to address:port, or kNoSocket.
Native OpenUdp(uint32_t address, uint16_t port) {
  Native s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) {
    return kNoSocket;
  }
#if REX_PLATFORM_WIN32
  // A datagram to a closed port must not fail the next receive (WSAECONNRESET).
  constexpr DWORD kSioUdpConnReset = 0x9800000C;
  BOOL report = FALSE;
  DWORD returned = 0;
  WSAIoctl(s, kSioUdpConnReset, &report, sizeof(report), nullptr, 0, &returned, nullptr,
           nullptr);
#endif
  const auto name = Endpoint(address, port);
  if (bind(s, reinterpret_cast<const sockaddr*>(&name), sizeof(name)) != 0) {
    CloseNative(s);
    return kNoSocket;
  }
  return s;
}

struct Pump {
  uint32_t stand_in = 0;
  uint16_t port = 0;
  Native socket = kNoSocket;
};

struct State {
  std::mutex mutex;
  bool configured = false;
  sockaddr_in relay{};
  uint32_t relay_address = 0;
  uint16_t relay_port = 0;
  uint64_t xuid = 0;
  Token token{};
  uint32_t copy_address = 0;  // --wwe_online_ip, 0 when unset
  uint32_t game_address = 0;  // where the game's peer sockets listen
  Native socket = kNoSocket;
  PeerTable table;
  RouteTable routes;
  bool relaying = false;  // some peer goes through the relay: ping it
  std::vector<Pump> pumps;
};

State& state() {
  static State s;
  return s;
}

// Listens on a stand-in address at every peer port; the caller holds the state lock.
bool OpenPumps(State& s, uint32_t stand_in) {
  std::vector<Pump> opened;
  for (const uint16_t port : kPeerPorts) {
    const Native socket = OpenUdp(stand_in, port);
    if (socket == kNoSocket) {
      for (const auto& pump : opened) {
        CloseNative(pump.socket);
      }
      return false;
    }
    opened.push_back({stand_in, port, socket});
  }
  s.pumps.insert(s.pumps.end(), opened.begin(), opened.end());
  return true;
}

// The caller holds the state lock.
std::optional<uint32_t> AddPeer(State& s, uint64_t xuid) {
  const bool known = s.table.StandInFor(xuid).has_value();
  const auto stand_in =
      s.table.Add(xuid, [&](uint32_t address) { return OpenPumps(s, address); });
  if (!stand_in) {
    REXKRNL_ERROR("[online] relay: no stand-in address could be bound for {:016X}", xuid);
  } else if (!known) {
    REXKRNL_INFO("[online] relay: peer {:016X} at stand-in {}", xuid, AddressText(*stand_in));
  }
  return stand_in;
}

void SendToRelay(const State& s, const std::vector<uint8_t>& packet) {
  sendto(s.socket, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
         reinterpret_cast<const sockaddr*>(&s.relay), sizeof(s.relay));
}

uint64_t Micros(Clock::time_point t) {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count());
}

// Pings answered since the last report, in milliseconds.
struct RoundTrips {
  std::vector<double> samples;
  Clock::time_point last_report = Clock::now();

  void Report(Clock::time_point now) {
    if (now - last_report < kReportInterval || samples.empty()) {
      return;
    }
    const auto [low, high] = std::minmax_element(samples.begin(), samples.end());
    double sum = 0;
    for (const double sample : samples) {
      sum += sample;
    }
    REXKRNL_INFO(
        "[online] relay round trip {:.1f} ms average, {:.1f} min, {:.1f} max ({} pings)",
        sum / samples.size(), *low, *high, samples.size());
    samples.clear();
    last_report = now;
  }
};

// A datagram from the relay.
void FromRelay(State& s, const uint8_t* data, size_t size, bool& welcomed,
               RoundTrips& round_trips) {
  const auto message = Parse(data, size);
  if (!message) {
    return;
  }
  switch (message->kind) {
    case Message::kWelcome:
      if (!welcomed) {
        REXKRNL_INFO("[online] relay at {}:{} welcomed {:016X}", AddressText(s.relay_address),
                     s.relay_port, message->value);
      }
      welcomed = true;
      break;
    case Message::kPong:
      round_trips.samples.push_back(double(Micros(Clock::now()) - message->value) / 1000.0);
      break;
    case Message::kDeliver: {
      // From the peer's stand-in address and port to the game's own port.
      std::lock_guard lock(s.mutex);
      if (s.routes.Get(message->value) == Route::kUndecided) {
        REXKRNL_INFO("[online] peer {:016X}: relay (it reached us through the relay)",
                     message->value);
      }
      s.routes.Relayed(message->value);
      s.relaying = true;
      const auto stand_in = AddPeer(s, message->value);
      if (!stand_in) {
        break;
      }
      const auto pump = std::find_if(s.pumps.begin(), s.pumps.end(), [&](const Pump& p) {
        return p.stand_in == *stand_in && p.port == message->src_port;
      });
      if (pump == s.pumps.end()) {
        break;
      }
      const auto to = Endpoint(s.game_address, message->dst_port);
      sendto(pump->socket, reinterpret_cast<const char*>(message->payload), int(message->size),
             0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
      break;
    }
  }
}

void Run() {
  auto& s = state();
  std::vector<uint8_t> buffer(2048);
  bool welcomed = false;
  RoundTrips round_trips;
  Clock::time_point last_hello{}, last_ping{};
  for (;;) {
    const auto now = Clock::now();
    if (now - last_hello >= (welcomed ? kKeepAlive : kHelloUntilWelcome)) {
      SendToRelay(s, Hello(s.xuid, s.token));
      last_hello = now;
    }
    bool relaying;
    {
      std::lock_guard lock(s.mutex);
      relaying = s.relaying;
    }
    if (welcomed && relaying && now - last_ping >= kPingInterval) {
      SendToRelay(s, Ping(Micros(now)));
      last_ping = now;
    }
    round_trips.Report(now);

    std::vector<Pump> pumps;
    {
      std::lock_guard lock(s.mutex);
      pumps = s.pumps;
    }
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s.socket, &readable);
    Native highest = s.socket;
    for (const auto& pump : pumps) {
      FD_SET(pump.socket, &readable);
      highest = std::max(highest, pump.socket);
    }
    timeval timeout{0, 50000};
    if (select(int(highest + 1), &readable, nullptr, nullptr, &timeout) <= 0) {
      continue;
    }
    if (FD_ISSET(s.socket, &readable)) {
      const int n =
          recv(s.socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
      if (n > 0) {
        FromRelay(s, buffer.data(), size_t(n), welcomed, round_trips);
      }
    }
    // The game sent to a stand-in address: wrap it for that peer.
    for (const auto& pump : pumps) {
      if (!FD_ISSET(pump.socket, &readable)) {
        continue;
      }
      sockaddr_in from{};
      AddressLength from_length = sizeof(from);
      const int n = recvfrom(pump.socket, reinterpret_cast<char*>(buffer.data()),
                             int(buffer.size()), 0, reinterpret_cast<sockaddr*>(&from),
                             &from_length);
      if (n < 0) {
        continue;
      }
      std::optional<uint64_t> peer;
      {
        std::lock_guard lock(s.mutex);
        peer = s.table.XuidAt(pump.stand_in);
      }
      if (peer) {
        SendToRelay(s, Send(*peer, ntohs(from.sin_port), pump.port, buffer.data(), size_t(n)));
      }
    }
  }
}

// Answers every probe on the socket, from the address it came from.
void AnswerProbes(Native responder) {
  uint8_t buffer[64];
  for (;;) {
    sockaddr_in from{};
    AddressLength from_length = sizeof(from);
    const int n = recvfrom(responder, reinterpret_cast<char*>(buffer), int(sizeof(buffer)), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_length);
    if (n < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    const auto nonce = ParseProbe(buffer, size_t(n));
    if (nonce && !REXCVAR_GET(wwe_online_drop_direct)) {
      const auto answer = ProbeAnswer(*nonce);
      sendto(responder, reinterpret_cast<const char*>(answer.data()), int(answer.size()), 0,
             reinterpret_cast<const sockaddr*>(&from), from_length);
    }
  }
}

// Whether the peer at `address` answers a direct probe within kProbeTries * kProbeEvery.
bool ProbeDirect(uint32_t copy_address, uint32_t address) {
  const Native socket = OpenUdp(ImplicitBindAddress(address, copy_address), 0);
  if (socket == kNoSocket) {
    return false;
  }
  const uint64_t nonce = Micros(Clock::now()) ^ (uint64_t(address) << 32);
  const auto probe = Probe(nonce);
  const auto to = Endpoint(address, upnp::Peers().External(address, kProbePort));
  uint8_t buffer[64];
  bool answered = false;
  for (int i = 0; i < kProbeTries && !answered; ++i) {
    sendto(socket, reinterpret_cast<const char*>(probe.data()), int(probe.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    const auto deadline = Clock::now() + kProbeEvery;
    for (auto now = Clock::now(); !answered && now < deadline; now = Clock::now()) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(socket, &readable);
      const auto wait =
          std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
      timeval timeout{};
      timeout.tv_sec = decltype(timeout.tv_sec)(wait / 1000000);
      timeout.tv_usec = decltype(timeout.tv_usec)(wait % 1000000);
      if (select(int(socket + 1), &readable, nullptr, nullptr, &timeout) <= 0) {
        continue;
      }
      const int n = recv(socket, reinterpret_cast<char*>(buffer), int(sizeof(buffer)), 0);
      answered = n > 0 && ParseProbeAnswer(buffer, size_t(n)) == nonce;
    }
  }
  CloseNative(socket);
  return answered;
}

}  // namespace

bool Forced() {
  return REXCVAR_GET(wwe_online_force_relay);
}

void Configure(uint32_t relay_address, uint16_t relay_port, uint64_t own_xuid,
               const Token& token, uint32_t copy_address) {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  if (s.configured) {
    return;
  }
  s.socket = OpenUdp(0, 0);
  if (s.socket == kNoSocket) {
    REXKRNL_ERROR("[online] relay: cannot open a socket");
    return;
  }
  s.relay = Endpoint(relay_address, relay_port);
  s.relay_address = relay_address;
  s.relay_port = relay_port;
  s.xuid = own_xuid;
  s.token = token;
  s.copy_address = copy_address;
  // A socket bound to every address is reached on loopback.
  s.game_address = copy_address ? copy_address : 0x7F000001;
  s.configured = true;
  std::thread(Run).detach();
}

void ListenForProbes(uint32_t copy_address) {
  static std::once_flag once;
  std::call_once(once, [copy_address] {
    const Native responder = OpenUdp(copy_address, kProbePort);
    if (responder == kNoSocket) {
      REXKRNL_ERROR("[online] cannot listen for direct probes on UDP {}", kProbePort);
      return;
    }
    std::thread(AnswerProbes, responder).detach();
  });
}

bool Configured() {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  return s.configured;
}

uint32_t AddressFor(uint64_t peer_xuid, uint32_t direct) {
  auto& s = state();
  Route route;
  uint32_t copy_address;
  {
    std::lock_guard lock(s.mutex);
    if (!s.configured) {
      return direct;
    }
    if (Forced() && s.routes.Get(peer_xuid) == Route::kUndecided) {
      REXKRNL_INFO("[online] peer {:016X}: relay (forced)", peer_xuid);
      s.routes.Relayed(peer_xuid);
    }
    route = s.routes.Get(peer_xuid);
    copy_address = s.copy_address;
  }
  if (route == Route::kUndecided) {
    const auto start = Clock::now();
    const bool answered = ProbeDirect(copy_address, direct);
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    std::lock_guard lock(s.mutex);
    route = s.routes.Settle(peer_xuid, answered);
    if (route == Route::kDirect) {
      REXKRNL_INFO("[online] peer {:016X}: direct at {} (probe answered in {} ms)", peer_xuid,
                   AddressText(direct), waited);
    } else {
      REXKRNL_INFO("[online] peer {:016X}: relay ({} did not answer a direct probe in {} ms)",
                   peer_xuid, AddressText(direct), waited);
    }
  }
  if (route == Route::kDirect) {
    return direct;
  }
  std::lock_guard lock(s.mutex);
  s.relaying = true;
  return AddPeer(s, peer_xuid).value_or(direct);
}

bool DropsDirect(uint32_t address, uint16_t port) {
  return REXCVAR_GET(wwe_online_drop_direct) && IsPeerPort(port) && !PeerAt(address);
}

std::optional<uint64_t> PeerAt(uint32_t address) {
  if (!IsLoopback(address)) {
    return std::nullopt;
  }
  auto& s = state();
  std::lock_guard lock(s.mutex);
  return s.table.XuidAt(address);
}

}  // namespace rex::kernel::xam::live::relay
