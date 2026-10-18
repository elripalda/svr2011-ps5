/**
 * @file        session_data.cpp
 * @brief       Xbox LIVE session data (see session_data.h).
 *
 * After the Xenia netplay fork's XSession::FillSessionSearchResult, GetSessions and
 * XLiveAPI::XSessionCreate/SessionSearch (src/xenia/kernel/xsession.cc, XLiveAPI.cpp;
 * Copyright 2026 Xenia Canary, BSD license).
 */

#include <rex/kernel/xam/live/session_data.h>

#include <rex/kernel/xam/live/identity.h>
#include <rex/kernel/xam/live/xlive_json.h>

#include <cstring>

#include <fmt/format.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace rex::kernel::xam::live {

namespace {

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const std::vector<uint8_t>& data) {
  std::string out;
  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t n = (uint32_t(data[i]) << 16) |
                       (i + 1 < data.size() ? uint32_t(data[i + 1]) << 8 : 0) |
                       (i + 2 < data.size() ? data[i + 2] : 0);
    out += kBase64[(n >> 18) & 63];
    out += kBase64[(n >> 12) & 63];
    out += i + 1 < data.size() ? kBase64[(n >> 6) & 63] : '=';
    out += i + 2 < data.size() ? kBase64[n & 63] : '=';
  }
  return out;
}

std::vector<uint8_t> Unbase64(std::string_view text) {
  std::vector<uint8_t> out;
  uint32_t bits = 0;
  int count = 0;
  for (char c : text) {
    const char* at = std::strchr(kBase64, c);
    if (c == '=' || !c || !at) {
      continue;
    }
    bits = (bits << 6) | uint32_t(at - kBase64);
    if (++count == 4) {
      out.push_back(uint8_t(bits >> 16));
      out.push_back(uint8_t(bits >> 8));
      out.push_back(uint8_t(bits));
      bits = 0;
      count = 0;
    }
  }
  if (count == 3) {
    out.push_back(uint8_t(bits >> 10));
    out.push_back(uint8_t(bits >> 2));
  } else if (count == 2) {
    out.push_back(uint8_t(bits >> 4));
  }
  return out;
}

uint64_t ReadBe(const std::vector<uint8_t>& value) {
  uint64_t n = 0;
  for (uint8_t b : value) {
    n = (n << 8) | b;
  }
  return n;
}

std::vector<uint8_t> WriteBe(uint64_t n, size_t size) {
  std::vector<uint8_t> out(size);
  for (size_t i = 0; i < size; ++i) {
    out[size - 1 - i] = uint8_t(n >> (8 * i));
  }
  return out;
}

size_t NumberSize(uint8_t type) {
  switch (type) {
    case kUserDataInt32:
    case kUserDataFloat:
    case kUserDataContext:
      return 4;
    case kUserDataInt64:
    case kUserDataDouble:
    case kUserDataDateTime:
      return 8;
    default:
      return 0;
  }
}

void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24);
  b[at + 1] = uint8_t(v >> 16);
  b[at + 2] = uint8_t(v >> 8);
  b[at + 3] = uint8_t(v);
}

}  // namespace

UserProperty GuestProperty(uint32_t id, const uint8_t* data, uint32_t size) {
  // The bytes stay as the guest gave them: the game sets 4-byte values on its 8-byte
  // (int64) properties 0x2000000D-0F and reads them back as 32-bit numbers.
  UserProperty p;
  p.id = id;
  p.type = uint8_t((id >> 28) & 0xF);
  p.value.assign(data, data + size);
  return p;
}

std::string SessionJson(const SessionRecord& s, uint32_t user_index) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("sessionId");
  w.String(fmt::format("{:016x}", s.session_id).c_str());
  w.Key("xuid");
  w.String(fmt::format("{:016X}", s.host_xuid).c_str());
  w.Key("flags");
  w.Uint(s.flags);
  w.Key("publicSlotsCount");
  w.Uint(s.public_slots);
  w.Key("privateSlotsCount");
  w.Uint(s.private_slots);
  w.Key("userIndex");
  w.Uint(user_index);
  w.Key("hostAddress");
  w.String(s.host_address.c_str());
  w.Key("localAddress");
  w.String(s.local_address.c_str());
  w.Key("macAddress");
  w.String(MacText(s.mac).c_str());
  w.Key("port");
  w.Uint(s.port);
  w.Key("buildId");
  w.String(s.build_id.c_str());
  w.EndObject();
  return buffer.GetString();
}

