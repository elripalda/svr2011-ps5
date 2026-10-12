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
}  // namespace rex::ui

namespace svr2011::hub {
// The hub's fonts (from OnConfigureFonts).
void ConfigureFonts(ImFontAtlas* atlas);
ImFont* MenuFont();
ImFont* TitleFont();
// Shows the hub; `launch` runs (on the UI thread) on LAUNCH GAME.
void Show(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate, const Paths& paths, std::string player,
          std::function<void()> launch);
}  // namespace svr2011::hub
#endif
