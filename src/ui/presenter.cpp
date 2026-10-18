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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/external_frame.h>
#include <rex/thread.h>
#include <rex/ui/present_pacing.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#endif

REXCVAR_DEFINE_BOOL(host_present_from_non_ui_thread, true, "UI/Presenter",
                    "Allow presentation from non-UI thread");

REXCVAR_DEFINE_BOOL(present_letterbox, true, "UI/Presenter",
                    "Enable letterboxing for non-native aspect ratios");

REXCVAR_DEFINE_INT32(present_safe_area_x, 90, "UI/Presenter",
                     "Horizontal safe area percentage (0-100)")
    .range(0, 100);

REXCVAR_DEFINE_INT32(present_safe_area_y, 90, "UI/Presenter",
                     "Vertical safe area percentage (0-100)")
    .range(0, 100);

#if defined(REX_HAS_FIDELITYFX_SDK)
REXCVAR_DEFINE_STRING(present_effect, "bilinear", "UI/Presenter",
                      "Guest output effect: bilinear, cas, fsr, fsr2, fsr3")
    .allowed({"bilinear", "cas", "fsr", "fsr2", "fsr3"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(present_cas_additional_sharpness,
                      rex::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault,
                      "UI/Presenter", "Additional CAS sharpness in [0, 1]")
    .range(rex::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMin,
           rex::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessMax)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(present_fsr_max_upsampling_passes,
                     rex::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax,
                     "UI/Presenter", "Maximum chained FSR EASU passes")
    .range(1, int32_t(rex::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax))
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(present_fsr_sharpness_reduction,
                      rex::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault,
                      "UI/Presenter", "FSR RCAS sharpness reduction in stops")
    .range(rex::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMin,
           rex::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionMax)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(
    present_fsr_quality_mode, "auto", "UI/Presenter",
    "Temporal FSR quality mode: auto, nativeaa, quality, balanced, performance, ultra_performance")
    .allowed({"auto", "nativeaa", "quality", "balanced", "performance", "ultra_performance"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#else
REXCVAR_DEFINE_STRING(present_effect, "bilinear", "UI/Presenter", "Guest output effect: bilinear")
    .allowed({"bilinear"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif

REXCVAR_DEFINE_BOOL(present_dither, false, "UI/Presenter",
                    "Enable output dithering in the final present pass")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(present_allow_overscan_cutoff, false, "UI/Presenter",
                    "Allow overscan cutoff based on safe area settings")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(present_pace_to_guest_vblank, true, "UI/Presenter",
                    "Present each guest frame at the next guest vblank, as the console does, "
                    "instead of as soon as it is submitted")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(present_pacing_stats, false, "UI/Presenter",
                    "Log paced presentation intervals, repeated vblanks and replaced frames "
                    "every 5 seconds");
REXCVAR_DEFINE_STRING(present_pacing_log, "", "UI/Presenter",
                      "CSV file receiving one row per paced presentation (empty = off)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// SvR 2011 submits frames 1.5 ms (median) and 3.7 ms (p99.9) after the vblank; 5 ms catches
// all but a handful while cutting ~12 ms of latency compared with the next vblank.
REXCVAR_DEFINE_INT32(present_pacing_offset_us, 5000, "UI/Presenter",
                     "Present each paced frame this many microseconds after the guest vblank it "
                     "follows; 0 = on the next vblank. Lower values cut latency, but frames that "
                     "finish later than the offset wait a whole extra vblank")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// At a fixed 360 Hz, vblank + 5 ms is only 0.56 ms before a refresh; see ChoosePresentOffsetUs.
REXCVAR_DEFINE_INT32(present_pacing_min_margin_us, 1000, "UI/Presenter",
                     "When the guest vblank follows a fixed-refresh display, the least time a "
                     "paced present keeps before the display's next refresh; closer presents move "
                     "to this long before the refresh after it. 0 = never move")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(present_wait, true, "UI/Presenter",
                    "With vsync, use present-wait (VK_KHR_present_wait2, then VK_KHR_present_wait) "
                    "to keep one frame queued and log when each present was shown; off or "
                    "unavailable keeps timer pacing only")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(present_pacing_thread_priority, true, "UI/Presenter",
                    "Run the pacing thread in the MMCSS Games task (else at highest priority) and "
                    "ask Windows for full-speed timers")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(present_repeat_held_vblanks, true, "UI/Presenter",
                    "When a guest vblank brings no new frame and the guest vblank does not follow "
                    "a fixed-refresh display (variable refresh, host clock), present the last "
                    "frame again so the display keeps the guest rate; off skips that present")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
using GuestOutputPaintConfig = rex::ui::Presenter::GuestOutputPaintConfig;

GuestOutputPaintConfig::Effect ParsePresentEffect(const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (lowered == "cas") {
    return GuestOutputPaintConfig::Effect::kCas;
  }
  if (lowered == "fsr") {
    return GuestOutputPaintConfig::Effect::kFsr;
  }
  if (lowered == "fsr2") {
    return GuestOutputPaintConfig::Effect::kFsr2;
  }
  if (lowered == "fsr3") {
    return GuestOutputPaintConfig::Effect::kFsr3;
  }
#endif
  return GuestOutputPaintConfig::Effect::kBilinear;
}

#if defined(REX_HAS_FIDELITYFX_SDK)
bool IsTemporalFsrCompatibilityEffect(GuestOutputPaintConfig::Effect effect) {
  return effect == GuestOutputPaintConfig::Effect::kFsr2 ||
         effect == GuestOutputPaintConfig::Effect::kFsr3;
}

GuestOutputPaintConfig::FsrQualityMode ParsePresentFsrQualityMode(const std::string& mode_name) {
  std::string lowered = mode_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "nativeaa" || lowered == "native_aa" || lowered == "native") {
    return GuestOutputPaintConfig::FsrQualityMode::kNativeAa;
  }
  if (lowered == "quality") {
    return GuestOutputPaintConfig::FsrQualityMode::kQuality;
  }
  if (lowered == "balanced") {
    return GuestOutputPaintConfig::FsrQualityMode::kBalanced;
  }
  if (lowered == "performance") {
    return GuestOutputPaintConfig::FsrQualityMode::kPerformance;
  }
  if (lowered == "ultra_performance" || lowered == "ultra") {
    return GuestOutputPaintConfig::FsrQualityMode::kUltraPerformance;
  }
  return GuestOutputPaintConfig::FsrQualityMode::kAuto;
}

bool GetFsrQualityModeApiValue(GuestOutputPaintConfig::FsrQualityMode mode,
                               uint32_t& api_mode_out) {
  switch (mode) {
    case GuestOutputPaintConfig::FsrQualityMode::kAuto:
      return false;
    case GuestOutputPaintConfig::FsrQualityMode::kNativeAa:
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
      api_mode_out = FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
#else
      api_mode_out = 0;
#endif
      return true;
    case GuestOutputPaintConfig::FsrQualityMode::kQuality:
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
      api_mode_out = FFX_UPSCALE_QUALITY_MODE_QUALITY;
#else
      api_mode_out = 1;
#endif
      return true;
    case GuestOutputPaintConfig::FsrQualityMode::kBalanced:
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
      api_mode_out = FFX_UPSCALE_QUALITY_MODE_BALANCED;
#else
      api_mode_out = 2;
#endif
      return true;
    case GuestOutputPaintConfig::FsrQualityMode::kPerformance:
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
      api_mode_out = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
#else
      api_mode_out = 3;
#endif
      return true;
    case GuestOutputPaintConfig::FsrQualityMode::kUltraPerformance:
#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
      api_mode_out = FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
#else
      api_mode_out = 4;
#endif
      return true;
  }
  return false;
}

float GetFsrQualityModeRatioFallback(GuestOutputPaintConfig::FsrQualityMode mode) {
  switch (mode) {
    case GuestOutputPaintConfig::FsrQualityMode::kNativeAa:
      return 1.0f;
    case GuestOutputPaintConfig::FsrQualityMode::kQuality:
      return 1.5f;
    case GuestOutputPaintConfig::FsrQualityMode::kBalanced:
      return 1.7f;
    case GuestOutputPaintConfig::FsrQualityMode::kPerformance:
      return 2.0f;
    case GuestOutputPaintConfig::FsrQualityMode::kUltraPerformance:
      return 3.0f;
    case GuestOutputPaintConfig::FsrQualityMode::kAuto:
    default:
      return 1.0f;
  }
}

bool QueryTemporalFsrRenderResolutionFromQualityMode(uint32_t display_width,
                                                     uint32_t display_height,
                                                     GuestOutputPaintConfig::FsrQualityMode mode,
                                                     uint32_t& render_width_out,
                                                     uint32_t& render_height_out) {
  uint32_t api_mode = 0;
  if (!GetFsrQualityModeApiValue(mode, api_mode)) {
    return false;
  }

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query_desc = {};
  query_desc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
  query_desc.header.pNext = nullptr;
  query_desc.displayWidth = display_width;
  query_desc.displayHeight = display_height;
  query_desc.qualityMode = api_mode;
  query_desc.pOutRenderWidth = &render_width_out;
  query_desc.pOutRenderHeight = &render_height_out;
  if (ffxQuery(nullptr, &query_desc.header) == FFX_API_RETURN_OK && render_width_out &&
      render_height_out) {
    return true;
  }
#endif

  float ratio = GetFsrQualityModeRatioFallback(mode);
  render_width_out = std::max(uint32_t(1), uint32_t(float(display_width) / ratio + 0.5f));
  render_height_out = std::max(uint32_t(1), uint32_t(float(display_height) / ratio + 0.5f));
  return true;
}

void LogTemporalFsrCompatibilityPathOnce() {
  static std::atomic<bool> temporal_fsr_compatibility_logged = false;
  if (!temporal_fsr_compatibility_logged.exchange(true)) {
    REXLOG_WARN(
        "present_effect=fsr2/fsr3 uses an experimental temporal upscaler path "
        "with synthesized depth/motion inputs and may fall back to spatial FSR");
  }
}

void LogTemporalFsrQualityModeInputLimitOnce() {
  static std::atomic<bool> temporal_fsr_quality_mode_input_limit_logged = false;
  if (!temporal_fsr_quality_mode_input_limit_logged.exchange(true)) {
    REXLOG_WARN(
        "present_fsr_quality_mode requested a render size larger than the "
        "guest output; using guest output size");
  }
}
#endif  // defined(REX_HAS_FIDELITYFX_SDK)

GuestOutputPaintConfig BuildGuestOutputPaintConfigFromCVar() {
  GuestOutputPaintConfig config;
  GuestOutputPaintConfig::Effect parsed_effect = ParsePresentEffect(REXCVAR_GET(present_effect));
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (IsTemporalFsrCompatibilityEffect(parsed_effect)) {
    LogTemporalFsrCompatibilityPathOnce();
  }
#endif
  config.SetAllowOverscanCutoff(REXCVAR_GET(present_allow_overscan_cutoff));
  config.SetEffect(parsed_effect);
#if defined(REX_HAS_FIDELITYFX_SDK)
  config.SetCasAdditionalSharpness(float(REXCVAR_GET(present_cas_additional_sharpness)));
  config.SetFsrMaxUpsamplingPasses(
      uint32_t(std::max(int32_t(1), REXCVAR_GET(present_fsr_max_upsampling_passes))));
  config.SetFsrSharpnessReduction(float(REXCVAR_GET(present_fsr_sharpness_reduction)));
  config.SetFsrQualityMode(ParsePresentFsrQualityMode(REXCVAR_GET(present_fsr_quality_mode)));
#endif
  config.SetDither(REXCVAR_GET(present_dither));
  return config;
}

}  // namespace

namespace rex {
namespace ui {

void Presenter::FatalErrorHostGpuLossCallback([[maybe_unused]] bool is_responsible,
                                              [[maybe_unused]] bool statically_from_ui_thread) {
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

Presenter::~Presenter() {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);
  StopGuestOutputPacing();

#if REX_PLATFORM_WIN32
  if (dxgi_ui_tick_thread_.joinable()) {
    {
      std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
      dxgi_ui_tick_thread_shutdown_ = true;
    }
    dxgi_ui_tick_control_condition_.notify_all();
    dxgi_ui_tick_thread_.join();
  }
#endif  // XE_PLATFORM

  if (window_) {
    Window* old_window = window_;
    // Null the pointer to prevent an infinite loop between SetPresenter and
    // SetWindowSurfaceFromUIThread calling each other.
    window_ = nullptr;
    old_window->SetPresenter(nullptr);
  }
}

void Presenter::SetWindowSurfaceFromUIThread(Window* new_window, Surface* new_surface) {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);

  // There can't be a valid surface pointer without a window, as a surface is
  // created and owned by the window.
  assert_false(new_surface && !new_window);

  if (window_ == new_window && (!window_ || surface_ == new_surface)) {
    // Nothing has changed (or a recursive SetWindowSurfaceFromUIThread >
    // SetPresenter > SetWindowSurfaceFromUIThread call).
    return;
  }

  // Disconnect from the current surface.
  if (surface_) {
    // Take ownership of painting, and also stop accepting paint requests from
    // the guest output thread - the window (which is required for making them)
    // may be going away, and there will be a forced paint when the connection
    // becomes available.
    SetPaintModeFromUIThread(PaintMode::kNone);
    DisconnectPaintingFromSurfaceFromUIThread(
        SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    surface_ = nullptr;
    UpdateSurfaceMonitorFromUIThread(true);
  }

  if (window_ != new_window) {
    // The window pointer may be accessed by the guest output thread if painting
    // is possible (or was possible, but the paint attempt has resulted in the
    // implementation reporting that the connection has become outdated).
    // However, a painting connection is currently not established at all, so
    // it's safe to modify the window pointer here.

    // Detach from the old window if attaching to a different one or just
    // detaching. SetPresenter for the new window might have been called without
    // it having been called with nullptr for the old window.
    if (window_) {
      Window* old_window = window_;
      // Null the pointer to prevent an infinite loop between SetPresenter and
      // SetWindowSurfaceFromUIThread calling each other.
      window_ = nullptr;
      old_window->SetPresenter(nullptr);
    }

    // Attach to the new one.
    // This function is called from SetPresenter - don't need to notify the
    // window of this, as it itself has triggered this.
    window_ = new_window;
  }

  if (new_surface) {
    assert_true(paint_mode_ == PaintMode::kNone);
    surface_ = new_surface;
    UpdateSurfaceMonitorFromUIThread(true);
    assert_true(surface_paint_connection_state_ ==
                SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    bool request_repaint;
    UpdateSurfacePaintConnectionFromUIThread(&request_repaint, true);
    // Request to paint as soon as possible in the UI thread if connected
    // successfully.
    if (request_repaint) {
      RequestPaintOrConnectionRecoveryViaWindow(true);
    }
  }
}

void Presenter::OnSurfaceMonitorUpdateFromUIThread(bool old_monitor_potentially_disconnected) {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);

  if (!surface_) {
    return;
  }

  UpdateSurfaceMonitorFromUIThread(old_monitor_potentially_disconnected);
}

void Presenter::OnSurfaceResizeFromUIThread() {
  // No intrusive lifetime management must be performed from UI drawers - defer
  // it if needed.
  assert_false(is_executing_ui_drawers_);

  if (!surface_) {
    return;
  }

  // Let the UI thread take ownership of painting (so the connection can be
  // updated) in a smooth way - downgrade to kUIThreadOnRequest rather than
  // kNone, because a forced repaint may not be necessary if, for example, the
  // size internally turns out to be the same after the update, and in this case
  // the current image may be kept - but the new one must not be missed either
  // if it becomes available during the resize.
  if (paint_mode_ == PaintMode::kGuestOutputThreadImmediately) {
    SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);
  }

  bool request_repaint;
  UpdateSurfacePaintConnectionFromUIThread(&request_repaint, true);

  // Request to repaint as soon as possible in the UI thread if needed.
  if (request_repaint) {
    RequestPaintOrConnectionRecoveryViaWindow(true);
  }
}

void Presenter::PaintFromUIThread(bool force_paint) {
  // If there is no surface, this will be a no-op, nothing outdated, nothing to
  // paint. However, an explicit monitor check is needed because UI framerate
  // limiting may be tied to signals from the OS for the monitor - but painting
  // may still occur, for instance, if drawing to a composition surface in the
  // OS (which will still be live even if the window goes outside any monitor).
  // But a surface check still won't cause harm, for simplicity.
  if (!InSurfaceOnMonitorFromUIThread()) {
    return;
  }

  // Defer changes to the paint mode as well as window paint requests, and do
  // them in this function so they're consistent with the assumptions made here.
  assert_false(is_in_ui_thread_paint_);
  is_in_ui_thread_paint_ = true;
  request_guest_output_paint_after_current_ui_thread_paint_ = false;
  request_ui_paint_after_current_ui_thread_paint_ = false;

  // Actualize the connection if the UI needs to be drawn if there was some
  // explicit paint request (the guest output has been refreshed, and the guest
  // output thread was asked not to present directly due as the UI needs to be
  // drawn, or some surface state change has happened so the guest output needs
  // to be displayed as soon as possible without waiting for the guest to
  // refresh it, or the guest output thread has been notified that the
  // connection has become outdated and has requested the UI thread to
  // reconnect).
  bool draw_ui = !ui_drawers_.empty();
  bool do_paint = force_paint || draw_ui;
  // Reset ui_thread_paint_requested_ unconditionally also, regardless of
  // whether the UI needs to be drawn - the flag may be set to try reconnecting,
  // for example.
  bool paint_requested = ui_thread_paint_requested_.exchange(false, std::memory_order_relaxed);
  if (paint_requested) {
    do_paint = true;
  }
  if (do_paint) {
    ui_paints_[force_paint ? 0 : draw_ui ? 1 : 2].fetch_add(1, std::memory_order_relaxed);
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
      ui_paints_[3].fetch_add(1, std::memory_order_relaxed);
    }
  }
  PaintResult paint_result = PaintResult::kNotPresented;
  bool request_repaint_at_tick = false;
  bool request_repaint_immediately = false;
  if (do_paint) {
    // Take ownership of painting if it's currently owned by the guest output
    // thread (downgrade from kGuestOutputThreadImmediately to
    // kUIThreadOnRequest - not to kNone so if during this paint a new guest
    // output frame is generated, the notification will still be sent to the UI
    // thread rather than dropped, so the frame won't be skipped). This is
    // needed to be able not only to paint, but also to try to recover from an
    // outdated surface.
    if (paint_mode_ == PaintMode::kGuestOutputThreadImmediately) {
      SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);
    }
    // Try to recover from the connection becoming outdated in the previous
    // paint.
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
      UpdateSurfacePaintConnectionFromUIThread(nullptr, false);
    }
    // If still paintable or recovered successfully, paint.
    if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable) {
      // The paint mode might have been set to kNone when the connection was
      // marked as outdated last time. Or, if wasn't reconnecting, there was
      // some other incorrect situation that caused the paint mode to be set to
      // kNone for an active connection. Make sure that the current paint mode
      // is consistent with painting from the UI thread.
      SetPaintModeFromUIThread(PaintMode::kUIThreadOnRequest);

      // Limit the frame rate of the UI, usually to the monitor refresh rate,
      // in a way so that the UI won't be stealing all the remaining GPU
      // resources if it's repainted continuously, and the window system itself
      // doesn't limit the frame rate.
      WaitForUITickFromUIThread();

      paint_result = PaintAndPresent(draw_ui);
      if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
        // Request another PaintFromUIThread which will try to recover from the
        // outdated connection in the next frame (not immediately, so the
        // windowing system has some time to prepare what may be required to
        // recover from it, such as to send a resize event).
        request_repaint_immediately = true;
      }
    }
    // If can't paint anymore, notify the paint mode refresh below (which is not
    // guaranteed to have access to have ownership of painting as it's taken
    // here only conditionally, thus can't know whether the connection is
    // actually in a paintable state).
    if (surface_paint_connection_state_ != SurfacePaintConnectionState::kConnectedPaintable) {
      SetPaintModeFromUIThread(PaintMode::kNone);
    }
  }

  // Transfer the ownership of painting back to the guest output thread if it
  // was taken or if needed for any reason (however, it's taken conditionally -
  // no guarantees that the actual connection state is accessible here, so only
  // checking whether the mode is not kNone currently, not the connection
  // state), and overall synchronize the state taking into account both what has
  // been done in this function and what could have been done by the UI drawer
  // callbacks.
  if (paint_mode_ != PaintMode::kNone) {
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(true));
  }
  is_in_ui_thread_paint_ = false;

  // Check if the device has been lost. There's no point in requesting repaint
  // if it has happened anyway, it won't be possible to satisfy such request
  // with the current Presenter.
  if (paint_result == PaintResult::kGpuLostExternally ||
      paint_result == PaintResult::kGpuLostResponsible) {
    if (host_gpu_loss_callback_) {
      host_gpu_loss_callback_(paint_result == PaintResult::kGpuLostResponsible, true);
    }
    // The loss callback might have destroyed the presenter, must not do
    // anything with `this` anymore.
    return;
  }

  // Request refresh if needed.
  // Can't check the exact paintability as the connection state may currently
  // be owned by the guest output thread, so check conservatively via
  // paint_mode_.
  if (paint_mode_ != PaintMode::kNone) {
    // Immediately paint the guest output if requested explicitly or if the UI
    // has hidden itself.
    if (request_guest_output_paint_after_current_ui_thread_paint_ ||
        (draw_ui && ui_drawers_.empty())) {
      request_repaint_immediately = true;
    }
    if (request_ui_paint_after_current_ui_thread_paint_ && !ui_drawers_.empty()) {
      request_repaint_at_tick = true;
    }
  }
  if (request_repaint_at_tick || request_repaint_immediately) {
    RequestPaintOrConnectionRecoveryViaWindow(request_repaint_immediately);
  }
}

