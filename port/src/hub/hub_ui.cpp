// WWE SmackDown vs. Raw 2011 - the PS5 hub (see hub_ui.h): the frame, the
// Home, Settings, Mods, DLC and About pages, and the controls. (The other
// pages: hub_pages.cpp, hub_online.cpp and hub_setup.cpp.)
#include "hub_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "hub_style.h"

namespace svr2011::hub {

namespace fs = std::filesystem;

// (hub_ui.h's tab ids: the pages in hub_pages.cpp and hub_setup.cpp use them)
static const char* const kTabNames[kTabCount] = {"HOME",  "SETTINGS", "MODS",   "ROSTER", "TEXTURES", "MUSIC",  "MOVIES",
                                                 "PAINT", "SAVES",    "ONLINE", "DLC",    "ABOUT"};

namespace {

// A mod folder type as a heading: "MatchTypes" -> "MATCH TYPES".
std::string TypeHeading(const std::string& type) {
  std::string out;
  for (size_t i = 0; i < type.size(); ++i) {
    const char c = type[i];
    if (i && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(type[i - 1])))
      out += ' ';
    out += char(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string Unquote(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return v;
}

}  // namespace

// ----------------------------------------------------------------- settings

struct Hub::Setting {
  const char* section;
  const char* key;
  const char* label;
  const char* help;
  bool quoted;                      // a TOML string
  std::vector<const char*> values;  // as written to the file
  std::vector<const char*> labels;
  const char* fallback;             // the value when the file has none
};

static const std::vector<Hub::Setting>& Settings() {
  static const std::vector<Hub::Setting> rows = {
      {"DISPLAY", "native_max_scale", "Resolution",
       "How sharp the game is drawn. 4K matches what the PS5 sends to the TV. Auto draws above 4K when there is "
       "room, for the cleanest picture.",
       false, {"1", "2", "3", "4"}, {"720p (Original)", "1440p", "4K", "Auto"}, "3"},
      {"DISPLAY", "native_aa", "Anti-Aliasing", "Smooths jagged edges. Auto picks a level that suits the resolution.",
       false, {"0", "1", "2", "3", "4"}, {"Auto", "Off", "2x", "3x", "4x"}, "0"},
      {"DISPLAY", "frame_rate", "Frame Rate", "60 fps is smoother. The game runs at the same speed either way.", false,
       {"60", "30"}, {"60 fps", "30 fps"}, "60"},
      {"DISPLAY", "vsync", "VSync", "Waits for the TV, so the picture never tears.", false, {"true", "false"},
       {"On", "Off"}, "true"},
      {"DISPLAY", "show_fps", "FPS Counter", "Shows the frame rate at the top of the screen.", false, {"true", "false"},
       {"On", "Off"}, "true"},
      {"DISPLAY", "native_texture_quality", "Textures", "Medium and Low use smaller textures to save memory.", true,
       {"high", "medium", "low"}, {"High", "Medium", "Low"}, "high"},
      {"DISPLAY", "native_prepare_pipelines", "Prepare Graphics",
       "Builds the game's shaders while you are in the menus, so matches don't stutter the first time.", false,
       {"true", "false"}, {"On", "Off"}, "true"},
      {"DISPLAY", "depth_of_field", "Depth of Field", "Far shots blur what is out of focus, as the game was made.",
       false, {"true", "false"}, {"On", "Off"}, "true"},
      {"DISPLAY", "motion_blur", "Motion Blur", "The game's radial blur and after-image trails on big moves.", false,
       {"true", "false"}, {"On", "Off"}, "true"},
      {"DISPLAY", "soft_filter", "Soft Filter",
       "The game's own soft filter over the picture. It was made for 720p and blurs wide shots at higher "
       "resolutions.",
       false, {"true", "false"}, {"On", "Off"}, "false"},
      {"GAME", "user_language", "Language", "Menus and on-screen text. The commentary is always in English.", false,
       {"1", "4", "3", "5", "6"}, {"English", "Fran\xC3\xA7" "ais", "Deutsch", "Espa\xC3\xB1ol", "Italiano"}, "1"},
      {"GAME", "unlock_everything", "Unlock Everything",
       "Every superstar, arena and item from the start. Turn it off to unlock them as you play. DLC stays either way.",
       false, {"true", "false"}, {"On", "Off"}, "true"},
      {"GAME", "replays", "Replays", "Instant replays after finishers and at the end of a match.", false,
       {"true", "false"}, {"On", "Off"}, "true"},
      {"GAME", "mixed_gender_matches", "Mixed-Gender Matches",
       "Divas and male superstars can be picked for the same match.", false, {"true", "false"}, {"On", "Off"}, "true"},
      {"GAME", "managers_tile", "Managers Tile",
       "On the character select screen, the \"?\" tile opens the managers (Paul Bearer, The Hurricane...) instead of "
       "a random pick.",
       false, {"true", "false"}, {"On", "Off"}, "true"},
      {"GAME", "skip_intros", "Skip Intro Movies", "Go straight to the title screen.", false, {"true", "false"},
       {"On", "Off"}, "false"},
      {"GAME", "skip_training", "Skip Practice Ring", "Skip the practice ring behind the title screen.", false,
       {"true", "false"}, {"On", "Off"}, "false"},
      {"HUB", "hub_music_volume", "Menu Music", "How loud the music behind the hub is. R3 plays the next song.", false,
       {"0", "10", "20", "30", "40", "50", "60", "70", "80", "90", "100"},
       {"Off", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"}, "80"},
  };
  return rows;
}

// --------------------------------------------------------------------- data

void Hub::Init(const Paths& paths, const Assets& assets, std::string player, std::string version) {
  paths_ = paths;
  a_ = assets;
  player_ = Upper(player.empty() ? std::string("PLAYER") : player);
  version_ = std::move(version);
  Load();
}

void Hub::Load() {
  config_.clear();
  std::ifstream in(paths_.config);
  for (std::string line; std::getline(in, line);) {
    const size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line[a] == '#') continue;
    if (line[a] == '[') break;  // (top-level keys only)
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(a, eq - a), v = line.substr(eq + 1);
    k.erase(k.find_last_not_of(" \t") + 1);
    v.erase(0, v.find_first_not_of(" \t"));
    if (const size_t hash = v.find(" #"); hash != std::string::npos && v[0] != '"') v.erase(hash);
    v.erase(v.find_last_not_of(" \t\r") + 1);
    config_.emplace_back(k, v);
  }
  LoadMods();
  LoadDlc();
  LoadRoster();
  LoadPacks();
  LoadMusic();
  LoadMovies();
  LoadSaves();
  // (the disc images etc. are looked for only when the game isn't installed yet)
  state_ = install::Look(paths_.game);
  // (the game's files come from the app store; the bundled mods are set up here, once)
  if (install::BundledPending(paths_.game)) StartBundled();
  saves_ = 0;
  std::error_code ec;
  for (const auto& e : install::ListDir(paths_.saves)) {
    const std::string n = e.path().filename().string();
    if (e.is_regular_file(ec) && !n.empty() && n[0] != '.') ++saves_;
  }
  std::ifstream pt(paths_.game / "playtime.txt");
  for (std::string line; std::getline(pt, line);) {
    if (line.rfind("seconds=", 0) == 0) played_seconds_ = std::strtoull(line.c_str() + 8, nullptr, 10);
    if (line.rfind("sessions=", 0) == 0) sessions_ = uint32_t(std::strtoul(line.c_str() + 9, nullptr, 10));
  }
}

void Hub::LoadMods() {
  mods_.clear();
  LoadDrops();
  std::error_code ec;
  for (const auto& type : install::ListDir(paths_.game / "Mods")) {
    if (!type.is_directory(ec) || type.path().filename().string().rfind('.', 0) == 0) continue;  // (.incoming)
    for (const auto& dir : install::ListDir(type.path())) {
      if (!dir.is_directory(ec)) continue;
      Mod m;
      m.folder = dir.path();
      std::ifstream man(dir.path() / "manifest.txt");
      for (std::string line; std::getline(man, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        k.erase(k.find_last_not_of(" \t") + 1);
        v.erase(0, v.find_first_not_of(" \t"));
        v.erase(v.find_last_not_of(" \t\r") + 1);
        if (k == "id") m.id = v;
        else if (k == "name") m.name = v;
        else if (k == "author") m.author = v;
        else if (k == "version") m.version = v;
      }
      if (m.name.empty()) m.name = dir.path().filename().string();
      m.type = type.path().filename().string();  // (grouped by their folder: the manifests say "arena", "Arenas"...)
      m.enabled = !fs::exists(dir.path() / "disabled", ec);
      m.bytes = FolderBytes(dir.path());
      mods_.push_back(std::move(m));
    }
  }
  std::sort(mods_.begin(), mods_.end(), [](const Mod& a, const Mod& b) {
    return a.type != b.type ? a.type < b.type : Upper(a.name) < Upper(b.name);
  });
}

void Hub::LoadDrops() {
  drops_.clear();
  for (const fs::path& f : install::FindMods(paths_.game)) {
    Drop d;
    d.file = f;
    std::error_code ec;
    d.bytes = fs::file_size(f, ec);
    install::PeekMod(f, &d.info);
    drops_.push_back(std::move(d));
  }
}

void Hub::LoadDlc() {
  dlc_.clear();
  std::error_code ec;
  const fs::path title_dir = paths_.game / "UserData" / "0000000000000000" / "5451085D";
  const fs::path installed = title_dir / "00000002", headers = title_dir / "Headers" / "00000002";
  // (an install the app was closed in the middle of)
  for (const auto& e : install::ListDir(installed))
    if (const std::string f = e.path().filename().string();
        e.is_directory(ec) && f.size() > 11 && f.compare(f.size() - 11, 11, ".installing") == 0)
      fs::remove_all(e.path(), ec);
  for (const auto& e : install::ListDir(paths_.game / "DLC")) {
    if (!e.is_regular_file(ec)) continue;
    Dlc d;
    d.path = e.path();
    d.file = e.path().filename().string();
    d.bytes = e.file_size(ec);
    d.name = d.file;
    if (const size_t p = d.name.rfind(" ("); p != std::string::npos && d.name.back() == ')' &&
        d.name.find_first_not_of("0123456789", p + 2) == d.name.size() - 1)
      d.name.resize(p);
    // installed: its folder, and the .header the game lists it by (the game's
    // own install at start names the folder after the file as it is)
    d.installed = (fs::is_directory(installed / d.name, ec) && fs::exists(headers / (d.name + ".header"), ec)) ||
                  (fs::is_directory(installed / d.file, ec) && fs::exists(headers / (d.file + ".header"), ec));
    // the STFS header: magic, content type (0x344: 2 = DLC), title (0x360), display name (0x411, UTF-16BE)
    {
      std::ifstream f(e.path(), std::ios::binary);
      unsigned char head[0x491] = {};
      const bool read = bool(f.read(reinterpret_cast<char*>(head), sizeof(head)));
      const bool stfs = read && (!std::memcmp(head, "LIVE", 4) || !std::memcmp(head, "PIRS", 4) ||
                                 !std::memcmp(head, "CON ", 4));
      auto be32 = [&](size_t o) {
        return uint32_t(head[o]) << 24 | uint32_t(head[o + 1]) << 16 | uint32_t(head[o + 2]) << 8 | head[o + 3];
      };
      d.other = !stfs || be32(0x344) != 2 || be32(0x360) != 0x5451085D;
      for (size_t k = 0x411; stfs && k + 1 < sizeof(head); k += 2) {
        const unsigned c = unsigned(head[k]) << 8 | head[k + 1];
        if (!c) break;
        if (c < 0x80) d.title += char(c);
        else if (c < 0x800) d.title += char(0xC0 | c >> 6), d.title += char(0x80 | (c & 0x3F));
        else d.title += char(0xE0 | c >> 12), d.title += char(0x80 | (c >> 6 & 0x3F)), d.title += char(0x80 | (c & 0x3F));
      }
      if (d.title.empty()) d.title = stfs ? "Content package " + d.name.substr(0, 8) : d.file;
    }
    dlc_.push_back(std::move(d));
  }
  std::sort(dlc_.begin(), dlc_.end(), [](const Dlc& a, const Dlc& b) {
    return a.other != b.other ? b.other : a.file < b.file;
  });
}

std::vector<Hub::Dlc> Hub::DlcToInstall() const {
  std::vector<Dlc> out;
  for (const Dlc& d : dlc_)
    if (!d.installed && !d.other && !dlc_failed_.count(d.file)) out.push_back(d);
  return out;
}

std::string Hub::Value(const std::string& key, const std::string& fallback) const {
  for (const auto& [k, v] : config_)
    if (k == key) return Unquote(v);
  return fallback;
}

void Hub::Save(const std::string& key, const std::string& value) {
  bool found = false;
  for (auto& [k, v] : config_)
    if (k == key) v = value, found = true;
  if (!found) config_.emplace_back(key, value);
  // The file: that key's line replaced (or added before the first [section]).
  std::vector<std::string> lines;
  {
    std::ifstream in(paths_.config);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  const std::string entry = key + " = " + value;
  size_t insert_at = lines.size();
  bool done = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& l = lines[i];
    const size_t first = l.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (l[first] == '[') {
      insert_at = std::min(insert_at, i);
      break;
    }
    const size_t eq = l.find('=');
    if (eq == std::string::npos || l[first] == '#') continue;
    std::string k = l.substr(first, eq - first);
    k.erase(k.find_last_not_of(" \t") + 1);
    if (k == key) {
      lines[i] = entry;
      done = true;
      break;
    }
  }
  if (!done) lines.insert(lines.begin() + std::ptrdiff_t(insert_at), entry);
  const fs::path tmp = paths_.config.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    for (const std::string& l : lines) out << l << "\n";
  }
  std::error_code ec;
  fs::rename(tmp, paths_.config, ec);
}

// ------------------------------------------------------------------ drawing

float Hub::Ease(float& v, float target, float speed) {
  v = v < -0.5f ? target : v + (target - v) * (1.0f - std::exp(-dt_ * speed));
  return v;
}

float& Hub::Knob(int item, bool on) {
  auto [it, fresh] = knobs_.try_emplace(tab_ << 16 | item, on ? 1.0f : 0.0f);
  Ease(it->second, on ? 1.0f : 0.0f, 14);
  return it->second;
}

void Hub::Fade(int first_vertex, float alpha) {
  if (alpha >= 0.999f) return;
  for (int i = first_vertex; i < dl_->VtxBuffer.Size; ++i) {
    ImU32& c = dl_->VtxBuffer[i].col;
    c = WithAlpha(c, alpha);
  }
}

void Hub::Text(ImFont* f, float size, float x, float y, ImU32 col, const std::string& t, float align, float spacing) {
  if (!f) f = ImGui::GetFont();
  float px = x - TextWidth(f, size, t, spacing) * align;
  if (spacing <= 0) {
    dl_->AddText(f, size * s_, P(px, y), col, t.c_str());
    return;
  }
  // (letter by letter: a UTF-8 letter's bytes stay together)
  for (size_t i = 0; i < t.size();) {
    size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(t[i]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    const char* a = t.c_str() + i;
    const char* b = t.c_str() + std::min(t.size(), i + n);
    dl_->AddText(f, size * s_, P(px, y), col, a, b);
    px += f->CalcTextSizeA(size, FLT_MAX, 0, a, b).x + spacing;
    i += n;
  }
}

float Hub::TextWidth(ImFont* f, float size, const std::string& t, float spacing) const {
  if (!f) f = ImGui::GetFont();
  if (spacing <= 0) return f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str()).x;
  float w = 0;
  for (size_t i = 0; i < t.size();) {
    size_t n = 1;
    const unsigned char c = static_cast<unsigned char>(t[i]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    w += f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str() + i, t.c_str() + std::min(t.size(), i + n)).x + spacing;
    i += n;
  }
  return t.empty() ? 0 : w - spacing;
}

std::string Hub::Fit(ImFont* f, float size, const std::string& t, float width) const {
  if (TextWidth(f, size, t) <= width) return t;
  std::string s = t;
  while (!s.empty() && TextWidth(f, size, s + "...") > width) {
    s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // (whole UTF-8 letters)
  }
  return s + "...";
}

// Text broken into lines of at most `width`; returns the height used.
float Hub::Wrapped(ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t, float line) {
  std::istringstream words(t);
  std::string word, cur;
  float yy = y;
  while (words >> word) {
    const std::string next = cur.empty() ? word : cur + " " + word;
    if (!cur.empty() && TextWidth(f, size, next) > width) {
      Text(f, size, x, yy, col, cur);
      yy += size * line;
      cur = word;
    } else {
      cur = next;
    }
  }
  if (!cur.empty()) Text(f, size, x, yy, col, Fit(f, size, cur, width)), yy += size * line;
  return yy - y;
}

void Hub::Rect(float x, float y, float w, float h, ImU32 col, float r) {
  dl_->AddRectFilled(P(x, y), P(x + w, y + h), col, r * s_);
}

// The DualSense's buttons: the pictures in hub/buttons, else drawn in one colour.
void Hub::Glyph(uint32_t button, float cx, float cy, ImU32 col) {
  if (auto it = a_.buttons.find(button); it != a_.buttons.end() && it->second) {
    const float h = 24;  // (the pictures' buttons fill about two thirds of them)
    dl_->AddImage(it->second.id, P(cx - h, cy - h), P(cx + h, cy + h), ImVec2(0, 0), ImVec2(1, 1), col);
    return;
  }
  const ImVec2 c = P(cx, cy);
  const float R = 15 * s_, k = 6.2f * s_, t = 2.2f * s_;
  switch (button) {
    case kCross:
    case kCircle:
    case kSquare:
    case kTriangle:
      dl_->AddCircle(c, R, col, 40, 1.6f * s_);
      if (button == kCross) {
        dl_->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, t);
        dl_->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, t);
      } else if (button == kCircle) {
        dl_->AddCircle(c, k * 1.1f, col, 28, t);
      } else if (button == kSquare) {
        dl_->AddRect(ImVec2(c.x - k * 0.95f, c.y - k * 0.95f), ImVec2(c.x + k * 0.95f, c.y + k * 0.95f), col, 0, 0, t);
      } else {
        dl_->AddTriangle(ImVec2(c.x, c.y - k * 1.15f), ImVec2(c.x + k * 1.1f, c.y + k * 0.8f),
                         ImVec2(c.x - k * 1.1f, c.y + k * 0.8f), col, t);
      }
      break;
    case kLeft | kRight: {
      const float a = 7 * s_, gap = 5 * s_;
      dl_->AddTriangleFilled(ImVec2(c.x - gap, c.y - a), ImVec2(c.x - gap, c.y + a), ImVec2(c.x - gap - a * 1.2f, c.y), col);
      dl_->AddTriangleFilled(ImVec2(c.x + gap, c.y - a), ImVec2(c.x + gap, c.y + a), ImVec2(c.x + gap + a * 1.2f, c.y), col);
      break;
    }
    case kOptions:
      dl_->AddRect(ImVec2(c.x - 13 * s_, c.y - 11 * s_), ImVec2(c.x + 13 * s_, c.y + 11 * s_), col, 6 * s_, 0, 1.6f * s_);
      for (int i = -1; i <= 1; ++i)
        dl_->AddLine(ImVec2(c.x - 6 * s_, c.y + i * 4.5f * s_), ImVec2(c.x + 6 * s_, c.y + i * 4.5f * s_), col, 1.8f * s_);
      break;
    case kL1:
    case kR1:
      dl_->AddRect(ImVec2(c.x - 20 * s_, c.y - 13 * s_), ImVec2(c.x + 20 * s_, c.y + 13 * s_), col, 7 * s_, 0, 1.6f * s_);
      Text(a_.body_bold, 15, cx, cy - 9.5f, col, button == kL1 ? "L1" : "R1", 0.5f);
      break;
  }
}

// One hint at the bottom, right-aligned at `right`: the button, then what it does.
float Hub::Hint(float right, uint32_t button, const char* label) {
  const float tw = TextWidth(a_.body_bold, 20, label);
  const float gw = (button == kL1 || button == kR1) ? 40 : 30;
  const float x = right - tw;
  Text(a_.body_bold, 20, x, 1010, White(0.78f), label);
  Glyph(button, x - 12 - gw * 0.5f, 1023, White(0.9f));
  return x - 12 - gw - 40;
}

// A switch; `on` slides from 0 (off) to 1 (on).
void Hub::Switch(float x, float y, float on, bool focused) {
  const float w = 58, h = 32;
  const ImU32 off = focused ? Black(0.16f) : White(0.18f);
  Rect(x, y, w, h, Mix(off, kAccent, on), h * 0.5f);
  const float t = Smooth(on);
  const float kx = x + h * 0.5f + (w - h) * t;
  const float stretch = 4 * std::sin(t * 3.14159f);  // (the knob stretches a little while it moves)
  dl_->AddRectFilled(P(kx - 12.5f - stretch * 0.5f, y + 3.5f), P(kx + 12.5f + stretch * 0.5f, y + h - 3.5f), White(1),
                     12.5f * s_);
}

void Hub::PageTitle(const char* title, const std::string& subtitle) {
  Text(a_.display, 60, 112, 132, kText, title, 0, 1);
  Text(a_.body, 22, 114, 214, White(0.6f), subtitle);
}

void Hub::Empty(const char* title, const std::string& text) {
  Text(a_.display, 40, kListX, kListY + 40, kText, title);
  Wrapped(a_.body, 23, kListX, kListY + 110, 820, White(0.65f), text);
}

void Hub::List(std::vector<Row>& rows, int sel, float rh, const std::function<void(int, float, float)>& draw) {
  const float hh = 50, view = kListView, x = kListX, w = kListW, y0 = kListY;
  float yy = 0;
  int sel_row = -1;
  for (size_t k = 0; k < rows.size(); ++k) {
    Row& r = rows[k];
    if (r.item < 0) {
      yy += k ? 14 : 0;
      r.y = yy;
      yy += hh;
    } else {
      r.y = yy;
      if (r.item == sel) sel_row = int(k);
      yy += rh;
    }
  }
  const float total = yy;
  if (sel_row < 0) return;
  // keep the focused row in view (with its heading, when it's the first under it)
  {
    float top = rows[size_t(sel_row)].y, bottom = top + rh;
    if (sel_row > 0 && rows[size_t(sel_row) - 1].item < 0) top = rows[size_t(sel_row) - 1].y;
    float target = scroll_px_;
    if (top < target) target = top;
    if (bottom > target + view) {
      // (to the first row or heading that keeps it in view: no row cut in half at the top)
      target = bottom - view;
      for (const Row& r : rows)
        if (r.y >= target - 0.5f) {
          target = r.y;
          break;
        }
    }
    scroll_px_ = std::clamp(target, 0.0f, std::max(0.0f, total - view));
  }
  Ease(scroll_f_, scroll_px_, 18);
  Ease(focus_y_, rows[size_t(sel_row)].y, 20);
  dl_->PushClipRect(P(0, y0 - 2), P(kW, y0 + view), true);
  const float push = press_ * 4;
  Rect(x + push, y0 + focus_y_ - scroll_f_ + push * 0.5f, w - push * 2, rh - 6 - push, kText, 10);
  for (size_t k = 0; k < rows.size(); ++k) {
    const Row& row = rows[k];
    const float ry = y0 + row.y - scroll_f_;
    if (ry < y0 - rh - 20 || ry > y0 + view + 20) continue;
    if (row.item < 0) {
      Text(a_.body_bold, 15, x + 24, ry + 20, White(0.45f), row.heading, 0, 3);
      continue;
    }
    const float over = std::clamp(1 - std::fabs(focus_y_ - row.y) / (rh * 0.7f), 0.0f, 1.0f);
    // (a thin line between rows that sit together)
    if (k + 1 < rows.size() && rows[k + 1].item >= 0)
      Rect(x + 24, ry + rh - 3, w - 48, 1, White(0.06f * (1 - over)));
    draw(row.item, ry, over);
  }
  dl_->PopClipRect();
  if (total > view) {  // where the list is
    const float bh = view * view / total;
    const float by = y0 + (view - bh) * scroll_f_ / (total - view);
    Rect(x + w + 16, y0, 3, view - 6, White(0.08f), 1.5f);
    Rect(x + w + 16, by, 3, bh - 6, White(0.5f), 1.5f);
  }
}

void Hub::Panel(const std::function<void(float, float, float)>& draw) {
  const int v0 = dl_->VtxBuffer.Size;
  draw(kPanelX + (1 - Smooth(panel_in_)) * 16, kPanelY, kPanelW);
  Fade(v0, Smooth(panel_in_));
}

float Hub::Facts(float x, float y, float w, const std::vector<std::pair<std::string, std::string>>& facts) {
  for (const auto& [k, v] : facts) {
    Text(a_.body_bold, 15, x, y + 5, White(0.45f), k, 0, 2.5f);
    y += std::max(44.0f, Wrapped(a_.body_bold, 22, x + 160, y, w - 160, White(0.82f), v) + 14);
  }
  return y;
}

void Hub::DrawBackdrop() {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Rgba(8, 9, 11));
  if (a_.hero) {
    // The picture covers the screen, slowly zooming in and out (a 50 s
    // breath) and drifting sideways with the pages.
    const float breath = 0.5f - 0.5f * std::cos(time_ * 6.2832f / 50.0f);
    const float zoom = 1.04f + 0.06f * breath;
    Ease(hero_x_, (float(tab_) / float(kTabCount - 1) - 0.5f) * 0.024f, 3);
    const float sa = size.x / size.y, ia = a_.hero.width / a_.hero.height;
    float uw = 1, vh = 1;
    if (ia > sa) uw = sa / ia;
    else vh = ia / sa;
    uw /= zoom, vh /= zoom;
    const float room = (1 - uw) * 0.5f;
    const float cx = 0.5f + std::clamp(hero_x_ + 0.004f * std::sin(time_ * 0.07f), -room, room);
    const ImVec2 uv0(cx - uw * 0.5f, 0.42f - vh * 0.42f), uv1(cx + uw * 0.5f, 0.42f + vh * 0.58f);
    dl_->AddImage(a_.hero.id, ImVec2(0, 0), size, uv0, uv1);
  }
  // The pages over it: the picture steps back
  Ease(shade_, tab_ == kHome ? 0.0f : 0.84f, 7);
  if (shade_ > 0.003f) dl_->AddRectFilled(ImVec2(0, 0), size, Black(shade_));
  // (on the home page, the lower left darkens a little under the logo)
  if (shade_ < 0.6f) {
    const float k = 1 - shade_ / 0.6f;
    dl_->AddRectFilledMultiColor(ImVec2(0, P(0, 330).y), ImVec2(P(1200, 0).x, size.y), Black(0), Black(0), Black(0),
                                 Black(0.7f * k));
  }
  // the top bar and the bottom stay readable over a bright picture
  dl_->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, P(0, 230).y), Black(0.72f), Black(0.72f), Black(0), Black(0));
  const float fade_from = 430 + 330 * std::clamp(shade_ / 0.84f, 0.0f, 1.0f);
  dl_->AddRectFilledMultiColor(ImVec2(0, P(0, fade_from).y), ImVec2(size.x, size.y), Black(0), Black(0), Black(0.94f),
                               Black(0.94f));
}

