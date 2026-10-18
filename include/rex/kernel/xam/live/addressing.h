/**
 * @file        addressing.h
 * @brief       Which address a copy binds and which address it reaches a peer at.
 *
 * Like an Xbox's XNADDR, a player has a local address (ina, on its own network) and a
 * public one (inaOnline, what the game server sees). Peers behind one router share the
 * public address, so they reach each other locally. Addresses are IPv4 in host order.
 */

#pragma once

#include <cstdint>

namespace rex::kernel::xam::live {

// The game's peer ports: VDP 1000 (Net-Z) and UDP 9100.
constexpr uint16_t kPeerPorts[] = {1000, 9100};

constexpr bool IsPeerPort(uint16_t port) {
  for (const uint16_t peer_port : kPeerPorts) {
    if (port == peer_port) {
      return true;
    }
  }
  return false;
}

constexpr bool IsLoopback(uint32_t address) {
  return (address >> 24) == 127;
}

// The address to send to a peer with these XNADDR addresses, from a copy whose public
// address is `own_public`.
constexpr uint32_t PeerAddress(uint32_t peer_local, uint32_t peer_public, uint32_t own_public) {
  if (peer_local && (!peer_public || peer_public == own_public)) {
    return peer_local;
  }
  return peer_public;
}

struct BindTarget {
  uint32_t address = 0;
  uint16_t port = 0;
};

// A guest bind of `address:port` on a copy with --wwe_online_ip `copy_address` (0 when
// unset). Each copy on one PC keeps the peer ports on its own address. Its other sockets
// (the Rendez-Vous client's fixed 1001) take any port on the default route: that port
// would collide between copies, and a loopback address cannot reach the internet.
constexpr BindTarget PlanBind(uint32_t address, uint16_t port, uint32_t copy_address) {
  if (address != 0 || copy_address == 0) {
    return {address, port};
  }
  if (IsPeerPort(port)) {
    return {copy_address, port};
  }
  return {0, 0};
}

// The address an unbound socket sending to `destination` binds to first (0: none), so a
// peer on the same PC sees the copy's own address.
constexpr uint32_t ImplicitBindAddress(uint32_t destination, uint32_t copy_address) {
  return IsLoopback(destination) ? copy_address : 0;
}

}  // namespace rex::kernel::xam::live