bool Presenter::RefreshGuestOutput(
    uint32_t frontbuffer_width, uint32_t frontbuffer_height, uint32_t display_aspect_ratio_x,
    uint32_t display_aspect_ratio_y,
    std::function<bool(GuestOutputRefreshContext& context)> refresher) {
  GuestOutputProperties& writable_properties =
      guest_output_properties_[guest_output_mailbox_writable_];
  writable_properties.frontbuffer_width = frontbuffer_width;
  writable_properties.frontbuffer_height = frontbuffer_height;
  writable_properties.display_aspect_ratio_x = display_aspect_ratio_x;
  writable_properties.display_aspect_ratio_y = display_aspect_ratio_y;
  writable_properties.is_8bpc = false;
  bool is_active = writable_properties.IsActive();
  if (is_active) {
    if (!RefreshGuestOutputImpl(guest_output_mailbox_writable_, frontbuffer_width,
                                frontbuffer_height, refresher, writable_properties.is_8bpc)) {
      // If failed to refresh, don't send the currently writable image to the
      // mailbox as it may be in an undefined state. Don't disable the guest
      // output either though because the failure may be something transient.
      return false;
    }
    guest_output_active_last_refresh_ = true;
  } else {
    // Request presenting a blank image if there was a true image previously,
    // but not now.
    if (!guest_output_active_last_refresh_) {
      return false;
    }
    guest_output_active_last_refresh_ = false;
  }

  // Make the new image the next to present on the host (the "ready" one),
  // replacing the one already specified as the next (dropping it instead of
  // enqueueing the new image after it) to achieve the lowest latency (also,
  // after switching from UI thread painting to doing it in the guest output
  // thread, will immediately recover to having the latest frame always sent to
  // the host present call on the CPU and all frames reaching a present call).
  uint32_t last_acquired_and_ready =
      guest_output_mailbox_acquired_and_ready_.load(std::memory_order_relaxed);
  // Desired acquired = current acquired (changed only by the consumers).
  // Desired ready = current writable.
  // memory_order_acq_rel to acquire the new writable image and to release the
  // current one (to let the consumers take it, from ready to acquired).
  while (!guest_output_mailbox_acquired_and_ready_.compare_exchange_weak(
      last_acquired_and_ready,
      (last_acquired_and_ready & 3) | (guest_output_mailbox_writable_ << 2),
      std::memory_order_acq_rel, std::memory_order_relaxed)) {}
  // Now, it's known that `ready == writable` on the host presentation side.
  // Take the next `writable` with this assumption about its current value in
  // mind.
  uint32_t last_acquired = last_acquired_and_ready & 3;
  if (last_acquired == guest_output_mailbox_writable_) {
    // The new image has already been acquired by the time the compare-exchange
    // loop has finished (acquired == ready == currently writable).
    // It's a valid situation from the ownership perspective, and the semantics
    // of the weak compare-exchange explicitly permit spurious `false` results.
    // (3 - a - b) % 3 cannot be used here, as (3 - a - a) % 3 results in `a` -
    // the same index.
    // Take any free image. Preferably using + 1, not ^ 1, so if the guest needs
    // to await any GPU work referencing the image, it will wait for the frame 3
    // frames ago, not 2, if this happens repeatedly.
    guest_output_mailbox_writable_ = (guest_output_mailbox_writable_ + 1) % 3;
  } else {
    // Take the image other than the last acquired one and the new one,
    // currently not accessible to the host presentation.
    guest_output_mailbox_writable_ = (3 - last_acquired - guest_output_mailbox_writable_) % 3;
  }

  if (REXCVAR_GET(present_pace_to_guest_vblank)) {
    {
      std::lock_guard<std::mutex> lock(guest_output_pacing_mutex_);
      if (!guest_output_pacing_thread_.joinable() && !guest_output_pacing_shutdown_) {
        guest_output_pacing_thread_ = std::thread([this]() { GuestOutputPacingThread(); });
      }
    }
    guest_output_paced_frame_arrival_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    if (guest_output_paced_frame_pending_.exchange(true, std::memory_order_acq_rel)) {
      guest_output_paced_frames_replaced_.fetch_add(1, std::memory_order_relaxed);
      guest_output_paced_frames_replaced_since_present_.fetch_add(1, std::memory_order_relaxed);
    }
    return is_active;
  }
  PresentLatestGuestOutput();
  return is_active;
}