void Hub::DrawTopBar() {
  // the pages, between L1 and R1
  constexpr float kSize = 26, kSpacing = 2.2f, kGap = 40;
  float x = 112;
  Glyph(kL1, x + 20, 70, White(0.55f));
  x += 70;
  float sel_x = 0, sel_w = 0;
  for (int i = 0; i < kTabCount; ++i) {
    const bool sel = i == tab_;
    const float w = TextWidth(a_.nav, kSize, kTabNames[i], kSpacing);
    Text(a_.nav, kSize, x, 52, sel ? kText : White(0.5f), kTabNames[i], 0, kSpacing);
    if (sel) sel_x = x, sel_w = w;
    x += w + kGap;
  }
  Glyph(kR1, x - kGap + 34, 70, White(0.55f));
  // the underline glides to the page, stretching on the way
  const float before = tab_x_;
  Ease(tab_x_, sel_x, 16);
  Ease(tab_w_, sel_w, 22);
  const float travel = std::min(60.0f, std::fabs(sel_x - before) * 0.25f);
  Rect(tab_x_ - travel * 0.5f, 89, tab_w_ + travel, 4, kAccent, 2);
}

void Hub::DrawHints() {
  float r = 1808;
  if (kb_.open) {
    r = Hint(r, kCircle, "Cancel");
    r = Hint(r, kOptions, "Done");
    r = Hint(r, kL1, "Shift");
    r = Hint(r, kTriangle, "Space");
    r = Hint(r, kSquare, "Delete");
    r = Hint(r, kCross, "Type");
    return;
  }
  if (confirm_) {
    if (confirm_yes_ != "OK") r = Hint(r, kCircle, "Cancel");
    r = Hint(r, kCross, confirm_yes_.c_str());
    return;
  }
  if (choice_) {
    r = Hint(r, kCircle, "Cancel");
    r = Hint(r, kCross, "Choose");
    return;
  }
  if (br_.open) r = BrowserHints(r);
  if (!br_.open) {
  if (tab_ != kHome && state_.Installed()) r = Hint(r, kOptions, "Start Game");
  if (tab_ != kHome) r = Hint(r, kCircle, "Home");
  switch (tab_) {
    case kHome: r = Hint(r, kCross, "Start Game"); break;
    case kSettingsTab: r = Hint(r, kLeft | kRight, "Change"); break;
    case kModsTab:
      r = Hint(r, kSquare, "Find Mods");
      if (item_[tab_] < int(drops_.size())) r = Hint(r, kCross, "Install");
      else if (!mods_.empty()) r = Hint(r, kCross, "Turn On / Off");
      break;
    case kPaintTab:
      if (paint_ok_) {
        r = Hint(r, kTriangle, "Save All as PNG");
        r = Hint(r, kCross, "Choose");
      }
      break;
    case kRosterTab: r = Hint(r, kLeft | kRight, "Category"); break;
    case kPacksTab:
      if (!packs_.empty()) {
        r = Hint(r, kTriangle, "Higher Priority");
        r = Hint(r, kCross, "Turn On / Off");
      }
      break;
    case kMusicTab:
      if (!songs_.empty()) r = Hint(r, kCross, previewing_ >= 0 ? "Stop" : "Listen");
      break;
    case kSavesTab: r = Hint(r, kCross, item_[tab_] == 0 ? "Back Up" : "Restore"); break;
    case kOnlineTab: r = Hint(r, kCross, item_[tab_] == 2 ? "Turn On / Off" : item_[tab_] == 3 ? "Change" : "Select"); break;
    case kDlcTab:
      if (task_ && task_kind_ == 6) break;
      if (DlcToInstall().size() > 1 && dlc_install_) r = Hint(r, kTriangle, "Install All");
      if (!dlc_.empty() && dlc_install_) {
        const Dlc& d = dlc_[size_t(std::clamp(item_[tab_], 0, int(dlc_.size()) - 1))];
        if (!d.other) r = Hint(r, kCross, d.installed ? "Options" : "Install");
      }
      break;
  }
  }
  // a message (bottom left, rising in), or the version
  if (toast_time_ > 0) {
    const float a = Smooth(std::min(toast_time_ * 3, (3.0f - toast_time_) * 6));
    const float w = TextWidth(a_.body_bold, 20, toast_) + 44;
    const float y = 1002 + (1 - a) * 14;
    Rect(112, y, w, 42, White(0.14f * a), 21);
    dl_->AddCircleFilled(P(132, y + 21), 4.5f * s_, WithAlpha(kAccent, a), 16);
    Text(a_.body_bold, 20, 146, y + 8, White(0.92f * a), toast_);
  } else if (const std::string status = !DlcStatus().empty() && tab_ != kDlcTab ? DlcStatus() : BundledStatus();
             !status.empty()) {
    // (the first start: a pulsing dot while the bundled mods are set up)
    dl_->AddCircleFilled(P(118, 1023), 4.5f * s_, WithAlpha(kAccent, 0.5f + 0.5f * std::sin(time_ * 6)), 16);
    Text(a_.body_bold, 18, 134, 1012, White(0.7f), status);
  } else {
    Text(a_.body, 18, 112, 1012, White(0.32f), "SmackDown vs. Raw Recomp " + version_ + "  \xC2\xB7  PlayStation 5");
  }
}