namespace {

void WriteContexts(rapidjson::Writer<rapidjson::StringBuffer>& w,
                   const std::vector<UserContext>& contexts) {
  w.StartArray();
  for (const auto& c : contexts) {
    w.StartObject();
    w.Key("id");
    w.Uint(c.id);
    w.Key("value");
    w.Uint(c.value);
    w.EndObject();
  }
  w.EndArray();
}

}  // namespace

std::string PropertiesJson(const std::vector<UserContext>& contexts,
                           const std::vector<UserProperty>& properties) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("contexts");
  WriteContexts(w, contexts);
  w.Key("properties");
  w.StartArray();
  for (const auto& p : properties) {
    w.StartObject();
    w.Key("id");
    w.Uint(p.id);
    w.Key("type");
    w.Uint(p.type);
    w.Key("bytes");
    w.String(Base64(p.value).c_str());
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return buffer.GetString();
}

std::string SearchJson(uint32_t search_index, uint32_t results, uint32_t users,
                       uint64_t searcher_xuid, const std::string& build_id,
                       const std::vector<UserContext>& contexts) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
  w.StartObject();
  w.Key("searchIndex");
  w.Uint(search_index);
  w.Key("resultsCount");
  w.Uint(results);
  w.Key("numUsers");
  w.Uint(users);
  w.Key("searcher_xuid");
  w.String(fmt::format("{:016X}", searcher_xuid).c_str());
  w.Key("buildId");
  w.String(build_id.c_str());
  w.Key("contexts");
  WriteContexts(w, contexts);
  w.EndObject();
  return buffer.GetString();
}

namespace {

uint32_t GetUint(const rapidjson::Value& o, const char* key) {
  return o.HasMember(key) && o[key].IsUint() ? o[key].GetUint() : 0;
}

std::string GetString(const rapidjson::Value& o, const char* key) {
  return o.HasMember(key) && o[key].IsString() ? o[key].GetString() : "";
}

SessionRecord ParseSession(const rapidjson::Value& o) {
  SessionRecord s;
  try {
    s.session_id = std::stoull(GetString(o, "sessionId"), nullptr, 16);
    s.host_xuid = std::stoull(GetString(o, "xuid"), nullptr, 16);
  } catch (...) {
  }
  s.flags = GetUint(o, "flags");
  s.public_slots = GetUint(o, "publicSlotsCount");
  s.private_slots = GetUint(o, "privateSlotsCount");
  s.open_public = GetUint(o, "openPublicSlotsCount");
  s.open_private = GetUint(o, "openPrivateSlotsCount");
  s.filled_public = GetUint(o, "filledPublicSlotsCount");
  s.filled_private = GetUint(o, "filledPrivateSlotsCount");
  s.host_address = GetString(o, "hostAddress");
  s.local_address = GetString(o, "localAddress");
  if (const auto mac = ParseMac(GetString(o, "macAddress"))) {
    s.mac = *mac;
  }
  s.port = uint16_t(GetUint(o, "port"));
  s.build_id = GetString(o, "buildId");
  if (o.HasMember("contexts") && o["contexts"].IsArray()) {
    for (const auto& c : o["contexts"].GetArray()) {
      if (c.IsObject()) {
        s.contexts.push_back({GetUint(c, "id"), GetUint(c, "value")});
      }
    }
  }
  if (o.HasMember("properties") && o["properties"].IsArray()) {
    for (const auto& j : o["properties"].GetArray()) {
      if (!j.IsObject()) {
        continue;
      }
      UserProperty p;
      p.id = GetUint(j, "id");
      p.type = uint8_t(GetUint(j, "type"));
      const size_t number = NumberSize(p.type);
      if (number && j.HasMember("int") && j["int"].IsInt64()) {
        p.value = WriteBe(uint64_t(j["int"].GetInt64()), number);
      } else if (number && j.HasMember("double") && j["double"].IsNumber()) {
        const double d = j["double"].GetDouble();
        if (p.type == kUserDataFloat) {
          const float f = float(d);
          uint32_t bits;
          std::memcpy(&bits, &f, 4);
          p.value = WriteBe(bits, 4);
        } else {
          uint64_t bits;
          std::memcpy(&bits, &d, 8);
          p.value = WriteBe(bits, 8);
        }
      } else {
        p.value = Unbase64(GetString(j, "bytes"));
      }
      s.properties.push_back(std::move(p));
    }
  }
  return s;
}

}  // namespace