void Presenter::OnGuestVblankRateChange(std::chrono::steady_clock::duration period) {
  std::lock_guard<std::mutex> lock(guest_output_pacing_mutex_);
  guest_output_pacing_vblank_period_ = period;
  guest_output_pacing_period_rejects_ = 0;
  guest_output_pacing_skip_period_ = true;
}

void Presenter::OnGuestVblank(std::chrono::steady_clock::time_point vblank_time,
                              uint32_t display_refreshes_per_vblank) {
  {
    std::lock_guard<std::mutex> lock(guest_output_pacing_mutex_);
    if (!guest_output_pacing_thread_.joinable()) {
      return;
    }
    guest_output_pacing_display_every_ = display_refreshes_per_vblank;
    if (guest_output_pacing_skip_period_) {
      guest_output_pacing_skip_period_ = false;
      ++guest_output_pacing_vblank_count_;
    } else if (guest_output_pacing_vblank_count_++) {
      auto period = vblank_time - guest_output_pacing_vblank_time_;
      auto& current = guest_output_pacing_vblank_period_;
      // Ignore single gaps from missed ticks; a real rate change moves by less than half a
      // period, or keeps disagreeing (a wrong estimate must not stick).
      constexpr uint32_t kRejectsToAdopt = 8;
      if (current <= decltype(period)::zero() || (period > current / 2 && period < current * 3 / 2) ||
          ++guest_output_pacing_period_rejects_ >= kRejectsToAdopt) {
        current = period;
        guest_output_pacing_period_rejects_ = 0;
      }
    }
    guest_output_pacing_vblank_time_ = vblank_time;
  }
  guest_output_pacing_condition_.notify_one();
}

void Presenter::StopGuestOutputPacing() {
  std::thread thread;
  {
    std::lock_guard<std::mutex> lock(guest_output_pacing_mutex_);
    guest_output_pacing_shutdown_ = true;
    thread = std::move(guest_output_pacing_thread_);
  }
  guest_output_pacing_condition_.notify_one();
  if (thread.joinable()) {
    thread.join();
  }
}