// A question over everything (restore a backup?), rising in.
void Hub::DrawConfirm() {
  Ease(confirm_in_, confirm_ ? 1.0f : 0.0f, 14);
  if (confirm_in_ < 0.01f) return;
  const float a = Smooth(confirm_in_);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.6f * a));
  const float w = 760, x = (kW - w) * 0.5f;
  const int v0 = dl_->VtxBuffer.Size;
  const float y = 360 + (1 - a) * 30;
  // (its height: the text's lines, wrapped as Wrapped does)
  int lines = 0;
  {
    std::istringstream words(confirm_text_);
    std::string word, cur;
    while (words >> word) {
      const std::string next = cur.empty() ? word : cur + " " + word;
      if (!cur.empty() && TextWidth(a_.body, 22, next) > w - 96) ++lines, cur = word;
      else cur = next;
    }
    if (!cur.empty()) ++lines;
  }
  const float h = 120 + lines * 22 * 1.4f + 48;
  Rect(x, y, w, h, Rgba(24, 25, 29), 16);
  dl_->AddRectFilled(P(x, y), P(x + 6, y + h), kAccent, 16 * s_, ImDrawFlags_RoundCornersLeft);
  Text(a_.display, 40, x + 48, y + 44, kText, confirm_title_);
  Wrapped(a_.body, 22, x + 48, y + 120, w - 96, White(0.72f), confirm_text_);
  Fade(v0, a);
}

