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


namespace {

// GET MODS on the Mods page: the QR code of the SmackDown vs. Raw Recomp
// Discord (its mods channel has .svrmod files), https://discord.gg/5JRGZVfcRE
// - version 3, error correction M, 29 x 29 (1: dark).
constexpr const char* kModsUrl = "discord.gg/5JRGZVfcRE";
constexpr const char* kModsQr[] = {
    "11111110111010011111001111111",
    "10000010010011101110001000001",
    "10111010111000111100101011101",
    "10111010010010100010101011101",
    "10111010011011110001001011101",
    "10000010101000010100001000001",
    "11111110101010101010101111111",
    "00000000010010111001000000000",
    "10100011001001100100100100101",
    "01000100101111100010101100011",
    "11101011011000010001000001101",
    "11111000001111000000111001000",
    "01011011001101011001001100001",
    "11110100110010001001011100011",
    "11111011100111101001011000001",
    "01001101101010111001101000000",
    "00110010000111100000101000001",
    "00010101110111100001001100111",
    "11001111101100010011001011001",
    "00101001000001000011100010000",
    "11100010111101011101111111010",
    "00000000110000001010100011101",
    "11111110101111101010101010001",
    "10000010000000111001100010010",
    "10111010000101101101111111000",
    "10111010011000100001100011110",
    "10111010101110011100110010011",
    "10000010011010100010111001000",
    "11111110110110111110110011001",
};
constexpr int kQrSize = int(sizeof(kModsQr) / sizeof(kModsQr[0]));

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

// A mod folder type as one of it: "MatchTypes" -> "Match type".
std::string TypeName(const std::string& type) {
  if (type == "Superstars") return "Superstar";
  if (type == "Arenas") return "Arena";
  if (type == "MatchTypes") return "Match type";
  if (type == "Backgrounds") return "Background";
  if (type == "Signs") return "Sign pack";
  if (type == "Media") return "Media pack";
  return type;
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
  // (superstars first, then arenas and match types: My Mods' tabs in that order)
  auto rank = [](const std::string& t) {
    const char* order[] = {"Superstars", "Arenas", "MatchTypes", "Backgrounds", "Media", "Signs"};
    for (int i = 0; i < 6; ++i)
      if (t == order[i]) return i;
    return 9;
  };
  std::sort(mods_.begin(), mods_.end(), [&](const Mod& a, const Mod& b) {
    if (a.type != b.type) return rank(a.type) != rank(b.type) ? rank(a.type) < rank(b.type) : a.type < b.type;
    return Upper(a.name) < Upper(b.name);
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
  const float w = 56, h = 32, t = Smooth(on);
  if (focused) {  // (on the red focus: a white track when on)
    GradRect(x, y, w, h, Mix(Black(0.22f), White(0.98f), t), Mix(Black(0.18f), White(0.9f), t), h * 0.5f);
  } else {
    if (t > 0.02f) Glow(x, y, w, h, h * 0.5f, WithAlpha(kAccent, 0.6f * t), 12);
    GradRect(x, y, w, h, Mix(White(0.16f), kAccentHi, t), Mix(White(0.12f), kAccentLo, t), h * 0.5f);
  }
  const float kx = x + h * 0.5f + (w - h) * t;
  const float stretch = 4 * std::sin(t * 3.14159f);  // (the knob stretches a little while it moves)
  const ImU32 knob = focused ? Mix(White(0.9f), kAccent, t) : Mix(White(0.78f), White(1), t);
  dl_->AddRectFilled(P(kx - 12.5f - stretch * 0.5f, y + 3.5f + 1.5f), P(kx + 12.5f + stretch * 0.5f, y + h - 3.5f + 1.5f),
                     Black(0.25f), 12.5f * s_);
  dl_->AddRectFilled(P(kx - 12.5f - stretch * 0.5f, y + 3.5f), P(kx + 12.5f + stretch * 0.5f, y + h - 3.5f), knob, 12.5f * s_);
}

void Hub::PageTitle(const char* title, const std::string& subtitle) {
  list_y_ = kListY;  // (a page with tabs moves it down: PageTabs)
  Text(a_.display, 56, kPageX, 44, kText, title, 0, 1.5f);
  Text(a_.body, 20, kPageX + 2, 118, White(0.6f), subtitle);
}

void Hub::Empty(const char* title, const std::string& text) {
  Glass(kListX, list_y_ + 10, kListW, 230, 18, 0.9f);
  Text(a_.display, 36, kListX + 40, list_y_ + 44, kText, title);
  Wrapped(a_.body, 21, kListX + 40, list_y_ + 104, kListW - 80, White(0.65f), text);
}

void Hub::List(std::vector<Row>& rows, int sel, float rh, const std::function<void(int, float, float)>& draw) {
  const float hh = 44, x = kListX, w = kListW, y0 = list_y_, view = kListBottom - list_y_;
  float yy = 0;
  int sel_row = -1;
  for (size_t k = 0; k < rows.size(); ++k) {
    Row& r = rows[k];
    if (r.item < 0) {
      yy += k ? 12 : 0;
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
  // the rows' panes, then the focus over them (red and glowing; on the rail: quiet glass)
  dl_->PushClipRect(P(0, y0 - 2), P(kW, y0 + view), true);
  for (const Row& row : rows) {
    const float ry = y0 + row.y - scroll_f_;
    if (row.item < 0 || ry < y0 - rh - 20 || ry > y0 + view + 20) continue;
    GradRect(x, ry, w, rh - 6, White(0.045f), White(0.02f), 12);
  }
  dl_->PopClipRect();
  const float push = press_ * 4, fy = y0 + focus_y_ - scroll_f_ + push * 0.5f;
  dl_->PushClipRect(P(0, y0 - 40), P(kW, y0 + view + 40), true);
  if (rail_focus_) {
    GradRect(x, fy, w, rh - 6, White(0.12f), White(0.05f), 12);
  } else {
    FocusFill(x + push, fy, w - push * 2, rh - 6 - push, 12);
    Ring(x + push, fy, w - push * 2, rh - 6 - push, 12);
  }
  dl_->PopClipRect();
  dl_->PushClipRect(P(0, y0 - 2), P(kW, y0 + view), true);
  for (size_t k = 0; k < rows.size(); ++k) {
    const Row& row = rows[k];
    const float ry = y0 + row.y - scroll_f_;
    if (ry < y0 - rh - 20 || ry > y0 + view + 20) continue;
    if (row.item < 0) {
      Rect(x + 6, ry + 25, 14, 2, kAccent, 1);
      Text(a_.body_bold, 13, x + 32, ry + 18, White(0.45f), row.heading, 0, 3.2f);
      continue;
    }
    const float over = std::clamp(1 - std::fabs(focus_y_ - row.y) / (rh * 0.7f), 0.0f, 1.0f);
    draw(row.item, ry, over);
  }
  dl_->PopClipRect();
  if (total > view) {  // where the list is
    const float bh = std::max(40.0f, view * view / total);
    const float by = y0 + (view - bh) * scroll_f_ / (total - view);
    Rect(x + w + 14, y0, 4, view - 6, White(0.07f), 2);
    Rect(x + w + 14, by, 4, bh - 6, White(0.5f), 2);
  }
}

void Hub::Panel(const std::function<void(float, float, float)>& draw) {
  const int v0 = dl_->VtxBuffer.Size;
  draw(kPanelX + (1 - Smooth(panel_in_)) * 16, list_y_, kPanelW);
  Fade(v0, Smooth(panel_in_));
}

float Hub::Facts(float x, float y, float w, const std::vector<std::pair<std::string, std::string>>& facts) {
  for (const auto& [k, v] : facts) {
    Text(a_.body_bold, 13, x, y + 6, White(0.45f), k, 0, 2.6f);
    y += std::max(40.0f, Wrapped(a_.body_bold, 20, x + 136, y, w - 136, White(0.85f), v) + 12);
  }
  return y;
}

void Hub::DrawBackdrop() {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, kGround);
  if (a_.hero) {
    // The game's picture, its middle right of the page, slowly zooming in and out (a 50 s breath)
    // and drifting a little with the pages.
    const float breath = 0.5f - 0.5f * std::cos(time_ * 6.2832f / 50.0f);
    const float zoom = 1.05f + 0.06f * breath;
    Ease(hero_x_, (float(tab_) / float(kTabCount - 1) - 0.5f) * 40.0f, 3);
    const float h = kH * zoom, w = h * a_.hero.width / a_.hero.height;
    const float cx = hero_center_ + hero_x_ + 8 * std::sin(time_ * 0.07f), cy = kH * 0.47f;
    dl_->AddImage(a_.hero.id, P(cx - w * 0.5f, cy - h * 0.47f), P(cx + w * 0.5f, cy + h * 0.53f));
  }
  // The pages over it: the picture steps back
  Ease(shade_, tab_ == kHome ? 0.0f : 0.82f, 7);
  if (shade_ > 0.003f) dl_->AddRectFilled(ImVec2(0, 0), size, Black(shade_));
  const float home = 1 - std::clamp(shade_ / 0.82f, 0.0f, 1.0f);
  // Home: the page's side darkens so the logo and buttons read over the picture
  if (home > 0.01f) {
    dl_->AddRectFilledMultiColor(P(0, 0), P(1380, kH), Black(0.86f * home), Black(0), Black(0), Black(0.86f * home));
    dl_->AddRectFilledMultiColor(P(0, 560), P(kW, kH), Black(0), Black(0), Black(0.94f * home), Black(0.94f * home));
  }
  // stage light: a red bloom from above, and the edges falling off
  const float bloom = 0.2f + 0.8f * (1 - home);  // (Home has the picture's own light)
  for (int i = 0; i < 6; ++i) {
    const float k = float(i) / 6;
    dl_->AddEllipseFilled(P(1250, -60), ImVec2((900 - 120 * i) * s_, (360 - 48 * i) * s_),
                          WithAlpha(kAccent, (0.035f + 0.015f * k) * bloom), 0, 48);
  }
  dl_->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, P(0, 180).y), Black(0.45f), Black(0.45f), Black(0), Black(0));
  dl_->AddRectFilledMultiColor(P(0, kH - 140), ImVec2(size.x, size.y), Black(0), Black(0), Black(0.7f), Black(0.7f));
}



void Hub::DrawHints() {
  float r = kPageR;
  if (kb_.open) {
    r = Hint(r, kCircle, "Cancel");
    r = Hint(r, kOptions, "Done");
    r = Hint(r, kL1, "Shift");
    r = Hint(r, kTriangle, "Space");
    r = Hint(r, kSquare, "Delete");
    r = Hint(r, kCross, "Type");
  } else if (confirm_) {
    if (confirm_yes_ != "OK") r = Hint(r, kCircle, "Cancel");
    r = Hint(r, kCross, confirm_yes_.c_str());
  } else if (choice_) {
    r = Hint(r, kCircle, "Cancel");
    r = Hint(r, kCross, "Choose");
  } else if (qr_open_) {
    r = Hint(r, kCircle, "Close");
  } else if (dl_open_) {
    DownloadsHints(r);
  } else if (menu_open_) {
    r = Hint(r, kCircle, "Close");
    r = Hint(r, kCross, "Choose");
  } else if (br_.open) {
    BrowserHints(r);
  } else if (rail_focus_) {
    if (state_.Installed()) r = Hint(r, kOptions, "Start Game");
    if (tab_ != kHome) r = Hint(r, kCircle, "Home");
    r = Hint(r, kCross, "Open");
  } else {
    if (tab_ != kHome && state_.Installed()) r = Hint(r, kOptions, "Start Game");
    if (tab_ != kHome && !(tab_ == kDiscoverTab && DiscoverOpen())) r = Hint(r, kCircle, "Back");
    switch (tab_) {
      case kHome: r = Hint(r, kCross, home_sel_ == 0 ? "Start Game" : "Open"); break;
      case kSettingsTab: r = Hint(r, kLeft | kRight, "Change"); break;
      case kModsTab: {
        r = Hint(r, kSquare, "Find Mods");
        const std::vector<int> shown = ModsShown();
        if (!shown.empty()) {
          const int k = shown[size_t(std::clamp(item_[tab_], 0, int(shown.size()) - 1))];
          r = Hint(r, kTriangle, "Options");
          r = Hint(r, kCross, k < int(drops_.size()) ? "Install" : "Turn On / Off");
        }
        break;
      }
      case kDiscoverTab: DiscoverHints(r); break;
      case kDownloadsTab: DownloadsHints(r); break;
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
      case kMusicTab: {
        const std::vector<int> shown = MusicShown();
        if (!shown.empty()) {
          const int k = shown[size_t(std::clamp(item_[tab_], 0, int(shown.size()) - 1))];
          r = Hint(r, kCross, k < 0 ? "Play" : previewing_ == k ? "Stop" : "Listen");
        }
        break;
      }
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
  // a message (bottom left of the page, rising in), or what's being set up
  if (toast_time_ > 0) {
    const float a = Smooth(std::min(toast_time_ * 3, (3.0f - toast_time_) * 6));
    const int v0 = dl_->VtxBuffer.Size;
    const float w = std::min(880.0f, TextWidth(a_.body_bold, 19, toast_) + 82), y = 996 + (1 - a) * 16, h = 50;
    Glow(kPageX, y + 6, w, h, h * 0.5f, Black(0.6f), 24);
    GradRect(kPageX, y, w, h, Rgba(248, 249, 251), Rgba(226, 228, 233), h * 0.5f);
    Glow(kPageX + 12, y + 11, 28, 28, 14, WithAlpha(kAccent, 0.7f), 10);
    dl_->AddCircleFilled(P(kPageX + 26, y + 25), 14 * s_, kAccent, 24);
    Icon(kIconCheck, kPageX + 26, y + 25, 16, White(1), 2.6f);
    TextMid(a_.body_bold, 19, kPageX + 52, y + 25, kDark, Fit(a_.body_bold, 19, toast_, w - 70));
    Fade(v0, a);
  } else if (const std::string status = !DlcStatus().empty() && tab_ != kDlcTab ? DlcStatus() : BundledStatus();
             !status.empty()) {
    // (the first start: a pulsing dot while the bundled mods are set up)
    Glow(kPageX + 2, 1017, 9, 9, 4.5f, WithAlpha(kAccent, 0.5f + 0.5f * std::sin(time_ * 6)), 8);
    dl_->AddCircleFilled(P(kPageX + 6, 1021), 4.5f * s_, WithAlpha(kAccent, 0.5f + 0.5f * std::sin(time_ * 6)), 16);
    TextMid(a_.body_bold, 18, kPageX + 22, 1021, White(0.75f), status);
  }
}

// A question over everything (restore a backup?), rising in.
void Hub::DrawConfirm() {
  Ease(confirm_in_, confirm_ ? 1.0f : 0.0f, 14);
  if (confirm_in_ < 0.01f) return;
  const float a = Smooth(confirm_in_);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.62f * a));
  const float w = 780, x = (kW - w) * 0.5f;
  const int v0 = dl_->VtxBuffer.Size;
  // (its height: the text's lines, wrapped as Wrapped does)
  int lines = 0;
  {
    std::istringstream words(confirm_text_);
    std::string word, cur;
    while (words >> word) {
      const std::string next = cur.empty() ? word : cur + " " + word;
      if (!cur.empty() && TextWidth(a_.body, 22, next) > w - 112) ++lines, cur = word;
      else cur = next;
    }
    if (!cur.empty()) ++lines;
  }
  const float h = 124 + lines * 22 * 1.4f + 52;
  const float y = (kH - h) * 0.5f - 20 + (1 - a) * 30;
  GlassPanel(x, y, w, h, 24);
  Glow(x + 48, y + 48, 40, 3, 1.5f, WithAlpha(kAccent, 0.8f), 8);
  Rect(x + 48, y + 48, 40, 3, kAccent, 1.5f);
  Text(a_.display, 40, x + 56, y + 62, kText, confirm_title_);
  Wrapped(a_.body, 22, x + 56, y + 126, w - 112, White(0.72f), confirm_text_);
  Fade(v0, a);
}

// ------------------------------------------------------------------- pages

void Hub::DrawHome() {
  // first open: the logo rises in, then the buttons, then the new mods
  const float in_logo = Smooth((time_ - 0.15f) / 0.8f), in_btn = Smooth((time_ - 0.4f) / 0.6f),
              in_row = Smooth((time_ - 0.6f) / 0.6f);
  int v0 = dl_->VtxBuffer.Size;
  const float lx = kPageX - 8;
  float lw = 760, lh = 290;
  if (a_.logo) {
    lh = lw * a_.logo.height / a_.logo.width;
    if (lh > 300) lh = 300, lw = lh * a_.logo.width / a_.logo.height;
    const float ly = 150 + (300 - lh) * 0.5f + (1 - in_logo) * 40;
    for (int i = 0; i < 8; ++i) {  // a soft shadow under it
      const float a = float(i) * 0.785f, d = 12;
      const float dx = std::cos(a) * d, dy = std::sin(a) * d + 10;
      dl_->AddImage(a_.logo.id, P(lx + dx, ly + dy), P(lx + lw + dx, ly + lh + dy), ImVec2(0, 0), ImVec2(1, 1), Black(0.11f));
    }
    dl_->AddImage(a_.logo.id, P(lx, ly), P(lx + lw, ly + lh));
  } else {
    Text(a_.display, 120, kPageX, 200, kText, "SMACKDOWN");
    Text(a_.display, 120, kPageX, 320, kAccent, "VS RAW 2011");
  }
  Fade(v0, in_logo);

  // what's going on: mods on, online, a download
  v0 = dl_->VtxBuffer.Size;
  {
    int on = 0;
    for (const Mod& m : mods_) on += m.enabled;
    const bool online = Value("online_enabled", "false") == "true" && !Unquote(Value("online_token", "")).empty();
    float frac = 0;
    int waiting = 0, ready = 0;
    const bool going = DownloadStatus(&frac, &waiting, &ready);
    const float cy = 478, cw = 270, ch = 46;
    for (int c = 0; c < 3; ++c) {
      const float cx = kPageX + c * (cw + 14);
      Glass(cx, cy, cw, ch, ch * 0.5f, 0.95f);
      std::string label;
      if (c == 0) {
        label = std::to_string(on) + (on == 1 ? " mod on" : " mods on");
        Glow(cx + 22, cy + 19, 8, 8, 4, WithAlpha(kAccent, 0.8f), 7);
        dl_->AddCircleFilled(P(cx + 26, cy + 23), 4 * s_, kAccent, 12);
      } else if (c == 1) {
        label = online ? "Online with PC players" : "Online is off";
        Icon(kIconOnline, cx + 26, cy + 23, 18, White(0.85f), 2);
      } else if (going) {
        label = "Downloading  \xC2\xB7  " + std::to_string(int(frac * 100 + 0.5f)) + "%";
        Progress(cx + 26, cy + 23, 8, frac, 3, White(0.18f), false);
      } else {
        label = ready ? std::to_string(ready) + (ready == 1 ? " new mod ready" : " new mods ready") : "Built on PC " + (version_.size() > 1 && (version_[0] == 'v' || version_[0] == 'V') ? version_.substr(1) : version_);
        Icon(ready ? kIconDownload : kIconAbout, cx + 26, cy + 23, 18, White(0.85f), 2);
      }
      TextMid(a_.body_bold, 17, cx + 46, cy + ch * 0.5f, White(0.88f), Fit(a_.body_bold, 17, label, cw - 60));
    }
  }

  // START GAME and DISCOVER MODS: the focused one glows (START GAME breathes)
  const bool on_page = !rail_focus_;
  const float by = 556 + (1 - in_btn) * 24, bw = 400, bh = 92;
  for (int b = 0; b < 2; ++b) {
    const bool focus = on_page && home_sel_ == b;
    const float push = focus ? press_ * 6 : 0;
    const float bx = kPageX + b * (bw + 20) + push, y = by + push, w = bw - push * 2, h = bh - push * 2;
    if (b == 0 || focus) {
      const float pulse = focus ? 0.5f + 0.5f * std::sin(time_ * 2.4f) : 0;
      FocusFill(bx, y, w, h, 16, focus ? 0.75f + 0.35f * pulse : 0.35f);
      if (focus) {
        Ring(bx, y, w, h, 16);
        const float ring = 5 + 3 * pulse;
        dl_->AddRect(P(bx - ring, y - ring), P(bx + w + ring, y + h + ring), WithAlpha(kAccent, 0.35f + 0.25f * pulse),
                     (16 + ring) * s_, 0, 2 * s_);
      }
    } else {
      Glass(bx, y, w, h, 16, 1.1f);
    }
    const char* label = b == 0 ? "START GAME" : "DISCOVER MODS";
    const float tw = TextWidth(a_.display, b == 0 ? 38 : 32, label, 2) + 44;
    const float tx = bx + (w - tw) * 0.5f;
    if (b == 0) Icon(kIconPlay, tx + 13, y + h * 0.5f, 28, kText);
    else Icon(kIconDiscover, tx + 13, y + h * 0.5f, 28, kText, 2.2f);
    TextMid(a_.display, b == 0 ? 38 : 32, tx + 44, y + h * 0.5f, kText, label, 0, 2);
  }
  Fade(v0, in_btn);

  // NEW IN DISCOVER: the newest mods on the server (when the console is online)
  v0 = dl_->VtxBuffer.Size;
  DrawHomeFresh(716, on_page, in_row);
  Fade(v0, in_row);
}

void Hub::DrawStartup(const std::string& step, const std::string& detail, float fraction, float dt, float alpha) {
  dt_ = std::clamp(dt, 0.0f, 0.1f);
  time_ += dt_;
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  s_ = std::min(size.x / kW, size.y / kH);
  ox_ = (size.x - kW * s_) * 0.5f;
  oy_ = (size.y - kH * s_) * 0.5f;
  dl_ = ImGui::GetForegroundDrawList();
  const int v0 = dl_->VtxBuffer.Size;
  tab_ = kHome;
  hero_center_ = kW * 0.5f;  // (no rail here: the picture in the middle, covering the screen)
  DrawBackdrop();
  // the logo, where the home page has it
  const float lx = 104, lw = a_.logo && a_.logo.width > a_.logo.height * 2.4f ? 700.0f : 640.0f;
  if (a_.logo) {
    const float lh = lw * a_.logo.height / a_.logo.width, ly = 862 - 36 - lh;
    for (int i = 0; i < 8; ++i) {
      const float a = float(i) * 0.785f, d = 10;
      const float dx = std::cos(a) * d, dy = std::sin(a) * d + 8;
      dl_->AddImage(a_.logo.id, P(lx + dx, ly + dy), P(lx + lw + dx, ly + lh + dy), ImVec2(0, 0), ImVec2(1, 1), Black(0.1f));
    }
    dl_->AddImage(a_.logo.id, P(lx, ly), P(lx + lw, ly + lh));
  } else {
    Text(a_.display, 96, lx, 640, kText, "SMACKDOWN VS RAW");
  }
  // where START GAME was: what's being done, a spinner and a bar
  const float bx = 112, by = 870;
  const float a0 = time_ * 5;
  dl_->PathArcTo(P(bx + 20, by + 24), 17 * s_, a0, a0 + 4.2f, 32);
  dl_->PathStroke(kAccent, 0, 4 * s_);
  Text(a_.display, 40, bx + 56, by, kText, Upper(step), 0, 2);
  const float bary = by + 70, barw = 760;
  Rect(bx, bary, barw, 6, White(0.14f), 3);
  static float shown = 0;
  if (fraction >= 0) {
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    shown = shown > f ? f : shown + (f - shown) * (1 - std::exp(-dt_ * 8));
    Rect(bx, bary, std::max(6.0f, barw * shown), 6, kAccent, 3);
  } else {
    shown = 0;
    const float seg = 200, t = std::fmod(time_ * 0.6f, 1.0f), x0 = bx - seg + (barw + seg) * t;
    const float a = std::max(bx, x0), b = std::min(bx + barw, x0 + seg);
    if (b > a) Rect(a, bary, b - a, 6, kAccent, 3);
  }
  if (!detail.empty()) Text(a_.body, 21, bx, bary + 22, White(0.62f), Fit(a_.body, 21, detail, barw + 200));
  Text(a_.body, 18, 112, 1012, White(0.32f), "SmackDown vs. Raw Recomp " + version_ + "  \xC2\xB7  PlayStation 5");
  Fade(v0, alpha);
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
  auto is_switch = [](const Setting& r) {
    return r.labels.size() == 2 && std::strcmp(r.labels[0], "On") == 0 && std::strcmp(r.labels[1], "Off") == 0;
  };
  const float x = kListX, w = kListW;
  List(rows, sel, 62, [&](int i, float yy, float over) {
    const auto& r = set[size_t(i)];
    const float cy = yy + 28;
    TextMid(a_.body_bold, 21, x + 24, cy, Mix(White(0.9f), kInk, over), r.label);
    const std::string v = r.labels[size_t(SettingIndex(r))];
    const bool focused = i == sel && !rail_focus_;
    if (is_switch(r)) {
      Switch(x + w - 22 - 56, cy - 16, Knob(i, SettingIndex(r) == 0), focused);
      return;
    }
    if (std::strcmp(r.key, "hub_music_volume") == 0) {
      // a slider: the bar fills to the volume, the value beside it
      const float frac = float(SettingIndex(r)) / float(r.values.size() - 1);
      auto& shown = knobs_[kSettingsTab << 16 | (0x8000 + i)];
      Ease(shown, frac, 16);
      const float bw = 260, br = x + w - 24 - 74, bx = br - bw;
      Rect(bx, cy - 3, bw, 6, Mix(White(0.16f), White(0.3f), over), 3);
      if (focused) Rect(bx, cy - 3, std::max(6.0f, bw * shown), 6, White(1), 3);
      else {
        Glow(bx, cy - 3, std::max(6.0f, bw * shown), 6, 3, WithAlpha(kAccent, 0.6f), 8);
        Rect(bx, cy - 3, std::max(6.0f, bw * shown), 6, kAccent, 3);
      }
      const float kx = bx + bw * shown;
      dl_->AddCircleFilled(P(kx, cy), (focused ? 11 : 8) * s_, White(1), 24);
      TextMid(focused ? a_.body_bold : a_.body, 20, x + w - 24, cy, Mix(White(focused ? 1.0f : 0.6f), kInk, over), v, 1);
      return;
    }
    if (focused) {
      // the value slides in from the side it was changed toward
      const float vr = x + w - 46;
      const float slide = (1 - Smooth(value_in_)) * 18 * value_dir_;
      const int v0 = dl_->VtxBuffer.Size;
      TextMid(a_.body_bold, 20, vr + slide, cy, kInk, v, 1);
      Fade(v0, 0.25f + 0.75f * Smooth(value_in_));
      const float vl = vr - TextWidth(a_.body_bold, 20, v) - 18;
      const float nudge_l = value_dir_ < 0 ? (1 - Smooth(value_in_)) * 6 : 0;
      const float nudge_r = value_dir_ > 0 ? (1 - Smooth(value_in_)) * 6 : 0;
      dl_->AddTriangleFilled(P(vl - nudge_l, cy - 7), P(vl - nudge_l, cy + 7), P(vl - 9 - nudge_l, cy), White(0.95f));
      dl_->AddTriangleFilled(P(vr + 14 + nudge_r, cy - 7), P(vr + 14 + nudge_r, cy + 7), P(vr + 23 + nudge_r, cy), White(0.95f));
    } else {
      TextMid(a_.body_bold, 19, x + w - 24, cy, Mix(White(0.6f), kInk, over), v, 1);
    }
  });
  // the focused setting, explained
  const auto& r = set[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    Text(a_.display, 42, px, py, kText, r.label);
    py += 66;
    py += Wrapped(a_.body, 21, px, py, pw, White(0.68f), r.help, 1.45f);
    py += 26;
    const int cur = SettingIndex(r);
    if (std::strcmp(r.key, "hub_music_volume") == 0) {
      const float shown = knobs_[kSettingsTab << 16 | (0x8000 + sel)];
      Text(a_.display, 64, px, py, kText, r.labels[size_t(cur)]);
      py += 100;
      Rect(px, py, pw, 10, White(0.14f), 5);
      Glow(px, py, std::max(10.0f, pw * shown), 10, 5, WithAlpha(kAccent, 0.7f), 12);
      Rect(px, py, std::max(10.0f, pw * shown), 10, kAccent, 5);
      const float pop = 1 + 0.4f * (1 - Smooth(value_in_));
      dl_->AddCircleFilled(P(px + pw * shown, py + 5), 15 * pop * s_, White(1), 28);
      Text(a_.body, 18, px, py + 30, White(0.45f), "Off");
      Text(a_.body, 18, px + pw, py + 30, White(0.45f), "100%", 1);
      return;
    }
    // the choices: each a glass row, the current one lit
    for (int k = 0; k < int(r.labels.size()); ++k) {
      const bool c = k == cur;
      const float rh = 50;
      if (c) GradRect(px, py, pw, rh, White(0.11f), White(0.05f), 12), dl_->AddRect(P(px, py), P(px + pw, py + rh), White(0.12f), 12 * s_);
      else GradRect(px, py, pw, rh, White(0.035f), White(0.02f), 12);
      const float pop = c ? 1 + 0.6f * (1 - Smooth(value_in_)) : 1;
      if (c) {
        Glow(px + 18, py + rh * 0.5f - 6, 12, 12, 6, WithAlpha(kAccent, 0.9f), 9);
        dl_->AddCircleFilled(P(px + 24, py + rh * 0.5f), 6 * pop * s_, kAccent, 20);
      } else {
        dl_->AddCircle(P(px + 24, py + rh * 0.5f), 5.5f * s_, White(0.35f), 20, 1.5f * s_);
      }
      TextMid(c ? a_.body_bold : a_.body, 19, px + 44, py + rh * 0.5f, c ? kText : White(0.55f), r.labels[size_t(k)]);
      py += rh + 8;
    }
  });
}

// A QR code drawn as squares: crisp at any size. On white, with the quiet
// zone (2 modules) scanners need.
void Hub::DrawQr(float x, float y, float size) {
  Rect(x, y, size, size, Rgba(255, 255, 255), size * 0.04f);
  const float m = size / float(kQrSize + 4), o = 2 * m;
  for (int r = 0; r < kQrSize; ++r)
    for (int c = 0; c < kQrSize; ++c)
      if (kModsQr[r][c] == '1')
        dl_->AddRectFilled(P(x + o + c * m, y + o + r * m), P(x + o + (c + 1) * m + 0.35f, y + o + (r + 1) * m + 0.35f),
                           Rgba(8, 9, 11));
}

// GET MODS: in the Mods page's corner; Triangle shows it big.
void Hub::DrawGetMods() {
  const float x = kPanelX, y = 838, w = kPanelW, h = 132;
  Rect(x, y, w, h, White(0.06f), 14);
  DrawQr(x + 14, y + 14, h - 28);
  const float tx = x + h + 8, tw = w - h - 26;
  Text(a_.body_bold, 15, tx, y + 18, White(0.5f), "GET MODS", 0, 3);
  Wrapped(a_.body, 19, tx, y + 46, tw, White(0.78f), "Browse the community's mods online, or scan for the SmackDown vs. Raw Recomp Discord.");
  Glyph(kTriangle, tx + 11, y + h - 25, White(0.6f));
  Text(a_.body_bold, 17, tx + 30, y + h - 37, White(0.6f), "Discover");
}

void Hub::DrawQrBig() {
  Ease(qr_in_, qr_open_ ? 1.0f : 0.0f, 14);
  if (qr_in_ < 0.01f) return;
  const float a = Smooth(qr_in_);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const int v0 = dl_->VtxBuffer.Size;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.82f));
  const float q = 560, w = 1240, h = q + 120, x = (kW - w) * 0.5f, y = (kH - h) * 0.5f + (1 - a) * 30;
  GlassPanel(x, y, w, h, 24);
  DrawQr(x + 60, y + 60, q);
  const float tx = x + q + 120, tw = w - q - 180;
  Text(a_.body_bold, 16, tx, y + 76, White(0.5f), "GET MODS", 0, 3);
  float ty = y + 112;
  ty += Wrapped(a_.display, 48, tx, ty, tw, kText, "Scan with your phone", 1.1f) + 24;
  ty += Wrapped(a_.body, 22, tx, ty, tw, White(0.75f),
                "It opens the SmackDown vs. Raw Recomp Discord. Its mods channel has superstars, arenas, moves and more "
                "as .svrmod files.") + 18;
  ty += Wrapped(a_.body, 22, tx, ty, tw, White(0.75f),
                "Copy them to a USB drive, plug it in, and press Square on this page to find and install them.") + 30;
  Text(a_.body_bold, 22, tx, ty, kAccent, kModsUrl);
  Fade(v0, a);
}

