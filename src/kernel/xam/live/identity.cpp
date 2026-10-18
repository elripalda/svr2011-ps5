/**
 * @file        identity.cpp
 * @brief       The install's online identity (see identity.h).
 */

#include <rex/kernel/xam/live/identity.h>

#include <fstream>
#include <sstream>

#include <fmt/format.h>
#include <rapidjson/document.h>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::kernel::xam::live {

OnlineIdentity NewIdentity(std::mt19937_64& random) {
  OnlineIdentity identity;
  do {
    identity.xuid = 0x0009000000000000ull | (random() & 0x0000FFFFFFFFFFFFull);
  } while ((identity.xuid & 0x0000FFFFFFFFFFFFull) == 0);
  const uint64_t bits = random();
  for (size_t i = 0; i < identity.mac.size(); ++i) {
    identity.mac[i] = uint8_t(bits >> (8 * i));
  }
  identity.mac[0] = uint8_t((identity.mac[0] & 0xFC) | 0x02);
  return identity;
}

std::string MacText(const std::array<uint8_t, 6>& mac) {
  return fmt::format("{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", mac[0], mac[1], mac[2], mac[3],
                     mac[4], mac[5]);
}

std::optional<std::array<uint8_t, 6>> ParseMac(std::string_view text) {
  std::string digits;
  for (char c : text) {
    if (c != ':' && c != '-') {
      digits.push_back(c);
    }
  }
  if (digits.size() != 12) {
    return std::nullopt;
  }
  std::array<uint8_t, 6> mac{};
  for (size_t i = 0; i < mac.size(); ++i) {
    unsigned value = 0;
    for (size_t j = 0; j < 2; ++j) {
      const char c = digits[i * 2 + j];
      unsigned digit;
      if (c >= '0' && c <= '9') {
        digit = unsigned(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = unsigned(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = unsigned(c - 'A' + 10);
      } else {
        return std::nullopt;
      }
      value = value * 16 + digit;
    }
    mac[i] = uint8_t(value);
  }
  return mac;
}

std::string SerializeIdentity(const OnlineIdentity& identity) {
  return fmt::format("{{\n  \"xuid\": \"{:016X}\",\n  \"mac\": \"{}\",\n  \"machine\": \"{}\"\n}}\n",
                     identity.xuid, MacText(identity.mac), identity.machine);
}

std::optional<OnlineIdentity> ParseIdentity(std::string_view json) {
  rapidjson::Document doc;
  doc.Parse(json.data(), json.size());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("xuid") ||
      !doc["xuid"].IsString() || !doc.HasMember("mac") || !doc["mac"].IsString()) {
    return std::nullopt;
  }
  OnlineIdentity identity;
  try {
    identity.xuid = std::stoull(doc["xuid"].GetString(), nullptr, 16);
  } catch (...) {
    return std::nullopt;
  }
  const auto mac = ParseMac(doc["mac"].GetString());
  if ((identity.xuid >> 48) != 0x0009 || !mac) {
    return std::nullopt;
  }
  identity.mac = *mac;
  if (doc.HasMember("machine") && doc["machine"].IsString()) {
    identity.machine = doc["machine"].GetString();
  }
  return identity;
}

std::string MachineKey() {
  std::string id;
#if REX_PLATFORM_WIN32
  char guid[64] = {};
  DWORD size = sizeof(guid);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                   RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, guid, &size) == ERROR_SUCCESS) {
    id = guid;
  }
#else
  for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
    std::ifstream in(path);
    if (in >> id && !id.empty()) {
      break;
    }
  }
#endif
  if (id.empty()) {
    return {};
  }
  uint64_t hash = 0xCBF29CE484222325ull;  // FNV-1a
  for (const char c : id) {
    hash = (hash ^ uint8_t(c)) * 0x100000001B3ull;
  }
  return fmt::format("{:016x}", hash);
}

OnlineIdentity LoadOrCreateIdentity(const std::filesystem::path& folder,
                                    std::mt19937_64& random, std::string_view machine) {
  const auto file = folder / kIdentityFileName;
  {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      std::stringstream text;
      text << in.rdbuf();
      auto identity = ParseIdentity(text.str());
      if (identity && (machine.empty() || identity->machine == machine)) {
        return *identity;
      }
    }
  }
  OnlineIdentity identity = NewIdentity(random);
  identity.machine = std::string(machine);
  std::error_code ignored;
  std::filesystem::create_directories(folder, ignored);
  std::ofstream(file, std::ios::binary | std::ios::trunc) << SerializeIdentity(identity);
  return identity;
}

}  // namespace rex::kernel::xam::live