// ------------------------------------------------------------------- pages

void Hub::DrawHome() {
  // first open: the logo rises in, then the button
  const float in_logo = Smooth((time_ - 0.15f) / 0.8f), in_btn = Smooth((time_ - 0.45f) / 0.6f);
  // (a wider logo - 2.6:1 - is drawn wider: the same area on the screen)
  const float lx = 104, lw = a_.logo && a_.logo.width > a_.logo.height * 2.4f ? 700.0f : 640.0f;
  int v0 = dl_->VtxBuffer.Size;
  if (a_.logo) {
    const float lh = lw * a_.logo.height / a_.logo.width;
    const float ly = 862 - 36 - lh + (1 - in_logo) * 40;
    for (int i = 0; i < 8; ++i) {  // a soft shadow under it
      const float a = float(i) * 0.785f, d = 10;
      const float dx = std::cos(a) * d, dy = std::sin(a) * d + 8;
      dl_->AddImage(a_.logo.id, P(lx + dx, ly + dy), P(lx + lw + dx, ly + lh + dy), ImVec2(0, 0), ImVec2(1, 1), Black(0.1f));
    }
    dl_->AddImage(a_.logo.id, P(lx, ly), P(lx + lw, ly + lh));
  } else {
    Text(a_.display, 96, lx, 640, kText, "SMACKDOWN VS RAW");
  }
  Fade(v0, in_logo);

  // START GAME: breathes softly; pressed, it gives a little and flashes
  v0 = dl_->VtxBuffer.Size;
  const float push = press_ * 6;
  const float bx = 112 + push, by = 862 + push + (1 - in_btn) * 24, bw = 420 - push * 2, bh = 84 - push * 2;
  const float pulse = 0.5f + 0.5f * std::sin(time_ * 2.4f);
  const float ring = 5 + 3 * pulse;
  dl_->AddRect(P(bx - ring, by - ring), P(bx + bw + ring, by + bh + ring), White(0.14f + 0.2f * pulse), (12 + ring) * s_, 0,
               2 * s_);
  Rect(bx, by, bw, bh, Mix(kText, kAccent, press_ * 0.9f), 12);
  const ImU32 ink = Mix(kInk, kText, press_);
  const float tx = bx + 40;
  dl_->AddTriangleFilled(P(tx, by + bh * 0.5f - 15), P(tx + 26, by + bh * 0.5f), P(tx, by + bh * 0.5f + 15), ink);
  Text(a_.display, 40, tx + 48, by + bh * 0.5f - 23, ink, "START GAME", 0, 2);
  Fade(v0, in_btn);

}

