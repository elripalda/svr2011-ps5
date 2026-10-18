/**
 * @file        xsession.h
 * @brief       Xbox LIVE sessions (XSession*) against the game server's XWS API.
 *
 * WWE All Stars recomp, online play. Ported in part from the Xenia netplay fork's XSession
 * (src/xenia/kernel/xsession.cc, Copyright 2026 Xenia Canary, BSD license): Player Match
 * hosting, search, details, modify, delete, join and leave. No arbitration, stats or
 * migration. Only used with --wwe_online; otherwise the session messages keep their stubs.
 */

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/kernel/xam/live/session_data.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xam::live {

// The contexts and properties the game sets for a user (XGIUserSetContextEx and
// XGIUserSetPropertyEx); a hosted session advertises them.
void SetUserContext(uint32_t user_index, uint32_t id, uint32_t value);
void SetUserProperty(uint32_t user_index, const UserProperty& property);
std::vector<UserContext> UserContexts(uint32_t user_index);
std::vector<UserProperty> UserProperties(uint32_t user_index);

// The SHA-256 of the running executable (hex): only copies of the same build see each
// other's sessions. Computed once.
const std::string& BuildId();

class XSession : public system::XObject {
 public:
  static const Type kObjectType = Type::Session;

  explicit XSession(system::KernelState* kernel_state);

  // The guest object: 4 bytes holding the handle (as XAM's).
  X_STATUS Initialize();

  struct Member {
    uint64_t xuid = 0;
    uint32_t user_index = 0xFE;  // XUserIndexNone
    bool is_private = false;
    bool local = false;
  };

  // Guards everything below: XGI messages for one session come from several threads.
  std::mutex mutex;
  bool created = false;
  bool host = false;
  bool deleted = false;
  uint64_t session_id = 0;
  uint64_t nonce = 0;
  uint32_t flags = 0;
  uint32_t user_index_host = 0xFE;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint32_t game_type = 0;
  uint32_t game_mode = 0;
  uint32_t state = 0;  // XSESSION_STATE: 0 lobby, 2 in game
  std::array<uint8_t, 0x3C> info = {};
  std::map<uint64_t, Member> members;

  uint32_t FilledPublic() const;
  uint32_t FilledPrivate() const;
};

// XamSessionCreateHandle / XamSessionRefObjByHandle.
X_RESULT CreateSessionHandle(uint32_t* handle_out);
X_RESULT SessionObjectByHandle(uint32_t handle, uint32_t* guest_object_out);

// XGI session messages; `obj_ptr` is the guest object from XamSessionRefObjByHandle.
X_RESULT SessionCreate(uint32_t obj_ptr, uint32_t flags, uint32_t public_slots,
                       uint32_t private_slots, uint32_t user_index, uint32_t session_info_ptr,
                       uint32_t nonce_ptr);
X_RESULT SessionDelete(uint32_t obj_ptr);
X_RESULT SessionModify(uint32_t obj_ptr, uint32_t flags, uint32_t public_slots,
                       uint32_t private_slots);
X_RESULT SessionStart(uint32_t obj_ptr);
X_RESULT SessionEnd(uint32_t obj_ptr);
X_RESULT SessionJoin(uint32_t obj_ptr, uint32_t count, uint32_t xuids_ptr, uint32_t indices_ptr,
                     uint32_t private_ptr);
X_RESULT SessionLeave(uint32_t obj_ptr, uint32_t count, uint32_t xuids_ptr,
                      uint32_t indices_ptr);
X_RESULT SessionDetails(uint32_t obj_ptr, uint32_t size_ptr, uint32_t details_ptr);
X_RESULT SessionSearch(uint32_t search_index, uint32_t user_index, uint32_t max_results,
                       uint32_t num_contexts, uint32_t contexts_ptr, uint32_t buffer_size,
                       uint32_t results_ptr, uint32_t num_users);

}  // namespace rex::kernel::xam::live
