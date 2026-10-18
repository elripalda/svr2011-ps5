/**
 * @file        live.cpp
 * @brief       --wwe_online: Xbox LIVE sign-in against the game server (see live.h).
 *
 * After the Xenia netplay fork's XLiveAPI::Init, Getwhoami, RegisterPlayer, FindPlayer and
 * GetServers (src/xenia/kernel/XLiveAPI.cpp, Copyright 2026 Xenia Canary, BSD license).
 */

#include <rex/kernel/xam/live/live.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <random>

#include <fmt/format.h>

#include <rex/kernel/xam/live/http.h>
#include <rex/kernel/xam/live/identity.h>
#include <rex/kernel/xam/live/relay.h>
#include <rex/kernel/xam/live/upnp.h>
#include <rex/kernel/xam/live/xsession.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REXCVAR_DEFINE_BOOL(wwe_online, false, "Kernel",
                    "Online play (experimental): sign in to Xbox LIVE against the game server");
REXCVAR_DEFINE_STRING(wwe_online_server, "https://allstars.komplexaci.cz", "Kernel",
                      "Online play: the game server (host, host:port or URL; HTTP port 36000 "
                      "for a bare host). The owner's server by default");
REXCVAR_DEFINE_STRING(wwe_online_ip, "", "Kernel",
                      "Online play: bind every socket to this address, e.g. 127.0.0.2 for a "
                      "second copy on one PC");
REXCVAR_DEFINE_STRING(wwe_online_name, "", "Kernel",
                      "Online play: the name other players see (the profile's gamertag when "
                      "empty)");