std::vector<SessionRecord> ParseSessions(std::string_view body) {
  rapidjson::Document doc;
  doc.Parse(body.data(), body.size());
  std::vector<SessionRecord> sessions;
  if (doc.HasParseError()) {
    return sessions;
  }
  if (doc.IsObject()) {
    sessions.push_back(ParseSession(doc));
  } else if (doc.IsArray()) {
    for (const auto& o : doc.GetArray()) {
      if (o.IsObject()) {
        sessions.push_back(ParseSession(o));
      }
    }
  }
  return sessions;
}

void WriteSessionInfo(uint8_t* out, uint64_t session_id, uint32_t local_address,
                      uint32_t public_address, const std::array<uint8_t, 6>& mac,
                      uint16_t port) {
  std::memset(out, 0, 0x3C);
  for (int i = 0; i < 8; ++i) {
    out[i] = uint8_t(session_id >> (56 - 8 * i));
  }
  for (int i = 0; i < 4; ++i) {
    out[8 + i] = uint8_t(local_address >> (24 - 8 * i));    // ina
    out[12 + i] = uint8_t(public_address >> (24 - 8 * i));  // inaOnline
  }
  out[16] = uint8_t(port >> 8);  // wPortOnline
  out[17] = uint8_t(port);
  std::memcpy(out + 18, mac.data(), 6);  // abEnet
  out[24 + 16] = 1;                      // abOnline: SGADDR platform type, Xbox 360
  for (int i = 0; i < 16; ++i) {
    out[44 + i] = uint8_t(i);  // keyExchangeKey (the fork's constant XNKEY)
  }
}

std::vector<uint8_t> PackSearchResults(const std::vector<SessionRecord>& sessions,
                                       uint32_t guest_base, uint32_t buffer_size) {
  constexpr size_t kHeader = 8, kResult = 0x5C, kContext = 8, kProperty = 0x18;
  auto align8 = [](size_t n) { return (n + 7) & ~size_t(7); };
  auto extra = [](const SessionRecord& s) {
    size_t n = 0;
    for (const auto& p : s.properties) {
      if (!NumberSize(p.type)) {
        n += (p.value.size() + 7) & ~size_t(7);
      }
    }
    return n;
  };
  // How many sessions fit, with all their data.
  size_t count = 0, tail = 0;
  for (; count < sessions.size(); ++count) {
    const auto& s = sessions[count];
    const size_t more = align8(s.contexts.size() * kContext) +
                        align8(s.properties.size() * kProperty) + extra(s);
    if (align8(kHeader + (count + 1) * kResult) + tail + more > buffer_size) {
      break;
    }
    tail += more;
  }
  std::vector<uint8_t> out(align8(kHeader + count * kResult) + tail);
  Put32(out, 0, uint32_t(count));
  Put32(out, 4, count ? guest_base + uint32_t(kHeader) : 0);
  size_t cursor = align8(kHeader + count * kResult);
  for (size_t i = 0; i < count; ++i) {
    const auto& s = sessions[i];
    const size_t r = kHeader + i * kResult;
    const uint32_t public_address = ParseIPv4(s.host_address).value_or(0);
    WriteSessionInfo(&out[r], s.session_id, ParseIPv4(s.local_address).value_or(public_address),
                     public_address, s.mac, s.port);
    Put32(out, r + 60, s.open_public);
    Put32(out, r + 64, s.open_private);
    Put32(out, r + 68, s.filled_public);
    Put32(out, r + 72, s.filled_private);
    Put32(out, r + 76, uint32_t(s.properties.size()));
    Put32(out, r + 80, uint32_t(s.contexts.size()));

    Put32(out, r + 88, s.contexts.empty() ? 0 : guest_base + uint32_t(cursor));
    for (const auto& c : s.contexts) {
      Put32(out, cursor, c.id);
      Put32(out, cursor + 4, c.value);
      cursor += kContext;
    }
    cursor = align8(cursor);

    const size_t properties = cursor;
    Put32(out, r + 84, s.properties.empty() ? 0 : guest_base + uint32_t(properties));
    cursor = align8(cursor + s.properties.size() * kProperty);
    for (size_t j = 0; j < s.properties.size(); ++j) {
      const auto& p = s.properties[j];
      const size_t at = properties + j * kProperty;
      Put32(out, at, p.id);
      out[at + 8] = p.type;
      if (NumberSize(p.type)) {
        std::memcpy(&out[at + 16], p.value.data(), std::min<size_t>(p.value.size(), 8));
      } else {
        Put32(out, at + 16, uint32_t(p.value.size()));
        Put32(out, at + 20, guest_base + uint32_t(cursor));
        std::memcpy(&out[cursor], p.value.data(), p.value.size());
        cursor += (p.value.size() + 7) & ~size_t(7);
      }
    }
  }
  return out;
}

}  // namespace rex::kernel::xam::live