int Hub::MusicVolume() const {
  return std::clamp(std::atoi(Value("hub_music_volume", "80").c_str()), 0, 100);
}

int Hub::SettingIndex(const Setting& r) const {
  const std::string v = Value(r.key, r.fallback);
  for (size_t k = 0; k < r.values.size(); ++k)
    if (v == r.values[k]) return int(k);
  for (size_t k = 0; k < r.values.size(); ++k)
    if (std::string(r.fallback) == r.values[k]) return int(k);
  return 0;
}

void Hub::DrawSettings() {
  PageTitle("SETTINGS", "Saved as you change them. They take effect when the game starts.");
  const auto& set = Settings();
  const int sel = std::clamp(item_[tab_], 0, int(set.size()) - 1);
  std::vector<Row> rows;
  for (int i = 0; i < int(set.size()); ++i) {
    if (i == 0 || std::strcmp(set[size_t(i)].section, set[size_t(i) - 1].section)) rows.push_back({-1, set[size_t(i)].section});
    rows.push_back({i});
  }
  const float x = kListX, w = kListW;
  List(rows, sel, 54, [&](int i, float yy, float over) {
    const auto& r = set[size_t(i)];
    Text(a_.body_bold, 23, x + 24, yy + 11, Mix(White(0.92f), kInk, over), r.label);
    const std::string v = r.labels[size_t(SettingIndex(r))];
    if (std::strcmp(r.key, "hub_music_volume") == 0) {
      // a slider: the bar fills to the volume, the value beside it
      const float frac = float(SettingIndex(r)) / float(r.values.size() - 1);
      auto& shown = knobs_[kSettingsTab << 16 | (0x8000 + i)];
      Ease(shown, frac, 16);
      const float bw = 260, br = x + w - 24 - 74, bx = br - bw, cy = yy + 24;
      const ImU32 track = Mix(White(0.16f), Black(0.14f), over), fill = kAccent;
      Rect(bx, cy - 3, bw, 6, track, 3);
      Rect(bx, cy - 3, std::max(6.0f, bw * shown), 6, fill, 3);
      const float kx = bx + bw * shown;
      dl_->AddCircleFilled(P(kx, cy), (i == sel ? 11 : 8) * s_, i == sel ? Mix(White(1), kInk, over) : White(0.9f), 24);
      if (i == sel) dl_->AddCircle(P(kx, cy), 11 * s_, kAccent, 24, 2.5f * s_);
      Text(i == sel ? a_.body_bold : a_.body, 23, x + w - 24, yy + 11, Mix(White(i == sel ? 1.0f : 0.6f), kInk, over), v, 1);
      return;
    }
    if (i == sel) {
      // the value slides in from the side it was changed toward
      const float vr = x + w - 46, cy = yy + 24;
      const float slide = (1 - Smooth(value_in_)) * 18 * value_dir_;
      const int v0 = dl_->VtxBuffer.Size;
      Text(a_.body_bold, 23, vr + slide, yy + 11, kInk, v, 1);
      Fade(v0, 0.25f + 0.75f * Smooth(value_in_));
      const float vl = vr - TextWidth(a_.body_bold, 23, v) - 18;
      const float nudge_l = value_dir_ < 0 ? (1 - Smooth(value_in_)) * 6 : 0;
      const float nudge_r = value_dir_ > 0 ? (1 - Smooth(value_in_)) * 6 : 0;
      dl_->AddTriangleFilled(P(vl - nudge_l, cy - 7), P(vl - nudge_l, cy + 7), P(vl - 9 - nudge_l, cy), Black(0.75f * over));
      dl_->AddTriangleFilled(P(vr + 14 + nudge_r, cy - 7), P(vr + 14 + nudge_r, cy + 7), P(vr + 23 + nudge_r, cy),
                             Black(0.75f * over));
    } else {
      Text(a_.body, 23, x + w - 24, yy + 11, Mix(White(0.6f), kInk, over), v, 1);
    }
  });
  // the focused setting, explained
  const auto& r = set[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    Text(a_.display, 44, px, py, kText, r.label);
    py += 72;
    py += Wrapped(a_.body, 23, px, py, pw, White(0.68f), r.help);
    py += 34;
    const int cur = SettingIndex(r);
    if (std::strcmp(r.key, "hub_music_volume") == 0) {
      const float frac = float(cur) / float(r.values.size() - 1);
      const float shown = knobs_[kSettingsTab << 16 | (0x8000 + sel)];
      Text(a_.display, 64, px, py, kText, r.labels[size_t(cur)]);
      py += 110;
      Rect(px, py, pw, 10, White(0.14f), 5);
      Rect(px, py, std::max(10.0f, pw * shown), 10, kAccent, 5);
      const float pop = 1 + 0.4f * (1 - Smooth(value_in_));
      dl_->AddCircleFilled(P(px + pw * shown, py + 5), 15 * pop * s_, White(1), 28);
      Text(a_.body, 19, px, py + 30, White(0.45f), "Off");
      Text(a_.body, 19, px + pw, py + 30, White(0.45f), "100%", 1);
      (void)frac;
      return;
    }
    for (int k = 0; k < int(r.labels.size()); ++k) {
      const bool c = k == cur;
      const float pop = c ? 1 + 0.6f * (1 - Smooth(value_in_)) : 1;
      if (c) dl_->AddCircleFilled(P(px + 8, py + 14), 6 * pop * s_, kAccent, 20);
      else dl_->AddCircle(P(px + 8, py + 14), 5.5f * s_, White(0.3f), 20, 1.5f * s_);
      Text(c ? a_.body_bold : a_.body, 22, px + 30, py, c ? kText : White(0.45f), r.labels[size_t(k)]);
      py += 42;
    }
  });
}

