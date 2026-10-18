/**
 * @file        graphics/display_vblank.h
 * @brief       The host display's real vblank
 *
 * The guest vblank runs on the host clock and does not follow the monitor. This
 * waits on the monitor's own vblank (Windows: the display driver's vblank event
 * for the primary display), so the two can be compared or locked together.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace rex::graphics {

class DisplayVblank {
 public:
  using Clock = std::chrono::steady_clock;

  DisplayVblank() = default;
  DisplayVblank(const DisplayVblank&) = delete;
  DisplayVblank& operator=(const DisplayVblank&) = delete;
  ~DisplayVblank();

  // Opens the primary display; false when the platform or driver offers no vblank event.
  bool Open();
  // Blocks until the display's next vblank and returns when it happened, or nothing on failure.
  std::optional<Clock::time_point> Wait();
  const std::string& name() const { return name_; }
  // The refresh rate of the display's current mode (0 if unknown). Under variable refresh the
  // measured rate follows the frames instead.
  double mode_hz() const { return mode_hz_; }

 private:
  double mode_hz_ = 0.0;
  uint32_t adapter_ = 0;
  uint32_t source_id_ = 0;
  std::string name_;
};

}  // namespace rex::graphics