void Presenter::GuestOutputPacingThread() {
  rex::thread::set_current_thread_name("Guest Output Pacing");
  using Clock = std::chrono::steady_clock;
  {
    // Keep this thread's time when other programs run: MMCSS Games, else highest priority.
    const bool raise = REXCVAR_GET(present_pacing_thread_priority);
    const bool joined = raise && rex::thread::JoinMmcssGames();
    const pacing::ThreadPriorityChoice choice = pacing::ChoosePacingThreadPriority(raise, joined);
    if (choice == pacing::ThreadPriorityChoice::kHighest) {
      rex::thread::SetCurrentThreadPriorityHighest();
    }
    const bool high_qos = raise && rex::thread::RequestHighQosTimers();
    REXLOG_INFO("Guest output pacing thread: priority {}, full-speed timers {}",
                pacing::ThreadPriorityChoiceName(choice), high_qos ? "on" : "off");
  }
  // Guest frames arrive shortly after each vblank. Present each one at a fixed offset after its
  // vblank (by default the next tick): wake ahead of that moment and spin the last stretch. Waking
  // on the vblank signal instead lands up to ~0.7 ms late and varies. This is not a guest thread,
  // so spinning here does not take time from pinned guest cores.
  constexpr auto kWakeAhead = std::chrono::microseconds(1000);
  pacing::PresentWaiter* const waiter = GetPresentWaiter();
  Clock::time_point last_target{};
  Clock::time_point last_present{}, window_start = Clock::now();
  uint32_t presents = 0, repeated_vblanks = 0, repeated_since_present = 0;
  // Present-wait over the stats window.
  uint32_t waited_presents = 0, wait_timeouts = 0, unshown_max = 0, shown_count = 0;
  double wait_sum_ms = 0.0, wait_max_ms = 0.0, shown_sum_ms = 0.0, shown_max_ms = 0.0;
  bool present_wait_seen = false;
  long long logged_offset_us = -1;
  const bool repeat_held = REXCVAR_GET(present_repeat_held_vblanks);
  REXLOG_INFO("Guest output pacing: vblanks without a new frame {}",
              repeat_held ? "present the last frame again unless the display is fixed-refresh"
                          : "are skipped");
  bool presented_before = false;
  uint32_t repeat_presents = 0;
  std::FILE* log = nullptr;
  if (!REXCVAR_GET(present_pacing_log).empty()) {
    log = std::fopen(REXCVAR_GET(present_pacing_log).c_str(), "w");
    if (log) {
      std::fputs(pacing::SwapLogHeader(), log);
    }
  }
  const Clock::time_point log_origin = Clock::now();
  if (log) {
    // The log line's wall-clock time lines up present_us 0 with other runtime log events.
    REXLOG_INFO("Paced presentation log started (present_us 0)");
  }
  auto us = [](Clock::duration d) {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
  };
  Clock::time_point last_logged_present{};
  double interval_sum_ms = 0.0, interval_square_sum_ms = 0.0, interval_max_ms = 0.0;
  while (true) {
    Clock::time_point vblank_time;
    Clock::duration period;
    uint64_t vblank_count;
    uint32_t display_every;
    {
      std::unique_lock<std::mutex> lock(guest_output_pacing_mutex_);
      guest_output_pacing_condition_.wait(lock, [&]() {
        return guest_output_pacing_shutdown_ || guest_output_pacing_vblank_count_ >= 2;
      });
      if (guest_output_pacing_shutdown_) {
        if (log) {
          std::fclose(log);
        }
        rex::thread::LeaveMmcss();
        return;
      }
      vblank_time = guest_output_pacing_vblank_time_;
      period = guest_output_pacing_vblank_period_;
      vblank_count = guest_output_pacing_vblank_count_;
      display_every = guest_output_pacing_display_every_;
    }
    if (period <= Clock::duration::zero()) {
      continue;
    }
    Clock::duration offset = period;
    if (int32_t offset_us = REXCVAR_GET(present_pacing_offset_us); offset_us > 0) {
      const long long display_period_us = display_every ? us(period) / display_every : 0;
      const long long chosen_us = pacing::ChoosePresentOffsetUs(
          offset_us, display_period_us, REXCVAR_GET(present_pacing_min_margin_us));
      if (chosen_us != logged_offset_us) {
        logged_offset_us = chosen_us;
        REXLOG_INFO("Guest output pacing: present {} us after the guest vblank{}", chosen_us,
                    display_period_us ? fmt::format(" (fixed display, refresh {} us)",
                                                    display_period_us)
                                      : std::string());
      }
      offset = std::min<Clock::duration>(std::chrono::microseconds(chosen_us), period);
    }
    // The next present moment after the last one, anchored to the latest ideal vblank time.
    Clock::time_point target = vblank_time + offset;
    while (target <= last_target + period / 2) {
      target += period;
    }
    Clock::time_point now = Clock::now();
    if (target - now > period * 2) {
      // The guest clock stalled or jumped; resynchronize on the next signal.
      last_target = Clock::time_point{};
      rex::thread::PreciseSleep(std::chrono::microseconds(1000));
      continue;
    }
    pacing::SleepThenSpin(
        target, Clock::duration(kWakeAhead), [] { return Clock::now(); },
        [](Clock::duration d) {
          rex::thread::PreciseSleep(std::chrono::duration_cast<std::chrono::microseconds>(d));
        },
        [] { std::this_thread::yield(); });
    last_target = target;
    const pacing::VblankPresent vblank_present = pacing::ChooseVblankPresent(
        guest_output_paced_frame_pending_.exchange(false, std::memory_order_acq_rel),
        repeat_held, display_every != 0, presented_before);
    if (vblank_present != pacing::VblankPresent::kNewFrame) {
      ++repeated_vblanks;
      ++repeated_since_present;
      if (vblank_present == pacing::VblankPresent::kRepeat) {
        // The last frame again, at the moment a new one would have been presented. Not a row in
        // the swap log: its rows stay one per guest frame.
        if (waiter) {
          waiter->WaitForShown(0, period);
        }
        PresentLatestGuestOutput();
        ++repeat_presents;
      }
      continue;
    }
    presented_before = true;
    // With present-wait, keep one frame queued: wait until the previous present was shown. On
    // time this costs nothing; it holds a present only when the display fell behind.
    pacing::PresentWaiter::QueueWait queue_wait;
    if (waiter) {
      queue_wait = waiter->WaitForShown(0, period);
    }
    PresentLatestGuestOutput();
    const Clock::time_point presented = Clock::now();
    std::optional<pacing::PresentWaiter::Shown> shown;
    if (waiter) {
      shown = waiter->TakeLatestShown();
    }
    if (queue_wait.active) {
      present_wait_seen = true;
      const double wait_ms = std::chrono::duration<double, std::milli>(queue_wait.waited).count();
      if (queue_wait.waited > Clock::duration::zero()) {
        ++waited_presents;
      }
      wait_timeouts += queue_wait.timed_out;
      wait_sum_ms += wait_ms;
      wait_max_ms = std::max(wait_max_ms, wait_ms);
      unshown_max = std::max(unshown_max, queue_wait.unshown_before);
    }
    if (shown) {
      const double shown_ms =
          std::chrono::duration<double, std::milli>(shown->shown - shown->presented).count();
      ++shown_count;
      shown_sum_ms += shown_ms;
      shown_max_ms = std::max(shown_max_ms, shown_ms);
    }
    if (log) {
      Clock::time_point arrival{Clock::duration(
          guest_output_paced_frame_arrival_.load(std::memory_order_relaxed))};
      pacing::SwapLogRow row;
      row.present_us = us(presented - log_origin);
      row.interval_us =
          last_logged_present == Clock::time_point{} ? 0LL : us(presented - last_logged_present);
      row.repeated_vblanks = repeated_since_present;
      row.arrival_after_vblank_us = us(arrival - (target - offset));
      row.latency_us = us(presented - arrival);
      row.guest_vblank = vblank_count;
      // Guest frames dropped before display since the last present.
      row.frames_replaced =
          guest_output_paced_frames_replaced_since_present_.exchange(0, std::memory_order_relaxed);
      // The native renderer's frame the last guest swap showed (0 without one).
      row.external_sequence = system::external_frame::LastSwappedSequence();
      row.present_wait = queue_wait.active;
      row.present_id = waiter ? waiter->LastId() : 0;
      row.wait_us = us(queue_wait.waited);
      row.unshown = queue_wait.unshown_before;
      if (shown) {
        row.has_shown = true;
        row.shown_id = shown->id;
        row.shown_us = us(shown->shown - log_origin);
        row.shown_after_present_us = us(shown->shown - shown->presented);
      }
      std::fputs(pacing::FormatSwapLogRow(row).c_str(), log);
      last_logged_present = presented;
    }
    repeated_since_present = 0;
    if (!REXCVAR_GET(present_pacing_stats)) {
      continue;
    }
    now = Clock::now();
    if (last_present != Clock::time_point{}) {
      double interval_ms = std::chrono::duration<double, std::milli>(now - last_present).count();
      interval_sum_ms += interval_ms;
      interval_square_sum_ms += interval_ms * interval_ms;
      interval_max_ms = std::max(interval_max_ms, interval_ms);
      ++presents;
    }
    last_present = now;
    if (now - window_start >= std::chrono::seconds(5) && presents) {
      double mean_ms = interval_sum_ms / presents;
      double stdev_ms =
          std::sqrt(std::max(0.0, interval_square_sum_ms / presents - mean_ms * mean_ms));
      REXLOG_INFO(
          "Paced presentation 5s: {} frames, interval {:.3f} ms mean, {:.3f} ms stdev, "
          "{:.3f} ms max; {} vblanks without a new frame ({} presented the last frame again), "
          "{} frames replaced before display, "
          "{} host presents; window paints: {} forced, {} UI, {} requested, {} outdated",
          presents, mean_ms, stdev_ms, interval_max_ms, repeated_vblanks, repeat_presents,
          guest_output_paced_frames_replaced_.exchange(0, std::memory_order_relaxed),
          host_presents_.exchange(0, std::memory_order_relaxed),
          ui_paints_[0].exchange(0, std::memory_order_relaxed),
          ui_paints_[1].exchange(0, std::memory_order_relaxed),
          ui_paints_[2].exchange(0, std::memory_order_relaxed),
          ui_paints_[3].exchange(0, std::memory_order_relaxed));
      if (present_wait_seen) {
        REXLOG_INFO(
            "Present-wait 5s: {} presents waited on the previous one ({:.3f} ms mean over all, "
            "{:.3f} ms max, {} timeouts), most unshown {}; present to shown {:.3f} ms mean, "
            "{:.3f} ms max over {} shown",
            waited_presents, wait_sum_ms / presents, wait_max_ms, wait_timeouts, unshown_max,
            shown_count ? shown_sum_ms / shown_count : 0.0, shown_max_ms, shown_count);
      } else {
        REXLOG_INFO("Present-wait 5s: off (timer pacing)");
      }
      presents = repeated_vblanks = repeat_presents = 0;
      interval_sum_ms = interval_square_sum_ms = interval_max_ms = 0.0;
      waited_presents = wait_timeouts = unshown_max = shown_count = 0;
      wait_sum_ms = wait_max_ms = shown_sum_ms = shown_max_ms = 0.0;
      present_wait_seen = false;
      window_start = now;
    }
  }
}

void Presenter::PresentLatestGuestOutput() {
  // Trigger the presentation on the host.
  PaintResult paint_result = PaintResult::kNotPresented;
  {
    std::lock_guard<std::mutex> paint_mode_mutex_lock(paint_mode_mutex_);
    switch (paint_mode_) {
      case PaintMode::kNone:
        // Neither painting nor window paint requesting is accessible.
        break;
      case PaintMode::kUIThreadOnRequest:
        // Only window paint requesting is accessible.
        RequestPaintOrConnectionRecoveryViaWindow(true);
        break;
      case PaintMode::kGuestOutputThreadImmediately:
        // Both painting and window paint requesting are accessible.
        if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable) {
          paint_result = PaintAndPresent(false);
          if (surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedOutdated) {
            RequestPaintOrConnectionRecoveryViaWindow(true);
          }
        }
        break;
    }
  }
  // Handle GPU loss when not in the middle of the function anymore, and
  // lifecycle management from the GPU loss callback is fine on the UI thread.
  if (host_gpu_loss_callback_) {
    if (paint_result == PaintResult::kGpuLostResponsible) {
      host_gpu_loss_callback_(true, false);
    } else if (paint_result == PaintResult::kGpuLostExternally) {
      host_gpu_loss_callback_(false, false);
    }
  }
}

void Presenter::SetGuestOutputPaintConfigFromUIThread(const GuestOutputPaintConfig& new_config) {
  // For simplicity, this may be called externally repeatedly.
  // Lock the mutex only when something has been modified, and also don't
  // request UI thread guest output redraws when not needed.
  bool modified = false;
  bool request_repaint = false;
  if (guest_output_paint_config_.GetEffect() != new_config.GetEffect()) {
    modified = true;
    request_repaint = true;
  }
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (guest_output_paint_config_.GetFsrSharpnessReduction() !=
      new_config.GetFsrSharpnessReduction()) {
    modified = true;
    if (new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr ||
        new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
        new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3) {
      request_repaint = true;
    }
  }
  if (guest_output_paint_config_.GetCasAdditionalSharpness() !=
      new_config.GetCasAdditionalSharpness()) {
    modified = true;
    if (new_config.GetEffect() == GuestOutputPaintConfig::Effect::kCas ||
        new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr ||
        new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
        new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3) {
      request_repaint = true;
    }
  }
#endif
  if (guest_output_paint_config_.GetDither() != new_config.GetDither()) {
    modified = true;
    request_repaint = true;
  }
#if defined(REX_HAS_FIDELITYFX_SDK)
  if (guest_output_paint_config_.GetFsrQualityMode() != new_config.GetFsrQualityMode()) {
    modified = true;
    if (new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
        new_config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3) {
      request_repaint = true;
    }
  }
#endif
  if (modified) {
    {
      std::unique_lock<std::mutex> config_lock(guest_output_paint_config_mutex_);
      guest_output_paint_config_ = new_config;
    }
    // Coarsely check the availability of painting and of the window (for
    // calling RequestPaint) via paint_mode_ because the actual painting
    // connection state may currently be owned not by the UI thread.
    if (request_repaint && paint_mode_ != PaintMode::kNone) {
      if (is_in_ui_thread_paint_) {
        // Defer until the end of the current paint if called from, for
        // instance, a UI drawer.
        request_guest_output_paint_after_current_ui_thread_paint_ = true;
      } else {
        RequestPaintOrConnectionRecoveryViaWindow(true);
      }
    }
  }
}

