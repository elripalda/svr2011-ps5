/**
 * @file        xlive_json.h
 * @brief       The JSON the online client exchanges with the game server's XWS API.
 *
 * Shapes follow the Xenia netplay fork (AdrianCassar/xenia-canary,
 * netplay_canary_experimental 6dbaa1f, src/xenia/kernel/json/), BSD-licensed; the server
 * side is server/internal/xws.
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/kernel/xam/live/port_map.h>

namespace rex::kernel::xam::live {

// --wwe_online_server as the API's base URL: "host" and "host:port" get http:// (port
// 36000 by default), a URL is kept; always one trailing slash.
std::string ApiBaseUrl(std::string_view server);

// GET whoami: the address the server sees this copy at.
std::optional<std::string> ParseWhoami(std::string_view body);

struct PlayerRecord {
  uint64_t xuid = 0;
  uint64_t machine_id = 0;
  std::string host_address;   // public: the address whoami gave
  std::string local_address;  // on its own network (XNADDR ina)
  std::string gamertag;
  std::array<uint8_t, 6> mac = {};
  std::string build_id;  // see BuildId(); sent, never parsed
  PortMap mapped_ports;  // --wwe_online_upnp: game port -> the router's external port
};

// The game the port is built on (WWE All Stars Recomp 1.0.0: Title Update 2), sent at sign-in;
// a game server for another version refuses the player (tu2-dlc ticket 06).
inline constexpr const char* kGameVersion = "tu2";

// The POST players reply: the relay's "address:port" and this player's token for it, and,
// for a player with mapped ports, whether the server reached the mapped probe port.
struct Registration {
  std::string relay_address;
  std::string relay_token;
  std::optional<bool> reachable;
};
std::optional<Registration> ParseRegistration(std::string_view body);

// The message of a refused sign-in (HTTP 426: the server runs another game version).
std::optional<std::string> ParseRefusal(std::string_view body);

// POST players body, and the players/find reply.
std::string PlayerJson(const PlayerRecord& player);
std::optional<PlayerRecord> ParsePlayer(std::string_view body);

struct TitleServer {
  uint32_t address = 0;  // host order
  uint32_t flags = 0;
  std::string description;
};

// GET title/{id}/servers: the LSP servers XTitleServerCreateEnumerator lists.
std::vector<TitleServer> ParseTitleServers(std::string_view body);

// "a.b.c.d" in host order, or nullopt.
std::optional<uint32_t> ParseIPv4(std::string_view text);

}  // namespace rex::kernel::xam::live
