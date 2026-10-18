/**
 * @file        session_data.h
 * @brief       Xbox LIVE session data: contexts, properties, the server's session records,
 *              and search results in guest memory.
 *
 * After the Xenia netplay fork's XSession and XLiveAPI session calls
 * (src/xenia/kernel/xsession.cc, XLiveAPI.cpp; Copyright 2026 Xenia Canary, BSD license).
 * Properties travel as JSON objects ({id, type, bytes}) rather than the fork's host-endian
 * binary blobs; server/internal/xws stores them as given.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::kernel::xam::live {

// XUSER_DATA types; a property ID carries its type in the top nibble.
inline constexpr uint8_t kUserDataContext = 0;
inline constexpr uint8_t kUserDataInt32 = 1;
inline constexpr uint8_t kUserDataInt64 = 2;
inline constexpr uint8_t kUserDataDouble = 3;
inline constexpr uint8_t kUserDataWString = 4;
inline constexpr uint8_t kUserDataFloat = 5;
inline constexpr uint8_t kUserDataBinary = 6;
inline constexpr uint8_t kUserDataDateTime = 7;

struct UserContext {
  uint32_t id = 0;
  uint32_t value = 0;
  bool operator==(const UserContext&) const = default;
};

// A property value as the guest set it: big-endian numbers, UTF-16BE text with its
// terminator, raw bytes. It travels base64-encoded ("bytes"); ParseSessions also reads
// "int" and "double" values.
struct UserProperty {
  uint32_t id = 0;
  uint8_t type = 0;
  std::vector<uint8_t> value;
};

// XGIUserSetPropertyEx's value (`size` bytes at `data`) for property `id`.
UserProperty GuestProperty(uint32_t id, const uint8_t* data, uint32_t size);

struct SessionRecord {
  uint64_t session_id = 0;
  uint64_t host_xuid = 0;
  uint32_t flags = 0;
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  uint32_t open_public = 0;
  uint32_t open_private = 0;
  uint32_t filled_public = 0;
  uint32_t filled_private = 0;
  std::string host_address;   // public
  std::string local_address;  // on the host's own network; empty: the public one
  std::array<uint8_t, 6> mac = {};
  uint16_t port = 0;
  std::string build_id;
  std::vector<UserContext> contexts;
  std::vector<UserProperty> properties;
};

// POST title/{id}/sessions.
std::string SessionJson(const SessionRecord& session, uint32_t user_index);
// POST title/{id}/sessions/{id}/properties.
std::string PropertiesJson(const std::vector<UserContext>& contexts,
                           const std::vector<UserProperty>& properties);
// POST title/{id}/sessions/search.
std::string SearchJson(uint32_t search_index, uint32_t results, uint32_t users,
                       uint64_t searcher_xuid, const std::string& build_id,
                       const std::vector<UserContext>& contexts);
// A search reply (an array) or one session (an object).
std::vector<SessionRecord> ParseSessions(std::string_view body);

// XSESSION_SEARCHRESULT_HEADER, the XSESSION_SEARCHRESULTs, then their contexts, properties
// and property data, as the guest buffer at `guest_base` (big-endian, at most
// `buffer_size` bytes). Sessions that don't fit are left out.
std::vector<uint8_t> PackSearchResults(const std::vector<SessionRecord>& sessions,
                                       uint32_t guest_base, uint32_t buffer_size);

// XSESSION_INFO (0x3C bytes): the session ID, the host's XNADDR (local address in ina,
// public in inaOnline) and the key-exchange key.
void WriteSessionInfo(uint8_t* out, uint64_t session_id, uint32_t local_address,
                      uint32_t public_address, const std::array<uint8_t, 6>& mac,
                      uint16_t port);

}  // namespace rex::kernel::xam::live
