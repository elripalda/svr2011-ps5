/**
 * @file        system/external_frame.cpp
 * @brief       Hand-off between an app-side native renderer and the GPU plugin
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/external_frame.h>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>

#include <rex/cvar.h>
#include <rex/system/swap_choice.h>

REXCVAR_DEFINE_BOOL(external_frame_in_order, true, "GPU",
                    "Each guest swap shows the app-rendered frame that belongs to it, not the "
                    "newest one (the newest skips and repeats frames when the GPU runs behind)");

namespace rex::system::external_frame {

namespace {
// The app draws into a frame's image again three frames later, so the newest published frame
// and the one before it are intact.
constexpr uint64_t kMaxSwapLag = 1;

std::atomic<bool> g_enabled{false};
std::mutex g_mutex;
Frame g_latest;
// The last few published frames by sequence (slot = sequence % size).
std::array<Frame, 4> g_recent;
std::optional<uint64_t> g_last_shown;
// Sequence + 1 of the frame the last swap showed; 0 before the first swap.
std::atomic<uint64_t> g_last_swapped{0};
}  // namespace

void SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_release); }

bool IsEnabled() { return g_enabled.load(std::memory_order_acquire); }

void Publish(const Frame& frame) {
  std::lock_guard lock(g_mutex);
  g_latest = frame;
  g_recent[frame.sequence % g_recent.size()] = frame;
}

Frame Latest() {
  std::lock_guard lock(g_mutex);
  return g_latest;
}

SwapStep TakeForSwap() {
  SwapStep step;
  std::lock_guard lock(g_mutex);
  if (!g_latest.image_view) {
    return step;
  }
  auto available = [](uint64_t sequence) {
    const Frame& f = g_recent[sequence % g_recent.size()];
    return f.image_view && f.sequence == sequence;
  };
  SwapChoice choice;
  if (REXCVAR_GET(external_frame_in_order)) {
    choice = ChooseSwapFrame(g_last_shown, g_latest.sequence, kMaxSwapLag, available);
  } else {
    // The newest frame at every swap.
    choice.show = g_latest.sequence;
    if (g_last_shown) {
      choice.repeated = choice.show == *g_last_shown;
      choice.skipped = choice.show > *g_last_shown + 1 ? choice.show - *g_last_shown - 1 : 0;
    }
  }
  step.frame = available(choice.show) ? g_recent[choice.show % g_recent.size()] : g_latest;
  step.repeated = choice.repeated;
  step.skipped = choice.skipped;
  g_last_shown = step.frame.sequence;
  g_last_swapped.store(step.frame.sequence + 1, std::memory_order_release);
  return step;
}

uint64_t LastSwappedSequence() {
  const uint64_t last = g_last_swapped.load(std::memory_order_acquire);
  return last ? last - 1 : 0;
}

}  // namespace rex::system::external_frame
