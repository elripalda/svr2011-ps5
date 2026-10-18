/**
 * @file        port_map.h
 * @brief       UPnP port mappings (ticket 18): a player's game ports and the router's
 *              external ports they are reached at.
 *
 * A player with --wwe_online_upnp registers its mappings; a peer reaching it at its
 * public address sends to the external ports, and datagrams from those ports reach the
 * game from the game's own ports.
 */

#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace rex::kernel::xam::live {

// Game port -> the router's external port.
using PortMap = std::map<uint16_t, uint16_t>;

inline uint16_t ExternalPort(const PortMap& ports, uint16_t game_port) {
  const auto found = ports.find(game_port);
  return found == ports.end() ? game_port : found->second;
}

inline uint16_t GamePort(const PortMap& ports, uint16_t external_port) {
  for (const auto& [game, external] : ports) {
    if (external == external_port) {
      return game;
    }
  }
  return external_port;
}

// The external port to ask the router for: the game port itself, then a thousand higher
// for each attempt the router refused (another device holds the port).
constexpr uint16_t CandidateExternalPort(uint16_t game_port, int attempt) {
  return uint16_t(game_port + 1000 * attempt);
}

// The peers reached at their public address, with their mappings.
class PeerPorts {
 public:
  void Remember(uint32_t address, const PortMap& ports) {
    std::lock_guard lock(mutex_);
    if (ports.empty()) {
      by_address_.erase(address);
    } else {
      by_address_[address] = ports;
    }
  }
  uint16_t External(uint32_t address, uint16_t game_port) const {
    std::lock_guard lock(mutex_);
    const auto found = by_address_.find(address);
    return found == by_address_.end() ? game_port : ExternalPort(found->second, game_port);
  }
  uint16_t Game(uint32_t address, uint16_t external_port) const {
    std::lock_guard lock(mutex_);
    const auto found = by_address_.find(address);
    return found == by_address_.end() ? external_port : GamePort(found->second, external_port);
  }

 private:
  mutable std::mutex mutex_;
  std::map<uint32_t, PortMap> by_address_;
};

}  // namespace rex::kernel::xam::live
