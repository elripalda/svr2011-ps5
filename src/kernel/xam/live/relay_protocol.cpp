/**
 * @file        relay_protocol.cpp
 * @brief       The relay's wire format and stand-in addresses (see relay_protocol.h).
 */

#include <rex/kernel/xam/live/relay_protocol.h>

#include <cstring>

namespace rex::kernel::xam::live::relay {

namespace {

constexpr char kMagic[4] = {'A', 'S', 'R', '1'};
constexpr char kProbeMagic[4] = {'A', 'S', 'P', '1'};
enum ProbeType : uint8_t { kProbe = 1, kProbeAnswer = 2 };

std::vector<uint8_t> ProbePacket(ProbeType type, uint64_t nonce) {
  std::vector<uint8_t> out(kProbeMagic, kProbeMagic + sizeof(kProbeMagic));
  out.push_back(type);
  for (int i = 7; i >= 0; --i) {
    out.push_back(uint8_t(nonce >> (8 * i)));
  }
  return out;
}

std::optional<uint64_t> ParseProbePacket(ProbeType type, const uint8_t* data, size_t size) {
  if (size != sizeof(kProbeMagic) + 9 || std::memcmp(data, kProbeMagic, sizeof(kProbeMagic)) ||
      data[4] != type) {
    return std::nullopt;
  }
  uint64_t nonce = 0;
  for (int i = 0; i < 8; ++i) {
    nonce = (nonce << 8) | data[5 + i];
  }
  return nonce;
}
constexpr size_t kHeader = 5;
enum Type : uint8_t { kHello = 1, kWelcome, kPing, kPong, kSend, kDeliver };
// 127.200.0.1 .. 127.200.255.254
constexpr uint32_t kStandIns = 0xFFFE;

std::vector<uint8_t> Packet(Type type) {
  std::vector<uint8_t> out(kMagic, kMagic + sizeof(kMagic));
  out.push_back(type);
  return out;
}

void Put(std::vector<uint8_t>& out, uint64_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) {
    out.push_back(uint8_t(value >> (8 * i)));
  }
}

uint64_t Get(const uint8_t* data, int bytes) {
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::optional<Token> ParseToken(std::string_view hex) {
  Token token;
  if (hex.size() != token.size() * 2) {
    return std::nullopt;
  }
  for (size_t i = 0; i < token.size(); ++i) {
    const int high = HexDigit(hex[2 * i]), low = HexDigit(hex[2 * i + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    token[i] = uint8_t(high << 4 | low);
  }
  return token;
}

std::vector<uint8_t> Hello(uint64_t xuid, const Token& token) {
  auto out = Packet(kHello);
  Put(out, xuid, 8);
  out.insert(out.end(), token.begin(), token.end());
  return out;
}

std::vector<uint8_t> Ping(uint64_t stamp) {
  auto out = Packet(kPing);
  Put(out, stamp, 8);
  return out;
}

std::vector<uint8_t> Send(uint64_t to, uint16_t src, uint16_t dst, const uint8_t* payload,
                          size_t size) {
  auto out = Packet(kSend);
  Put(out, to, 8);
  Put(out, src, 2);
  Put(out, dst, 2);
  if (size) {
    out.insert(out.end(), payload, payload + size);
  }
  return out;
}

std::optional<Message> Parse(const uint8_t* data, size_t size) {
  if (size < kHeader + 8 || std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    return std::nullopt;
  }
  Message message{};
  message.value = Get(data + kHeader, 8);
  switch (data[4]) {
    case kWelcome:
      message.kind = Message::kWelcome;
      return size == kHeader + 8 ? std::optional(message) : std::nullopt;
    case kPong:
      message.kind = Message::kPong;
      return size == kHeader + 8 ? std::optional(message) : std::nullopt;
    case kDeliver:
      if (size < kHeader + 12) {
        return std::nullopt;
      }
      message.kind = Message::kDeliver;
      message.src_port = uint16_t(Get(data + kHeader + 8, 2));
      message.dst_port = uint16_t(Get(data + kHeader + 10, 2));
      message.payload = data + kHeader + 12;
      message.size = size - kHeader - 12;
      return message;
    default:
      return std::nullopt;
  }
}

std::optional<uint32_t> PeerTable::Add(uint64_t xuid,
                                       const std::function<bool(uint32_t)>& usable) {
  if (const auto found = by_xuid_.find(xuid); found != by_xuid_.end()) {
    return found->second;
  }
  for (uint32_t i = 0; i < kStandIns; ++i) {
    const uint32_t address = kFirstStandIn + i;
    if (by_address_.count(address) || !usable(address)) {
      continue;
    }
    by_xuid_[xuid] = address;
    by_address_[address] = xuid;
    return address;
  }
  return std::nullopt;
}

std::optional<uint64_t> PeerTable::XuidAt(uint32_t stand_in) const {
  const auto found = by_address_.find(stand_in);
  return found == by_address_.end() ? std::nullopt : std::optional(found->second);
}

std::vector<uint8_t> Probe(uint64_t nonce) {
  return ProbePacket(kProbe, nonce);
}

std::vector<uint8_t> ProbeAnswer(uint64_t nonce) {
  return ProbePacket(kProbeAnswer, nonce);
}

std::optional<uint64_t> ParseProbe(const uint8_t* data, size_t size) {
  return ParseProbePacket(kProbe, data, size);
}

std::optional<uint64_t> ParseProbeAnswer(const uint8_t* data, size_t size) {
  return ParseProbePacket(kProbeAnswer, data, size);
}

Route RouteTable::Get(uint64_t xuid) const {
  const auto found = routes_.find(xuid);
  return found == routes_.end() ? Route::kUndecided : found->second;
}

Route RouteTable::Settle(uint64_t xuid, bool answered) {
  auto& route = routes_[xuid];
  if (route == Route::kUndecided) {
    route = answered ? Route::kDirect : Route::kRelay;
  }
  return route;
}

void RouteTable::Relayed(uint64_t xuid) {
  auto& route = routes_[xuid];
  if (route == Route::kUndecided) {
    route = Route::kRelay;
  }
}

std::optional<uint32_t> PeerTable::StandInFor(uint64_t xuid) const {
  const auto found = by_xuid_.find(xuid);
  return found == by_xuid_.end() ? std::nullopt : std::optional(found->second);
}

}  // namespace rex::kernel::xam::live::relay