void Presenter::SetCasFromUIThread(bool enabled, float sharpness) {
#if defined(REX_HAS_FIDELITYFX_SDK)
  GuestOutputPaintConfig config = guest_output_paint_config_;
  config.SetEffect(enabled ? GuestOutputPaintConfig::Effect::kCas
                           : GuestOutputPaintConfig::Effect::kBilinear);
  if (enabled) {
    config.SetCasAdditionalSharpness(sharpness);
  }
  SetGuestOutputPaintConfigFromUIThread(config);
#else
  (void)enabled;
  (void)sharpness;
#endif
}

void Presenter::AddUIDrawerFromUIThread(UIDrawer* drawer, size_t z_order) {
  assert_not_null(drawer);
  // Obtain whether the iterator list was empty before erasing in case of
  // replacing with a new entry with a different Z order happens.
  bool drawers_were_empty = ui_drawers_.empty();
  uint64_t drawer_last_draw = UINT64_MAX;
  // Check if already added.
  for (auto it_existing = ui_drawers_.begin(); it_existing != ui_drawers_.end(); ++it_existing) {
    if (it_existing->second.drawer != drawer) {
      continue;
    }
    if (it_existing->first == z_order) {
      return;
    }
    // Keep the same last draw index to prevent the drawer from being executed
    // twice if increasing its Z order during the drawer loop.
    drawer_last_draw = it_existing->second.last_draw;
    // If removing the drawer that is the next in the current drawer loop, skip
    // it (in a multimap, only one element iterator is invalidated).
    if (is_executing_ui_drawers_ && ui_draw_next_iterator_ == it_existing) {
      ++ui_draw_next_iterator_;
    }
    ui_drawers_.erase(it_existing);
    break;
  }
  auto it_new = ui_drawers_.emplace(z_order, UIDrawerReference(drawer, drawer_last_draw));
  // If adding to the Z layer currently being processed (for drawing, from the
  // lowest to the highest), or to layers in between the current and the
  // previously next, make sure the new drawer is executed too.
  if (is_executing_ui_drawers_ && z_order >= ui_draw_current_z_order_ &&
      (ui_draw_next_iterator_ == ui_drawers_.end() || z_order < ui_draw_next_iterator_->first)) {
    ui_draw_next_iterator_ = it_new;
  }
  HandleUIDrawersChangeFromUIThread(drawers_were_empty);
}

void Presenter::RemoveUIDrawerFromUIThread(UIDrawer* drawer) {
  assert_not_null(drawer);
  for (auto it_existing = ui_drawers_.begin(); it_existing != ui_drawers_.end(); ++it_existing) {
    if (it_existing->second.drawer != drawer) {
      continue;
    }
    // If removing the drawer that is the next in the current drawer loop, skip
    // it (in a multimap, only one element iterator is invalidated).
    if (is_executing_ui_drawers_ && ui_draw_next_iterator_ == it_existing) {
      ++ui_draw_next_iterator_;
    }
    ui_drawers_.erase(it_existing);
    HandleUIDrawersChangeFromUIThread(false);
    return;
  }
}

void Presenter::RequestUIPaintFromUIThread() {
  if (is_in_ui_thread_paint_) {
    // The paint request will be done once in the end of PaintFromUIThread
    // according to the actual state at the moment that happens. It's common for
    // drawers to call this (even every frame), and no need to do too many OS
    // paint request calls.
    request_ui_paint_after_current_ui_thread_paint_ = true;
    return;
  }
  // The connection state may be owned by the guest output thread now rather
  // than the UI thread, check whether it's not pointless to make the request
  // coarsely via paint_mode_.
  if (!ui_drawers_.empty() && paint_mode_ != PaintMode::kNone) {
    // The window must be present, otherwise the conditions wouldn't have been
    // met.
    window_->RequestPaint();
  }
}

bool Presenter::InitializeCommonSurfaceIndependent() {
  {
    std::lock_guard<std::mutex> config_lock(guest_output_paint_config_mutex_);
    guest_output_paint_config_ = BuildGuestOutputPaintConfigFromCVar();
  }

  // Initialize UI frame rate limiting.
#if REX_PLATFORM_WIN32
  dxgi_ui_tick_thread_ = std::thread(&Presenter::DXGIUITickThread, this);
#endif  // XE_PLATFORM

  return true;
}

std::unique_lock<std::mutex> Presenter::ConsumeGuestOutput(
    uint32_t& mailbox_index_or_max_if_inactive_out, GuestOutputProperties* properties_out,
    GuestOutputPaintConfig* paint_config_out) {
  if (paint_config_out) {
    // Get the up-to-date guest output paint configuration settings set by the
    // UI thread.
    std::unique_lock<std::mutex> config_lock(guest_output_paint_config_mutex_);
    *paint_config_out = guest_output_paint_config_;
  }

  // Lock the mutex to make sure the image that will be acquired now is owned
  // exclusively by the calling thread for the time while this mutex is still
  // locked (it needs to be held by the consumer while working with anything
  // that depends on the image now being acquired or its index in the mailbox).
  std::unique_lock<std::mutex> consumer_lock(guest_output_mailbox_consumer_mutex_);
  // Acquire the up-to-date ready guest image (may be new, in this case the last
  // acquired one will be released, or still the same or no refresh has happened
  // since the last consumption).
  // memory_order_relaxed here because the ready index from this load will be
  // used directly only if it's the same as during the last consumption - thus
  // the image has already been acquired previously, no need for
  // memory_order_acquire (if the image has been acquired by a different
  // consumer though, the consumer mutex performs memory access ordering).
  uint32_t old_acquired_and_ready =
      guest_output_mailbox_acquired_and_ready_.load(std::memory_order_relaxed);
  // Desired acquired = current ready.
  // Desired ready = current ready (changed only by the producer).
  uint32_t desired_acquired_and_ready =
      (old_acquired_and_ready & ~uint32_t(3)) | (old_acquired_and_ready >> 2);
  // Either the same image as during the last consumption, or a new one, is
  // satisfying. However, if it's new, using memory_order_acq_rel to acquire the
  // new ready image (to make it acquired) and to release the old acquired image
  // (to let the producer take it as writable).
  while (old_acquired_and_ready != desired_acquired_and_ready &&
         !guest_output_mailbox_acquired_and_ready_.compare_exchange_weak(
             old_acquired_and_ready, desired_acquired_and_ready, std::memory_order_acq_rel,
             std::memory_order_relaxed)) {
    desired_acquired_and_ready =
        (old_acquired_and_ready & ~uint32_t(3)) | (old_acquired_and_ready >> 2);
  }
  uint32_t mailbox_index = desired_acquired_and_ready & 3;
  // Give the current acquired image to the caller, or UINT32_MAX if it's
  // inactive.
  const GuestOutputProperties& properties = guest_output_properties_[mailbox_index];
  mailbox_index_or_max_if_inactive_out = properties.IsActive() ? mailbox_index : UINT32_MAX;
  if (properties_out) {
    *properties_out = properties;
  }
  return std::move(consumer_lock);
}

