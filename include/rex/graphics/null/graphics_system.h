/**
 * @file        graphics/null/graphics_system.h
 * @brief       Guest GPU that keeps the command stream moving but draws nothing.
 *
 * Added for the WWE SmackDown vs. Raw 2008 native renderer. The game's own D3D library keeps
 * running unmodified (device creation, ring buffer, fences, interrupts, vblank pacing); this
 * backend consumes what it writes through the shared CommandProcessor (MMIO, fences, events,
 * interrupts, waits) and skips every draw, resolve and swap. The app renders natively by hooking
 * the game's D3D draw calls, and owns the window: there is no provider or presenter here, so
 * ReXApp runs in its detached mode (OnCreateImmediateDrawer).
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::null_gpu {

class NullGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "Null"; }

  // No host GPU and no presenter: the app brings its own renderer.
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::null_gpu
