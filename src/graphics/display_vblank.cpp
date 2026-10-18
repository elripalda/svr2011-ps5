/**
 * @file        graphics/display_vblank.cpp
 * @brief       The host display's real vblank
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include "display_vblank.h"

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
// d3dkmthk.h needs NTSTATUS from winternl.h and the D3DKMT types from the Windows SDK.
#include <winternl.h>
#include <d3dkmthk.h>
#endif

namespace rex::graphics {

#if REX_PLATFORM_WIN32

DisplayVblank::~DisplayVblank() {
  if (adapter_) {
    D3DKMT_CLOSEADAPTER close = {};
    close.hAdapter = adapter_;
    D3DKMTCloseAdapter(&close);
  }
}

bool DisplayVblank::Open() {
  MONITORINFOEXW monitor = {};
  monitor.cbSize = sizeof(monitor);
  HMONITOR handle = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  if (!GetMonitorInfoW(handle, &monitor)) {
    return false;
  }
  HDC dc = CreateDCW(monitor.szDevice, nullptr, nullptr, nullptr);
  if (!dc) {
    return false;
  }
  D3DKMT_OPENADAPTERFROMHDC open = {};
  open.hDc = dc;
  NTSTATUS status = D3DKMTOpenAdapterFromHdc(&open);
  DeleteDC(dc);
  if (status != 0) {
    return false;
  }
  adapter_ = open.hAdapter;
  source_id_ = open.VidPnSourceId;
  char name[64] = {};
  WideCharToMultiByte(CP_UTF8, 0, monitor.szDevice, -1, name, sizeof(name) - 1, nullptr, nullptr);
  name_ = name;
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1) {
    mode_hz_ = double(mode.dmDisplayFrequency);
  }
  return true;
}

std::optional<DisplayVblank::Clock::time_point> DisplayVblank::Wait() {
  if (!adapter_) {
    return std::nullopt;
  }
  D3DKMT_WAITFORVERTICALBLANKEVENT wait = {};
  wait.hAdapter = adapter_;
  wait.VidPnSourceId = source_id_;
  if (D3DKMTWaitForVerticalBlankEvent(&wait) != 0) {
    return std::nullopt;
  }
  return Clock::now();
}

#else

DisplayVblank::~DisplayVblank() = default;
bool DisplayVblank::Open() { return false; }
std::optional<DisplayVblank::Clock::time_point> DisplayVblank::Wait() { return std::nullopt; }

#endif

}  // namespace rex::graphics