Presenter::GuestOutputPaintFlow Presenter::GetGuestOutputPaintFlow(
    const GuestOutputProperties& properties, uint32_t host_rt_width, uint32_t host_rt_height,
    uint32_t max_rt_width, uint32_t max_rt_height, const GuestOutputPaintConfig& config) const {
  GuestOutputPaintFlow flow = {};

  // FIXME(Triang3l): Configuration variables racing with per-game config
  // loading.

  assert_not_zero(max_rt_width);
  assert_not_zero(max_rt_height);

  // Initialize one clear rectangle for the case of drawing no guest output, for
  // consistency with fewer state dependencies.
  flow.letterbox_clear_rectangle_count = 1;
  flow.letterbox_clear_rectangles[0].width = host_rt_width;
  flow.letterbox_clear_rectangles[0].height = host_rt_height;

  // For safety such as division by zero prevention.
  if (!properties.IsActive() || !host_rt_width || !host_rt_height ||
      !surface_width_in_paint_connection_ || !surface_height_in_paint_connection_) {
    return flow;
  }

  flow.properties = properties;

  // Multiplication-division rounding to the nearest.
  auto rescale_unsigned = [](uint32_t value, uint32_t new_scale, uint32_t old_scale) -> uint32_t {
    return uint32_t((uint64_t(value) * new_scale + (old_scale >> 1)) / old_scale);
  };
  auto rescale_signed = [](int32_t value, uint32_t new_scale, uint32_t old_scale) -> int32_t {
    // Plus old_scale / 2 for positive values, minus old_scale / 2 for
    // negative values for consistent rounding for both positive and
    // negative values (as the `/` operator rounds towards zero).
    // Example:
    // (-3 - 1) / 3 == -1
    // (-2 - 1) / 3 == -1
    // (-1 - 1) / 3 == 0
    // ---
    // (0 + 1) / 3 == 0
    // (1 + 1) / 3 == 0
    // (2 + 1) / 3 == 1
    return int32_t((int64_t(value) * new_scale + int32_t(old_scale >> 1) * (value < 0 ? -1 : 1)) /
                   old_scale);
  };

  // Final output location and dimensions.
  // All host location calculations are DPI-independent, conceptually depending
  // only on the aspect ratios, not the absolute values.
  uint32_t output_width, output_height;
  if (uint64_t(surface_width_in_paint_connection_) * properties.display_aspect_ratio_y >
      uint64_t(surface_height_in_paint_connection_) * properties.display_aspect_ratio_x) {
    // The window is wider that the source - crop along Y to preserve the aspect
    // ratio while stretching throughout the entire surface's width, then limit
    // the Y cropping via letterboxing or stretching along X.
    uint32_t present_safe_area;
    if (config.GetAllowOverscanCutoff() && REXCVAR_GET(present_safe_area_y) > 0 &&
        REXCVAR_GET(present_safe_area_y) < 100) {
      present_safe_area = uint32_t(REXCVAR_GET(present_safe_area_y));
    } else {
      present_safe_area = 100;
    }
    // Scale the desired width by the H:W aspect ratio (inverse of W:H) to get
    // the height.
    output_height =
        rescale_unsigned(surface_width_in_paint_connection_, properties.display_aspect_ratio_y,
                         properties.display_aspect_ratio_x);
    bool letterbox = false;
    if (output_height * present_safe_area > surface_height_in_paint_connection_ * 100) {
      // Don't crop out more than the safe area margin - letterbox or stretch.
      output_height = rescale_unsigned(surface_height_in_paint_connection_, 100, present_safe_area);
      letterbox = true;
    }
    if (letterbox && REXCVAR_GET(present_letterbox)) {
      output_width = rescale_unsigned(surface_height_in_paint_connection_ * 100,
                                      properties.display_aspect_ratio_x,
                                      properties.display_aspect_ratio_y * present_safe_area);
      // output_width might have been rounded up already by rescale_unsigned, so
      // rounding down in this division.
      flow.output_x = (int32_t(surface_width_in_paint_connection_) - int32_t(output_width)) / 2;
    } else {
      output_width = surface_width_in_paint_connection_;
      flow.output_x = 0;
    }
    // output_height might have been rounded up already by rescale_unsigned, so
    // rounding down in this division.
    flow.output_y = (int32_t(surface_height_in_paint_connection_) - int32_t(output_height)) / 2;
  } else {
    // The window is taller that the source - crop along X to preserve the
    // aspect ratio while stretching throughout the entire surface's height,
    // then limit the X cropping via letterboxing or stretching along Y.
    uint32_t present_safe_area;
    if (config.GetAllowOverscanCutoff() && REXCVAR_GET(present_safe_area_x) > 0 &&
        REXCVAR_GET(present_safe_area_x) < 100) {
      present_safe_area = uint32_t(REXCVAR_GET(present_safe_area_x));
    } else {
      present_safe_area = 100;
    }
    // Scale the desired height by the W:H aspect ratio to get the width.
    output_width =
        rescale_unsigned(surface_height_in_paint_connection_, properties.display_aspect_ratio_x,
                         properties.display_aspect_ratio_y);
    bool letterbox = false;
    if (output_width * present_safe_area > surface_width_in_paint_connection_ * 100) {
      // Don't crop out more than the safe area margin - letterbox or stretch.
      output_width = rescale_unsigned(surface_width_in_paint_connection_, 100, present_safe_area);
      letterbox = true;
    }
    if (letterbox && REXCVAR_GET(present_letterbox)) {
      output_height = rescale_unsigned(surface_width_in_paint_connection_ * 100,
                                       properties.display_aspect_ratio_y,
                                       properties.display_aspect_ratio_x * present_safe_area);
      // output_height might have been rounded up already by rescale_unsigned,
      // so rounding down in this division.
      flow.output_y = (int32_t(surface_height_in_paint_connection_) - int32_t(output_height)) / 2;
    } else {
      output_height = surface_height_in_paint_connection_;
      flow.output_y = 0;
    }
    // output_width might have been rounded up already by rescale_unsigned, so
    // rounding down in this division.
    flow.output_x = (int32_t(surface_width_in_paint_connection_) - int32_t(output_width)) / 2;
  }

  // Convert the location from surface pixels (which have 1:1 aspect ratio
  // relatively to the physical display) to render target pixels (the render
  // target size may be arbitrary with any aspect ratio, but if it's different
  // than the surface size, the OS is expected to stretch it to the surface
  // boundaries), preserving the aspect ratio.
  if (host_rt_width != surface_width_in_paint_connection_) {
    flow.output_x =
        rescale_signed(flow.output_x, host_rt_width, surface_width_in_paint_connection_);
    output_width =
        rescale_unsigned(output_width, host_rt_width, surface_width_in_paint_connection_);
  }
  if (host_rt_height != surface_height_in_paint_connection_) {
    flow.output_y =
        rescale_signed(flow.output_y, host_rt_height, surface_height_in_paint_connection_);
    output_height =
        rescale_unsigned(output_height, host_rt_height, surface_height_in_paint_connection_);
  }

  // The out-of-bounds checks are needed for correct letterbox calculations.
  // Though this normally shouldn't happen, but in case of rounding issues with
  // extreme values.
  int32_t output_right = flow.output_x + int32_t(output_width);
  int32_t output_bottom = flow.output_y + int32_t(output_height);
  // In rare cases (for example, when surface and render-target coordinate
  // spaces diverge unexpectedly), the computed output may become anchored at
  // the top-left and smaller than the host render target. This results in only
  // a top-left region being presented with black right/bottom areas.
  if (flow.output_x >= 0 && flow.output_y >= 0 && flow.output_x <= 1 && flow.output_y <= 1 &&
      output_width < host_rt_width && output_height < host_rt_height) {
    static std::atomic<bool> logged_top_left_expand = false;
    if (!logged_top_left_expand.exchange(true)) {
      REXLOG_WARN(
          "Presenter: expanding guest output from {}x{} to host render target "
          "{}x{} to avoid top-left-only presentation",
          output_width, output_height, host_rt_width, host_rt_height);
    }
    output_width = host_rt_width;
    output_height = host_rt_height;
    output_right = int32_t(output_width);
    output_bottom = int32_t(output_height);
  }
  if (!output_width || !output_height || output_right <= 0 || output_bottom <= 0 ||
      flow.output_x >= int32_t(host_rt_width) || flow.output_y >= int32_t(host_rt_height)) {
    return flow;
  }

  // The output image may have a part of it outside the final render target (if
  // using the overscan area to stretch the image to the entire surface while
  // preserving the guest aspect ratio if it differs from the host one, for
  // instance). While the final render target size is known to be within the
  // host render target / image size limit, the intermediate images may be
  // larger than that as they include the overscan area that will be outside the
  // screen. Make sure the intermediate images can't be larger than the maximum
  // render target size.
  uint32_t output_width_clamped = std::min(output_width, max_rt_width);
  uint32_t output_height_clamped = std::min(output_height, max_rt_height);

#if defined(REX_HAS_FIDELITYFX_SDK)
  if (config.GetEffect() == GuestOutputPaintConfig::Effect::kCas ||
      config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr ||
      config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
      config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3) {
    // FidelityFX Super Resolution and Contrast Adaptive Sharpening only work
    // good for up to 2x2 upscaling due to the way they fetch texels.
    // CAS is primarily a sharpening filter, not an upscaling one (its upscaling
    // eliminates reduces blurriness, but doesn't preserve the shapes of edges,
    // and executing it multiple times will only result in oversharpening. So,
    // using it for scales only of up to 2x2, then simply stretching with
    // bilinear filtering.
    // EASU of FSR, however, preserves edges, it's not supposed to blur them or
    // to make them jagged, so it can be executed multiple times - running
    // multiple EASU passes for scale factors of over 2x2.
    // Just one EASU pass rather than multiple for scaling to factors bigger
    // than 2x2 (especially significantly bigger, such as 1152x640 to 3840x2160,
    // or 3.333x3.375) results in blurry edges and an overall noisy look,
    // multiple passes improve visual stability.
    std::pair<uint32_t, uint32_t> ffx_last_size;
    if (flow.effect_count) {
      ffx_last_size = flow.effect_output_sizes[flow.effect_count - 1];
    } else {
      ffx_last_size.first = properties.frontbuffer_width;
      ffx_last_size.second = properties.frontbuffer_height;
    }
    bool is_temporal_effect = config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr2 ||
                              config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr3;
    bool temporal_quality_mode_forced =
        is_temporal_effect &&
        config.GetFsrQualityMode() != GuestOutputPaintConfig::FsrQualityMode::kAuto;
    if ((config.GetEffect() == GuestOutputPaintConfig::Effect::kFsr || is_temporal_effect) &&
        ((ffx_last_size.first < output_width_clamped ||
          ffx_last_size.second < output_height_clamped) ||
         temporal_quality_mode_forced)) {
      // AMD FidelityFX Super Resolution - upsample along at least one axis.
      // Using the output size clamped to the maximum render target size here as
      // EASU will always write to intermediate images, and RCAS supports only
      // 1:1.
      if (is_temporal_effect) {
        uint32_t temporal_input_width = ffx_last_size.first;
        uint32_t temporal_input_height = ffx_last_size.second;
        uint32_t quality_mode_render_width = 0;
        uint32_t quality_mode_render_height = 0;
        if (QueryTemporalFsrRenderResolutionFromQualityMode(
                output_width_clamped, output_height_clamped, config.GetFsrQualityMode(),
                quality_mode_render_width, quality_mode_render_height)) {
          quality_mode_render_width =
              std::max(uint32_t(1), std::min(quality_mode_render_width, output_width_clamped));
          quality_mode_render_height =
              std::max(uint32_t(1), std::min(quality_mode_render_height, output_height_clamped));
          if (quality_mode_render_width < temporal_input_width ||
              quality_mode_render_height < temporal_input_height) {
            temporal_input_width = quality_mode_render_width;
            temporal_input_height = quality_mode_render_height;
            if (temporal_input_width != ffx_last_size.first ||
                temporal_input_height != ffx_last_size.second) {
              assert_true(flow.effect_count < flow.effects.size());
              flow.effect_output_sizes[flow.effect_count] =
                  std::make_pair(temporal_input_width, temporal_input_height);
              // Pre-temporal quality mode may request a lower render
              // resolution. Use CAS resample since bilinear is not allowed in
              // intermediate passes.
              flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kCasResample;
              ffx_last_size.first = temporal_input_width;
              ffx_last_size.second = temporal_input_height;
            }
          } else if (quality_mode_render_width > temporal_input_width ||
                     quality_mode_render_height > temporal_input_height) {
            LogTemporalFsrQualityModeInputLimitOnce();
          }
        }

        // A single temporal upscaler dispatch can target the final clamped
        // size directly, unlike the spatial multi-pass chain.
        ffx_last_size.first = output_width_clamped;
        ffx_last_size.second = output_height_clamped;
        assert_true(flow.effect_count < flow.effects.size());
        flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
        flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kFsrEasu;
      } else {
        uint32_t easu_max_passes = config.GetFsrMaxUpsamplingPasses();
        uint32_t easu_pass_count = 0;
        while (easu_pass_count < easu_max_passes &&
               (ffx_last_size.first < output_width_clamped ||
                ffx_last_size.second < output_height_clamped)) {
          ffx_last_size.first = std::min(ffx_last_size.first * uint32_t(2), output_width_clamped);
          ffx_last_size.second =
              std::min(ffx_last_size.second * uint32_t(2), output_height_clamped);
          assert_true(flow.effect_count < flow.effects.size());
          flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
          flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kFsrEasu;
          ++easu_pass_count;
        }
      }
      assert_true(flow.effect_count < flow.effects.size());
      flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
      flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kFsrRcas;
    } else {
      // AMD FidelityFX Contrast Adaptive Sharpening - sharpen or downsample, or
      // upsample up to 2x2 if CAS is specified to be used for upscaling too.
      // Using the unclamped output size as CAS may be the last pass - if a
      // bilinear pass is needed afterwards, and the CAS pass will be writing to
      // an intermediate image, the CAS pass output size will be clamped while
      // adding the bilinear stretch.
      std::pair<uint32_t, uint32_t> pre_cas_size = ffx_last_size;
      ffx_last_size.first = std::min(ffx_last_size.first * uint32_t(2), output_width);
      ffx_last_size.second = std::min(ffx_last_size.second * uint32_t(2), output_height);
      assert_true(flow.effect_count < flow.effects.size());
      flow.effect_output_sizes[flow.effect_count] = ffx_last_size;
      flow.effects[flow.effect_count++] = ffx_last_size == pre_cas_size
                                              ? GuestOutputPaintEffect::kCasSharpen
                                              : GuestOutputPaintEffect::kCasResample;
    }
  }
#endif  // defined(REX_HAS_FIDELITYFX_SDK)

  std::pair<uint32_t, uint32_t>* last_pre_bilinear_effect_size =
      flow.effect_count ? &flow.effect_output_sizes[flow.effect_count - 1] : nullptr;
  if (!last_pre_bilinear_effect_size || last_pre_bilinear_effect_size->first != output_width ||
      last_pre_bilinear_effect_size->second != output_height) {
    // If not using FidelityFX, or it has reached its upscaling capabilities,
    // but more is needed, stretch via bilinear filtering.
    // Clamp the output size of the last effect to the maximum render target
    // size because it will go to an intermediate image now.
    if (last_pre_bilinear_effect_size) {
#if defined(REX_HAS_FIDELITYFX_SDK)
      // RCAS only works for 1:1, clamping must be done explicitly for FSR.
      assert_false(flow.effects[flow.effect_count - 1] == GuestOutputPaintEffect::kFsrRcas &&
                   (last_pre_bilinear_effect_size->first > max_rt_width ||
                    last_pre_bilinear_effect_size->second > max_rt_height));
#endif
      last_pre_bilinear_effect_size->first =
          std::min(last_pre_bilinear_effect_size->first, max_rt_width);
      last_pre_bilinear_effect_size->second =
          std::min(last_pre_bilinear_effect_size->second, max_rt_height);
    }
    assert_true(flow.effect_count < flow.effects.size());
    flow.effect_output_sizes[flow.effect_count] = std::make_pair(output_width, output_height);
    flow.effects[flow.effect_count++] = GuestOutputPaintEffect::kBilinear;
  }

  assert_not_zero(flow.effect_count);

  if (config.GetDither()) {
    // Dithering must be applied only to the final effect since resampling and
    // sharpening filters may considering the dithering noise features and
    // amplify it.
    GuestOutputPaintEffect& last_effect = flow.effects[flow.effect_count - 1];
    switch (last_effect) {
      case GuestOutputPaintEffect::kBilinear:
        // Dithering has no effect for 1:1 copying of a 8bpc image.
        if (!properties.is_8bpc || flow.effect_count > 1 ||
            output_width != properties.frontbuffer_width ||
            output_height != properties.frontbuffer_height) {
          last_effect = GuestOutputPaintEffect::kBilinearDither;
        }
        break;
#if defined(REX_HAS_FIDELITYFX_SDK)
      case GuestOutputPaintEffect::kCasSharpen:
        last_effect = GuestOutputPaintEffect::kCasSharpenDither;
        break;
      case GuestOutputPaintEffect::kCasResample:
        last_effect = GuestOutputPaintEffect::kCasResampleDither;
        break;
      case GuestOutputPaintEffect::kFsrRcas:
        last_effect = GuestOutputPaintEffect::kFsrRcasDither;
        break;
#endif
      default:
        break;
    }
  }

#ifndef NDEBUG
  for (size_t i = 0; i + 1 < flow.effect_count; ++i) {
    assert_true(CanGuestOutputPaintEffectBeIntermediate(flow.effects[i]));
  }
  assert_true(CanGuestOutputPaintEffectBeFinal(flow.effects[flow.effect_count - 1]));
#endif

  // Calculate the letterbox geometry.
  if (flow.effect_count) {
    flow.letterbox_clear_rectangle_count = 0;
    uint32_t letterbox_mid_top = uint32_t(std::max(flow.output_y, int32_t(0)));
    // Top.
    if (letterbox_mid_top) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_top =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_top.x = 0;
      letterbox_clear_rectangle_top.y = 0;
      letterbox_clear_rectangle_top.width = host_rt_width;
      letterbox_clear_rectangle_top.height = letterbox_mid_top;
    }
    uint32_t letterbox_mid_bottom = std::min(uint32_t(output_bottom), host_rt_height);
    uint32_t letterbox_mid_height = letterbox_mid_bottom - letterbox_mid_top;
    // Middle-left.
    if (flow.output_x > 0) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_left =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_left.x = 0;
      letterbox_clear_rectangle_left.y = letterbox_mid_top;
      letterbox_clear_rectangle_left.width = uint32_t(flow.output_x);
      letterbox_clear_rectangle_left.height = letterbox_mid_height;
    }
    // Middle-right.
    if (uint32_t(output_right) < host_rt_width) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_right =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_right.x = uint32_t(output_right);
      letterbox_clear_rectangle_right.y = letterbox_mid_top;
      letterbox_clear_rectangle_right.width = host_rt_width - uint32_t(output_right);
      letterbox_clear_rectangle_right.height = letterbox_mid_height;
    }
    // Bottom.
    if (letterbox_mid_bottom < host_rt_height) {
      assert_true(flow.letterbox_clear_rectangle_count < flow.letterbox_clear_rectangles.size());
      GuestOutputPaintFlow::ClearRectangle& letterbox_clear_rectangle_top =
          flow.letterbox_clear_rectangles[flow.letterbox_clear_rectangle_count++];
      letterbox_clear_rectangle_top.x = 0;
      letterbox_clear_rectangle_top.y = letterbox_mid_bottom;
      letterbox_clear_rectangle_top.width = host_rt_width;
      letterbox_clear_rectangle_top.height = host_rt_height - letterbox_mid_bottom;
    }
  }

  return flow;
}

