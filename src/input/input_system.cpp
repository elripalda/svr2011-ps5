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

#include <algorithm>
#include <cmath>

#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
namespace rex::input {

namespace {

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  StopVibration();
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  // The one place the vibration line's backend is set, so every factory names
  // the driver that actually loaded rather than the one it asked for.
  if (const std::string_view name = driver->vibration_backend(); !name.empty()) {
    std::lock_guard lock(vibration_mutex_);
    if (backend_name_ == "none") {
      backend_name_ = name;
    } else {
      backend_name_ += '+';
      backend_name_ += name;
    }
  }
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetGuestInputHold(std::function<bool()> hold) {
  std::lock_guard lock(hold_mutex_);
  guest_input_hold_ = std::move(hold);
}

bool InputSystem::GuestInputHeld() {
  std::function<bool()> hold;
  {
    std::lock_guard lock(hold_mutex_);
    hold = guest_input_hold_;
  }
  // Called unlocked: the hold may read HeldState (a combo that opens a menu).
  return hold && hold();
}

bool InputSystem::HeldState(uint32_t user_index, X_INPUT_STATE* out_state) {
  std::lock_guard lock(hold_mutex_);
  if (user_index >= 4 || !has_real_state_[user_index]) {
    return false;
  }
  *out_state = real_state_[user_index];
  return true;
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    if (user_index < 4) {
      std::lock_guard lock(hold_mutex_);
      has_real_state_[user_index] = false;
    }
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (user_index < 4) {
    std::lock_guard lock(hold_mutex_);
    real_state_[user_index] = merged;
    has_real_state_[user_index] = true;
  }
  if (GuestInputHeld()) {
    merged.gamepad = {};  // the packet number still advances with the real pad
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  return SetState(user_index, vibration, user_index);
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration,
                               uint32_t raw_user_index) {
  SCOPE_profile_cpu_f("hid");
  if ((user_index & 0xFF) == 0xFF) {
    // "Any user" rumbles player 1. XamInputSetState pins it too; repeated here
    // for callers that bypass XAM, like the tests.
    user_index = 0;
  }
  if (!assignment_) {
    RecordVibration(raw_user_index, user_index, *vibration, X_ERROR_DEVICE_NOT_CONNECTED);
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  {
    std::lock_guard drive(vibration_drive_mutex_);
    if (vibration_stopped_) {
      return X_ERROR_DEVICE_NOT_CONNECTED;  // the pads were stopped for good
    }
    X_INPUT_VIBRATION output = *vibration;
    if (vibration_output_) {
      output = vibration_output_(user_index, *vibration);
    }
    X_INPUT_VIBRATION* sent = &output;
    for (DeviceId id : ids) {
      auto* driver = DriverForDevice(id);
      const DeviceInfo* info = DeviceInfoFor(id);
      if (!driver || !info) {
        continue;
      }
      X_RESULT result = driver->SetDeviceVibration(id, sent);
      if (info->synthetic) {
        any_synthetic = true;
        continue;
      }
      any_pad = true;
      if (result == X_ERROR_SUCCESS) {
        pad_rumbled = true;
      } else {
        pad_error = result;
      }
    }
  }
  std::optional<X_RESULT> pads;
  if (any_pad) {
    pads = pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  RecordVibration(raw_user_index, user_index, *vibration, pads);
  if (pads) {
    return *pads;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

void InputSystem::RecordVibration(uint32_t raw_user_index, uint32_t user_index,
                                  const X_INPUT_VIBRATION& vibration,
                                  std::optional<X_RESULT> pads) {
  std::optional<std::string> trace;
  std::optional<std::string> line;
  {
    std::lock_guard lock(vibration_mutex_);
    const auto now = VibrationLog::Clock::now();
    vibration_.Record(raw_user_index, user_index, vibration.left_motor_speed,
                      vibration.right_motor_speed, pads);
    trace = vibration_trace_.Take(raw_user_index, user_index, vibration.left_motor_speed,
                                  vibration.right_motor_speed, pads, now);
    if (vibration_log_.PeriodicDue(now)) {
      line = vibration_log_.TakeIfChanged(vibration_, VibrationLineKind::kPeriodic, backend_name_);
    }
  }
  if (trace) {
    REXLOG_INFO("{}", *trace);
  }
  if (line) {
    REXLOG_INFO("{}", *line);
  }
}

VibrationLedger InputSystem::vibration_ledger() {
  std::lock_guard lock(vibration_mutex_);
  return vibration_;
}

std::string InputSystem::vibration_backend() {
  std::lock_guard lock(vibration_mutex_);
  return backend_name_;
}

std::string InputSystem::VibrationLine(VibrationLineKind kind) {
  std::lock_guard lock(vibration_mutex_);
  return vibration_.Line(kind, backend_name_);
}

void InputSystem::StopVibration() {
  {
    std::lock_guard drive(vibration_drive_mutex_);
    if (vibration_stopped_) {
      return;
    }
    vibration_stopped_ = true;
    std::vector<DeviceInfo> devices;
    for (auto& driver : drivers_) {
      devices.clear();
      driver->EnumerateDevices(devices);
      for (const auto& device : devices) {
        if (device.synthetic) {
          continue;
        }
        X_INPUT_VIBRATION zero = {};
        // A pad that went away answers "not connected"; the others still stop.
        driver->SetDeviceVibration(device.id, &zero);
      }
    }
  }
  REXLOG_INFO("{}", VibrationLine(VibrationLineKind::kFinal));
  // Written out now: the window-X path hard-exits soon after.
  rex::FlushLogging();
}

void InputSystem::set_vibration_output(VibrationOutput output) {
  std::lock_guard drive(vibration_drive_mutex_);
  vibration_output_ = std::move(output);
}

bool InputSystem::LogVibration(VibrationLineKind kind) {
  std::optional<std::string> line;
  {
    std::lock_guard lock(vibration_mutex_);
    line = vibration_log_.TakeIfChanged(vibration_, kind, backend_name_);
  }
  if (!line) {
    return false;
  }
  REXLOG_INFO("{}", *line);
  return true;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (GuestInputHeld()) {
    return X_ERROR_EMPTY;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input
