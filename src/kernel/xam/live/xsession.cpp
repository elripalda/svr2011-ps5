/**
 * @file        xsession.cpp
 * @brief       Xbox LIVE sessions against the game server (see xsession.h).
 *
 * Ported in part from the Xenia netplay fork's XSession and XLiveAPI session calls
 * (src/xenia/kernel/xsession.cc, XLiveAPI.cpp; Copyright 2026 Xenia Canary, BSD license).
 */

#include <rex/kernel/xam/live/xsession.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>

#include <fmt/format.h>

#include <rex/kernel/xam/live/http.h>
#include <rex/kernel/xam/live/live.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/string/utf8.h>
#include <rex/system/kernel_state.h>

#include "crypto/sha256.h"

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::kernel::xam::live {

using namespace rex::system;

namespace {

constexpr uint32_t kMaxUsers = 4;
constexpr uint32_t kSessionHost = 0x01;  // XSESSION_CREATE_HOST
constexpr uint32_t kContextGameType = 0x800A;
constexpr uint32_t kContextGameMode = 0x800B;
constexpr uint8_t kXnkidOnlinePeer = 0x80;
constexpr uint16_t kPlayerPort = 36000;  // XNADDR wPortOnline, as the fork reports it

struct UserData {
  std::mutex mutex;
  std::map<uint32_t, uint32_t> contexts[kMaxUsers];
  std::map<uint32_t, UserProperty> properties[kMaxUsers];
};

UserData& user_data() {
  static UserData data;
  return data;
}

std::string ApiUrl(const std::string& endpoint) {
  return ApiBaseUrl(REXCVAR_GET(wwe_online_server)) +
         fmt::format("title/{:08X}/{}", REX_KERNEL_STATE()->title_id(), endpoint);
}

std::string AddressText(uint32_t address) {
  return fmt::format("{}.{}.{}.{}", address >> 24, (address >> 16) & 0xFF,
                     (address >> 8) & 0xFF, address & 0xFF);
}

uint64_t Random64() {
  static std::mutex mutex;
  static std::mt19937_64 random(std::random_device{}() ^
                                uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::lock_guard lock(mutex);
  return random();
}

object_ref<XSession> SessionAt(uint32_t obj_ptr) {
  if (!obj_ptr) {
    return nullptr;
  }
  const uint32_t handle =
      memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(obj_ptr));
  return REX_KERNEL_OBJECTS()->LookupObject<XSession>(handle);
}

uint32_t ContextValue(uint32_t user_index, uint32_t id) {
  auto& d = user_data();
  std::lock_guard lock(d.mutex);
  if (user_index >= kMaxUsers) {
    return 0;
  }
  const auto it = d.contexts[user_index].find(id);
  return it == d.contexts[user_index].end() ? 0 : it->second;
}

void PostJson(const std::string& endpoint, const std::string& body, const char* what) {
  const auto response = HttpRequest("POST", ApiUrl(endpoint), body);
  if (response.status != 201) {
    REXKRNL_ERROR("[online] {} failed (HTTP {})", what, response.status);
  }
}

// The host advertises the contexts and properties its user set, plus its name.
void UploadProperties(const XSession& session) {
  auto properties = UserProperties(session.user_index_host);
  UserProperty hostname;
  hostname.id = 0x40008109;  // XPROPERTY_GAMER_HOSTNAME
  hostname.type = kUserDataWString;
  for (char16_t c : rex::string::to_utf16(Gamertag())) {
    hostname.value.push_back(uint8_t(c >> 8));
    hostname.value.push_back(uint8_t(c));
  }
  hostname.value.insert(hostname.value.end(), {0, 0});
  properties.push_back(hostname);
  PostJson(fmt::format("sessions/{:016x}/properties", session.session_id),
           PropertiesJson(UserContexts(session.user_index_host), properties),
           "session properties");
}

}  // namespace

void SetUserContext(uint32_t user_index, uint32_t id, uint32_t value) {
  auto& d = user_data();
  std::lock_guard lock(d.mutex);
  if (user_index < kMaxUsers) {
    d.contexts[user_index][id] = value;
  }
}

void SetUserProperty(uint32_t user_index, const UserProperty& property) {
  auto& d = user_data();
  std::lock_guard lock(d.mutex);
  if (user_index < kMaxUsers) {
    d.properties[user_index][property.id] = property;
  }
}

std::vector<UserContext> UserContexts(uint32_t user_index) {
  auto& d = user_data();
  std::lock_guard lock(d.mutex);
  std::vector<UserContext> out;
  if (user_index < kMaxUsers) {
    for (const auto& [id, value] : d.contexts[user_index]) {
      out.push_back({id, value});
    }
  }
  return out;
}

std::vector<UserProperty> UserProperties(uint32_t user_index) {
  auto& d = user_data();
  std::lock_guard lock(d.mutex);
  std::vector<UserProperty> out;
  if (user_index < kMaxUsers) {
    for (const auto& [id, property] : d.properties[user_index]) {
      out.push_back(property);
    }
  }
  return out;
}

const std::string& BuildId() {
  static const std::string id = [] {
    std::filesystem::path exe;
#if REX_PLATFORM_WIN32
    wchar_t path[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    exe = std::filesystem::path(std::wstring(path, n));
#else
    exe = std::filesystem::read_symlink("/proc/self/exe");
#endif
    std::ifstream in(exe, std::ios::binary);
    sha256::SHA256 sha;
    std::vector<char> chunk(1 << 20);
    while (in) {
      in.read(chunk.data(), std::streamsize(chunk.size()));
      sha.add(chunk.data(), size_t(in.gcount()));
    }
    const std::string hash = sha.getHash();
    REXKRNL_INFO("[online] build ID {} ({})", hash, exe.filename().string());
    return hash;
  }();
  return id;
}

XSession::XSession(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

X_STATUS XSession::Initialize() {
  auto* native = CreateNative(4);
  if (!native) {
    return X_STATUS_NO_MEMORY;
  }
  memory::store_and_swap<uint32_t>(native, handle());
  return X_STATUS_SUCCESS;
}

uint32_t XSession::FilledPublic() const {
  return uint32_t(std::count_if(members.begin(), members.end(),
                                [](const auto& m) { return !m.second.is_private; }));
}

uint32_t XSession::FilledPrivate() const {
  return uint32_t(members.size()) - FilledPublic();
}

X_RESULT CreateSessionHandle(uint32_t* handle_out) {
  auto session = object_ref<XSession>(new XSession(REX_KERNEL_STATE()));
  const X_STATUS status = session->Initialize();
  if (XFAILED(status)) {
    return status;
  }
  *handle_out = session->handle();
  return X_ERROR_SUCCESS;
}

X_RESULT SessionObjectByHandle(uint32_t handle, uint32_t* guest_object_out) {
  auto session = REX_KERNEL_OBJECTS()->LookupObject<XSession>(handle);
  if (!session) {
    return X_STATUS_INVALID_HANDLE;
  }
  session->RetainHandle();
  *guest_object_out = session->guest_object();
  return X_ERROR_SUCCESS;
}

X_RESULT SessionCreate(uint32_t obj_ptr, uint32_t flags, uint32_t public_slots,
                       uint32_t private_slots, uint32_t user_index, uint32_t session_info_ptr,
                       uint32_t nonce_ptr) {
  auto session = SessionAt(obj_ptr);
  if (!session) {
    return X_ERROR_FUNCTION_FAILED;
  }
  std::lock_guard lock(session->mutex);
  if (session->created) {
    return X_ERROR_FUNCTION_FAILED;
  }
  auto* memory = REX_KERNEL_MEMORY();
  uint8_t* info = memory->TranslateVirtual(session_info_ptr);
  session->created = true;
  session->flags = flags;
  session->max_public = public_slots;
  session->max_private = private_slots;
  session->game_type = ContextValue(user_index, kContextGameType);
  session->game_mode = ContextValue(user_index, kContextGameMode);
  session->nonce = Random64();
  if (nonce_ptr) {
    memory::store_and_swap<uint64_t>(memory->TranslateVirtual(nonce_ptr), session->nonce);
  }

  if (flags & kSessionHost) {
    session->host = true;
    session->user_index_host = user_index;
    session->session_id = (uint64_t(kXnkidOnlinePeer) << 56) | (Random64() & 0x00FFFFFFFFFFFFFFull);
    WriteSessionInfo(info, session->session_id, LocalAddress(), OnlineAddress(), Mac(),
                     kPlayerPort);
    SessionRecord record;
    record.session_id = session->session_id;
    record.host_xuid = OnlineXuid();
    record.flags = flags;
    record.public_slots = public_slots;
    record.private_slots = private_slots;
    record.host_address = AddressText(OnlineAddress());
    record.local_address = AddressText(LocalAddress());
    record.mac = Mac();
    record.port = kPlayerPort;
    record.build_id = BuildId();
    PostJson("sessions", SessionJson(record, user_index), "session create");
    UploadProperties(*session);
    REXKRNL_INFO("[online] hosting session {:016X} (flags {:X}, {} public, {} private)",
                 session->session_id, flags, public_slots, private_slots);
  } else {
    // Joining: the game passes the XSESSION_INFO a search returned.
    for (int i = 0; i < 8; ++i) {
      session->session_id = (session->session_id << 8) | info[i];
    }
    REXKRNL_INFO("[online] joining session {:016X}", session->session_id);
  }
  std::memcpy(session->info.data(), info, session->info.size());
  return X_ERROR_SUCCESS;
}

X_RESULT SessionDelete(uint32_t obj_ptr) {
  auto session = SessionAt(obj_ptr);
  if (!session) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard lock(session->mutex);
  if (session->deleted) {
    return X_ERROR_SUCCESS;
  }
  session->deleted = true;
  if (session->host) {
    const auto response =
        HttpRequest("DELETE", ApiUrl(fmt::format("sessions/{:016x}", session->session_id)));
    if (response.status != 200) {
      REXKRNL_ERROR("[online] session delete failed (HTTP {})", response.status);
    }
  }
  REXKRNL_INFO("[online] deleted session {:016X}", session->session_id);
  return X_ERROR_SUCCESS;
}

X_RESULT SessionModify(uint32_t obj_ptr, uint32_t flags, uint32_t public_slots,
                       uint32_t private_slots) {
  auto session = SessionAt(obj_ptr);
  if (!session) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard lock(session->mutex);
  // Only the modifier flags change; the creation flags (host, uses) stay.
  constexpr uint32_t kModifiers = 0x0F80;
  session->flags = (session->flags & ~kModifiers) | (flags & kModifiers);
  session->max_public = public_slots;
  session->max_private = private_slots;
  if (session->host) {
    PostJson(fmt::format("sessions/{:016x}/modify", session->session_id),
             fmt::format(R"({{"flags":{},"publicSlotsCount":{},"privateSlotsCount":{}}})",
                         session->flags, public_slots, private_slots),
             "session modify");
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SessionStart(uint32_t obj_ptr) {
  if (auto session = SessionAt(obj_ptr)) {
    std::lock_guard lock(session->mutex);
    session->state = 2;  // XSESSION_STATE in game
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SessionEnd(uint32_t obj_ptr) {
  if (auto session = SessionAt(obj_ptr)) {
    std::lock_guard lock(session->mutex);
    session->state = 0;  // back in the lobby
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SessionJoin(uint32_t obj_ptr, uint32_t count, uint32_t xuids_ptr, uint32_t indices_ptr,
                     uint32_t private_ptr) {
  auto session = SessionAt(obj_ptr);
  if (!session) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard lock(session->mutex);
  auto* memory = REX_KERNEL_MEMORY();
  const bool local = xuids_ptr == 0;
  std::vector<std::pair<uint64_t, bool>> joined;
  for (uint32_t i = 0; i < count; ++i) {
    XSession::Member member;
    member.local = local;
    if (local) {
      member.user_index =
          memory::load_and_swap<uint32_t>(memory->TranslateVirtual(indices_ptr + i * 4));
      member.xuid = OnlineXuid();
    } else {
      member.xuid = memory::load_and_swap<uint64_t>(memory->TranslateVirtual(xuids_ptr + i * 8));
    }
    member.is_private =
        private_ptr &&
        memory::load_and_swap<uint32_t>(memory->TranslateVirtual(private_ptr + i * 4)) &&
        session->FilledPrivate() < session->max_private;
    if (session->members.contains(member.xuid)) {
      continue;
    }
    session->members[member.xuid] = member;
    joined.emplace_back(member.xuid, member.is_private);
    REXKRNL_INFO("[online] {} {:016X} joined session {:016X} ({} slot)",
                 local ? "local" : "remote", member.xuid, session->session_id,
                 member.is_private ? "private" : "public");
  }
  if (session->host && !joined.empty()) {
    std::string xuids, privates;
    for (const auto& [xuid, is_private] : joined) {
      xuids += fmt::format("{}\"{:016X}\"", xuids.empty() ? "" : ",", xuid);
      privates += fmt::format("{}{}", privates.empty() ? "" : ",", is_private ? "true" : "false");
    }
    PostJson(fmt::format("sessions/{:016x}/join", session->session_id),
             fmt::format(R"({{"xuids":[{}],"privateSlots":[{}]}})", xuids, privates),
             "session join");
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SessionLeave(uint32_t obj_ptr, uint32_t count, uint32_t xuids_ptr,
                      uint32_t indices_ptr) {
  auto session = SessionAt(obj_ptr);
  if (!session) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard lock(session->mutex);
  auto* memory = REX_KERNEL_MEMORY();
  std::string xuids;
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t xuid =
        xuids_ptr ? memory::load_and_swap<uint64_t>(memory->TranslateVirtual(xuids_ptr + i * 8))
                  : OnlineXuid();
    if (session->members.erase(xuid)) {
      xuids += fmt::format("{}\"{:016X}\"", xuids.empty() ? "" : ",", xuid);
      REXKRNL_INFO("[online] {:016X} left session {:016X}", xuid, session->session_id);
    }
  }
  if (session->host && !xuids.empty()) {
    PostJson(fmt::format("sessions/{:016x}/leave", session->session_id),
             fmt::format(R"({{"xuids":[{}]}})", xuids), "session leave");
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SessionDetails(uint32_t obj_ptr, uint32_t size_ptr, uint32_t details_ptr) {
  auto session = SessionAt(obj_ptr);
  if (!session || !details_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  std::lock_guard lock(session->mutex);
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t buffer_size =
      size_ptr ? memory::load_and_swap<uint32_t>(memory->TranslateVirtual(size_ptr)) : 0x80;
  constexpr uint32_t kDetails = 0x80, kMember = 0x10;
  if (buffer_size < kDetails) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  // XSESSION_LOCAL_DETAILS, then as many XSESSION_MEMBERs as fit.
  uint8_t* d = memory->TranslateVirtual(details_ptr);
  std::memset(d, 0, kDetails);
  auto put = [&](uint32_t at, uint32_t v) { memory::store_and_swap<uint32_t>(d + at, v); };
  put(0, session->host ? session->user_index_host : 0xFE);
  put(4, session->game_type);
  put(8, session->game_mode);
  put(12, session->flags);
  put(16, session->max_public);
  put(20, session->max_private);
  put(24, session->max_public - std::min(session->max_public, session->FilledPublic()));
  put(28, session->max_private - std::min(session->max_private, session->FilledPrivate()));
  const uint32_t local_members = uint32_t(std::count_if(
      session->members.begin(), session->members.end(), [](const auto& m) { return m.second.local; }));
  put(32, local_members);
  const uint32_t fits = (buffer_size - kDetails) / kMember;
  const uint32_t returned = std::min<uint32_t>(fits, uint32_t(session->members.size()));
  put(36, returned);
  put(40, session->state);
  memory::store_and_swap<uint64_t>(d + 48, session->nonce);
  std::memcpy(d + 56, session->info.data(), session->info.size());
  put(124, returned ? details_ptr + kDetails : 0);
  uint32_t i = 0;
  for (const auto& [xuid, member] : session->members) {
    if (i == returned) {
      break;
    }
    uint8_t* m = d + kDetails + i * kMember;
    memory::store_and_swap<uint64_t>(m, xuid);
    memory::store_and_swap<uint32_t>(m + 8, member.user_index);
    memory::store_and_swap<uint32_t>(m + 12, member.is_private ? 1 : 0);
    ++i;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SessionSearch(uint32_t search_index, uint32_t user_index, uint32_t max_results,
                       uint32_t num_contexts, uint32_t contexts_ptr, uint32_t buffer_size,
                       uint32_t results_ptr, uint32_t num_users) {
  if (!SignedIn()) {
    return X_ERROR_FUNCTION_FAILED;
  }
  auto* memory = REX_KERNEL_MEMORY();
  std::vector<UserContext> contexts;
  for (uint32_t i = 0; i < num_contexts && contexts_ptr; ++i) {
    const uint8_t* c = memory->TranslateVirtual(contexts_ptr + i * 8);
    contexts.push_back({memory::load_and_swap<uint32_t>(c), memory::load_and_swap<uint32_t>(c + 4)});
  }
  const auto response = HttpRequest(
      "POST", ApiUrl("sessions/search"),
      SearchJson(search_index, max_results, std::max<uint32_t>(1, num_users), OnlineXuid(),
                 BuildId(), contexts));
  std::vector<SessionRecord> sessions;
  if (response.status == 201) {
    sessions = ParseSessions(response.body);
  } else {
    REXKRNL_ERROR("[online] session search failed (HTTP {})", response.status);
  }
  if (sessions.size() > max_results) {
    sessions.resize(max_results);
  }
  const auto packed = PackSearchResults(sessions, results_ptr, buffer_size);
  std::memcpy(memory->TranslateVirtual(results_ptr), packed.data(), packed.size());
  REXKRNL_INFO("[online] session search {} found {} session(s)", search_index,
               memory::load_and_swap<uint32_t>(packed.data()));
  return X_ERROR_SUCCESS;
}

}  // namespace rex::kernel::xam::live
