/**
 * @file        system/host_gpu_loss.h
 * @brief       An app-side handler for a lost host GPU device
 *
 * GraphicsSystem calls the handler when the host GPU device is lost, before it
 * ends the game with a fatal error. A handler may end the process itself and
 * not return.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

namespace rex::system::host_gpu_loss {

using Handler = void (*)();

void SetHandler(Handler handler);
// The handler, or null when none is set.
Handler GetHandler();

}  // namespace rex::system::host_gpu_loss
