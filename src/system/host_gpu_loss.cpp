/**
 * @file        system/host_gpu_loss.cpp
 * @brief       An app-side handler for a lost host GPU device
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/system/host_gpu_loss.h>

#include <atomic>

namespace rex::system::host_gpu_loss {
namespace {
std::atomic<Handler> handler{nullptr};
}  // namespace

void SetHandler(Handler new_handler) { handler.store(new_handler); }

Handler GetHandler() { return handler.load(); }

}  // namespace rex::system::host_gpu_loss
