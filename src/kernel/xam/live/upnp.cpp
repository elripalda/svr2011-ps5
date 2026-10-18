/**
 * @file        upnp.cpp
 * @brief       --wwe_online_upnp: the game's peer ports mapped on the router (see upnp.h).
 *
 * After the Xenia netplay fork's UPnP::DiscoverValidIGD, AddPort and RemovePort
 * (src/xenia/kernel/upnp.cc, Copyright 2026 Xenia Canary, BSD license).
 */

#include <rex/kernel/xam/live/upnp.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <miniupnpc.h>
#include <upnpcommands.h>

#include <rex/kernel/xam/live/addressing.h>
#include <rex/kernel/xam/live/relay_protocol.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(wwe_online_upnp, false, "Kernel",
                    "Online play: map the game's peer ports on the router with UPnP, so "
                    "players elsewhere can reach this PC directly");

namespace rex::kernel::xam::live::upnp {

namespace {

constexpr uint16_t kGamePorts[] = {kPeerPorts[0], kPeerPorts[1], relay::kProbePort};
// External ports tried per game port (CandidateExternalPort) when the router refuses one.
constexpr int kAttempts = 4;
constexpr const char* kLeaseSeconds = "3600";
constexpr auto kRefreshEvery = std::chrono::minutes(45);
constexpr const char* kDescription = "WWE All Stars recomp";
// UPnP IGD error codes.
constexpr int kConflictInMappingEntry = 718;
constexpr int kOnlyPermanentLeasesSupported = 725;

struct Router {
  std::mutex mutex;
  bool found = false;
  bool permanent = false;  // the router takes only leases without an end
  UPNPUrls urls{};
  IGDdatas data{};
  char lan_address[64] = {};
  PortMap mapped;
};

Router& router() {
  static Router r;
  return r;
}

// The caller holds the router's lock.
int Add(Router& r, uint16_t game_port, uint16_t external_port) {
  const auto external = std::to_string(external_port);
  const auto internal = std::to_string(game_port);
  int result = UPNP_AddPortMapping(r.urls.controlURL, r.data.first.servicetype,
                                   external.c_str(), internal.c_str(), r.lan_address,
                                   kDescription, "UDP", nullptr,
                                   r.permanent ? "0" : kLeaseSeconds);
  if (result == kOnlyPermanentLeasesSupported) {
    r.permanent = true;
    result = UPNP_AddPortMapping(r.urls.controlURL, r.data.first.servicetype, external.c_str(),
                                 internal.c_str(), r.lan_address, kDescription, "UDP", nullptr,
                                 "0");
  }
  return result;
}

// Re-adding a mapping renews its lease.
void RefreshLeases() {
  for (;;) {
    std::this_thread::sleep_for(kRefreshEvery);
    auto& r = router();
    std::lock_guard lock(r.mutex);
    if (r.permanent) {
      return;
    }
    for (const auto& [game_port, external_port] : r.mapped) {
      if (const int result = Add(r, game_port, external_port); result != UPNPCOMMAND_SUCCESS) {
        REXKRNL_WARN("[online] UPnP: renewing UDP {} failed ({})", external_port, result);
      }
    }
  }
}

// The caller holds the router's lock.
void RemoveLocked(Router& r) {
  if (!r.found) {
    return;
  }
  for (const auto& [game_port, external_port] : r.mapped) {
    const auto external = std::to_string(external_port);
    const int result = UPNP_DeletePortMapping(r.urls.controlURL, r.data.first.servicetype,
                                              external.c_str(), "UDP", nullptr);
    if (result == UPNPCOMMAND_SUCCESS) {
      REXKRNL_INFO("[online] UPnP: removed UDP {}", external_port);
    } else {
      REXKRNL_WARN("[online] UPnP: removing UDP {} failed ({})", external_port, result);
    }
  }
  r.mapped.clear();
  FreeUPNPUrls(&r.urls);
  r.found = false;
}

void Map() {
  auto& r = router();
  std::lock_guard lock(r.mutex);
  int error = 0;
  UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
  if (!devices) {
    REXKRNL_WARN("[online] UPnP: no router answered (error {})", error);
    return;
  }
  char wan_address[64] = {};
  const int status = UPNP_GetValidIGD(devices, &r.urls, &r.data, r.lan_address,
                                      sizeof(r.lan_address), wan_address, sizeof(wan_address));
  freeUPNPDevlist(devices);
  if (status != UPNP_CONNECTED_IGD && status != UPNP_PRIVATEIP_IGD) {
    REXKRNL_WARN("[online] UPnP: no connected internet gateway (status {})", status);
    if (status != UPNP_NO_IGD) {
      FreeUPNPUrls(&r.urls);
    }
    return;
  }
  r.found = true;
  REXKRNL_INFO("[online] UPnP: router at {} (this PC {}, public address {})",
               r.urls.controlURL ? r.urls.controlURL : "?", r.lan_address, wan_address);
  for (const uint16_t game_port : kGamePorts) {
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
      const uint16_t external_port = CandidateExternalPort(game_port, attempt);
      const int result = Add(r, game_port, external_port);
      if (result == UPNPCOMMAND_SUCCESS) {
        r.mapped[game_port] = external_port;
        REXKRNL_INFO("[online] UPnP: UDP {} -> {}:{}{}", external_port, r.lan_address,
                     game_port, r.permanent ? " (permanent lease)" : "");
        break;
      }
      if (result != kConflictInMappingEntry) {
        REXKRNL_WARN("[online] UPnP: mapping UDP {} failed ({})", external_port, result);
        break;
      }
    }
  }
  if (r.mapped.size() != std::size(kGamePorts)) {
    // A reachable probe port would vouch for game ports the router refused.
    REXKRNL_WARN("[online] UPnP: not every port could be mapped, so none is used");
    RemoveLocked(r);
    return;
  }
  std::atexit(RemoveMappings);
  std::thread(RefreshLeases).detach();
}

}  // namespace

bool Enabled() {
  return REXCVAR_GET(wwe_online_upnp);
}

PortMap MapPorts() {
  if (!Enabled()) {
    return {};
  }
  static std::once_flag once;
  std::call_once(once, Map);
  auto& r = router();
  std::lock_guard lock(r.mutex);
  return r.mapped;
}

void RemoveMappings() {
  auto& r = router();
  std::lock_guard lock(r.mutex);
  RemoveLocked(r);
}

PeerPorts& Peers() {
  static PeerPorts peers;
  return peers;
}

}  // namespace rex::kernel::xam::live::upnp
