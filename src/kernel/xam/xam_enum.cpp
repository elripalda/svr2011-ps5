/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>

#include <rex/kernel/xam/live/live.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/net_trace.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <fmt/format.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L518
uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, mapped_void buffer_ptr,
                        uint32_t buffer_size, uint32_t* items_returned, uint32_t overlapped_ptr) {
  assert_true(flags == 0);

  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer_ptr.guest_address(), buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length,
                       mapped_u32 items_returned, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result =
      xeXamEnumerate(handle, flags, buffer, buffer_length,
                     !overlapped.guest_address() ? &dummy : nullptr, overlapped.guest_address());
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}

u32 XamCreateEnumeratorHandle_entry(u32 user_index, u32 app_id, u32 open_message,
                                    u32 close_message, u32 extra_size, u32 item_count, u32 flags,
                                    mapped_u32 handle_out) {
  // Signed in to Live, XTitleServerCreateEnumerator (XLiveBase 0x58039, the Live list of LSP
  // title servers) lists the game server's Rendez-Vous address (after the Xenia netplay
  // fork's XLiveBaseApp, BSD license).
  constexpr uint32_t kXLiveBase = 0xFC;
  constexpr uint32_t kTitleServerEnumerate = 0x58039;
  constexpr size_t kTitleServerInfoSize = 0xD0;  // XTITLE_SERVER_INFO
  if (app_id != kXLiveBase || open_message != kTitleServerEnumerate || !handle_out ||
      !live::SignedIn()) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), item_count,
                                                 kTitleServerInfoSize);
  void* extra = nullptr;
  auto result =
      e->Initialize(user_index, app_id, open_message, close_message, flags, extra_size, &extra);
  if (XFAILED(result)) {
    return result;
  }
  for (const auto& server : live::TitleServers()) {
    if (e->item_count() >= item_count) {
      break;
    }
    // inaServer (network order), dwFlags, szServerInfo[200].
    uint8_t* item = e->AppendItem();
    std::memset(item, 0, kTitleServerInfoSize);
    memory::store_and_swap<uint32_t>(item, server.address);
    memory::store_and_swap<uint32_t>(item + 4, server.flags);
    std::memcpy(item + 8, server.description.data(),
                std::min<size_t>(server.description.size(), kTitleServerInfoSize - 9));
  }
  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, mapped_u32 out_object_ptr) {
  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  e->RetainHandle();

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e->guest_object();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_TRACE_EXPORT(__imp__XamEnumerate, rex::kernel::xam::XamEnumerate_entry)
REX_TRACE_EXPORT(__imp__XamCreateEnumeratorHandle, rex::kernel::xam::XamCreateEnumeratorHandle_entry)
REX_EXPORT(__imp__XamGetPrivateEnumStructureFromHandle,
           rex::kernel::xam::XamGetPrivateEnumStructureFromHandle_entry)
