/**
 * @file        system/external_frame.h
 * @brief       Hand-off between an app-side native renderer and the GPU plugin
 *
 * When enabled, the GPU plugin skips its own draws and resolves (it still
 * processes the command stream for fences and interrupts), and presents the
 * latest frame the app published instead of the guest front buffer.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

namespace rex::system::external_frame {

struct Frame {
  // VkImageView on the GPU plugin's device, in SHADER_READ_ONLY_OPTIMAL layout,
  // with every write to it submitted before Publish.
  uint64_t image_view = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t sequence = 0;
};

void SetEnabled(bool enabled);
bool IsEnabled();

void Publish(const Frame& frame);
// The most recently published frame; image_view is 0 before the first one.
Frame Latest();

// What a guest swap showed compared with the swap before it.
struct SwapStep {
  Frame frame;
  bool repeated = false;  // the same frame as the previous swap
  uint64_t skipped = 0;   // published frames that no swap showed
};
// Latest() for a guest swap, remembering its sequence so that repeats and skips
// against the previous swap can be reported.
SwapStep TakeForSwap();
// The sequence the last swap showed (0 also before the first swap).
uint64_t LastSwappedSequence();

}  // namespace rex::system::external_frame
