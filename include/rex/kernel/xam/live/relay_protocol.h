/**
 * @file        relay_protocol.h
 * @brief       The game server's relay: its wire format, the stand-in addresses of
 *              relayed peers, and each peer's route (direct or relayed).
 *
 * The wire format is server/internal/relay's (see there): "ASR1", a type byte, then
 * big-endian fields. A relayed peer is reached at a stand-in loopback address, as a real
 * Xbox hands out a secure address per peer; the transport (relay.h) listens there.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace rex::kernel::xam::live::relay {

using Token = std::array<uint8_t, 16>;

// The registration's relayToken (32 hex digits).
std::optional<Token> ParseToken(std::string_view hex);

std::vector<uint8_t> Hello(uint64_t xuid, const Token& token);
std::vector<uint8_t> Ping(uint64_t stamp);
// A game datagram for player `to`, from the game's port `src` to its port `dst`.
std::vector<uint8_t> Send(uint64_t to, uint16_t src, uint16_t dst, const uint8_t* payload,
                          size_t size);

// What the relay sends a player.
struct Message {
  enum Kind { kWelcome, kPong, kDeliver } kind;
  uint64_t value = 0;  // welcome: own XUID; pong: the ping's stamp; deliver: the sender
  uint16_t src_port = 0;
  uint16_t dst_port = 0;
  const uint8_t* payload = nullptr;  // into the parsed buffer
  size_t size = 0;
};
std::optional<Message> Parse(const uint8_t* data, size_t size);

// 127.200.0.1: the first stand-in address.
constexpr uint32_t kFirstStandIn = 0x7FC80001;

// Relayed peers and their stand-in addresses.
class PeerTable {
 public:
  // The peer's stand-in address: the one it has, else the first free candidate `usable`
  // accepts (the transport binds there; another copy on this PC may hold it).
  std::optional<uint32_t> Add(uint64_t xuid, const std::function<bool(uint32_t)>& usable);
  std::optional<uint64_t> XuidAt(uint32_t stand_in) const;
  std::optional<uint32_t> StandInFor(uint64_t xuid) const;

 private:
  std::map<uint64_t, uint32_t> by_xuid_;
  std::map<uint32_t, uint64_t> by_address_;
};

// The direct probe (ticket 17): a copy asks a peer's probe port whether it can reach the
// peer at all; "ASP1", a type byte, the nonce. The answer echoes the nonce.
constexpr uint16_t kProbePort = 9101;
std::vector<uint8_t> Probe(uint64_t nonce);
std::vector<uint8_t> ProbeAnswer(uint64_t nonce);
std::optional<uint64_t> ParseProbe(const uint8_t* data, size_t size);
std::optional<uint64_t> ParseProbeAnswer(const uint8_t* data, size_t size);

enum class Route { kUndecided, kDirect, kRelay };

// Each peer's route for the session: a probe settles it, and it stays put after.
class RouteTable {
 public:
  Route Get(uint64_t xuid) const;
  // The probe was answered or timed out; returns the peer's route.
  Route Settle(uint64_t xuid, bool answered);
  // The relay delivered from this peer: an undecided peer is settled on the relay, so both
  // sides of a pair agree.
  void Relayed(uint64_t xuid);

 private:
  std::map<uint64_t, Route> routes_;
};

}  // namespace rex::kernel::xam::live::relay
