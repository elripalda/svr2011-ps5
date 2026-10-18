/**
 * @file        relay.h
 * @brief       The peer transport through the game server's relay (ticket 16).
 *
 * A relayed peer is reached at a stand-in loopback address (relay_protocol.h). The
 * transport listens there on the game's peer ports (VDP 1000, UDP 9100): what the game
 * sends to a stand-in goes to the relay wrapped for that peer, and what the relay delivers
 * from a peer reaches the game from the peer's stand-in address and port. The game sees no
 * difference.
 *
 * Without --wwe_online_force_relay a peer is probed directly first (ticket 17): it is
 * reached directly when it answers, through the relay otherwise.
 */

#pragma once

#include <cstdint>
#include <optional>

#include <rex/cvar.h>
#include <rex/kernel/xam/live/relay_protocol.h>

REXCVAR_DECLARE(bool, wwe_online_force_relay);
REXCVAR_DECLARE(bool, wwe_online_drop_direct);

namespace rex::kernel::xam::live::relay {

// --wwe_online_force_relay: every peer goes through the relay.
bool Forced();

// Answers direct probes on the probe port at --wwe_online_ip (0: every address), from
// before registration on: the server checks a UPnP player's mapped probe port then.
void ListenForProbes(uint32_t copy_address);

// After sign-in: the relay's address and port, this player's XUID and token, and
// --wwe_online_ip (0 when unset), where the game's peer sockets listen. Starts the
// transport.
void Configure(uint32_t relay_address, uint16_t relay_port, uint64_t own_xuid,
               const Token& token, uint32_t copy_address);
bool Configured();

// The address the game reaches a peer at: `direct` once the peer answered a direct probe,
// else its stand-in address (listening once this returns). The first call for a peer
// probes it, waiting up to about 0.6 s; the choice is logged and kept for the session.
uint32_t AddressFor(uint64_t peer_xuid, uint32_t direct);

// --wwe_online_drop_direct (developer): a datagram to a peer port that is not a stand-in
// address is dropped, and probes go unanswered, as if the peers could not reach each other.
bool DropsDirect(uint32_t address, uint16_t port);

// The peer behind a stand-in address.
std::optional<uint64_t> PeerAt(uint32_t address);

}  // namespace rex::kernel::xam::live::relay
