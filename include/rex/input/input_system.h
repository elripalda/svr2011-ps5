#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/input/vibration_ledger.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  /// Host menus over the game (an in-game settings page): while hold returns
  /// true, the game reads an idle controller and no keystrokes, and the real
  /// state stays readable through HeldState. Adapted from wwe-svr2011-pc
  /// (KaikoClanworth1), used with the author's permission.
  void SetGuestInputHold(std::function<bool()> hold);
  /// The last real state the game polled for user_index, as if no hold were
  /// on. False if the user has not been polled or has no controller.
  bool HeldState(uint32_t user_index, X_INPUT_STATE* out_state);

  /// Replaces any previous assignment. Call before the guest starts polling.
  void SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  /// As above, with the player index the guest passed before XamInputSetState
  /// pinned it, for the vibration ledger and trace.
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration, uint32_t raw_user_index);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);

  /// Every vibration request the guest sent so far (a copy).
  VibrationLedger vibration_ledger();
  /// The pad drivers the vibration line names, as AddDriver added them
  /// ("sdl", "xinput", "sdl+script"), or "none" when no pad driver loaded.
  std::string vibration_backend();
  std::string VibrationLine(VibrationLineKind kind);
  /// Logs the vibration line if it changed since the last one logged; returns
  /// whether it did. SetState calls it every 10 s.
  bool LogVibration(VibrationLineKind kind);
  /// Sets every real pad on every driver to zero and logs the final line, so
  /// no motor outlives the game. Synthetic devices are skipped. Later
  /// SetState calls are dropped, so a straggling guest thread cannot restart
  /// a motor. Shutdown and ReXApp::OnClosing call it; only the first call acts.
  void StopVibration();

  /// What a request sends to the pads: (user index after the pin, the guest's
  /// motor speeds) -> the motor speeds the user's pads get. Called once per
  /// request, so a change applies to the next one; the ledger and trace still
  /// record the guest's request.
  using VibrationOutput =
      std::function<X_INPUT_VIBRATION(uint32_t user_index, const X_INPUT_VIBRATION& requested)>;
  void set_vibration_output(VibrationOutput output);

 private:
  /// Re-enumerates every driver and notifies the assignment when the set
  /// changed.
  void RefreshDevices();
  InputDriver* DriverForDevice(DeviceId id);
  const DeviceInfo* DeviceInfoFor(DeviceId id) const;

  bool GuestInputHeld();

  /// Adds one request to the ledger and logs the periodic line when due.
  void RecordVibration(uint32_t raw_user_index, uint32_t user_index,
                       const X_INPUT_VIBRATION& vibration, std::optional<X_RESULT> pads);

  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::mutex hold_mutex_;
  std::function<bool()> guest_input_hold_;
  X_INPUT_STATE real_state_[4] = {};
  bool has_real_state_[4] = {};

  // Guards the ledger, its log, the request trace and the backend name.
  std::mutex vibration_mutex_;
  VibrationLedger vibration_;
  VibrationLog vibration_log_;
  VibrationTrace vibration_trace_;
  std::string backend_name_ = "none";
  // Serialises SetState's driver calls with StopVibration's; guards the
  // output.
  std::mutex vibration_drive_mutex_;
  bool vibration_stopped_ = false;
  VibrationOutput vibration_output_;

  std::unique_ptr<DeviceAssignment> assignment_;
  ActiveDeviceTracker active_devices_;

  // Ordered by ordinal. Ordinals are never recycled, so unplugging pad one
  // does not renumber pad two.
  std::vector<DeviceInfo> devices_;
  std::vector<InputDriver*> device_owners_;
};

/// Create a default InputSystem with SDL + NOP drivers.
/// In tool mode, only the NOP driver is added.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