void Presenter::ExecuteUIDrawersFromUIThread(UIDrawContext& ui_draw_context) {
  // May be called by the implementations only when requested.
  assert_true(is_in_ui_thread_paint_);
  // Drawers can add or remove drawers (including themselves), need to ensure
  // iterator validity in this case.
  assert_false(is_executing_ui_drawers_);
  ui_draw_next_iterator_ = ui_drawers_.begin();
  is_executing_ui_drawers_ = true;
  while (ui_draw_next_iterator_ != ui_drawers_.end()) {
    // The current iterator may be invalidated, and ui_draw_next_iterator_ may
    // be changed, during the execution of the drawer if the list of the drawers
    // is modified by it - don't assume that after the call
    // ui_draw_next_iterator_ will be the same as
    // std::next(ui_draw_next_iterator_) before it.
    auto it_current = ui_draw_next_iterator_++;
    // Don't draw twice if already drawn in this frame (may happen if the Z
    // order of a drawer was increased from below the current one to above it by
    // one of the drawers).
    if (it_current->second.last_draw != ui_draw_current_) {
      ui_draw_current_z_order_ = it_current->first;
      it_current->second.last_draw = ui_draw_current_;
      it_current->second.drawer->Draw(ui_draw_context);
    }
  }
  is_executing_ui_drawers_ = false;
  ++ui_draw_current_;
}

void Presenter::SetPaintModeFromUIThread(PaintMode new_mode) {
  // Can be modified only from the UI thread, so can skip locking if it's the
  // same.
  if (paint_mode_ == new_mode) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(paint_mode_mutex_);
    paint_mode_ = new_mode;
  }
  UpdateUITicksNeededFromUIThread();
}

Presenter::PaintMode Presenter::GetDesiredPaintModeFromUIThread(bool is_paintable) const {
  if (!is_paintable) {
    // The only case when kNone can be returned, for surface connection updates
    // when it's known that the UI thread currently has access to the connection
    // lifecycle.
    return PaintMode::kNone;
  }
  if (!REXCVAR_GET(host_present_from_non_ui_thread)) {
    return PaintMode::kUIThreadOnRequest;
  }
  if (surface_paint_connection_has_implicit_vsync_ &&
      !REXCVAR_GET(present_pace_to_guest_vblank)) {
    // Don't be causing host vertical sync CPU waits in the thread generating
    // the guest output. The pacing thread is dedicated, so it may wait.
    return PaintMode::kUIThreadOnRequest;
  }
  if (!ui_drawers_.empty()) {
    // The UI can be drawn only by the UI thread, and it needs to be drawn -
    // paint in the UI thread.
    return PaintMode::kUIThreadOnRequest;
  }
  // Only the guest output needs to be drawn - let the guest output thread
  // present immediately for a lower latency.
  return PaintMode::kGuestOutputThreadImmediately;
}

void Presenter::DisconnectPaintingFromSurfaceFromUIThread(SurfacePaintConnectionState new_state) {
  assert_false(IsConnectedSurfacePaintConnectionState(new_state));
  if (IsConnectedSurfacePaintConnectionState(surface_paint_connection_state_)) {
    DisconnectPaintingFromSurfaceFromUIThreadImpl();
  }
  surface_paint_connection_state_ = new_state;
  surface_paint_connection_has_implicit_vsync_ = false;
  surface_width_in_paint_connection_ = 0;
  surface_height_in_paint_connection_ = 0;
}

void Presenter::UpdateSurfacePaintConnectionFromUIThread(bool* repaint_needed_out,
                                                         bool update_paint_mode_to_desired) {
  assert_not_null(surface_);

  // Validate that painting lifecycle is accessible by the UI thread currently,
  // not given to the guest output thread. The mode can be modified only by
  // the UI thread, so no need to lock the mutex.
  assert_true(paint_mode_ != PaintMode::kGuestOutputThreadImmediately);

  // Initialize repaint_needed_out for failure cases.
  if (repaint_needed_out) {
    *repaint_needed_out = false;
  }

  // If the connection state is kUnconnectedSurfaceReportedUnusable, the
  // implementation has reported that the surface is not usable by the presenter
  // at all, and it's pointless to retry connecting to it.
  if (surface_paint_connection_state_ !=
      SurfacePaintConnectionState::kUnconnectedSurfaceReportedUnusable) {
    uint32_t surface_width = 0, surface_height = 0;
    bool surface_area_available = surface_->GetSize(surface_width, surface_height);
    if (!surface_area_available) {
      // The surface is currently zero-area (or has become zero-area), try again
      // when it's resized.
      DisconnectPaintingFromSurfaceFromUIThread(
          SurfacePaintConnectionState::kUnconnectedRetryAtStateChange);
    } else {
      bool is_reconnect = IsConnectedSurfacePaintConnectionState(surface_paint_connection_state_);
      bool is_vsync_implicit = false;
      SurfacePaintConnectResult connect_result = ConnectOrReconnectPaintingToSurfaceFromUIThread(
          *surface_, surface_width, surface_height,
          surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable,
          is_vsync_implicit);
      switch (connect_result) {
        case SurfacePaintConnectResult::kSuccess:
          if (repaint_needed_out) {
            *repaint_needed_out = true;
          }
          // Fallthrough to common success handling.
        case SurfacePaintConnectResult::kSuccessUnchanged:
          // Don't know yet what the first result was (success or suboptimal).
          surface_paint_connection_was_optimal_at_successful_paint_ = false;
          surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedPaintable;
          surface_paint_connection_has_implicit_vsync_ = is_vsync_implicit;
          surface_width_in_paint_connection_ = surface_width;
          surface_height_in_paint_connection_ = surface_height;
          if (!is_reconnect) {
            *repaint_needed_out = true;
          }
          break;
        case SurfacePaintConnectResult::kFailure:
          surface_paint_connection_state_ =
              SurfacePaintConnectionState::kUnconnectedRetryAtStateChange;
          break;
        case SurfacePaintConnectResult::kFailureSurfaceUnusable:
          surface_paint_connection_state_ =
              SurfacePaintConnectionState::kUnconnectedSurfaceReportedUnusable;
          break;
      }
    }
  }

  if (update_paint_mode_to_desired) {
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(
        surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable));
  }
}

