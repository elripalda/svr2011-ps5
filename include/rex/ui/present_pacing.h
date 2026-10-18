/**
 * @file        ui/present_pacing.h
 * @brief       Pure choices of the paced presenter: present-wait, present offset, swap log rows,
 *              pacing thread priority and the sleep-then-spin wait
 *
 * Free of Vulkan and Windows types so they can be tested without a GPU.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <string>

namespace rex::ui::pacing {

// How the presenter learns that a present has been shown.
enum class PresentWaitMode { kTimer, kWait2, kWait1 };

struct PresentWaitInputs {
  bool enabled = false;        // present_wait cvar
  bool fifo = false;           // the swapchain presents with vsync (FIFO)
  bool device_wait2 = false;   // presentId2 + presentWait2 enabled on the device
  bool surface_wait2 = false;  // the surface supports present id 2 and present wait 2
  bool device_wait1 = false;   // presentId + presentWait enabled (beside v2 where offered)
};

// Per surface: v2 first (AMD has only v2), then v1, then the timer. Only FIFO queues frames, so
// only FIFO waits; scene runs present without vsync and keep timer pacing.
inline PresentWaitMode ChoosePresentWait(const PresentWaitInputs& in) {
  if (!in.enabled || !in.fifo) {
    return PresentWaitMode::kTimer;
  }
  if (in.device_wait2 && in.surface_wait2) {
    return PresentWaitMode::kWait2;
  }
  return in.device_wait1 ? PresentWaitMode::kWait1 : PresentWaitMode::kTimer;
}

inline const char* PresentWaitModeName(PresentWaitMode mode) {
  switch (mode) {
    case PresentWaitMode::kWait2:
      return "present_wait2";
    case PresentWaitMode::kWait1:
      return "present_wait";
    default:
      return "timer";
  }
}

// The present moment after each guest vblank. display_period_us is the display's refresh period
// when the guest vblank is locked to a fixed-refresh display (ADR-0016), else 0: under variable
// refresh the display follows the present, and on the host clock the phase is unknown.
// Locked, a present that lands less than min_margin_us before a refresh can straddle it and show
// one refresh late now and then (2.78 ms of judder at 360 Hz). It then moves to min_margin_us
// before the following refresh, which shows every frame at the same refresh.
inline long long ChoosePresentOffsetUs(long long requested_us, long long display_period_us,
                                       long long min_margin_us) {
  if (display_period_us <= 0 || requested_us <= 0) {
    return requested_us;
  }
  const long long next_refresh =
      (requested_us + display_period_us - 1) / display_period_us * display_period_us;
  if (next_refresh - requested_us >= min_margin_us) {
    return requested_us;
  }
  return next_refresh + display_period_us - min_margin_us;
}

// What the pacing thread presents at a guest vblank's present moment. A fixed-refresh display
// shows the last frame again by itself when a vblank has no new frame. Under variable refresh a
// skipped present holds the panel for two vblanks (33 ms), and the next three frames then show
// 22.2, 12.6 and 15.0 ms instead of 16.7 (ticket 23); presenting the last frame again keeps the
// panel at the guest rate. locked_fixed: the guest vblank follows a fixed-refresh display.
enum class VblankPresent { kNewFrame, kRepeat, kSkip };

inline VblankPresent ChooseVblankPresent(bool new_frame, bool repeat_held, bool locked_fixed,
                                         bool presented_before) {
  if (new_frame) {
    return VblankPresent::kNewFrame;
  }
  return repeat_held && !locked_fixed && presented_before ? VblankPresent::kRepeat
                                                          : VblankPresent::kSkip;
}

// Waits until `deadline`: sleeps until `wake_ahead` before it, then spins on `now` (DXVK's
// util_sleep does the same). A timer wakes late by up to about a millisecond; the spin makes the
// last stretch exact. A sleep that overshoots the deadline does not spin.
template <typename TimePoint, typename Duration, typename Now, typename Sleep, typename Relax>
void SleepThenSpin(TimePoint deadline, Duration wake_ahead, Now&& now, Sleep&& sleep,
                   Relax&& relax) {
  const TimePoint start = now();
  if (deadline - start > wake_ahead) {
    sleep(deadline - start - wake_ahead);
  }
  while (now() < deadline) {
    relax();
  }
}

// Scheduling of the pacing thread. MMCSS "Games" lifts it into the foreground band without the
// risks of TIME_CRITICAL or REALTIME (which starve input threads); where MMCSS cannot be joined,
// THREAD_PRIORITY_HIGHEST is the fallback.
enum class ThreadPriorityChoice { kUnchanged, kMmcssGames, kHighest };

inline ThreadPriorityChoice ChoosePacingThreadPriority(bool enabled, bool mmcss_joined) {
  if (!enabled) {
    return ThreadPriorityChoice::kUnchanged;
  }
  return mmcss_joined ? ThreadPriorityChoice::kMmcssGames : ThreadPriorityChoice::kHighest;
}

inline const char* ThreadPriorityChoiceName(ThreadPriorityChoice choice) {
  switch (choice) {
    case ThreadPriorityChoice::kMmcssGames:
      return "MMCSS Games";
    case ThreadPriorityChoice::kHighest:
      return "highest";
    default:
      return "unchanged";
  }
}

// One row of the swap log (--present_pacing_log). Times are microseconds; present_us and
// shown_us count from the log's start.
struct SwapLogRow {
  long long present_us = 0;
  long long interval_us = 0;  // 0 on the first row
  uint32_t repeated_vblanks = 0;
  // Negative when the frame missed its own vblank's present moment and waited a tick.
  long long arrival_after_vblank_us = 0;
  long long latency_us = 0;  // guest frame arrival to present
  unsigned long long guest_vblank = 0;
  uint32_t frames_replaced = 0;
  unsigned long long external_sequence = 0;
  // Present-wait columns, empty under timer pacing.
  bool present_wait = false;
  unsigned long long present_id = 0;
  long long wait_us = 0;  // time blocked on the previous present before this one
  uint32_t unshown = 0;   // presents not yet shown when this one was about to be presented
  // The newest present shown since the previous row; empty when none was.
  bool has_shown = false;
  unsigned long long shown_id = 0;
  long long shown_us = 0;
  long long shown_after_present_us = 0;
};

inline const char* SwapLogHeader() {
  return "present_us,interval_us,repeated_vblanks,arrival_after_vblank_us,latency_us,"
         "guest_vblank,frames_replaced,external_sequence,"
         "present_id,wait_us,unshown,shown_id,shown_us,shown_after_present_us\n";
}

inline std::string FormatSwapLogRow(const SwapLogRow& r) {
  std::string line = std::to_string(r.present_us) + ',' + std::to_string(r.interval_us) + ',' +
                     std::to_string(r.repeated_vblanks) + ',' +
                     std::to_string(r.arrival_after_vblank_us) + ',' +
                     std::to_string(r.latency_us) + ',' + std::to_string(r.guest_vblank) + ',' +
                     std::to_string(r.frames_replaced) + ',' +
                     std::to_string(r.external_sequence) + ',';
  if (r.present_wait) {
    line += std::to_string(r.present_id) + ',' + std::to_string(r.wait_us) + ',' +
            std::to_string(r.unshown) + ',';
  } else {
    line += ",,,";
  }
  if (r.present_wait && r.has_shown) {
    line += std::to_string(r.shown_id) + ',' + std::to_string(r.shown_us) + ',' +
            std::to_string(r.shown_after_present_us);
  } else {
    line += ",,";
  }
  return line + '\n';
}

}  // namespace rex::ui::pacing
