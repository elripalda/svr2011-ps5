// WWE SmackDown vs. Raw 2011 - the PS5 hub on the console (ps5_hub.cpp).
#pragma once

#if defined(__PROSPERO__)
#include <functional>
#include <string>

#include "hub_ui.h"

struct ImFontAtlas;
struct ImFont;
namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class Window;
}  // namespace rex::ui

namespace svr2011::hub {
// The hub's fonts (from OnConfigureFonts).
void ConfigureFonts(ImFontAtlas* atlas);
ImFont* MenuFont();
ImFont* TitleFont();
// Shows the hub; `launch` runs (on the UI thread) on LAUNCH GAME.
void Show(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const Paths& paths, std::string player,
          std::function<void()> launch);
// The loading screen after LAUNCH GAME (opened by the hub): what's being set
// up (`fraction` < 0: no known end), painted now and then from that work on
// the UI thread, and faded out over the game when it's done.
void SetWindow(rex::ui::Window* window);
void StartupStep(const std::string& step, const std::string& detail = {}, float fraction = -1);
void PaintStartupScreen();
void CloseStartupScreen();
}  // namespace svr2011::hub
#endif