// The mod types there are (their folders: Superstars, Arenas, MatchTypes...), in mods_' order.
std::vector<std::string> Hub::ModTypes() const {
  std::vector<std::string> out;
  for (const Mod& m : mods_)
    if (out.empty() || out.back() != m.type) out.push_back(m.type);
  return out;
}

// The rows of My Mods' tab shown: ALL has the files ready to install first (0..drops-1), then
// every mod (drops + i); a type's tab, its mods.
std::vector<int> Hub::ModsShown() const {
  std::vector<int> out;
  const int nd = int(drops_.size()), tab = sub_[kModsTab];
  const std::vector<std::string> types = ModTypes();
  if (tab <= 0 || tab > int(types.size())) {
    for (int i = 0; i < nd; ++i) out.push_back(i);
    for (int i = 0; i < int(mods_.size()); ++i) out.push_back(nd + i);
  } else {
    for (int i = 0; i < int(mods_.size()); ++i)
      if (mods_[size_t(i)].type == types[size_t(tab - 1)]) out.push_back(nd + i);
  }
  return out;
}

namespace {
// A mod type's look: its colours and icon.
struct TypeLook {
  ImU32 hi, lo;
  int icon;
};
TypeLook LookOf(const std::string& type) {
  if (type == "Superstars") return {Rgba(209, 58, 67), Rgba(62, 11, 16), kIconStar};
  if (type == "Arenas") return {Rgba(60, 100, 214), Rgba(18, 31, 74), kIconArena};
  if (type == "MatchTypes") return {Rgba(138, 63, 192), Rgba(36, 16, 59), kIconBolt};
  if (type == "Backgrounds") return {Rgba(37, 165, 179), Rgba(11, 52, 56), kIconImage};
  if (type == "Signs") return {Rgba(47, 154, 102), Rgba(14, 51, 34), kIconImage};
  if (type == "Media") return {Rgba(226, 116, 46), Rgba(74, 26, 8), kIconMovies};
  return {Rgba(106, 109, 121), Rgba(20, 21, 25), kIconMods};
}
}  // namespace