namespace rex::kernel::xam::live {

namespace {

// A failed sign-in (server down) is retried this often, not on every call.
constexpr auto kRetryInterval = std::chrono::seconds(10);

struct State {
  std::mutex mutex;
  bool identity_loaded = false;
  OnlineIdentity identity;
  bool signed_in = false;
  std::chrono::steady_clock::time_point last_attempt{};
  uint32_t online_address = 0;
  uint32_t local_address = 0;
  bool servers_cached = false;
  std::vector<TitleServer> servers;
};

State& state() {
  static State s;
  return s;
}

std::string Url(const std::string& endpoint) {
  return ApiBaseUrl(REXCVAR_GET(wwe_online_server)) + endpoint;
}

std::string AddressText(uint32_t address) {
  return fmt::format("{}.{}.{}.{}", address >> 24, (address >> 16) & 0xFF,
                     (address >> 8) & 0xFF, address & 0xFF);
}

void LoadIdentity(State& s) {
  if (s.identity_loaded) {
    return;
  }
  std::random_device device;
  std::mt19937_64 random((uint64_t(device()) << 32) ^ device() ^
                         uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto folder = REX_KERNEL_STATE()->emulator()->user_data_root();
  s.identity = LoadOrCreateIdentity(folder, random, MachineKey());
  s.identity_loaded = true;
  REXKRNL_INFO("[online] identity XUID {:016X}, MAC {} ({})", s.identity.xuid,
               MacText(s.identity.mac), (folder / kIdentityFileName).string());
}

// The registration's relay, for peers that cannot be reached directly.
void ConfigureRelay(const std::string& registration_body, uint64_t xuid) {
  const auto registration = ParseRegistration(registration_body);
  const auto& address = registration ? registration->relay_address : std::string();
  const auto colon = address.rfind(':');
  const auto host = colon == std::string::npos ? std::nullopt
                                               : ParseIPv4(address.substr(0, colon));
  const auto token = registration ? relay::ParseToken(registration->relay_token) : std::nullopt;
  int port = 0;
  if (host) {
    port = std::atoi(address.c_str() + colon + 1);
  }
  if (!host || !token || port <= 0 || port > 0xFFFF) {
    REXKRNL_ERROR("[online] the game server offers no relay: every peer is reached directly");
    return;
  }
  relay::Configure(*host, uint16_t(port), xuid, *token, BindAddress());
}

// whoami, then players: the server learns this copy's XUID, name, address and MAC. Runs
// without the state lock; `identity` is loaded and only read meanwhile.
bool SignIn(State& s, const OnlineIdentity& identity) {
  const auto whoami = HttpRequest("GET", Url("whoami"));
  const auto address_text = whoami.status == 200 ? ParseWhoami(whoami.body) : std::nullopt;
  const auto address = address_text ? ParseIPv4(*address_text) : std::nullopt;
  if (!address) {
    REXKRNL_ERROR("[online] the game server at {} did not answer whoami (HTTP {})",
                  Url(""), whoami.status);
    return false;
  }
  const uint32_t local = BindAddress() ? BindAddress()
                                       : ParseIPv4(whoami.local_ip).value_or(*address);
  {
    std::lock_guard lock(s.mutex);
    s.online_address = *address;
    s.local_address = local;
  }

  PlayerRecord player;
  player.xuid = identity.xuid;
  player.gamertag = Gamertag();
  player.host_address = *address_text;
  player.local_address = AddressText(local);
  player.mac = identity.mac;
  player.machine_id = 0xFA00000000000000ull | (identity.xuid & 0x0000FFFFFFFFFFFFull);
  player.build_id = BuildId();
  relay::ListenForProbes(BindAddress());
  player.mapped_ports = upnp::MapPorts();
  const auto registered = HttpRequest("POST", Url("players"), PlayerJson(player));
  if (registered.status == 426) {
    // The server runs another game version: the player has to update the port.
    REXKRNL_ERROR("[online] the game server refused this version of the port: {}",
                  ParseRefusal(registered.body).value_or("update to play online"));
    return false;
  }
  if (registered.status != 201) {
    REXKRNL_ERROR("[online] registering with the game server failed (HTTP {})",
                  registered.status);
    return false;
  }
  REXKRNL_INFO("[online] signed in to {} as {:016X} \"{}\" at {}, local {}", Url(""),
               player.xuid, player.gamertag, *address_text, player.local_address);
  if (const auto it = player.mapped_ports.find(relay::kProbePort);
      it != player.mapped_ports.end()) {
    const auto registration = ParseRegistration(registered.body);
    if (registration && registration->reachable == true) {
      REXKRNL_INFO("[online] UPnP: the game server reached this PC at {}:{}", *address_text,
                   it->second);
    } else {
      REXKRNL_WARN("[online] UPnP: the game server could not reach this PC at {}:{}",
                   *address_text, it->second);
    }
  }
  ConfigureRelay(registered.body, player.xuid);
  return true;
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(wwe_online);
}

bool SignedIn() {
  if (!Enabled()) {
    return false;
  }
  auto& s = state();
  // One attempt at a time; callers wait for it, as the game expects an answer. The state
  // lock stays free meanwhile, so the XUID, MAC and address calls never wait on HTTP.
  static std::mutex attempt;
  std::lock_guard attempt_lock(attempt);
  OnlineIdentity identity;
  {
    std::lock_guard lock(s.mutex);
    if (s.signed_in) {
      return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (s.last_attempt != std::chrono::steady_clock::time_point{} &&
        now - s.last_attempt < kRetryInterval) {
      return false;
    }
    s.last_attempt = now;
    LoadIdentity(s);
    identity = s.identity;
  }
  const bool ok = SignIn(s, identity);
  std::lock_guard lock(s.mutex);
  s.signed_in = ok;
  return ok;
}

uint64_t OnlineXuid() {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  LoadIdentity(s);
  return s.identity.xuid;
}

std::array<uint8_t, 6> Mac() {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  LoadIdentity(s);
  return s.identity.mac;
}

std::string Gamertag() {
  const std::string& name = REXCVAR_GET(wwe_online_name);
  if (!name.empty()) {
    return name.substr(0, 15);
  }
  return REX_KERNEL_STATE()->user_profile()->name();
}

uint32_t BindAddress() {
  return ParseIPv4(REXCVAR_GET(wwe_online_ip)).value_or(0);
}

uint32_t OnlineAddress() {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  return s.online_address;
}

uint32_t LocalAddress() {
  auto& s = state();
  std::lock_guard lock(s.mutex);
  return s.local_address;
}

std::vector<TitleServer> TitleServers() {
  if (!SignedIn()) {
    return {};
  }
  auto& s = state();
  std::lock_guard lock(s.mutex);
  if (!s.servers_cached) {
    const auto response = HttpRequest(
        "GET", Url(fmt::format("title/{:08X}/servers", REX_KERNEL_STATE()->title_id())));
    if (response.status == 200) {
      s.servers = ParseTitleServers(response.body);
      s.servers_cached = true;
      for (const auto& server : s.servers) {
        REXKRNL_INFO("[online] LSP server {} \"{}\"", AddressText(server.address),
                     server.description);
      }
    } else {
      REXKRNL_ERROR("[online] title server list failed (HTTP {})", response.status);
    }
  }
  return s.servers;
}

void PostQosData(uint64_t session_id, const std::string& data) {
  const auto response = HttpRequest(
      "POST",
      Url(fmt::format("title/{:08X}/sessions/{:016x}/qos", REX_KERNEL_STATE()->title_id(),
                      session_id)),
      data);
  if (response.status != 201) {
    REXKRNL_ERROR("[online] QoS data for {:016X} failed (HTTP {})", session_id, response.status);
  }
}

std::string GetQosData(uint64_t session_id) {
  const auto response = HttpRequest(
      "GET", Url(fmt::format("title/{:08X}/sessions/{:016x}/qos", REX_KERNEL_STATE()->title_id(),
                             session_id)));
  return response.status == 200 ? response.body : std::string();
}

namespace {
std::optional<PlayerRecord> FindPlayerBy(const std::string& query) {
  if (!SignedIn()) {
    return std::nullopt;
  }
  const auto response = HttpRequest("POST", Url("players/find"), query);
  if (response.status != 201) {
    return std::nullopt;
  }
  return ParsePlayer(response.body);
}
}  // namespace

std::optional<PlayerRecord> FindPlayer(uint32_t address) {
  return FindPlayerBy(fmt::format(R"({{"hostAddress":"{}"}})", AddressText(address)));
}

std::optional<PlayerRecord> FindPlayerByMac(const std::array<uint8_t, 6>& mac) {
  return FindPlayerBy(fmt::format(R"({{"macAddress":"{}"}})", MacText(mac)));
}

std::optional<PlayerRecord> FindPlayerByXuid(uint64_t xuid) {
  return FindPlayerBy(fmt::format(R"({{"xuid":"{:016X}"}})", xuid));
}

}  // namespace rex::kernel::xam::live
