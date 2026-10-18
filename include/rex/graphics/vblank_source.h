/**
 * @file        graphics/vblank_source.h
 * @brief       Whether the guest vblank follows the host display's vblank
 *
 * The guest vblank normally runs on the host clock at the guest's rate (60 Hz; WWE:
 * 120 Hz in 120 fps matches). On a display whose real refresh is a little off
 * that rate or a multiple of it (59.94, 119.88), the two drift, and every few
 * seconds a frame is held or dropped. Driving the guest vblank from every Nth
 * display refresh removes that; the game then runs at the display's rate (0.1%
 * slow at 59.94 Hz). With variable refresh the display already follows the
 * frames, and other rates cannot show the guest's rate evenly, so those keep the
 * host clock.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cmath>
#include <cstdint>

namespace rex::graphics {

enum class VblankSource { kHostClock, kDisplay };

struct VblankDecision {
  VblankSource source = VblankSource::kHostClock;
  uint32_t display_per_guest = 0;  // display refreshes per guest vblank (kDisplay)
};

// display_hz: the display's measured refresh (0 when unknown). allowed: false in runs that must
// keep the fixed host clock (scene runs). tolerance: how far display_hz / N may be from guest_hz.
inline VblankDecision ChooseVblankSource(double display_hz, bool variable_refresh, bool allowed,
                                         double guest_hz = 60.0, double tolerance = 0.005) {
  if (!allowed || variable_refresh || display_hz <= 0.0) {
    return {};
  }
  const double every = std::round(display_hz / guest_hz);
  if (every < 1.0 || std::abs(display_hz / every - guest_hz) > guest_hz * tolerance) {
    return {};
  }
  return {VblankSource::kDisplay, uint32_t(every)};
}

}  // namespace rex::graphics
