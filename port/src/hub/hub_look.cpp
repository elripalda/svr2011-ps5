// WWE SmackDown vs. Raw 2011 - the PS5 hub's look (see hub_ui.h): glass, the red glowing focus,
// gradients, the line icons, and the side rail with the game's logo at its top. Every page draws
// with these, so the hub looks the same throughout.
#include <algorithm>
#include <cmath>
#include <map>

#include "hub_style.h"
#include "hub_ui.h"

namespace svr2011::hub {

namespace {

constexpr float kPi = 3.14159265f;

std::string Unquoted(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return v;
}

// The side rail: its groups and pages (the Tab enum is in this order).
struct RailEntry {
  int tab;
  const char* label;
  int icon;
};
struct RailGroup {
  const char* title;
  std::vector<RailEntry> entries;
};
const std::vector<RailGroup>& Rail() {
  static const std::vector<RailGroup> groups = {
      {"PLAY", {{kHome, "Home", kIconHome}, {kOnlineTab, "Online", kIconOnline}}},
      {"MODS", {{kModsTab, "My Mods", kIconMods}, {kDiscoverTab, "Discover", kIconDiscover},
                {kDownloadsTab, "Downloads", kIconDownload}}},
      {"CUSTOMIZE", {{kRosterTab, "Roster", kIconRoster}, {kPacksTab, "Textures", kIconTextures},
                     {kMusicTab, "Music", kIconMusic}, {kMoviesTab, "Movies", kIconMovies},
                     {kPaintTab, "Paint Tool", kIconPaint}}},
      {"SYSTEM", {{kSettingsTab, "Settings", kIconSettings}, {kSavesTab, "Saves", kIconSaves},
                  {kDlcTab, "DLC", kIconDlc}, {kAboutTab, "About", kIconAbout}}},
  };
  return groups;
}

// The rail's layout (virtual pixels).
constexpr float kRailItemX = 24, kRailItemW = kRailW - 48, kRailItemH = 42, kRailPitch = 44;
constexpr float kLogoTop = 34, kLogoW = 284, kLogoH = 104, kGroupsTop = 176, kGroupGap = 16, kGroupHead = 28;

// Each page's row top in the rail.
float RailRowY(int tab) {
  float y = kGroupsTop;
  for (const RailGroup& g : Rail()) {
    y += kGroupHead;
    for (const RailEntry& e : g.entries) {
      if (e.tab == tab) return y;
      y += kRailPitch;
    }
    y += kGroupGap;
  }
  return y;
}

}  // namespace

// ------------------------------------------------------------------ shapes

void Hub::GradRect(float x, float y, float w, float h, ImU32 top, ImU32 bottom, float r, bool across) {
  if (w <= 0 || h <= 0) return;
  const int v0 = dl_->VtxBuffer.Size;
  dl_->AddRectFilled(P(x, y), P(x + w, y + h), IM_COL32_WHITE, r * s_);
  const ImVec2 a = P(x, y);
  const float span = (across ? w : h) * s_;
  for (int i = v0; i < dl_->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl_->VtxBuffer[i];
    const float t = std::clamp(((across ? v.pos.x - a.x : v.pos.y - a.y)) / span, 0.0f, 1.0f);
    // (the anti-aliased fringe keeps its own fade: its alpha times the gradient's)
    const float fringe = float((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    v.col = WithAlpha(Mix(top, bottom, t), fringe);
  }
}

// A soft light (or shadow) around a rounded rectangle: layers that fade as they spread.
void Hub::Glow(float x, float y, float w, float h, float r, ImU32 col, float spread) {
  const float base = float((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
  if (base <= 0.004f || spread <= 0) return;
  constexpr int kLayers = 7;
  for (int i = kLayers; i >= 1; --i) {
    const float e = spread * float(i) / kLayers;
    const float k = 1.0f - float(i - 1) / kLayers;
    dl_->AddRectFilled(P(x - e, y - e), P(x + w + e, y + h + e), WithAlpha(col, 0.17f * k * k),
                       (r + e) * s_);
  }
}

// Glass: a dark, see-through pane with a lit top edge.
void Hub::Glass(float x, float y, float w, float h, float r, float k) {
  GradRect(x, y, w, h, Rgba(30, 31, 39, int(150 * k)), Rgba(14, 15, 20, int(170 * k)), r);
  GradRect(x, y, w, h, White(0.07f * k), White(0.015f * k), r);
  dl_->AddRect(P(x, y), P(x + w, y + h), White(0.09f * k), r * s_, 0, 1.0f * s_);
  if (w > 2 * r + 4) dl_->AddLine(P(x + r, y + 1), P(x + w - r, y + 1), White(0.13f * k), 1.0f * s_);
}

// An overlay's panel (a question, a list to pick from, the keyboard...): glass over a dark shadow.
void Hub::GlassPanel(float x, float y, float w, float h, float r) {
  Glow(x, y + 18, w, h, r, Black(0.85f), 70);
  GradRect(x, y, w, h, Rgba(32, 33, 42, 246), Rgba(17, 18, 23, 250), r);
  dl_->AddRect(P(x, y), P(x + w, y + h), White(0.12f), r * s_, 0, 1.0f * s_);
  dl_->AddLine(P(x + r, y + 1), P(x + w - r, y + 1), White(0.16f), 1.0f * s_);
}

// The focus: SmackDown vs. Raw red with a gloss, glowing.
void Hub::FocusFill(float x, float y, float w, float h, float r, float glow) {
  if (glow > 0.01f) Glow(x, y, w, h, r, WithAlpha(kAccent, std::min(1.0f, 0.95f * glow)), 30);
  GradRect(x, y, w, h, kAccentHi, kAccentLo, r);
  if (w > 2 * r + 4) dl_->AddLine(P(x + r, y + 1.5f), P(x + w - r, y + 1.5f), White(0.42f), 1.2f * s_);
}

void Hub::Ring(float x, float y, float w, float h, float r, float a) {
  dl_->AddRect(P(x + 1, y + 1), P(x + w - 1, y + h - 1), White(0.92f * a), (r - 1) * s_, 0, 2.2f * s_);
}

// Text centred on its capitals' middle (icons and words line up).
void Hub::TextMid(ImFont* f, float size, float x, float cy, ImU32 col, const std::string& t, float align, float spacing) {
  if (!f) f = ImGui::GetFont();
  static std::map<ImFont*, float> mid;  // (the capitals' middle, as a fraction of the size)
  auto it = mid.find(f);
  if (it == mid.end()) {
    float m = 0.5f;
    if (ImFontBaked* b = f->GetFontBaked(f->LegacySize))
      if (const ImFontGlyph* g = b->FindGlyphNoFallback('H'); g && b->Size > 0) m = (g->Y0 + g->Y1) * 0.5f / b->Size;
    it = mid.emplace(f, m).first;
  }
  Text(f, size, x, cy - it->second * size, col, t, align, spacing);
}

// A progress ring around (cx, cy).
void Hub::Progress(float cx, float cy, float radius, float frac, float thick, ImU32 track, bool glow) {
  dl_->AddCircle(P(cx, cy), radius * s_, track, 48, thick * s_);
  frac = std::clamp(frac, 0.0f, 1.0f);
  if (frac <= 0.002f) return;
  const float a0 = -kPi * 0.5f, a1 = a0 + frac * 2 * kPi;
  if (glow)
    for (int i = 3; i >= 1; --i) {
      dl_->PathArcTo(P(cx, cy), radius * s_, a0, a1, 48);
      dl_->PathStroke(WithAlpha(kAccent, 0.12f), 0, (thick + i * 5) * s_);
    }
  dl_->PathArcTo(P(cx, cy), radius * s_, a0, a1, 48);
  dl_->PathStroke(kAccent, 0, thick * s_);
}

// Four bars that bounce: something playing.
void Hub::Equalizer(float x, float bottom, float h, ImU32 col) {
  for (int b = 0; b < 4; ++b) {
    const float k = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(time_ * (7.0f + b * 2.3f) + b * 1.7f));
    Rect(x + b * 9, bottom - h * k, 5, h * k, col, 2);
  }
}

// ------------------------------------------------------------------ icons

void Hub::Icon(int icon, float cx, float cy, float size, ImU32 col, float thick) {
  const float k = size / 24.0f, ox = cx - 12 * k, oy = cy - 12 * k;
  const float t = thick * s_ * std::max(0.75f, k);
  auto Q = [&](float x, float y) { return P(ox + x * k, oy + y * k); };
  auto line = [&](std::initializer_list<ImVec2> pts, bool closed = false) {
    for (const ImVec2& p : pts) dl_->PathLineTo(Q(p.x, p.y));
    dl_->PathStroke(col, closed ? ImDrawFlags_Closed : 0, t);
  };
  auto circle = [&](float x, float y, float r) { dl_->AddCircle(Q(x, y), r * k * s_, col, 32, t); };
  auto dot = [&](float x, float y, float r) { dl_->AddCircleFilled(Q(x, y), r * k * s_, col, 12); };
  auto rect = [&](float x0, float y0, float x1, float y1, float r) {
    dl_->AddRect(Q(x0, y0), Q(x1, y1), col, r * k * s_, 0, t);
  };
  switch (icon) {
    case kIconHome:
      line({{3, 11}, {12, 4}, {21, 11}});
      line({{5.5f, 9.5f}, {5.5f, 20}, {18.5f, 20}, {18.5f, 9.5f}});
      line({{10, 20}, {10, 14.5f}, {14, 14.5f}, {14, 20}});
      break;
    case kIconOnline:
      circle(12, 12, 9);
      line({{3, 12}, {21, 12}});
      dl_->AddEllipse(Q(12, 12), ImVec2(4.2f * k * s_, 9 * k * s_), col, 0, 32, t);
      break;
    case kIconMods:
      line({{12, 3}, {21, 8}, {12, 13}, {3, 8}}, true);
      line({{3, 12.5f}, {12, 17.5f}, {21, 12.5f}});
      line({{3, 17}, {12, 22}, {21, 17}});
      break;
    case kIconDiscover:
      circle(12, 12, 9);
      line({{15.5f, 8.5f}, {13.5f, 13.5f}, {8.5f, 15.5f}, {10.5f, 10.5f}}, true);
      break;
    case kIconDownload:
      line({{12, 4}, {12, 15}});
      line({{7, 10}, {12, 15}, {17, 10}});
      line({{5, 20}, {19, 20}});
      break;
    case kIconDown:
      line({{6, 9}, {12, 15}, {18, 9}});
      break;
    case kIconRoster:
      circle(9, 8, 3.5f);
      dl_->PathArcTo(Q(9, 21), 6.5f * k * s_, kPi * 1.08f, kPi * 1.92f, 20);
      dl_->PathStroke(col, 0, t);
      dl_->PathArcTo(Q(16, 8), 3.5f * k * s_, -kPi * 0.5f, kPi * 0.5f, 12);
      dl_->PathStroke(col, 0, t);
      dl_->PathArcTo(Q(16.5f, 21), 5 * k * s_, kPi * 1.45f, kPi * 1.95f, 12);
      dl_->PathStroke(col, 0, t);
      break;
    case kIconTextures:
    case kIconImage:
      rect(3, 4, 21, 20, 2.5f);
      circle(9, 10, 2);
      line({{21, 16}, {16, 11}, {7, 20}});
      break;
    case kIconMusic:
      line({{9, 18}, {9, 5}, {20, 3}, {20, 16}});
      circle(6, 18, 3);
      circle(17, 16, 3);
      break;
    case kIconMovies:
      rect(3, 4, 21, 20, 2.5f);
      line({{7.5f, 4}, {7.5f, 20}});
      line({{16.5f, 4}, {16.5f, 20}});
      line({{3, 9.5f}, {7.5f, 9.5f}});
      line({{3, 14.5f}, {7.5f, 14.5f}});
      line({{16.5f, 9.5f}, {21, 9.5f}});
      line({{16.5f, 14.5f}, {21, 14.5f}});
      break;
    case kIconPaint:
      line({{18, 3}, {21, 6}, {11.5f, 15.5f}, {8.5f, 12.5f}}, true);
      dl_->PathLineTo(Q(8.5f, 12.5f));
      dl_->PathBezierCubicCurveTo(Q(5.5f, 12.5f), Q(4, 15), Q(4, 17), 10);
      dl_->PathBezierCubicCurveTo(Q(4, 18.5f), Q(3, 19.5f), Q(2, 20), 6);
      dl_->PathBezierCubicCurveTo(Q(7, 20.5f), Q(10.5f, 19), Q(10.5f, 15.5f), 10);
      dl_->PathStroke(col, 0, t);
      break;
    case kIconSettings:
      line({{4, 6}, {14, 6}});
      line({{18, 6}, {20, 6}});
      line({{4, 12}, {8, 12}});
      line({{12, 12}, {20, 12}});
      line({{4, 18}, {16, 18}});
      circle(16, 6, 2);
      circle(10, 12, 2);
      circle(18, 18, 2);
      break;
    case kIconSaves:
      line({{5, 3}, {16, 3}, {20, 7}, {20, 19}, {18, 21}, {6, 21}, {4, 19}, {4, 5}}, true);
      line({{8, 3}, {8, 8}, {15, 8}, {15, 3}});
      line({{8, 21}, {8, 14}, {16, 14}, {16, 21}});
      break;
    case kIconDlc:
      line({{21, 8}, {12, 3}, {3, 8}, {12, 13}}, true);
      line({{3, 8}, {3, 16}, {12, 21}, {21, 16}, {21, 8}});
      line({{12, 13}, {12, 21}});
      break;
    case kIconAbout:
      circle(12, 12, 9);
      line({{12, 11}, {12, 17}});
      dot(12, 7.6f, 1.3f);
      break;
    case kIconSearch:
      circle(11, 11, 7);
      line({{16.2f, 16.2f}, {20.5f, 20.5f}});
      break;
    case kIconCheck:
      line({{5, 12.5f}, {10, 17.5f}, {19.5f, 7}});
      break;
    case kIconPlay:
      dl_->AddTriangleFilled(Q(6, 3.5f), Q(21, 12), Q(6, 20.5f), col);
      break;
    case kIconTrash:
      line({{4, 7}, {20, 7}});
      line({{9, 7}, {9, 4}, {15, 4}, {15, 7}});
      line({{6, 7}, {7, 20}, {17, 20}, {18, 7}});
      break;
    case kIconPower:
      line({{12, 3}, {12, 11}});
      dl_->PathArcTo(Q(12, 13), 8 * k * s_, -kPi * 0.25f, kPi * 1.25f, 28);
      dl_->PathStroke(col, 0, t);
      break;
    case kIconStar: {
      for (int i = 0; i < 10; ++i) {
        const float a = -kPi * 0.5f + i * kPi / 5, r = (i % 2) ? 4.2f : 9.5f;
        dl_->PathLineTo(Q(12 + std::cos(a) * r, 12.5f + std::sin(a) * r));
      }
      dl_->PathStroke(col, ImDrawFlags_Closed, t);
      break;
    }
    case kIconArena:  // a ring: the posts and the ropes
      line({{4, 6}, {4, 19}});
      line({{20, 6}, {20, 19}});
      line({{4, 9}, {20, 9}});
      line({{4, 12.5f}, {20, 12.5f}});
      line({{4, 16}, {20, 16}});
      line({{2, 20.5f}, {22, 20.5f}});
      break;
    case kIconBolt:
      line({{13, 2}, {5, 13.5f}, {11, 13.5f}, {10, 22}, {19, 10}, {13, 10}}, true);
      break;
    case kIconFolder:
      line({{3, 6}, {9, 6}, {11, 8.5f}, {21, 8.5f}, {21, 19}, {3, 19}}, true);
      break;
  }
}

// ------------------------------------------------------------------ the side rail

void Hub::DrawRail() {
  Ease(rail_in_, 1, 9);
  const float slide = (1 - Smooth(rail_in_)) * -40;
  const int v0 = dl_->VtxBuffer.Size;
  ox_ += slide * s_;
  // the pane: glass from top to bottom, a shadow to its right
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float left = -ox_ / s_;  // (to the screen's edge, past the 16:9 area)
  dl_->AddRectFilledMultiColor(P(kRailW, 0), P(kRailW + 90, kH), Black(0.35f), Black(0), Black(0), Black(0.35f));
  GradRect(left, -2, kRailW - left, kH + 4, Rgba(20, 21, 28, 214), Rgba(9, 10, 13, 236));
  dl_->AddLine(P(kRailW, 0), P(kRailW, kH), White(0.07f), 1.0f * s_);
  (void)size;

  // the game's logo, top left
  if (a_.logo) {
    float lw = kLogoW, lh = lw * a_.logo.height / a_.logo.width;
    if (lh > kLogoH) lh = kLogoH, lw = lh * a_.logo.width / a_.logo.height;
    const float lx = 38 + (kLogoW - lw) * 0.5f, ly = kLogoTop + (kLogoH - lh) * 0.5f;
    for (int i = 0; i < 8; ++i) {  // (a soft shadow under it)
      const float a = float(i) * 0.785f;
      dl_->AddImage(a_.logo.id, P(lx + std::cos(a) * 5, ly + std::sin(a) * 5 + 5),
                    P(lx + lw + std::cos(a) * 5, ly + lh + std::sin(a) * 5 + 5), ImVec2(0, 0), ImVec2(1, 1), Black(0.12f));
    }
    dl_->AddImage(a_.logo.id, P(lx, ly), P(lx + lw, ly + lh));
  } else {
    Text(a_.display, 44, 40, kLogoTop + 10, kText, "SMACKDOWN");
    Text(a_.display, 36, 40, kLogoTop + 58, kAccent, "VS RAW 2011");
  }
  GradRect(38, kGroupsTop - 22, kLogoW, 1.2f, WithAlpha(kAccent, 0.9f), White(0.10f), 0, true);

  // where the focus is (eased): the page shown, or the controller's place on the rail
  const float target = RailRowY(tab_);
  Ease(rail_fy_, target, 18);
  if (rail_focus_) {
    const float push = press_ * 3;
    FocusFill(kRailItemX + push, rail_fy_ + push * 0.5f, kRailItemW - push * 2, kRailItemH - push, 10);
    Ring(kRailItemX + push, rail_fy_ + push * 0.5f, kRailItemW - push * 2, kRailItemH - push, 10, 0.85f);
  } else {
    GradRect(kRailItemX, rail_fy_, kRailItemW, kRailItemH, White(0.13f), White(0.02f), 10, true);
    Glow(kRailItemX, rail_fy_ + 9, 3, kRailItemH - 18, 1.5f, WithAlpha(kAccent, 0.9f), 7);
    Rect(kRailItemX, rail_fy_ + 9, 3, kRailItemH - 18, kAccent, 1.5f);
  }

  float y = kGroupsTop;
  for (const RailGroup& g : Rail()) {
    Text(a_.body_bold, 13, kRailItemX + 16, y + 2, White(0.40f), g.title, 0, 3.2f);
    y += kGroupHead;
    for (const RailEntry& e : g.entries) {
      const bool here = e.tab == tab_;
      const ImU32 col = here ? kText : White(0.64f);
      Icon(e.icon, kRailItemX + 16 + 11, y + kRailItemH * 0.5f, 22, col, 1.9f);
      TextMid(a_.body_bold, 20, kRailItemX + 54, y + kRailItemH * 0.5f, col, e.label);
      if (e.tab == kDownloadsTab) {  // a download going: its ring and percent
        float frac = 0;
        int waiting = 0, ready = 0;
        if (DownloadStatus(&frac, &waiting, &ready)) {
          const float rx = kRailItemX + kRailItemW - 16;
          const std::string pct = std::to_string(int(frac * 100 + 0.5f)) + "%";
          TextMid(a_.body_bold, 16, rx, y + kRailItemH * 0.5f, kText, pct, 1);
          Progress(rx - TextWidth(a_.body_bold, 16, pct) - 18, y + kRailItemH * 0.5f, 9, frac, 3,
                   White(here && rail_focus_ ? 0.35f : 0.16f), false);
        } else if (ready > 0) {
          const float rx = kRailItemX + kRailItemW - 16;
          const std::string n = std::to_string(ready);
          const float w = std::max(22.0f, TextWidth(a_.body_bold, 14, n) + 14);
          Glow(rx - w, y + 10, w, 22, 11, WithAlpha(kAccent, 0.6f), 8);
          Rect(rx - w, y + 10, w, 22, kAccent, 11);
          TextMid(a_.body_bold, 14, rx - w * 0.5f, y + 21, kText, n, 0.5f);
        }
      }
      y += kRailPitch;
    }
    y += kGroupGap;
  }

  // who's playing, and whether online is set up
  const float cy = 986, ch = 62;
  Glass(kRailItemX, cy, kRailItemW, ch, 14, 0.8f);
  dl_->AddCircleFilled(P(kRailItemX + 36, cy + ch * 0.5f), 21 * s_, Rgba(46, 48, 58), 32);
  dl_->AddCircle(P(kRailItemX + 36, cy + ch * 0.5f), 21 * s_, White(0.16f), 32, 1.2f * s_);
  TextMid(a_.display, 22, kRailItemX + 36, cy + ch * 0.5f, kText, player_.empty() ? "P" : player_.substr(0, 1), 0.5f);
  const bool online = Value("online_enabled", "false") == "true" && !Unquoted(Value("online_token", "")).empty();
  TextMid(a_.body_bold, 17, kRailItemX + 70, cy + 22, kText, Fit(a_.body_bold, 17, player_, kRailItemW - 86));
  dl_->AddCircleFilled(P(kRailItemX + 74, cy + 42), 3.5f * s_, online ? kAccent : White(0.3f), 12);
  TextMid(a_.body, 14, kRailItemX + 84, cy + 42, White(0.55f), online ? "Online, signed in" : "Playing offline");

  ox_ -= slide * s_;
  Fade(v0, Smooth(rail_in_));
}

// The rail with the controller on it: up / down go through the pages (each shows as the focus
// reaches it); right or Cross go into the page; Circle goes Home.
void Hub::RailInput(uint32_t p) {
  int t = tab_;
  if (p & kUp) t = std::max(0, t - 1);
  if (p & kDown) t = std::min(kTabCount - 1, t + 1);
  if (t != tab_) {
    GoTo(t, t > tab_ ? 1.0f : -1.0f);
    Play(Sound::kMove);
    return;
  }
  if (p & (kUp | kDown)) return;
  if (p & (kRight | kCross)) {
    rail_focus_ = false;
    press_ = 0.6f;
    if (tab_ == kHome) home_sel_ = 0;
    if (tab_ == kDiscoverTab) DiscoverEnsure();
    Play(Sound::kSelect);
    return;
  }
  if ((p & kCircle) && tab_ != kHome) {
    GoTo(kHome, -1);
    rail_focus_ = false;
    home_sel_ = 0;
    Play(Sound::kBack);
  }
}

// A page's tabs (L1 / R1), under its title: the names, each with its count, and an underline
// that glides to the one shown.
void Hub::PageTabs(const std::vector<std::pair<std::string, int>>& tabs, int sel) {
  constexpr float kSize = 25, kSpacing = 2.2f, kGap = 34, kY = 194;
  float x = kPageX;
  Glyph(kL1, x + 20, kY, White(0.6f));
  x += 40 + kGap;
  float sx = x, sw = 0;
  for (int i = 0; i < int(tabs.size()); ++i) {
    const bool on = i == sel;
    const std::string& name = tabs[size_t(i)].first;
    const float w = TextWidth(a_.nav, kSize, name, kSpacing);
    TextMid(a_.nav, kSize, x, kY, on ? kText : White(0.5f), name, 0, kSpacing);
    float total = w;
    if (tabs[size_t(i)].second >= 0) {
      const std::string n = std::to_string(tabs[size_t(i)].second);
      TextMid(a_.body_bold, 17, x + w + 9, kY + 1, on ? White(0.6f) : White(0.4f), n);
      total += 9 + TextWidth(a_.body_bold, 17, n);
    }
    if (on) sx = x, sw = total;
    x += total + kGap;
  }
  Glyph(kR1, x + 20 - 4, kY, White(0.6f));
  const float before = sub_x_;
  Ease(sub_x_, sx, 16);
  Ease(sub_w_, sw, 22);
  const float travel = std::min(50.0f, std::fabs(sx - before) * 0.25f);
  Glow(sub_x_ - travel * 0.5f - 4, kY + 22, sub_w_ + travel + 8, 3, 1.5f, WithAlpha(kAccent, 0.8f), 9);
  Rect(sub_x_ - travel * 0.5f - 4, kY + 22, sub_w_ + travel + 8, 3, kAccent, 1.5f);
}

}  // namespace svr2011::hub
