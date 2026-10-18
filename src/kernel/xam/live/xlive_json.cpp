/**
 * @file        xlive_json.cpp
 * @brief       XWS JSON (see xlive_json.h).
 *
 * Shapes from the Xenia netplay fork (src/xenia/kernel/json/player_object_json.cc,
 * XLiveAPI.cpp GetServers/Getwhoami), Copyright 2026 Xenia Canary, BSD license.
 */

#include <rex/kernel/xam/live/xlive_json.h>

#include <rex/kernel/xam/live/identity.h>

#include <cstdio>
#include <cstdlib>

#include <fmt/format.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace rex::kernel::xam::live {

std::string ApiBaseUrl(std::string_view server) {
  std::string url(server);
  if (url.find("://") == std::string::npos) {
    url = "http://" + url;
    const size_t host = url.find("://") + 3;
    if (url.find(':', host) == std::string::npos) {
      const size_t slash = url.find('/', host);
      url.insert(slash == std::string::npos ? url.size() : slash, ":36000");
    }
  }
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url + "/";
}

std::optional<uint32_t> ParseIPv4(std::string_view text) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = 0;
  const std::string s(text);
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 ||
      b > 255 || c > 255 || d > 255) {
    return std::nullopt;
  }
  return (a << 24) | (b << 16) | (c << 8) | d;
}

std::optional<std::string> ParseWhoami(std::string_view body) {
  rapidjson::Document doc;
  doc.Parse(body.data(), body.size());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("address") ||
      !doc["address"].IsString()) {
    return std::nullopt;
  }
  return std::string(doc["address"].GetString());
}

std::string PlayerJson(const PlayerRecord& player) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("xuid");
  writer.String(fmt::format("{:016X}", player.xuid).c_str());
  writer.Key("machineId");
  writer.String(fmt::format("{:016x}", player.machine_id).c_str());
  writer.Key("hostAddress");
  writer.String(player.host_address.c_str());
  writer.Key("localAddress");
  writer.String(player.local_address.c_str());
  writer.Key("gamertag");
  writer.String(player.gamertag.c_str());
  writer.Key("macAddress");
  writer.String(MacText(player.mac).c_str());
  if (!player.build_id.empty()) {
    writer.Key("buildId");
    writer.String(player.build_id.c_str());
  }
  writer.Key("gameVersion");
  writer.String(kGameVersion);
  if (!player.mapped_ports.empty()) {
    writer.Key("mappedPorts");
    writer.StartObject();
    for (const auto& [game, external] : player.mapped_ports) {
      writer.Key(std::to_string(game).c_str());
      writer.Uint(external);
    }
    writer.EndObject();
  }
  writer.Key("settings");
  writer.StartObject();
  writer.EndObject();
  writer.EndObject();
  return buffer.GetString();
}

std::optional<Registration> ParseRegistration(std::string_view body) {
  rapidjson::Document doc;
  doc.Parse(body.data(), body.size());
  if (doc.HasParseError() || !doc.IsObject()) {
    return std::nullopt;
  }
  Registration registration;
  if (doc.HasMember("relayAddress") && doc["relayAddress"].IsString()) {
    registration.relay_address = doc["relayAddress"].GetString();
  }
  if (doc.HasMember("relayToken") && doc["relayToken"].IsString()) {
    registration.relay_token = doc["relayToken"].GetString();
  }
  if (doc.HasMember("reachable") && doc["reachable"].IsBool()) {
    registration.reachable = doc["reachable"].GetBool();
  }
  return registration;
}

std::optional<std::string> ParseRefusal(std::string_view body) {
  rapidjson::Document doc;
  doc.Parse(body.data(), body.size());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("message") || !doc["message"].IsString()) {
    return std::nullopt;
  }
  return std::string(doc["message"].GetString());
}

std::optional<PlayerRecord> ParsePlayer(std::string_view body) {
  rapidjson::Document doc;
  doc.Parse(body.data(), body.size());
  if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("xuid") ||
      !doc["xuid"].IsString()) {
    return std::nullopt;
  }
  PlayerRecord player;
  try {
    player.xuid = std::stoull(doc["xuid"].GetString(), nullptr, 16);
    if (doc.HasMember("machineId") && doc["machineId"].IsString()) {
      player.machine_id = std::stoull(doc["machineId"].GetString(), nullptr, 16);
    }
  } catch (...) {
    return std::nullopt;
  }
  if (doc.HasMember("localAddress") && doc["localAddress"].IsString()) {
    player.local_address = doc["localAddress"].GetString();
  }
  if (doc.HasMember("hostAddress") && doc["hostAddress"].IsString()) {
    player.host_address = doc["hostAddress"].GetString();
  }
  if (doc.HasMember("gamertag") && doc["gamertag"].IsString()) {
    player.gamertag = doc["gamertag"].GetString();
  }
  if (doc.HasMember("macAddress") && doc["macAddress"].IsString()) {
    if (const auto mac = ParseMac(doc["macAddress"].GetString())) {
      player.mac = *mac;
    }
  }
  if (doc.HasMember("mappedPorts") && doc["mappedPorts"].IsObject()) {
    for (const auto& entry : doc["mappedPorts"].GetObject()) {
      const int game = std::atoi(entry.name.GetString());
      if (game > 0 && game <= 0xFFFF && entry.value.IsUint() && entry.value.GetUint() <= 0xFFFF) {
        player.mapped_ports[uint16_t(game)] = uint16_t(entry.value.GetUint());
      }
    }
  }
  return player;
}

std::vector<TitleServer> ParseTitleServers(std::string_view body) {
  rapidjson::Document doc;
  doc.Parse(body.data(), body.size());
  std::vector<TitleServer> servers;
  if (doc.HasParseError() || !doc.IsArray()) {
    return servers;
  }
  for (const auto& entry : doc.GetArray()) {
    if (!entry.IsObject() || !entry.HasMember("address") || !entry["address"].IsString()) {
      continue;
    }
    const auto address = ParseIPv4(entry["address"].GetString());
    if (!address) {
      continue;
    }
    TitleServer server;
    server.address = *address;
    if (entry.HasMember("flags") && entry["flags"].IsInt()) {
      server.flags = uint32_t(entry["flags"].GetInt());
    }
    if (entry.HasMember("description") && entry["description"].IsString()) {
      server.description = entry["description"].GetString();
    }
    servers.push_back(std::move(server));
  }
  return servers;
}

}  // namespace rex::kernel::xam::live
