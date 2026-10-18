/**
 * @file        graphics/null/graphics_system.cpp
 * @brief       Null guest GPU: synchronization only, no rendering (see the header).
 */

#include <rex/graphics/null/graphics_system.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace rex::graphics::null_gpu {

namespace {

// Runs the shared packet processing (register writes, fences, events, interrupts, waits); every
// operation that would need a host GPU is accepted and dropped.
class NullCommandProcessor final : public CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;

  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  Shader* LoadShader(xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override { return true; }
  bool IssueCopy() override { return true; }
};

}  // namespace

X_STATUS NullGraphicsSystem::SetupPresentation(::rex::ui::WindowedAppContext* app_context) {
  app_context_ = app_context;
  REXGPU_INFO("Null GPU: guest command stream for synchronization only; the app renders");
  return X_STATUS_SUCCESS;
}

void NullGraphicsSystem::CreateProvider(bool) {
  // No host graphics provider.
}

std::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::null_gpu