void Hub::DrawMods() {
  int on = 0;
  for (const Mod& m : mods_) on += m.enabled;
  PageTitle("MODS", mods_.empty() ? std::string("Mods you add show up here.")
                                  : std::to_string(mods_.size()) + " installed, " + std::to_string(on) +
                                        " on. Changes take effect when the game starts.");
  if (mods_.empty() && drops_.empty()) {
    Empty("No mods yet",
          "Press Square to find .svrmod files on a USB drive or in any folder, and install them from there. You "
          "can also copy .svrmod files over FTP into /data/homebrew/PPSA92011/Mods/: they show up here.");
    return;
  }
  const int nd = int(drops_.size());
  const int n = nd + int(mods_.size());
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < nd; ++i) {
    if (i == 0) rows.push_back({-1, "READY TO INSTALL  " + std::to_string(nd)});
    rows.push_back({i});
  }
  for (int i = 0; i < int(mods_.size()); ++i) {
    if (i == 0 || mods_[size_t(i)].type != mods_[size_t(i) - 1].type) {
      int count = 0;
      for (const Mod& m : mods_) count += m.type == mods_[size_t(i)].type;
      rows.push_back({-1, TypeHeading(mods_[size_t(i)].type) + "  " + std::to_string(count)});
    }
    rows.push_back({nd + i});
  }
  const float x = kListX, w = kListW;
  List(rows, sel, 76, [&](int item, float ry, float over) {
    if (item < nd) {
      const Drop& d = drops_[size_t(item)];
      const std::string name = d.info.name.empty() ? d.file.stem().string() : d.info.name;
      Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 24, name, w - 220));
      std::string sub = d.info.version.empty() ? std::string() : "v" + d.info.version + "  \xC2\xB7  ";
      sub += Size(d.bytes) + "  \xC2\xB7  " + d.file.filename().string();
      Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), Fit(a_.body, 18, sub, w - 260));
      const bool working = task_ && task_kind_ == 3 && task_file_ == d.file;
      const char* st = working ? "Installing..." : "Not installed";
      const float sx = x + w - 24 - TextWidth(a_.body_bold, 18, st);
      dl_->AddCircleFilled(P(sx - 14, ry + 35), 4.5f * s_, WithAlpha(kAccent, 0.6f + 0.4f * std::sin(time_ * (working ? 9 : 3))), 16);
      Text(a_.body_bold, 18, sx, ry + 24, Mix(White(0.9f), Black(0.7f), over), st);
      return;
    }
    const int i = item - nd;
    const Mod& m = mods_[size_t(i)];
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(m.enabled ? 0.94f : 0.5f), kInk, over), Fit(a_.body_bold, 24, m.name, w - 140));
    std::string sub = m.version.empty() ? std::string() : "v" + m.version + "  \xC2\xB7  ";
    sub += Size(m.bytes);
    if (!m.author.empty()) sub += "  \xC2\xB7  " + m.author;
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), Fit(a_.body, 18, sub, w - 140));
    Switch(x + w - 24 - 58, ry + 19, Knob(i, m.enabled), over > 0.5f);
  });
  if (sel < nd) {
    const Drop& d = drops_[size_t(sel)];
    Panel([&](float px, float py, float pw) {
      py += Wrapped(a_.display, 44, px, py, pw, kText, d.info.name.empty() ? d.file.stem().string() : d.info.name, 1.15f) + 22;
      py = Facts(px, py, pw,
                 {{"STATUS", d.info.type.empty() ? "Can't be read" : "Not installed yet"},
                  {"TYPE", d.info.type.empty() ? "-" : d.info.type},
                  {"VERSION", d.info.version.empty() ? "-" : d.info.version},
                  {"AUTHOR", d.info.author.empty() ? "-" : d.info.author},
                  {"FILE", d.file.filename().string()}});
      Wrapped(a_.body, 21, px, py + 10, pw, White(0.55f),
              "Press Cross to install it. The file is removed once it is installed: the mod is then in its folder in "
              "Mods.");
    });
    return;
  }
  const Mod& m = mods_[size_t(sel - nd)];
  Panel([&](float px, float py, float pw) {
    py += Wrapped(a_.display, 44, px, py, pw, kText, m.name, 1.15f) + 22;
    Facts(px, py, pw,
          {{"STATUS", m.enabled ? "On" : "Off"}, {"TYPE", m.type}, {"VERSION", m.version.empty() ? "-" : m.version},
           {"AUTHOR", m.author.empty() ? "-" : m.author}, {"SIZE", Size(m.bytes)}});
  });
}

void Hub::DrawDlc() {
  if (task_ && task_kind_ == 6) {
    DrawDlcProgress();
    return;
  }
  int installed = 0, waiting = 0;
  for (const Dlc& d : dlc_) installed += d.installed, waiting += !d.installed && !d.other;
  std::string sub = dlc_.empty() ? std::string("Downloadable content you add shows up here.")
                                 : std::to_string(dlc_.size()) + (dlc_.size() == 1 ? " package, " : " packages, ") +
                                       std::to_string(installed) + " installed.";
  if (waiting) sub += dlc_install_ ? " Press Cross to install." : " New ones install when the game starts.";
  PageTitle("DLC", sub);
  if (dlc_.empty()) {
    Empty("No DLC yet",
          "Put your DLC files (the Xbox 360 content packages) in /data/homebrew/PPSA92011/DLC/ over FTP, then come back "
          "to this page to install them. They're extra: the game doesn't need them.");
    return;
  }
  const int n = int(dlc_.size());
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) rows.push_back({i});
  const float x = kListX, w = kListW;
  List(rows, sel, 76, [&](int i, float ry, float over) {
    const Dlc& d = dlc_[size_t(i)];
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 24, d.title, w - 260));
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), Size(d.bytes));
    const char* st = d.other ? "Not for this game" : d.installed ? "Installed" : "Not installed";
    const bool lit = !d.installed && !d.other;
    const float sx = x + w - 24 - TextWidth(a_.body_bold, 18, st);
    if (lit)
      dl_->AddCircleFilled(P(sx - 14, ry + 35), 4.5f * s_, WithAlpha(kAccent, 0.6f + 0.4f * std::sin(time_ * 3)), 16);
    Text(a_.body_bold, 18, sx, ry + 24, Mix(White(lit ? 0.9f : 0.55f), Black(0.7f), over), st);
  });
  const Dlc& d = dlc_[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    py += Wrapped(a_.display, 44, px, py, pw, kText, d.title, 1.15f) + 22;
    py = Facts(px, py, pw,
               {{"STATUS", d.other ? "Not for this game" : d.installed ? "Installed" : "Not installed"},
                {"SIZE", Size(d.bytes)},
                {"FILE", d.file.size() > 24 ? d.file.substr(0, 20) + "..." : d.file}});
    Wrapped(a_.body, 19, px, py + 10, pw, White(0.5f),
            d.other ? "This file isn't SmackDown vs. Raw 2011 DLC, so it's left alone."
            : d.installed
                ? "Cross to install it again, or to delete it."
                : "Cross installs it now, with progress. A pack that isn't installed here installs when the game "
                  "starts instead, on a black screen.");
  });
}