bool Presenter::RequestPaintOrConnectionRecoveryViaWindow(bool force_ui_thread_paint_tick) {
  // Can be called from any thread if an existing window_ is available in it,
  // and it's known to have a Surface that will be the same throughout this
  // call - not doing any checks whether this request can be satisfied
  // theoretically. For safety, check whether the window exists unconditionally.
  assert_not_null(window_);
  assert_not_null(surface_);
  if (ui_thread_paint_requested_.exchange(true, std::memory_order_relaxed)) {
    // Invalidation pending already, no need to do it twice.
    return false;
  }
  if (force_ui_thread_paint_tick) {
    ForceUIThreadPaintTick();
  }
  window_->RequestPaint();
  return true;
}

void Presenter::UpdateSurfaceMonitorFromUIThread(bool old_monitor_potentially_disconnected) {
  // For dropping the monitor when the window is closing and is losing its
  // surface, the existence of `surface_` (which implies that `window_` exists
  // too) must be the condition for a non-null monitor, not just the existence
  // of `window_`.
#if REX_PLATFORM_WIN32
  HMONITOR surface_new_win32_monitor = nullptr;
  if (surface_) {
    HWND hwnd = static_cast<HWND>(window_->GetNativeWindowHandle());
    // The HWND may be non-existent if the window has been closed and destroyed
    // (the HWND, not the rex::ui::Window) already.
    if (hwnd) {
      surface_new_win32_monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    }
  }
  if (old_monitor_potentially_disconnected || surface_win32_monitor_ != surface_new_win32_monitor) {
    surface_win32_monitor_ = surface_new_win32_monitor;
    if (dxgi_ui_tick_factory_ && !dxgi_ui_tick_factory_->IsCurrent()) {
      // If a monitor has been newly connected, it won't appear in the old
      // factory, need to recreate it.
      {
        Microsoft::WRL::ComPtr<IDXGIOutput> old_factory_output_to_release;
        {
          std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
          old_factory_output_to_release = std::move(dxgi_ui_tick_output_);
        }
      }
      dxgi_ui_tick_factory_.Reset();
    }
    if (!dxgi_ui_tick_factory_) {
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_ui_tick_factory_)))) {
        REXLOG_ERROR("Presenter: Failed to create a DXGI factory");
      }
    }
    Microsoft::WRL::ComPtr<IDXGIOutput> new_dxgi_output;
    if (dxgi_ui_tick_factory_ && surface_new_win32_monitor) {
      new_dxgi_output =
          GetDXGIOutputForMonitor(dxgi_ui_tick_factory_.Get(), surface_new_win32_monitor);
    }
    // If the adapter was recreated, and the old output was released before its
    // destruction, notifying is still required - the vertical blank wait thread
    // might have entered the condition variable wait already as the output was
    // null.
    bool signal_dxgi_ui_tick_control;
    {
      std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
      bool dxgi_output_was_null = (dxgi_ui_tick_output_ == nullptr);
      dxgi_ui_tick_output_ = new_dxgi_output;
      signal_dxgi_ui_tick_control =
          dxgi_output_was_null && AreDXGIUITicksWaitable(dxgi_ui_tick_lock);
    }
    if (signal_dxgi_ui_tick_control) {
      dxgi_ui_tick_control_condition_.notify_all();
    }
  }
#endif  // XE_PLATFORM
}

bool Presenter::InSurfaceOnMonitorFromUIThread() const {
  if (!surface_) {
    return false;
  }
#if REX_PLATFORM_WIN32
  return surface_win32_monitor_ != nullptr;
#else
  return true;
#endif  // XE_PLATFORM
}

Presenter::PaintResult Presenter::PaintAndPresent(bool execute_ui_drawers) {
  assert_false(execute_ui_drawers && !is_in_ui_thread_paint_);
  assert_true(surface_paint_connection_state_ == SurfacePaintConnectionState::kConnectedPaintable);
  PaintResult result = PaintAndPresentImpl(execute_ui_drawers);
  if (result == PaintResult::kPresented || result == PaintResult::kPresentedSuboptimal) {
    host_presents_.fetch_add(1, std::memory_order_relaxed);
  }
  switch (result) {
    case PaintResult::kPresented:
      surface_paint_connection_was_optimal_at_successful_paint_ = true;
      break;
    case PaintResult::kPresentedSuboptimal:
      // Make outdated if previously optimal, now suboptimal, but don't cause
      // the connection to become outdated if it has been suboptimal from the
      // very beginning.
      if (surface_paint_connection_was_optimal_at_successful_paint_) {
        surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
      }
      break;
    case PaintResult::kNotPresentedConnectionOutdated:
      surface_paint_connection_state_ = SurfacePaintConnectionState::kConnectedOutdated;
      break;
    default:
      // Another issue not directly related to the surface connection.
      break;
  }
  return result;
}

void Presenter::HandleUIDrawersChangeFromUIThread(bool drawers_were_empty) {
  if (is_in_ui_thread_paint_) {
    // Defer the refresh so no dangerous lifecycle-related changes happen during
    // drawing.
    if (!ui_drawers_.empty()) {
      request_ui_paint_after_current_ui_thread_paint_ = true;
    }
    return;
  }

  if (paint_mode_ == PaintMode::kNone) {
    // Not connected, no point in refreshing (checking a more conservative
    // paint_mode_ because the actual connection state may currently be owned by
    // the guest output thread instead) or in toggling the ownership (the rest
    // of the function can assume it's not kNone).
    return;
  }

  if (ui_drawers_.empty() != drawers_were_empty) {
    // Require the UI thread to paint if it needs the UI, or let the guest
    // output thread paint immediately if not.
    SetPaintModeFromUIThread(GetDesiredPaintModeFromUIThread(true));
    // Make sure the ticks for limiting the UI frame rate are sent.
    UpdateUITicksNeededFromUIThread();
  }

  // Request painting so the changes to the UI drawer list are reflected as
  // quickly as possible.
  // RequestUIPaintFromUIThread is not enough, because a paint request is also
  // needed if disabling the UI, to force paint a frame without the UI as soon
  // as possible - it can't be dropped if ui_drawers_ is empty.
  // The coarse painting availability (and thus the availability of `window_`,
  // which is required for the paint mode to be anything else than kNone) has
  // already been checked above.
  ForceUIThreadPaintTick();
  window_->RequestPaint();
}

void Presenter::UpdateUITicksNeededFromUIThread() {
#if REX_PLATFORM_WIN32
  bool new_needed = AreUITicksNeededFromUIThread();
  if (dxgi_ui_ticks_needed_ == new_needed) {
    return;
  }
  bool signal_dxgi_ui_tick_control;
  {
    std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
    dxgi_ui_ticks_needed_ = new_needed;
    signal_dxgi_ui_tick_control = AreDXGIUITicksWaitable(dxgi_ui_tick_lock);
  }
  if (signal_dxgi_ui_tick_control) {
    dxgi_ui_tick_control_condition_.notify_all();
  }
#endif
}

void Presenter::WaitForUITickFromUIThread() {
#if REX_PLATFORM_WIN32
  if (!AreUITicksNeededFromUIThread()) {
    return;
  }
  std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  uint64_t last_vblank_before_wait = dxgi_ui_tick_last_vblank_;
  while (true) {
    // Guest output present requests should interrupt the wait as quickly as
    // possible as they should be fulfilled as early as possible.
    if (dxgi_ui_tick_force_requested_) {
      dxgi_ui_tick_force_requested_ = false;
      return;
    }
    if (!AreDXGIUITicksWaitable(dxgi_ui_tick_lock)) {
      return;
    }
    if (dxgi_ui_tick_last_vblank_ > dxgi_ui_tick_last_draw_) {
      // If there have been multiple vblanks during the wait for some reason,
      // next time draw the UI immediately.
      dxgi_ui_tick_last_draw_ =
          std::min(last_vblank_before_wait + uint64_t(1), dxgi_ui_tick_last_vblank_);
      return;
    }
    dxgi_ui_tick_signal_condition_.wait(dxgi_ui_tick_lock);
  }
#endif  // XE_PLATFORM
}

void Presenter::ForceUIThreadPaintTick() {
#if REX_PLATFORM_WIN32
  std::scoped_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  dxgi_ui_tick_force_requested_ = true;
#endif  // XE_PLATFORM
}

#if REX_PLATFORM_WIN32
Microsoft::WRL::ComPtr<IDXGIOutput> Presenter::GetDXGIOutputForMonitor(IDXGIFactory1* factory,
                                                                       HMONITOR monitor) {
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  for (UINT adapter_index = 0;
       SUCCEEDED(factory->EnumAdapters(adapter_index, adapter.ReleaseAndGetAddressOf()));
       ++adapter_index) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT output_index = 0; SUCCEEDED(adapter->EnumOutputs(output_index, &output));
         ++output_index) {
      DXGI_OUTPUT_DESC output_desc;
      if (SUCCEEDED(output->GetDesc(&output_desc)) && output_desc.Monitor == monitor) {
        return std::move(output);
      }
    }
  }
  return nullptr;
}

void Presenter::DXGIUITickThread() {
  std::unique_lock<std::mutex> dxgi_ui_tick_lock(dxgi_ui_tick_mutex_);
  while (true) {
    if (dxgi_ui_tick_thread_shutdown_) {
      return;
    }
    if (!AreDXGIUITicksWaitable(dxgi_ui_tick_lock)) {
      dxgi_ui_tick_control_condition_.wait(dxgi_ui_tick_lock);
      continue;
    }
    // Wait for vertical blank, with the mutex unlocked (holding a new reference
    // to the current output while it's happening) so subscribers can still do
    // early-out checks.
    bool wait_succeeded;
    {
      Microsoft::WRL::ComPtr<IDXGIOutput> dxgi_output = dxgi_ui_tick_output_;
      dxgi_ui_tick_lock.unlock();
      wait_succeeded = SUCCEEDED(dxgi_ui_tick_output_->WaitForVBlank());
    }
    dxgi_ui_tick_lock.lock();
    if (wait_succeeded) {
      ++dxgi_ui_tick_last_vblank_;
    } else {
      // Lost the ability to wait for a vertical blank on this output, notify
      // the waiting threads, and wait for a new one.
      dxgi_ui_tick_output_.Reset();
    }
    dxgi_ui_tick_signal_condition_.notify_all();
  }
}
#endif  // XE_PLATFORM

}  // namespace ui
}  // namespace rex
