/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <chrono>
#include <thread>
#include <rex/ps5_diag.h>
#include <rex/graphics/graphics_system.h>
#include <rex/timeline.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/host_gpu_loss.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "display_vblank.h"
#include <rex/graphics/vblank_source.h>

REXCVAR_DEFINE_BOOL(vblank_follows_display, true, "GPU",
                    "Drive the guest vblank from the display's own refresh when it is (nearly) a "
                    "multiple of the guest vblank's rate and not variable, so no frame is held or "
                    "dropped as the two drift; off keeps the fixed host clock (scene runs)")
    // The display vblank thread always runs and reads it once a second (WWE: live
    // Lock to display).
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(display_vblank_log, false, "GPU",
                    "Log the host display's measured refresh and the guest vblank's phase "
                    "against it once a second");
REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// [svr2011] The guest's vblank rate (its frame clock), Hz; 0 = the video
// mode's (vsync off: 1 kHz).
REXCVAR_DEFINE_DOUBLE(guest_vblank_hz, 0.0, "GPU", "Guest vblank rate in Hz (0: the video mode's)");
REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",
                    "Store shaders persistently and load them when loading games to avoid "
                    "runtime spikes and freezes when playing the game not for the first time.");

namespace {

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  {
    system::X_VIDEO_MODE video_mode;
    kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
    video_mode_vblank_hz_.store(std::max(1.0, double(float(video_mode.refresh_rate))),
                                std::memory_order_release);
  }
  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        double refresh_rate_hz = GuestVblankHz();
        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);
        uint64_t last_frame_time = chrono::Clock::QueryGuestTickCount();
        uint64_t display_ticks_seen = 0;
        // display_every: N when this vblank is every Nth refresh of the display, else 0.
        auto publish_vblank = [this](std::chrono::steady_clock::time_point ideal,
                                     uint32_t display_every = 0) {
          guest_vblank_time_.store(ideal.time_since_epoch().count(), std::memory_order_relaxed);
          guest_vblank_count_.fetch_add(1, std::memory_order_release);
          if (presenter_) {
            presenter_->OnGuestVblank(ideal, display_every);
          }
        };
        while (vsync_worker_running_) {
          // A rate change (SetGuestVblankRate) applies from the next vblank.
          const double want_hz = GuestVblankHz();
          if (want_hz != refresh_rate_hz) {
            refresh_rate_hz = want_hz;
            vsync_interval_ticks =
                std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
          }
          if (display_lock_every_.load(std::memory_order_acquire) && REXCVAR_GET(vsync)) {
            // The display drives the guest vblank. Wait for its signal; if it stops (the
            // display thread lost its event or unlocked), fall through to the host clock.
            std::unique_lock lock(display_tick_mutex_);
            display_tick_cv_.wait_for(lock, std::chrono::milliseconds(50), [&]() {
              return display_ticks_ != display_ticks_seen || !vsync_worker_running_ ||
                     !display_lock_every_.load(std::memory_order_acquire);
            });
            if (display_ticks_ != display_ticks_seen) {
              display_ticks_seen = display_ticks_;
              const auto ideal = std::chrono::steady_clock::time_point(
                  std::chrono::steady_clock::duration(display_tick_time_));
              lock.unlock();
              publish_vblank(ideal, display_lock_every_.load(std::memory_order_acquire));
              MarkVblank();
              last_frame_time = chrono::Clock::QueryGuestTickCount();
              continue;
            }
            lock.unlock();
            last_frame_time = chrono::Clock::QueryGuestTickCount() - vsync_interval_ticks;
          }
          uint64_t interval_ticks =
              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;
          uint64_t deadline = last_frame_time + interval_ticks;
          // Wake on the tick with a high-resolution timer instead of polling with Sleep(1).
          // Do not spin: this thread can share a core with a pinned guest thread.
          int64_t remaining_us = int64_t(deadline - chrono::Clock::QueryGuestTickCount()) *
                                 1000000 / int64_t(guest_tick_frequency);
          if (remaining_us > 0) {
            rex::thread::PreciseSleep(std::chrono::microseconds(remaining_us));
          }
          {
            // Report when the tick was due, not when this thread woke up.
            int64_t late_ticks = int64_t(chrono::Clock::QueryGuestTickCount() - deadline);
            publish_vblank(std::chrono::steady_clock::now() -
                           std::chrono::nanoseconds(std::max<int64_t>(late_ticks, 0) * 1000000000 /
                                                    int64_t(guest_tick_frequency)));
          }
          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          while (current_time - last_frame_time >= interval_ticks) {
            MarkVblank();
            last_frame_time += interval_ticks;
          }
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  // Always started, so Lock to display can be turned on during the session.
  display_vblank_running_ = true;
  display_vblank_thread_ = std::thread([this]() { DisplayVblankThread(); });

  return X_STATUS_SUCCESS;
}

double GraphicsSystem::GuestVblankHz() const {
  const double override_hz = guest_vblank_rate_override_.load(std::memory_order_acquire);
  if (override_hz > 0.0) {
    return override_hz;
  }
  return video_mode_vblank_hz_.load(std::memory_order_acquire);
}