void Hub::DrawAbout() {
  PageTitle("ABOUT", "SmackDown vs. Raw Recomp for PlayStation 5  \xC2\xB7  " + version_);
  struct Credit {
    const char* who;
    const char* what;
  };
  const Credit people[] = {
      {"KaikoClanworth1", "Created SmackDown vs. Raw Recomp, the PC port this is built on, and leads the project."},
      {"@elripalda", "Brought it to PlayStation 5: the PS5 port and this hub."},
      {"gitSothib", "Groundwork for the online sessions."},
      {"ReXGlue", "The static recompiler and runtime that turn the Xbox 360 game into a native program."},
      {"Xenia", "The Xbox 360 emulator the runtime's code comes from."},
  };
  float y = 276;
  for (const Credit& c : people) {
    Text(a_.display, 32, 112, y, kText, c.who);
    Wrapped(a_.body, 21, 112, y + 46, 860, White(0.62f), c.what);
    y += 112;
  }
  const float px = kPanelX;
  float py = 286;
  Text(a_.body_bold, 15, px, py, White(0.45f), "BUILT WITH", 0, 3);
  py += 40;
  const char* parts[] = {"PS5 Payload SDK", "Mesa RADV (Vulkan)", "plume and XenosRecomp", "Dear ImGui",
                         "stb_image, stb_vorbis, dr_mp3, dr_flac and miniz", "Oswald and Inter fonts (SIL Open Font License)"};
  for (const char* part : parts) {
    Text(a_.body, 21, px, py, White(0.78f), part);
    py += 36;
  }
  py += 30;
  Text(a_.body_bold, 15, px, py, White(0.45f), "PLEASE NOTE", 0, 3);
  py += 40;
  Wrapped(a_.body, 19, px, py, 680, White(0.55f),
          "An unofficial fan project, not affiliated with or endorsed by WWE, THQ, Yuke's, Microsoft or Sony. WWE "
          "SmackDown vs. Raw 2011 and all related names belong to their owners. No game files are included: you need "
          "your own copy of the game.");
}

// --------------------------------------------------------------- behaviour

int Hub::Items() const {
  switch (tab_) {
    case kSettingsTab: return int(Settings().size());
    case kModsTab: return int(drops_.size() + mods_.size());
    case kPaintTab: return paint_ok_ ? paint::kSlots : 0;
    case kRosterTab: return int(roster_.size());
    case kPacksTab: return packs_.empty() ? 0 : int(packs_.size()) + 1;  // (+ Dump Textures)
    case kMusicTab: return int(songs_.size());
    case kMoviesTab: return int(movies_.size());
    case kSavesTab: return 1 + int(backups_.size()) + int(save_files_.size());
    case kOnlineTab: return 4;
    case kDlcTab: return int(dlc_.size());
    default: return 0;
  }
}

void Hub::Activate(int dir) {
  const int item = item_[tab_];
  switch (tab_) {
    case kSettingsTab: {
      const auto& r = Settings()[size_t(item)];
      const int n = int(r.values.size());
      const bool slider = std::strcmp(r.key, "hub_music_volume") == 0;
      int idx = (SettingIndex(r) + dir + n) % n;
      if (slider) {  // (a slider stops at its ends)
        idx = std::clamp(SettingIndex(r) + dir, 0, n - 1);
        if (idx == SettingIndex(r)) {
          Play(Sound::kError);
          break;
        }
        if (audio_.music_volume) audio_.music_volume(std::atoi(r.values[size_t(idx)]));
      }
      Save(r.key, r.quoted ? std::string("\"") + r.values[size_t(idx)] + "\"" : std::string(r.values[size_t(idx)]));
      value_in_ = 0;
      value_dir_ = float(dir);
      Play(Sound::kSelect);
      break;
    }
    case kModsTab: {
      if (item < int(drops_.size())) {
        if (!Busy()) Play(Sound::kSelect), StartModInstall(drops_[size_t(item)].file);
        break;
      }
      if (item - int(drops_.size()) >= int(mods_.size())) break;
      Mod& m = mods_[size_t(item - int(drops_.size()))];
      std::error_code ec;
      if (m.enabled) std::ofstream(m.folder / "disabled") << "";
      else fs::remove(m.folder / "disabled", ec);
      m.enabled = !fs::exists(m.folder / "disabled", ec);
      Play(m.enabled ? Sound::kOn : Sound::kOff);
      Toast(m.name + (m.enabled ? " is on" : " is off"));
      break;
    }
    case kRosterTab: {
      if (item >= int(roster_.size())) break;
      Character& c = roster_[size_t(item)];
      c.category = (c.category + (dir == 0 ? 1 : dir) + 5) % 5;
      SaveRoster();
      value_in_ = 0;
      value_dir_ = float(dir == 0 ? 1 : dir);
      Play(Sound::kSelect);
      break;
    }
    case kPacksTab: {
      if (item == int(packs_.size())) {  // Dump Textures
        const bool on = Value("native_dump_textures", "false") != "true";
        Save("native_dump_textures", on ? "true" : "false");
        Play(on ? Sound::kOn : Sound::kOff);
        break;
      }
      if (item >= int(packs_.size())) break;
      Pack& p = packs_[size_t(item)];
      if (dir == 0) {  // on / off (a pack turned on goes last: the lowest priority)
        if (p.order >= 0) {
          for (Pack& o : packs_)
            if (o.order > p.order) --o.order;
          p.order = -1;
        } else {
          int last = -1;
          for (const Pack& o : packs_) last = std::max(last, o.order);
          p.order = last + 1;
        }
        Play(p.order >= 0 ? Sound::kOn : Sound::kOff);
        Toast(p.name + (p.order >= 0 ? " is on" : " is off"));
      } else if (p.order > 0) {  // higher priority: swaps with the one above
        for (Pack& o : packs_)
          if (o.order == p.order - 1) ++o.order;
        --p.order;
        Play(Sound::kSelect);
        Toast(p.name + ": priority " + std::to_string(p.order + 1));
      } else {
        Play(Sound::kError);
      }
      SavePacks();
      break;
    }
    case kMusicTab: {
      if (item >= int(songs_.size())) break;
      const Song& s = songs_[size_t(item)];
      if (previewing_ == item) {
        if (audio_.stop_preview) audio_.stop_preview();
        previewing_ = -1;
        Play(Sound::kBack);
      } else if (!s.playable) {
        Play(Sound::kError);
        Toast("The PS5 can't play this file: use MP3, WAV, FLAC or OGG");
      } else if (audio_.preview && audio_.preview(s.file)) {
        previewing_ = item;
        Play(Sound::kSelect);
      } else {
        Play(Sound::kError);
        Toast("This song can't be played");
      }
      break;
    }
    case kOnlineTab: {
      if (job_) {
        Play(Sound::kError);
        break;
      }
      const bool signed_in = !Unquote(Value("online_token", "")).empty();
      if (item == 0) {
        Play(Sound::kSelect);
        if (signed_in)
          Confirm("Sign out?", "The game won't use your account until you sign in again.", "Sign Out",
                  [this] { SignOut(); });
        else
          SignIn(false);
      } else if (item == 1) {
        if (signed_in) {
          Play(Sound::kError);
          Toast("Sign out first to make another account");
        } else {
          Play(Sound::kSelect);
          SignIn(true);
        }
      } else if (item == 2) {
        const bool on = Value("online_enabled", "false") != "true";
        Save("online_enabled", on ? "true" : "false");
        Play(on ? Sound::kOn : Sound::kOff);
      } else {
        Play(Sound::kSelect);
        Ask("Server", Unquote(Value("online_server", "https://sho-ti.me/svr")), false, [this](const std::string& v) {
          Save("online_server", "\"" + v + "\"");
          Toast(v.empty() ? "Server: the default" : "Server: " + v);
        });
      }
      break;
    }
    case kPaintTab: PaintAction(); break;
    case kSavesTab: {
      if (item == 0) {
        BackUpSaves("");
        Play(Sound::kSelect);
      } else if (item - 1 < int(backups_.size())) {
        const Backup b = backups_[size_t(item - 1)];
        Play(Sound::kSelect);
        Confirm("Restore this backup?", "Your saves are replaced by the ones from " + b.name +
                                            ". The saves you have now are backed up first, so nothing is lost.",
                "Restore", [this, b] { RestoreSaves(b); });
      }
      break;
    }
  }
}