void Hub::ToggleMod(Mod& m) {
  std::error_code ec;
  if (m.enabled) std::ofstream(m.folder / "disabled") << "";
  else fs::remove(m.folder / "disabled", ec);
  m.enabled = !fs::exists(m.folder / "disabled", ec);
  Play(m.enabled ? Sound::kOn : Sound::kOff);
  Toast(m.name + (m.enabled ? " is on" : " is off"));
}

void Hub::DrawMods() {
  int on = 0;
  for (const Mod& m : mods_) on += m.enabled;
  PageTitle("MY MODS", mods_.empty() ? std::string("Mods you install show up here.")
                                     : "What's on loads when the game starts. " + std::to_string(on) + " of " +
                                           std::to_string(mods_.size()) + " are on.");
  DownloadsWidget(kPageR, 52);
  const std::vector<std::string> types = ModTypes();
  {
    std::vector<std::pair<std::string, int>> tabs = {{"ALL", int(mods_.size() + drops_.size())}};
    for (const std::string& t : types) {
      int count = 0;
      for (const Mod& m : mods_) count += m.type == t;
      tabs.push_back({TypeHeading(t), count});
    }
    sub_[kModsTab] = std::clamp(sub_[kModsTab], 0, int(tabs.size()) - 1);
    PageTabs(tabs, sub_[kModsTab]);
  }
  list_y_ = kTabsListY;
  if (mods_.empty() && drops_.empty()) {
    Empty("No mods yet",
          "Get them on Discover, the community's mods, with one press. Or press Square to find .svrmod files on a USB "
          "drive or in any folder, and install them from there.");
    return;
  }
  const std::vector<int> shown = ModsShown();
  const int nd = int(drops_.size());
  const int n = int(shown.size());
  if (!n) return;
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) {
    const int k = shown[size_t(i)];
    if (sub_[kModsTab] == 0) {  // ALL: headings over the files to install and each type
      if (k < nd && i == 0) rows.push_back({-1, "READY TO INSTALL  " + std::to_string(nd)});
      if (k >= nd && (i == 0 || shown[size_t(i) - 1] < nd || mods_[size_t(k - nd)].type != mods_[size_t(shown[size_t(i) - 1] - nd)].type))
        rows.push_back({-1, TypeHeading(mods_[size_t(k - nd)].type)});
    }
    rows.push_back({i});
  }
  const float x = kListX, w = kListW;
  List(rows, sel, 86, [&](int i, float ry, float over) {
    const int k = shown[size_t(i)];
    const bool focused = i == sel && !rail_focus_;
    const float cy = ry + 40;
    if (k < nd) {
      const Drop& d = drops_[size_t(k)];
      const std::string name = d.info.name.empty() ? d.file.stem().string() : d.info.name;
      GradRect(x + 16, cy - 29, 104, 58, Rgba(80, 82, 94), Rgba(24, 25, 31), 9);
      Icon(kIconDownload, x + 68, cy, 26, White(0.9f), 2.2f);
      TextMid(a_.body_bold, 21, x + 140, cy - 12, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 21, name, w - 360));
      std::string sub = d.info.version.empty() ? std::string() : "v" + d.info.version + "  \xC2\xB7  ";
      sub += Size(d.bytes) + "  \xC2\xB7  " + d.file.filename().string();
      TextMid(a_.body, 16, x + 140, cy + 14, Mix(White(0.5f), White(0.82f), over), Fit(a_.body, 16, sub, w - 360));
      const bool working = task_ && task_kind_ == 3 && task_file_ == d.file;
      const char* st = working ? "Installing..." : "Ready to install";
      TextMid(a_.body_bold, 16, x + w - 24, cy, Mix(White(0.8f), White(1), over), st, 1);
      return;
    }
    const Mod& m = mods_[size_t(k - nd)];
    const TypeLook look = LookOf(m.type);
    GradRect(x + 16, cy - 29, 104, 58, look.hi, look.lo, 9);
    dl_->AddRect(P(x + 16, cy - 29), P(x + 120, cy + 29), White(0.12f), 9 * s_);
    Icon(look.icon, x + 68, cy, 26, White(0.92f), 2.2f);
    const float nx = x + 140;
    const std::string name = Fit(a_.body_bold, 21, m.name, w - 330);
    TextMid(a_.body_bold, 21, nx, cy - 12, Mix(White(m.enabled ? 0.95f : 0.62f), kInk, over), name);
    if (fresh_mods_.count(m.folder.string())) {  // downloaded this time: NEW
      const float bx = nx + TextWidth(a_.body_bold, 21, name) + 12;
      if (!focused) Glow(bx, cy - 23, 46, 22, 11, WithAlpha(kAccent, 0.6f), 8);
      Rect(bx, cy - 23, 46, 22, focused ? White(1) : kAccent, 11);
      TextMid(a_.body_bold, 11, bx + 23, cy - 12, focused ? kAccent : kText, "NEW", 0.5f, 1.5f);
    }
    std::string sub = TypeName(m.type);
    if (!m.version.empty()) sub += "  \xC2\xB7  v" + m.version;
    if (!m.author.empty()) sub += "  \xC2\xB7  " + m.author;
    sub += "  \xC2\xB7  " + Size(m.bytes);
    TextMid(a_.body, 16, nx, cy + 14, Mix(White(0.5f), White(0.82f), over), Fit(a_.body, 16, sub, w - 330));
    Switch(x + w - 24 - 56, cy - 16, Knob(k, m.enabled), focused);
  });
  const int k = shown[size_t(sel)];
  if (k < nd) {
    const Drop& d = drops_[size_t(k)];
    Panel([&](float px, float py, float pw) {
      GradRect(px, py, pw, 190, Rgba(80, 82, 94), Rgba(24, 25, 31), 18);
      Icon(kIconDownload, px + pw * 0.5f, py + 95, 64, White(0.85f), 2.4f);
      py += 214;
      py += Wrapped(a_.display, 38, px, py, pw, kText, d.info.name.empty() ? d.file.stem().string() : d.info.name, 1.1f) + 16;
      py = Facts(px, py, pw,
                 {{"TYPE", d.info.type.empty() ? "Can't be read" : d.info.type},
                  {"VERSION", d.info.version.empty() ? "-" : d.info.version},
                  {"BY", d.info.author.empty() ? "-" : d.info.author},
                  {"SIZE", Size(d.bytes)}});
      Wrapped(a_.body, 18, px, py + 8, pw, White(0.55f),
              "Cross installs it. The file is removed once it's in, and the mod shows up in this list.");
    });
    return;
  }
  const Mod& m = mods_[size_t(k - nd)];
  Panel([&](float px, float py, float pw) {
    const TypeLook look = LookOf(m.type);
    GradRect(px, py, pw, 190, look.hi, look.lo, 18);
    GradRect(px, py, pw, 190, Black(0), Black(0.45f), 18);
    dl_->AddRect(P(px, py), P(px + pw, py + 190), White(0.12f), 18 * s_);
    Icon(look.icon, px + pw * 0.5f, py + 80, 72, White(0.9f), 2.4f);
    TextMid(a_.body_bold, 13, px + 20, py + 168, White(0.8f), Upper(TypeHeading(m.type)), 0, 2.6f);
    py += 214;
    py += Wrapped(a_.display, 38, px, py, pw, kText, m.name, 1.1f) + 16;
    py = Facts(px, py, pw,
               {{"VERSION", m.version.empty() ? "-" : m.version},
                {"BY", m.author.empty() ? "-" : m.author},
                {"SIZE", Size(m.bytes)}});
    Wrapped(a_.body, 18, px, py + 8, pw, White(0.6f),
            m.enabled ? "On: the game loads it when it starts. Triangle for its options."
                      : "Off. Cross turns it on; the game loads it when it starts.");
  });
}

