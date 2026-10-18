/**
 * @file        identity.h
 * @brief       The install's online identity: a random online XUID and MAC address.
 *
 * WWE All Stars recomp, online play (.scratch/online-play/). Every copy of the game used
 * to be XUID 0xB13EBABEBABEBABE with MAC CC:CC:CC:CC:CC:CC, so two copies were one player.
 * The first online start writes online-identity.json to the user data folder, with this
 * machine's key: a copy of the folder on another machine gets its own identity there
 * (issue #2: two friends on one XUID never saw each other's sessions).
 */

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace rex::kernel::xam::live {

struct OnlineIdentity {
  uint64_t xuid = 0;
  std::array<uint8_t, 6> mac = {};
  std::string machine;  // MachineKey() of the machine it was made on
};

inline constexpr const char* kIdentityFileName = "online-identity.json";

// An online XUID (0x0009 followed by 48 random bits) and a random locally administered,
// unicast MAC.
OnlineIdentity NewIdentity(std::mt19937_64& random);

std::string SerializeIdentity(const OnlineIdentity& identity);
std::optional<OnlineIdentity> ParseIdentity(std::string_view json);

// The identity in `folder`, written there first when it is missing, unreadable or made on
// another machine than `machine` (a file without a machine counts as another's). An empty
// `machine` takes any readable file.
OnlineIdentity LoadOrCreateIdentity(const std::filesystem::path& folder,
                                    std::mt19937_64& random, std::string_view machine);

// This machine's key: a hash of its installation ID (Windows MachineGuid, /etc/machine-id),
// so the file never holds the ID itself. Empty when there is none.
std::string MachineKey();

// "02aabbcc0a01", as XWS stores MAC addresses.
std::string MacText(const std::array<uint8_t, 6>& mac);
std::optional<std::array<uint8_t, 6>> ParseMac(std::string_view text);

}  // namespace rex::kernel::xam::live