void Hub::Navigate(const Pad& pad) {
  const uint32_t p = pad.pressed;
  if (leaving_) return;
  if ((p & kR3) && audio_.next_song) audio_.next_song();  // (quietly: the music just changes)
  if (KeyboardInput(p)) return;
  if (confirm_) {  // a question: Cross does it, Circle doesn't
    if ((p & kCross) && confirm_in_ > 0.6f) {  // (not before it has been seen)
      auto action = std::move(confirm_);
      confirm_ = nullptr;
      action();
    } else if (p & kCircle) {
      confirm_ = nullptr;
      Play(Sound::kBack);
    }
    return;
  }
  if (choice_) {  // a list to pick from
    const int n = int(choice_items_.size());
    const int before = choice_sel_;
    if (p & kUp) choice_sel_ = (choice_sel_ + n - 1) % n;
    if (p & kDown) choice_sel_ = (choice_sel_ + 1) % n;
    if (choice_sel_ != before) Play(Sound::kMove);
    if ((p & kCross) && choice_in_ > 0.6f) {
      auto done = std::move(choice_);
      choice_ = nullptr;
      Play(Sound::kSelect);
      done(choice_sel_);
    } else if (p & kCircle) {
      choice_ = nullptr;
      Play(Sound::kBack);
    }
    return;
  }
  if (br_.open) {  // Find Mods
    BrowserInput(p);
    return;
  }
  // START GAME: once the game is installed (and nothing is being installed)
  auto start = [&] {
    if (task_) {
      Play(Sound::kError);
      Toast(task_kind_ == 4 ? "Wait: the bundled mods are being set up" : "Wait for the current job to finish");
    } else if (!state_.Installed()) {
      Play(Sound::kError);
      Notice("The game isn't installed",
             "Install SmackDown vs. Raw 2011 with the app store, or copy the files of your disc image into "
             "/data/homebrew/PPSA92011/ (default.xex, pac, sound, movies... beside eboot.bin).");
    } else if (std::vector<Dlc> dlc = DlcToInstall(); !dlc.empty() && dlc_install_) {
      // (the DLC not installed yet: here, with progress, rather than on the
      // game's black screen - then the game starts)
      Play(Sound::kSelect);
      GoTo(kDlcTab, 1);
      Toast(dlc.size() == 1 ? "Installing your DLC first" : "Installing your " + std::to_string(dlc.size()) + " DLC packs first");
      StartDlcInstalls(std::move(dlc), true);
    } else {
      press_ = 1, launch_ = true;
    }
  };
  if (p & kOptions) {
    start();
    return;
  }
  if (p & (kL1 | kR1)) {
    GoTo((tab_ + ((p & kR1) ? 1 : kTabCount - 1)) % kTabCount, (p & kR1) ? 1.0f : -1.0f);
    Play(Sound::kPage);
    return;
  }
  if ((p & kCircle) && tab_ == kDlcTab && task_ && task_kind_ == 6) {
    StopTask();
    dlc_then_start_ = false;
    Play(Sound::kBack);
    Toast("Stopping the DLC install");
    return;
  }
  if ((p & kCircle) && tab_ != kHome) {
    GoTo(kHome, -1);
    Play(Sound::kBack);
    return;
  }
  if (tab_ == kHome) {
    if (p & kCross) start();
    return;
  }
  if (tab_ == kPaintTab) {  // the logos: a grid of 5 x 4, Left / Right past its sides turn the page
    if (!paint_ok_) return;
    int& sel = item_[tab_];
    const int col = sel % 5, row = sel / 5, before = sel;
    if ((p & kLeft) && col == 0) {
      PaintPage((paint_page_ + paint::kPages - 1) % paint::kPages, -1);
      sel = row * 5 + 4;
      Play(Sound::kPage);
    } else if ((p & kRight) && col == 4) {
      PaintPage((paint_page_ + 1) % paint::kPages, 1);
      sel = row * 5;
      Play(Sound::kPage);
    } else {
      if (p & kLeft) --sel;
      if (p & kRight) ++sel;
      if ((p & kUp) && row > 0) sel -= 5;
      if ((p & kDown) && row < 3) sel += 5;
      if (sel != before) Play(Sound::kMove);
    }
    if (p & kCross) press_ = 1, Activate(0);
    if (p & kTriangle) press_ = 0.6f, PaintExportAll();
    return;
  }
  if (tab_ == kModsTab && (p & kSquare)) {  // Find Mods: .svrmod files on a USB drive...
    OpenBrowser();
    return;
  }
  const int n = Items();
  if (n == 0) return;
  int& sel = item_[tab_];
  const int before = sel;
  if (p & kUp) sel = (sel + n - 1) % n;
  if (p & kDown) sel = (sel + 1) % n;
  if (sel != before) Play(Sound::kMove);
  switch (tab_) {
    case kSettingsTab:
    case kRosterTab:
      if (p & kLeft) Activate(-1);
      if (p & (kRight | kCross)) Activate(1);
      if (p & (kLeft | kRight | kCross)) press_ = 0.6f;
      break;
    case kModsTab:
    case kMusicTab:
    case kSavesTab:
    case kOnlineTab:
      if (p & kCross) press_ = 1, Activate(0);
      break;
    case kDlcTab:
      if (task_ && task_kind_ == 6) break;
      if (p & kCross) press_ = 1, DlcActivate();
      if ((p & kTriangle) && DlcToInstall().size() > 1) {
        press_ = 0.6f;
        if (!Busy()) Play(Sound::kSelect), StartDlcInstalls(DlcToInstall(), false);
      }
      break;
    case kPacksTab:
      if (p & kCross) press_ = 1, Activate(0);
      if (p & kTriangle) press_ = 0.6f, Activate(-1);
      break;
  }
}

void Hub::GoTo(int tab, float dir) {
  if (tab == tab_) return;
  if (tab_ == kMusicTab && previewing_ >= 0) {
    if (audio_.stop_preview) audio_.stop_preview();
    previewing_ = -1;
  }
  tab_ = tab;
  scroll_f_ = -1;
  scroll_px_ = 0;
  focus_y_ = -1;  // (the highlight starts where the page's row is)
  page_in_ = 0;
  page_dir_ = dir;
  panel_in_ = 1;
  panel_item_ = item_[tab_];
  // (what these pages show can change while the hub is open: looked at again)
  if (tab == kPaintTab) LoadPaint();
  if (tab == kModsTab) LoadDrops();
  if (tab == kDlcTab && !(task_ && task_kind_ == 6)) LoadDlc();
}

bool Hub::Draw(const Pad& pad, float dt) {
  dt_ = std::clamp(dt, 0.0f, 0.1f);
  time_ += dt_;
  toast_time_ = std::max(0.0f, toast_time_ - dt_);
  PollJob();
  PollTask();
  Navigate(pad);
  if (launch_ && !leaving_) {
    leaving_ = true, press_ = 1;
    if (previewing_ >= 0 && audio_.stop_preview) audio_.stop_preview();
    Play(Sound::kStart);
  }
  if (previewing_ >= 0 && audio_.previewing && !audio_.previewing()) previewing_ = -1;  // (the song ended)
  if (item_[tab_] != panel_item_) panel_item_ = item_[tab_], panel_in_ = 0;
  Ease(panel_in_, 1, 10);
  Ease(value_in_, 1, 12);
  Ease(press_, 0, 9);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  s_ = std::min(size.x / kW, size.y / kH);
  ox_ = (size.x - kW * s_) * 0.5f;
  oy_ = (size.y - kH * s_) * 0.5f;
  dl_ = ImGui::GetForegroundDrawList();
  DrawBackdrop();
  DrawTopBar();
  // the page slides in from the side it came from, fading up
  Ease(page_in_, 1, 11);
  const int first = dl_->VtxBuffer.Size;
  const float shift = (1 - page_in_) * 48 * page_dir_;
  ox_ += shift * s_;
  switch (tab_) {
    case kHome: DrawHome(); break;
    case kSettingsTab: DrawSettings(); break;
    case kModsTab: DrawMods(); break;
    case kRosterTab: DrawRoster(); break;
    case kPacksTab: DrawPacks(); break;
    case kMusicTab: DrawMusic(); break;
    case kMoviesTab: DrawMovies(); break;
    case kSavesTab: DrawSaves(); break;
    case kOnlineTab: DrawOnline(); break;
    case kDlcTab: DrawDlc(); break;
    case kPaintTab: DrawPaint(); break;
    case kAboutTab: DrawAbout(); break;
  }
  ox_ -= shift * s_;
  Fade(first, Smooth(page_in_));
  DrawBrowser();
  DrawHints();
  DrawConfirm();
  DrawChoice();
  DrawKeyboard();
  // opening: up from black; START GAME: down to black
  const float open = Smooth(time_ / 0.7f);
  if (leaving_) leave_ = std::min(1.0f, leave_ + dt_ / 0.6f);
  const float black = std::max(1 - open, Smooth(leave_));
  if (black > 0.002f) dl_->AddRectFilled(ImVec2(0, 0), size, Black(black));
  return leaving_ && leave_ >= 1;
}

}  // namespace svr2011::hub
