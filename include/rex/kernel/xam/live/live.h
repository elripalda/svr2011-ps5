/**
 * @file        live.h
 * @brief       --wwe_online: Xbox LIVE sign-in against the game server.
 *
 * WWE All Stars recomp, online play (.scratch/online-play/, ADR-0021). With --wwe_online
 * the profile signs in to Live, registers with the game server's XWS API (whoami,
 * players) and lists the server's Rendez-Vous address as the title's LSP. The client
 * follows the Xenia netplay fork (AdrianCassar/xenia-canary, 6dbaa1f, BSD license).
 * Off by default; off, nothing here runs.
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/xam/live/xlive_json.h>

REXCVAR_DECLARE(bool, wwe_online);
REXCVAR_DECLARE(std::string, wwe_online_server);
REXCVAR_DECLARE(std::string, wwe_online_ip);
REXCVAR_DECLARE(std::string, wwe_online_name);

namespace rex::kernel::xam::live {

// --wwe_online.
bool Enabled();

// Enabled, and the game server answered: the profile is signed in to Live. The first call
// loads the identity and registers with the server.
bool SignedIn();

// The install's online XUID and MAC (online-identity.json).
uint64_t OnlineXuid();
std::array<uint8_t, 6> Mac();

// --wwe_online_name, or the profile's gamertag.
std::string Gamertag();

// --wwe_online_ip in host order: the copy's peer sockets bind to it (PlanBind). 0 when
// unset.
uint32_t BindAddress();

// The address the server sees this copy at (whoami, XNADDR inaOnline), host order; 0
// before sign-in.
uint32_t OnlineAddress();

// This copy's address on its own network (XNADDR ina): --wwe_online_ip, else the address
// its requests to the server leave from. 0 before sign-in.
uint32_t LocalAddress();

// The game server's LSP list (title/{id}/servers).
std::vector<TitleServer> TitleServers();

// The player the server has at an address, public or local (players/find).
std::optional<PlayerRecord> FindPlayer(uint32_t address);
// The player with an XNADDR's MAC (abEnet), or with a XUID.
std::optional<PlayerRecord> FindPlayerByMac(const std::array<uint8_t, 6>& mac);
std::optional<PlayerRecord> FindPlayerByXuid(uint64_t xuid);

// A hosted session's QoS data (XNetQosListen), for searchers' XNetQosLookup.
void PostQosData(uint64_t session_id, const std::string& data);
std::string GetQosData(uint64_t session_id);

}  // namespace rex::kernel::xam::live
