/**
 * @file        upnp.h
 * @brief       --wwe_online_upnp: the game's peer ports mapped on the router (ticket 18).
 *
 * After the Xenia netplay fork's UPnP class (src/xenia/kernel/upnp.cc, Copyright 2026
 * Xenia Canary, BSD license), with miniupnpc at the fork's pinned commit. Off by default:
 * without the switch no UPnP traffic is sent.
 */

#pragma once

#include <rex/cvar.h>
#include <rex/kernel/xam/live/port_map.h>

REXCVAR_DECLARE(bool, wwe_online_upnp);

namespace rex::kernel::xam::live::upnp {

bool Enabled();

// The first call (with the switch on) finds the router and maps UDP 1000, 9100 and the
// probe port to this PC, keeping the leases fresh. Game port -> external port; empty when
// the switch is off or no router answered.
PortMap MapPorts();

// Removes the mappings; the window's close runs it before the process ends.
void RemoveMappings();

// Peers reached at their public address and the router ports they mapped, for the
// datagrams to and from them.
PeerPorts& Peers();

}  // namespace rex::kernel::xam::live::upnp