// A mod's options (Triangle), beside its row.
void Hub::DrawModsMenu() {
  Ease(menu_in_, menu_open_ ? 1.0f : 0.0f, 16);
  if (menu_in_ < 0.01f) return;
  const std::vector<int> shown = ModsShown();
  if (shown.empty()) return;
  const int k = shown[size_t(std::clamp(item_[kModsTab], 0, int(shown.size()) - 1))];
  const int nd = int(drops_.size());
  std::vector<std::pair<int, std::string>> items;  // (icon, label)
  if (k < nd) items = {{kIconDownload, "Install"}, {kIconTrash, "Delete the File"}};
  else items = {{kIconPower, mods_[size_t(k - nd)].enabled ? "Turn Off" : "Turn On"}, {kIconTrash, "Delete"}};
  menu_sel_ = std::clamp(menu_sel_, 0, int(items.size()) - 1);
  const float a = Smooth(menu_in_);
  const int v0 = dl_->VtxBuffer.Size;
  const float rh = 56, w = 330, h = 20 + rh * items.size() + 12;
  const float row_y = list_y_ + focus_y_ - scroll_f_;
  const float x = kListX + kListW - w + 30, y = std::clamp(row_y + 70 + (1 - a) * 14, 200.0f, 960.0f - h);
  GlassPanel(x, y, w, h, 20);
  float yy = y + 10;
  for (int i = 0; i < int(items.size()); ++i) {
    const bool f = i == menu_sel_;
    const bool del = items[size_t(i)].first == kIconTrash;
    if (f) FocusFill(x + 10, yy, w - 20, rh, 12, 0.8f), Ring(x + 10, yy, w - 20, rh, 12);
    const ImU32 col = f ? kText : del ? Rgba(255, 122, 128) : White(0.86f);
    Icon(items[size_t(i)].first, x + 40, yy + rh * 0.5f, 22, col, 2.1f);
    TextMid(a_.body_bold, 19, x + 66, yy + rh * 0.5f, col, items[size_t(i)].second);
    yy += rh + (i + 2 == int(items.size()) ? 6 : 0);
  }
  Fade(v0, a);
}