void GraphicsSystem::SetGuestVblankRate(double hz) {
  hz = std::max(0.0, hz);
  if (guest_vblank_rate_override_.exchange(hz, std::memory_order_acq_rel) == hz) {
    return;
  }
  guest_vblank_rate_generation_.fetch_add(1, std::memory_order_acq_rel);
  // A display lock counts refreshes per guest vblank at the old rate: drop it, so the GPU VSync
  // thread runs on the host clock until the display thread locks again at the new rate.
  display_lock_every_.store(0, std::memory_order_release);
  display_tick_cv_.notify_all();
  if (presenter_) {
    presenter_->OnGuestVblankRateChange(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / GuestVblankHz())));
  }
  if (hz > 0.0) {
    REXGPU_INFO("Guest vblank rate: {:.0f} Hz", hz);
  } else {
    REXGPU_INFO("Guest vblank rate: the video mode's");
  }
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (display_vblank_thread_.joinable()) {
    // The thread wakes on the next display vblank and sees the flag.
    display_vblank_running_ = false;
    display_vblank_thread_.join();
  }
  display_lock_every_.store(0, std::memory_order_release);
  display_tick_cv_.notify_all();
  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }
  if (presenter_) {
    presenter_->StopGuestOutputPacing();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::DisplayVblankThread() {
  rex::thread::set_current_thread_name("Display VBlank");
  using Clock = DisplayVblank::Clock;
  const bool log = REXCVAR_GET(display_vblank_log);
  DisplayVblank display;
  if (!display.Open()) {
    REXGPU_WARN("Display vblank: no vblank event for the primary display; the guest vblank "
                "stays on the host clock");
    return;
  }
  REXGPU_INFO("Display vblank thread started on {} (mode {} Hz)", display.name(), display.mode_hz());
  // Variable refresh, once seen, stays assumed: with vsync the panel then refreshes exactly at the
  // frame rate, which looks like a fixed 60 Hz display, and locking to it would feed back.
  bool variable_refresh_seen = false;
  // Lock only after this many one-second windows in a row allow it, so a short calm stretch
  // under variable refresh does not lock.
  constexpr int kWindowsToLock = 3;
  int windows_allowing = 0;
  double period_estimate_ms = 0.0;
  uint64_t refreshes = 0;
  auto stop_following = [this](const char* reason) {
    if (display_lock_every_.exchange(0, std::memory_order_acq_rel)) {
      REXGPU_INFO("Guest vblank back on the host clock: {}", reason);
      display_tick_cv_.notify_all();
    }
  };
  Clock::time_point window_start{}, last{};
  // Gaps between the vblank events this thread saw; it can miss one, so a gap may span several
  // refreshes.
  std::vector<double> gaps_ms;
  uint64_t guest_count_start = 0;
  int64_t guest_time_start = 0;
  double last_phase_ms = -1.0;
  while (display_vblank_running_) {
    std::optional<Clock::time_point> vblank = display.Wait();
    if (!vblank) {
      REXGPU_WARN("Display vblank: waiting for the vblank failed; stopping");
      stop_following("the display's vblank event failed");
      return;
    }
    if (last != Clock::time_point{} && period_estimate_ms > 0.0) {
      // Count refreshes, not events: this thread can miss one.
      const double gap_ms = std::chrono::duration<double, std::milli>(*vblank - last).count();
      const uint64_t passed = std::max<uint64_t>(1, uint64_t(gap_ms / period_estimate_ms + 0.5));
      const uint32_t every = display_lock_every_.load(std::memory_order_acquire);
      if (every && (refreshes + passed) / every != refreshes / every) {
        {
          std::lock_guard lock(display_tick_mutex_);
          ++display_ticks_;
          display_tick_time_ = vblank->time_since_epoch().count();
        }
        display_tick_cv_.notify_one();
      }
      refreshes += passed;
    }
    if (window_start == Clock::time_point{}) {
      window_start = *vblank;
      guest_count_start = guest_vblank_count_.load(std::memory_order_acquire);
      guest_time_start = guest_vblank_time_.load(std::memory_order_relaxed);
      gaps_ms.clear();
      last = *vblank;
      continue;
    }
    gaps_ms.push_back(std::chrono::duration<double, std::milli>(*vblank - last).count());
    last = *vblank;
    if (last - window_start < std::chrono::seconds(1)) {
      continue;
    }
    // Rates over the window from first to last event; the phase is where the latest guest
    // vblank falls inside the display's refresh period.
    std::vector<double> sorted = gaps_ms;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median_gap_ms = sorted[sorted.size() / 2];
    // A gap well short of two refreshes is one refresh the display held longer, which only
    // variable refresh (FreeSync, G-Sync) does; a gap of about N refreshes means this thread missed
    // N - 1 events.
    uint64_t stretched = 0, missed = 0;
    for (double gap : gaps_ms) {
      double ratio = gap / median_gap_ms;
      if (ratio >= 1.8) {
        missed += uint64_t(ratio + 0.5) - 1;
      } else if (ratio >= 1.2) {
        ++stretched;
      }
    }
    const double display_period_ms = median_gap_ms;
    uint64_t guest_count = guest_vblank_count_.load(std::memory_order_acquire);
    int64_t guest_time = guest_vblank_time_.load(std::memory_order_relaxed);
    double guest_hz = 0.0;
    if (guest_count > guest_count_start && guest_time > guest_time_start) {
      guest_hz = double(guest_count - guest_count_start) /
                 std::chrono::duration<double>(Clock::duration(guest_time - guest_time_start)).count();
    }
    double offset_ms = std::chrono::duration<double, std::milli>(
                           Clock::time_point(Clock::duration(guest_time)) - last)
                           .count();
    double phase_ms = std::fmod(offset_ms, display_period_ms);
    if (phase_ms < 0) {
      phase_ms += display_period_ms;
    }
    double drift_ms = 0.0;
    if (last_phase_ms >= 0) {
      drift_ms = phase_ms - last_phase_ms;
      if (drift_ms > display_period_ms / 2) drift_ms -= display_period_ms;
      if (drift_ms < -display_period_ms / 2) drift_ms += display_period_ms;
    }
    last_phase_ms = phase_ms;
    period_estimate_ms = display_period_ms;
    // Two held refreshes in a second is variable refresh at work, not noise; so is a measured
    // rate away from the display mode's rate (the panel follows the frames).
    const double measured_hz = 1000.0 / display_period_ms;
    const bool off_mode = display.mode_hz() > 0.0 &&
                          std::abs(measured_hz - display.mode_hz()) > display.mode_hz() * 0.02;
    if ((stretched >= 2 || off_mode) && !variable_refresh_seen) {
      variable_refresh_seen = true;
      REXGPU_INFO("Display vblank: variable refresh is active ({:.3f} Hz measured, mode {} Hz, {} "
                  "refreshes held longer); the guest vblank stays on the host clock",
                  measured_hz, display.mode_hz(), stretched);
    }
    // Read each second: the setting can change during the session.
    const bool follow = REXCVAR_GET(vblank_follows_display);
    // The guest vblank's own rate, so a 120 Hz guest locks to every 3rd refresh of a 360 Hz
    // display, not every 6th.
    const uint64_t rate_generation =
        guest_vblank_rate_generation_.load(std::memory_order_acquire);
    const VblankDecision decision =
        ChooseVblankSource(measured_hz, variable_refresh_seen, follow, GuestVblankHz());
    if (decision.source == VblankSource::kDisplay) {
      if (++windows_allowing >= kWindowsToLock &&
          display_lock_every_.load(std::memory_order_acquire) != decision.display_per_guest) {
        display_lock_every_.store(decision.display_per_guest, std::memory_order_release);
        if (guest_vblank_rate_generation_.load(std::memory_order_acquire) != rate_generation) {
          // The rate changed after it was read: the ratio belongs to the old one.
          display_lock_every_.store(0, std::memory_order_release);
          windows_allowing = 0;
        } else {
          REXGPU_INFO("Guest vblank now follows the display: every {} refreshes of {:.3f} Hz",
                      decision.display_per_guest, 1000.0 / display_period_ms);
        }
      }
    } else {
      windows_allowing = 0;
      stop_following(!follow                ? "Lock to display is off"
                     : variable_refresh_seen ? "variable refresh is active"
                                             : "the display's refresh is not a multiple of the guest's");
    }
    if (log) {
      REXGPU_INFO(
          "Display vblank 1s: display {:.3f} Hz, {} refreshes held longer (variable refresh), "
          "{} events missed; guest {:.3f} Hz, guest phase {:.3f} ms of {:.3f} ms, drift {:+.3f} "
          "ms/s; guest vblank from {}",
          1000.0 / display_period_ms, stretched, missed, guest_hz, phase_ms, display_period_ms,
          drift_ms, display_lock_every_.load(std::memory_order_acquire) ? "the display" : "the host clock");
    }
    window_start = last;
    gaps_ms.clear();
    guest_count_start = guest_count;
    guest_time_start = guest_time;
  }
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  // [svr2011] Let the GPU thread log the device removal diagnostics (DRED
  // page fault address, breadcrumbs) first - the UI thread usually sees the
  // loss first, and FatalError ends the process.
  REXLOG_ERROR("Graphics device lost - waiting for diagnostics");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  rex::FlushLogging();
  if (system::host_gpu_loss::Handler handler = system::host_gpu_loss::GetHandler()) {
    handler();
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }
  rex::ps5diag::Add(rex::ps5diag::kInterrupts);

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  REX_TIMELINE(kInterruptBegin, source, cpu);
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
  REX_TIMELINE(kInterruptEnd, source, cpu);
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  rex::ps5diag::Add(rex::ps5diag::kVblanks);
  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!REXCVAR_GET(store_shaders)) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
