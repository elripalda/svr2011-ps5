/**
 * @file        system/swap_choice.h
 * @brief       Which app-published frame a guest swap shows
 *
 * The app publishes a frame at the guest's Present, before the swap packet that
 * belongs to it reaches the command processor. Showing the newest frame at each
 * swap goes wrong whenever the command processor runs a frame behind: one swap
 * shows the next frame early (skipping its own) and the following swap has
 * nothing newer (repeating it), while presents stay evenly spaced. So each swap
 * shows the frame after the one the previous swap showed.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <optional>

namespace rex::system::external_frame {

struct SwapChoice {
  uint64_t show = 0;      // the sequence to present
  uint64_t skipped = 0;   // published frames no swap will show
  bool repeated = false;  // the previous swap's frame again
};

// last_shown: the sequence the previous swap showed (none before the first swap).
// latest: the newest published sequence. max_lag: how far behind latest a frame may be and still
// be intact (the app may be drawing into older ones). available(s): whether frame s was published.
template <typename Available>
constexpr SwapChoice ChooseSwapFrame(std::optional<uint64_t> last_shown, uint64_t latest,
                                     uint64_t max_lag, Available available) {
  if (!last_shown) {
    return {latest, 0, false};
  }
  if (*last_shown >= latest) {
    return {*last_shown, 0, true};
  }
  uint64_t next = *last_shown + 1;
  const uint64_t oldest_intact = latest > max_lag ? latest - max_lag : 0;
  if (next < oldest_intact) {
    next = oldest_intact;
  }
  while (next < latest && !available(next)) {
    ++next;
  }
  return {next, next - *last_shown - 1, false};
}

constexpr SwapChoice ChooseSwapFrame(std::optional<uint64_t> last_shown, uint64_t latest,
                                     uint64_t max_lag) {
  return ChooseSwapFrame(last_shown, latest, max_lag, [](uint64_t) { return true; });
}

}  // namespace rex::system::external_frame