void Hub::ModsMenuInput(uint32_t p) {
  const std::vector<int> shown = ModsShown();
  if (shown.empty() || (p & (kCircle | kTriangle))) {
    menu_open_ = false;
    Play(Sound::kBack);
    return;
  }
  if (p & (kUp | kDown)) {
    menu_sel_ = menu_sel_ ? 0 : 1;
    Play(Sound::kMove);
  }
  if (!(p & kCross)) return;
  menu_open_ = false;
  const int k = shown[size_t(std::clamp(item_[kModsTab], 0, int(shown.size()) - 1))];
  const int nd = int(drops_.size());
  if (k < nd) {
    const Drop d = drops_[size_t(k)];
    if (menu_sel_ == 0) {
      if (!Busy()) Play(Sound::kSelect), StartModInstall(d.file);
    } else {
      Play(Sound::kSelect);
      Confirm("Delete this file?", d.file.filename().string() + " is removed from the console.", "Delete", [this, d] {
        std::error_code ec;
        fs::remove(d.file, ec);
        LoadDrops();
        Play(Sound::kOff);
        Toast(ec ? "The file couldn't be deleted" : "Deleted " + d.file.filename().string());
      });
    }
    return;
  }
  Mod& m = mods_[size_t(k - nd)];
  if (menu_sel_ == 0) {
    press_ = 1;
    ToggleMod(m);
    return;
  }
  Play(Sound::kSelect);
  const Mod copy = m;
  Confirm("Delete " + m.name + "?",
          "It's removed from the console. You can install it again later, from Discover or its .svrmod file.", "Delete",
          [this, copy] {
            std::error_code ec;
            fs::remove_all(copy.folder, ec);
            fresh_mods_.erase(copy.folder.string());
            LoadMods();
            Play(Sound::kOff);
            Toast(ec ? "It couldn't be deleted" : copy.name + " is deleted");
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
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), White(0.82f), over), Size(d.bytes));
    const char* st = d.other ? "Not for this game" : d.installed ? "Installed" : "Not installed";
    const bool lit = !d.installed && !d.other;
    const float sx = x + w - 24 - TextWidth(a_.body_bold, 18, st);
    if (lit)
      dl_->AddCircleFilled(P(sx - 14, ry + 35), 4.5f * s_, WithAlpha(kAccent, 0.6f + 0.4f * std::sin(time_ * 3)), 16);
    Text(a_.body_bold, 18, sx, ry + 24, Mix(White(lit ? 0.9f : 0.55f), White(0.95f), over), st);
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
  float y = kListY;
  for (const Credit& c : people) {
    GradRect(kPageX, y, kListW, 112, White(0.045f), White(0.02f), 14);
    Text(a_.display, 30, kPageX + 28, y + 18, kText, c.who);
    Wrapped(a_.body, 19, kPageX + 28, y + 62, kListW - 56, White(0.62f), c.what);
    y += 124;
  }
  const float px = kPanelX;
  float py = kListY + 6;
  Text(a_.body_bold, 13, px, py, White(0.45f), "BUILT WITH", 0, 3.2f);
  py += 34;
  const char* parts[] = {"PS5 Payload SDK", "Mesa RADV (Vulkan)", "plume and XenosRecomp", "Dear ImGui",
                         "stb_image, stb_vorbis, dr_mp3, dr_flac and miniz", "Oswald and Inter fonts (SIL Open Font License)"};
  for (const char* part : parts) py += Wrapped(a_.body, 19, px, py, kPanelW, White(0.78f), part) + 6;
  py += 26;
  Text(a_.body_bold, 13, px, py, White(0.45f), "PLEASE NOTE", 0, 3.2f);
  py += 34;
  Wrapped(a_.body, 17, px, py, kPanelW, White(0.55f),
          "An unofficial fan project, not affiliated with or endorsed by WWE, THQ, Yuke's, Microsoft or Sony. WWE "
          "SmackDown vs. Raw 2011 and all related names belong to their owners. No game files are included: you need "
          "your own copy of the game.");
}

// --------------------------------------------------------------- behaviour

int Hub::Items() const {
  switch (tab_) {
    case kSettingsTab: return int(Settings().size());
    case kModsTab: return int(ModsShown().size());
    case kPaintTab: return paint_ok_ ? paint::kSlots : 0;
    case kRosterTab: return int(roster_.size());
    case kPacksTab: return packs_.empty() ? 0 : int(packs_.size()) + 1;  // (+ Dump Textures)
    case kMusicTab: return int(MusicShown().size());
    case kMoviesTab: return int(movies_.size());
    case kSavesTab: return 1 + int(backups_.size()) + int(save_files_.size());
    case kOnlineTab: return 4;
    case kDlcTab: return int(dlc_.size());
    default: return 0;
  }
}

bool Hub::PageHasTabs() const { return tab_ == kDiscoverTab || tab_ == kModsTab || tab_ == kMusicTab; }

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
      const std::vector<int> shown = ModsShown();
      if (item < 0 || item >= int(shown.size())) break;
      const int k = shown[size_t(item)];
      if (k < int(drops_.size())) {
        if (!Busy()) Play(Sound::kSelect), StartModInstall(drops_[size_t(k)].file);
        break;
      }
      if (k - int(drops_.size()) >= int(mods_.size())) break;
      ToggleMod(mods_[size_t(k - int(drops_.size()))]);
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
      const std::vector<int> shown = MusicShown();
      if (item < 0 || item >= int(shown.size())) break;
      const int k = shown[size_t(item)];
      if (k < 0) {  // a menu song: it plays now (the preview stops)
        if (previewing_ >= 0 && audio_.stop_preview) audio_.stop_preview();
        previewing_ = -1;
        if (audio_.play_menu_song) audio_.play_menu_song(-1 - k);
        Play(Sound::kSelect);
        break;
      }
      const Song& s = songs_[size_t(k)];
      if (previewing_ == k) {
        if (audio_.stop_preview) audio_.stop_preview();
        previewing_ = -1;
        Play(Sound::kBack);
      } else if (!s.playable) {
        Play(Sound::kError);
        Toast("The PS5 can't play this file: use MP3, WAV, FLAC or OGG");
      } else if (audio_.preview && audio_.preview(s.file)) {
        previewing_ = k;
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
  if (qr_open_) {  // GET MODS, big: any of these closes it
    if (p & (kCircle | kCross | kTriangle)) qr_open_ = false, Play(Sound::kBack);
    return;
  }
  if (dl_open_) {  // the downloads widget, opened
    PopoverInput(p);
    return;
  }
  if (menu_open_) {  // a mod's options
    ModsMenuInput(p);
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
      rail_focus_ = false;
      Toast(dlc.size() == 1 ? "Installing your DLC first" : "Installing your " + std::to_string(dlc.size()) + " DLC packs first");
      StartDlcInstalls(std::move(dlc), true);
    } else if (float frac = 0; [&] {
                 int waiting = 0, ready = 0;
                 return DownloadStatus(&frac, &waiting, &ready);
               }()) {
      // (a mod still downloading: the game sets its mods up as it starts, so it stops first)
      Play(Sound::kSelect);
      Confirm("A mod is still downloading", "Stop it and start the game? You can download it again any time.",
              "Start Game", [this] {
                StopDownloads();
                press_ = 1, launch_ = true;
              });
    } else {
      press_ = 1, launch_ = true;
    }
  };
  if (p & kOptions) {
    start();
    return;
  }
  // L1 / R1: a page's tabs (Discover, My Mods, Music); elsewhere the pages, in the rail's order
  if (p & (kL1 | kR1)) {
    const int dir = (p & kR1) ? 1 : -1;
    if (PageHasTabs() && !(tab_ == kDiscoverTab && DiscoverOpen())) {
      int n = 3;
      if (tab_ == kDiscoverTab) n = 5;
      if (tab_ == kModsTab) n = 1 + int(ModTypes().size());
      if (n > 1) {
        sub_[tab_] = (sub_[tab_] + dir + n) % n;
        item_[tab_] = 0;
        scroll_f_ = -1, scroll_px_ = 0, focus_y_ = -1;
        panel_in_ = 0;
        if (tab_ == kDiscoverTab) DiscoverEnsure(), DiscoverLoad(0);  // (the new kind's list)
        Play(Sound::kPage);
      }
      return;
    }
    GoTo((tab_ + dir + kTabCount) % kTabCount, float(dir));
    Play(Sound::kPage);
    return;
  }
  if (rail_focus_) {
    RailInput(p);
    return;
  }
  if ((p & kCircle) && tab_ == kDlcTab && task_ && task_kind_ == 6) {
    StopTask();
    dlc_then_start_ = false;
    Play(Sound::kBack);
    Toast("Stopping the DLC install");
    return;
  }
  if (tab_ == kDiscoverTab) {
    DiscoverInput(p);
    return;
  }
  if (tab_ == kDownloadsTab) {
    DownloadsInput(p);
    return;
  }
  // Circle: back to the rail
  if (p & kCircle) {
    rail_focus_ = true;
    Play(Sound::kBack);
    return;
  }
  if (tab_ == kHome) {
    const int fresh = HomeFreshCount();
    const int before = home_sel_;
    if (p & kLeft) {
      if (home_sel_ == 0 || home_sel_ == 2) {
        rail_focus_ = true;
        Play(Sound::kBack);
        return;
      }
      --home_sel_;
    }
    if ((p & kRight) && home_sel_ < 1 + fresh && home_sel_ != 1) ++home_sel_;
    if ((p & kDown) && home_sel_ < 2 && fresh) home_sel_ = 2 + std::min(home_sel_, fresh - 1);
    if ((p & kUp) && home_sel_ >= 2) home_sel_ = home_sel_ == 2 ? 0 : 1;
    if (home_sel_ != before) Play(Sound::kMove);
    if (p & kCross) {
      if (home_sel_ == 0) {
        start();
      } else if (home_sel_ == 1) {
        press_ = 1;
        Play(Sound::kSelect);
        GoTo(kDiscoverTab, 1);
      } else {
        press_ = 1;
        Play(Sound::kSelect);
        OpenFresh(home_sel_ - 2);
      }
    }
    return;
  }
  if (tab_ == kPaintTab) {  // the logos: a grid of 5 x 4, Left / Right past its sides turn the page
    if (!paint_ok_) {
      if (p & kLeft) rail_focus_ = true, Play(Sound::kBack);
      return;
    }
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
  if (tab_ == kModsTab && (p & kTriangle) && Items() > 0) {  // a mod's options
    menu_open_ = true;
    menu_sel_ = 0;
    menu_in_ = 0;
    Play(Sound::kSelect);
    return;
  }
  // Left: back to the rail (pages where Left / Right change a value keep them: Circle goes back)
  if ((p & kLeft) && tab_ != kSettingsTab && tab_ != kRosterTab) {
    rail_focus_ = true;
    Play(Sound::kBack);
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
  sub_x_ = -1;
  dl_open_ = false;
  menu_open_ = false;
  list_y_ = kListY;
  // (what these pages show can change while the hub is open: looked at again)
  if (tab == kPaintTab) LoadPaint();
  if (tab == kModsTab) LoadDrops();
  if (tab == kDlcTab && !(task_ && task_kind_ == 6)) LoadDlc();
  if (tab == kDiscoverTab) DiscoverEnsure();
}

bool Hub::Draw(const Pad& pad, float dt) {
  dt_ = std::clamp(dt, 0.0f, 0.1f);
  time_ += dt_;
  toast_time_ = std::max(0.0f, toast_time_ - dt_);
  PollJob();
  PollTask();
  PollDiscover();
  PollDownloads();
  if (!fresh_asked_) fresh_asked_ = true, DiscoverFresh();  // (Home's NEW IN DISCOVER, once)
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
  // the page slides in from the side it came from, fading up
  Ease(page_in_, 1, 11);
  const int first = dl_->VtxBuffer.Size;
  const float shift = (1 - page_in_) * 48 * page_dir_;
  ox_ += shift * s_;
  switch (tab_) {
    case kHome: DrawHome(); break;
    case kSettingsTab: DrawSettings(); break;
    case kModsTab: DrawMods(); break;
    case kDiscoverTab: DrawDiscover(); break;
    case kDownloadsTab: DrawDownloads(); break;
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
  DrawRail();
  DrawModsMenu();
  DrawPopover();
  DrawBrowser();
  DrawHints();
  DrawQrBig();
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
