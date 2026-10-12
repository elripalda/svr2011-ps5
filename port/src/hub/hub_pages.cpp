// WWE SmackDown vs. Raw 2011 - the PS5 hub's Roster, Textures, Music, Movies
// and Saves pages (see hub_ui.h; the PC launcher's tabs of the same names).
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "hub_style.h"
#include "hub_ui.h"

namespace svr2011::hub {

namespace fs = std::filesystem;

namespace {

// The game's characters (the launcher's list: id, name, note).
struct RosterName {
  uint32_t id;
  const wchar_t* name;
  const wchar_t* note;
};
const RosterName kRoster[] = {
#include "../../launcher/roster_names.inc"
};

std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; *w; ++w) s += *w < 0x80 ? char(*w) : '?';
  return s;
}

const char* const kCategories[] = {"", "SMACKDOWN", "RAW", "NPC", "MODS"};
const char* const kCategoryLabels[] = {"Default", "SmackDown", "Raw", "NPC", "Mods"};

std::string Trim(std::string s) {
  s.erase(0, s.find_first_not_of(" \t\r"));
  s.erase(s.find_last_not_of(" \t\r") + 1);
  return s;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Formats the game can play on the PS5 (ps5/sound.h), and other music files
// (listed, but they won't play).
bool Playable(const fs::path& p) {
  const std::string e = Lower(p.extension().string());
  return e == ".mp3" || e == ".wav" || e == ".flac" || e == ".ogg";
}
bool MusicFile(const fs::path& p) {
  const std::string e = Lower(p.extension().string());
  return Playable(p) || e == ".m4a" || e == ".aac" || e == ".wma";
}

int64_t Modified(const fs::path& p) {
  struct stat st {};
  return stat(p.string().c_str(), &st) == 0 ? int64_t(st.st_mtime) : 0;
}

std::string Date(int64_t t) {
  if (!t) return "-";
  const std::time_t tt = std::time_t(t);
  std::tm tm{};
  localtime_r(&tt, &tm);
  char buf[48];
  std::strftime(buf, sizeof(buf), "%b %d, %Y  %H:%M", &tm);
  return buf;
}

// What a save file is, for people.
std::string SaveKind(const std::string& name) {
  const std::string l = Lower(name);
  if (l == "savedata.dat") return "Game save";
  if (l.size() > 2 && l.find("createsuperstar.cas") != std::string::npos)
    return "Created Superstar " + std::to_string(std::atoi(name.c_str()) + 1);
  if (l == "00painttool.pt") return "Paint Tool logos";
  const std::string e = fs::path(l).extension().string();
  if (e == ".rec") return "Replay";
  if (e == ".scn") return "Highlight reel";
  if (e == ".cae" || l.find("entrance") != std::string::npos) return "Created entrance";
  return "Save file";
}

// Copies a folder's contents (recursively) into another; skips the game's
// scratch folder (.mount). False on any error.
bool CopyInto(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) return false;
  for (const auto& e : install::ListDir(from)) {
    const std::string name = e.path().filename().string();
    if (name == ".mount") continue;
    std::error_code ec2;
    fs::copy(e.path(), to / name, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec2);
    if (ec2) return false;
  }
  return !ec;
}

}  // namespace

// --------------------------------------------------------------------- data

void Hub::LoadRoster() {
  roster_.clear();
  std::map<uint32_t, int> tags;
  {
    std::ifstream in(paths_.game / "UserData" / "sort_tags.txt");
    for (std::string line; std::getline(in, line);) {
      if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
      const size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string cat = Upper(Trim(line.substr(eq + 1)));
      for (int c = 1; c < 5; ++c)
        if (cat == kCategories[c]) tags[uint32_t(std::strtoul(line.c_str(), nullptr, 10))] = c;
    }
  }
  for (const RosterName& r : kRoster) {
    Character c;
    c.id = r.id;
    c.name = Narrow(r.name);
    c.note = Narrow(r.note);
    c.group = c.note.find("Not selectable") != std::string::npos ? "MANAGERS AND OTHERS"
              : c.note.find("DLC") != std::string::npos           ? "DLC"
              : c.note.find("Diva") != std::string::npos          ? "DIVAS"
                                                                  : "SUPERSTARS";
    if (auto it = tags.find(c.id); it != tags.end()) c.category = it->second;
    roster_.push_back(std::move(c));
  }
  // superstar mods (Mods/Superstars/slots.txt: folder <tab> id)
  {
    std::ifstream in(paths_.game / "Mods" / "Superstars" / "slots.txt");
    for (std::string line; std::getline(in, line);) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      const std::string folder = line.substr(0, tab);
      Character c;
      c.id = uint32_t(std::strtoul(line.c_str() + tab + 1, nullptr, 10));
      if (!c.id) continue;
      c.name = folder;
      std::ifstream man(paths_.game / "Mods" / "Superstars" / folder / "manifest.txt");
      for (std::string m; std::getline(man, m);)
        if (m.rfind("name", 0) == 0 && m.find('=') != std::string::npos) c.name = Trim(m.substr(m.find('=') + 1));
      c.note = "Superstar mod";
      c.group = "SUPERSTAR MODS";
      if (auto it = tags.find(c.id); it != tags.end()) c.category = it->second;
      roster_.push_back(std::move(c));
    }
  }
  static const std::vector<std::string> order = {"SUPERSTARS", "DIVAS", "DLC", "SUPERSTAR MODS", "MANAGERS AND OTHERS"};
  auto rank = [&](const std::string& g) { return std::find(order.begin(), order.end(), g) - order.begin(); };
  std::stable_sort(roster_.begin(), roster_.end(), [&](const Character& a, const Character& b) {
    return rank(a.group) != rank(b.group) ? rank(a.group) < rank(b.group) : Upper(a.name) < Upper(b.name);
  });
}

// UserData/sort_tags.txt: comments and other ids' lines kept, ours rewritten.
void Hub::SaveRoster() {
  const fs::path dir = paths_.game / "UserData", file = dir / "sort_tags.txt";
  std::set<uint32_t> ours;
  for (const Character& c : roster_) ours.insert(c.id);
  std::vector<std::string> keep;
  {
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string t = Trim(line);
      const bool entry = !t.empty() && t[0] != '#' && t.find('=') != std::string::npos;
      if (entry && ours.count(uint32_t(std::strtoul(t.c_str(), nullptr, 10)))) continue;
      keep.push_back(line);
    }
  }
  if (keep.empty()) {
    keep.push_back("# Sort categories for the game's character lists (the PS5 hub's Roster page):");
    keep.push_back("# <id> = SMACKDOWN | RAW | NPC | MODS");
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    for (const std::string& l : keep) out << l << "\r\n";
    for (const Character& c : roster_)
      if (c.category) out << c.id << " = " << kCategories[c.category] << "\r\n";
  }
  fs::rename(tmp, file, ec);
}

void Hub::LoadPacks() {
  packs_.clear();
  std::vector<std::string> order;
  {
    std::string v = Value("native_texture_packs", "");
    std::stringstream ss(v);
    for (std::string n; std::getline(ss, n, ';');)
      if (!Trim(n).empty()) order.push_back(Trim(n));
  }
  std::error_code ec;
  for (const auto& e : install::ListDir(paths_.game / "Texture Packs")) {
    if (!e.is_directory(ec)) continue;
    Pack p;
    p.name = e.path().filename().string();
    p.bytes = FolderBytes(e.path(), &p.files);
    if (auto it = std::find(order.begin(), order.end(), p.name); it != order.end()) p.order = int(it - order.begin());
    packs_.push_back(std::move(p));
  }
  // on ones first (by priority), then the rest by name
  std::sort(packs_.begin(), packs_.end(), [](const Pack& a, const Pack& b) {
    if ((a.order >= 0) != (b.order >= 0)) return a.order >= 0;
    if (a.order >= 0) return a.order < b.order;
    return Upper(a.name) < Upper(b.name);
  });
  // (priorities without gaps, for packs that went away)
  int next = 0;
  for (Pack& p : packs_)
    if (p.order >= 0) p.order = next++;
}

void Hub::SavePacks() {
  std::vector<const Pack*> on;
  for (const Pack& p : packs_)
    if (p.order >= 0) on.push_back(&p);
  std::sort(on.begin(), on.end(), [](const Pack* a, const Pack* b) { return a->order < b->order; });
  std::string v;
  for (const Pack* p : on) {
    std::string n = p->name;
    for (char& c : n)
      if (c == '"' || c == '\\' || c == ';') c = '_';
    v += (v.empty() ? "" : ";") + n;
  }
  Save("native_texture_packs", "\"" + v + "\"");
}

void Hub::LoadMusic() {
  songs_.clear();
  const fs::path dir = paths_.game / "Music";
  std::error_code ec;
  for (const auto& e : install::ListDir(dir)) {
    if (e.is_directory(ec)) {
      std::vector<fs::path> files;
      std::error_code ec2;
      for (auto s = fs::recursive_directory_iterator(e.path(), ec2); !ec2 && s != fs::recursive_directory_iterator();
           s.increment(ec2))
        if (s->is_regular_file(ec2) && MusicFile(s->path())) files.push_back(s->path());
      if (files.empty()) continue;
      std::sort(files.begin(), files.end());
      Song s;
      s.playlist = e.path().filename().string();
      s.file = files.front();
      s.folder = true;
      s.bytes = fs::file_size(s.file, ec2);
      s.playable = Playable(s.file);
      songs_.push_back(std::move(s));
    } else if (e.is_regular_file(ec) && MusicFile(e.path())) {
      Song s;
      s.playlist = e.path().stem().string();
      s.file = e.path();
      s.bytes = e.file_size(ec);
      s.playable = Playable(s.file);
      songs_.push_back(std::move(s));
    }
  }
  std::sort(songs_.begin(), songs_.end(), [](const Song& a, const Song& b) { return a.playlist < b.playlist; });
}

void Hub::LoadMovies() {
  movies_.clear();
  const fs::path dir = paths_.game / "Custom Movies";
  std::map<std::string, int> ids;
  {
    std::ifstream in(dir / "ids.txt");
    for (std::string line; std::getline(in, line);) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      ids[Trim(line.substr(tab + 1))] = std::atoi(line.c_str());
    }
  }
  std::error_code ec;
  for (const auto& e : install::ListDir(dir)) {
    if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".bik") continue;
    Movie m;
    m.file = e.path();
    m.name = e.path().stem().string();
    m.bytes = e.file_size(ec);
    if (auto it = ids.find(e.path().filename().string()); it != ids.end()) m.id = it->second;
    movies_.push_back(std::move(m));
  }
  std::sort(movies_.begin(), movies_.end(), [](const Movie& a, const Movie& b) { return Upper(a.name) < Upper(b.name); });
}

void Hub::LoadSaves() {
  save_files_.clear();
  backups_.clear();
  std::error_code ec;
  for (const auto& e : install::ListDir(paths_.saves)) {
    const std::string n = e.path().filename().string();
    if (!e.is_regular_file(ec) || n.empty() || n[0] == '.') continue;
    save_files_.push_back({n, e.file_size(ec), Modified(e.path())});
  }
  std::sort(save_files_.begin(), save_files_.end(), [](const SaveFile& a, const SaveFile& b) { return a.name < b.name; });
  for (const auto& e : install::ListDir(paths_.game / "SaveBackups")) {
    if (!e.is_directory(ec)) continue;
    Backup b;
    b.name = e.path().filename().string();
    b.bytes = FolderBytes(e.path(), &b.files);
    backups_.push_back(std::move(b));
  }
  // newest first (their names start with the date)
  std::sort(backups_.begin(), backups_.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
}

void Hub::BackUpSaves(const std::string& why) {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tm);
  fs::path dest = paths_.game / "SaveBackups" / (std::string(stamp) + why);
  std::error_code ec;
  for (int k = 2; fs::exists(dest, ec); ++k) dest = paths_.game / "SaveBackups" / (std::string(stamp) + why + " " + std::to_string(k));
  if (CopyInto(paths_.saves, dest)) {
    if (why.empty()) Toast("Saves backed up");
  } else {
    Toast("The backup couldn't be made");
    Play(Sound::kError);
  }
  LoadSaves();
}

void Hub::RestoreSaves(const Backup& b) {
  BackUpSaves(" (before restore)");
  std::error_code ec;
  for (const auto& e : install::ListDir(paths_.saves)) {
    if (e.path().filename() == ".mount") continue;
    std::error_code ec2;
    fs::remove_all(e.path(), ec2);
  }
  if (CopyInto(paths_.game / "SaveBackups" / b.name, paths_.saves)) {
    Toast("Backup restored");
    Play(Sound::kOn);
  } else {
    Toast("Restoring didn't finish: your saves are in the newest backup");
    Play(Sound::kError);
  }
  LoadSaves();
  item_[kSavesTab] = 0;
}

// ------------------------------------------------------------------- pages

void Hub::DrawRoster() {
  int moved = 0;
  for (const Character& c : roster_) moved += c.category != 0;
  PageTitle("ROSTER", "Where characters are listed when you sort the game's lists. " +
                          (moved ? std::to_string(moved) + " moved. " : std::string()) +
                          "Changes take effect when the game starts.");
  const int n = int(roster_.size());
  if (!n) return;
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) {
    if (i == 0 || roster_[size_t(i)].group != roster_[size_t(i) - 1].group) rows.push_back({-1, roster_[size_t(i)].group});
    rows.push_back({i});
  }
  const float x = kListX, w = kListW;
  List(rows, sel, 64, [&](int i, float ry, float over) {
    const Character& c = roster_[size_t(i)];
    Text(a_.body_bold, 23, x + 24, ry + 8, Mix(White(0.92f), kInk, over), Fit(a_.body_bold, 23, c.name, w - 300));
    Text(a_.body, 16, x + 24, ry + 37, Mix(White(0.4f), Black(0.5f), over), "ID " + std::to_string(c.id));
    const std::string v = kCategoryLabels[c.category];
    const ImU32 col = c.category ? kAccent : Mix(White(0.55f), Black(0.55f), over);
    if (i == sel) {
      const float vr = x + w - 46, cy = ry + 29;
      const float slide = (1 - Smooth(value_in_)) * 18 * value_dir_;
      const int v0 = dl_->VtxBuffer.Size;
      Text(a_.body_bold, 22, vr + slide, ry + 16, c.category ? kAccent : kInk, v, 1);
      Fade(v0, 0.25f + 0.75f * Smooth(value_in_));
      const float vl = vr - TextWidth(a_.body_bold, 22, v) - 18;
      dl_->AddTriangleFilled(P(vl, cy - 7), P(vl, cy + 7), P(vl - 9, cy), Black(0.75f * over));
      dl_->AddTriangleFilled(P(vr + 14, cy - 7), P(vr + 14, cy + 7), P(vr + 23, cy), Black(0.75f * over));
    } else {
      Text(c.category ? a_.body_bold : a_.body, 22, x + w - 24, ry + 16, col, v, 1);
    }
  });
  const Character& c = roster_[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    py += Wrapped(a_.display, 44, px, py, pw, kText, c.name, 1.15f) + 22;
    py = Facts(px, py, pw,
               {{"LISTED UNDER", c.category ? kCategoryLabels[c.category] : "Its own category"},
                {"ID", std::to_string(c.id)},
                {"NOTE", c.note.empty() ? "-" : c.note}});
    py += 20;
    Wrapped(a_.body, 21, px, py, pw, White(0.6f),
            "In lists like Superstar Threads, press the sort button to show one category: SmackDown, Raw, NPC "
            "(the ECW slot) or Mods. Default leaves the character where the game puts them.");
  });
}

void Hub::DrawPacks() {
  int on = 0;
  for (const Pack& p : packs_) on += p.order >= 0;
  PageTitle("TEXTURES", packs_.empty() ? std::string("Texture packs you add show up here.")
                                       : std::to_string(packs_.size()) + " packs, " + std::to_string(on) +
                                             " on. Changes take effect when the game starts.");
  if (packs_.empty()) {
    Empty("No texture packs yet",
          "Copy each pack as a folder over FTP into /data/homebrew/PPSA92011/Texture Packs/ - PNG or DDS files "
          "named by the texture they replace, as packs for the PC version come. They show up here the next time you "
          "open the app.");
    return;
  }
  const int n = int(packs_.size());
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n);
  std::vector<Row> rows;
  rows.push_back({-1, "PACKS  " + std::to_string(n)});
  for (int i = 0; i < n; ++i) rows.push_back({i});
  rows.push_back({-1, "FOR PACK MAKERS"});
  rows.push_back({n});
  const bool dump = Value("native_dump_textures", "false") == "true";
  const float x = kListX, w = kListW;
  List(rows, sel, 76, [&](int i, float ry, float over) {
    if (i == n) {
      Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(0.94f), kInk, over), "Dump Textures");
      Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), "Saves the game's textures to Texture Dumps");
      Switch(x + w - 24 - 58, ry + 19, Knob(i, dump), over > 0.5f);
      return;
    }
    const Pack& p = packs_[size_t(i)];
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(p.order >= 0 ? 0.94f : 0.5f), kInk, over),
         Fit(a_.body_bold, 24, p.name, w - 300));
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over),
         std::to_string(p.files) + (p.files == 1 ? " file" : " files") + "  \xC2\xB7  " + Size(p.bytes));
    if (p.order >= 0)
      Text(a_.body_bold, 17, x + w - 24 - 58 - 20, ry + 25, Mix(White(0.55f), Black(0.6f), over),
           "PRIORITY " + std::to_string(p.order + 1), 1, 1.5f);
    Switch(x + w - 24 - 58, ry + 19, Knob(i, p.order >= 0), over > 0.5f);
  });
  Panel([&](float px, float py, float pw) {
    if (sel == n) {
      Text(a_.display, 44, px, py, kText, "Dump Textures");
      py += 72;
      Wrapped(a_.body, 22, px, py, pw, White(0.65f),
              "Saves every texture the game shows into Texture Dumps, named by the hash a pack uses to replace it. "
              "It slows the game down: turn it off when you're done.");
      return;
    }
    const Pack& p = packs_[size_t(sel)];
    py += Wrapped(a_.display, 44, px, py, pw, kText, p.name, 1.15f) + 22;
    py = Facts(px, py, pw,
               {{"STATUS", p.order >= 0 ? "On" : "Off"},
                {"PRIORITY", p.order >= 0 ? std::to_string(p.order + 1) + " of " + std::to_string(on) : "-"},
                {"FILES", std::to_string(p.files)},
                {"SIZE", Size(p.bytes)}});
    py += 20;
    Wrapped(a_.body, 21, px, py, pw, White(0.6f),
            "When two packs change the same texture, the higher priority wins. A pack you turn on goes last; Triangle "
            "moves it up.");
  });
}

void Hub::DrawMusic() {
  PageTitle("MUSIC", songs_.empty() ? std::string("Songs you add show up here.")
                                    : std::to_string(songs_.size()) + " songs for USER PLAYLIST and the jukebox.");
  if (songs_.empty()) {
    Empty("No music yet",
          "Copy MP3, WAV, FLAC or OGG songs over FTP into /data/homebrew/PPSA92011/Music/. Each song becomes a "
          "playlist named after it (a folder works too: its first song). Pick them in Create An Entrance > Music > "
          "USER PLAYLIST.");
    return;
  }
  const int n = int(songs_.size());
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) rows.push_back({i});
  const float x = kListX, w = kListW;
  List(rows, sel, 76, [&](int i, float ry, float over) {
    const Song& s = songs_[size_t(i)];
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(s.playable ? 0.94f : 0.5f), kInk, over),
         Fit(a_.body_bold, 24, s.playlist, w - 200));
    std::string sub = Upper(s.file.extension().string().substr(1)) + "  \xC2\xB7  " + Size(s.bytes);
    if (s.folder) sub += "  \xC2\xB7  " + s.file.filename().string();
    if (!s.playable) sub += "  \xC2\xB7  won't play on PS5";
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), Fit(a_.body, 18, sub, w - 200));
    if (i == previewing_) {  // playing: a little equalizer
      for (int b = 0; b < 4; ++b) {
        const float hgt = 8 + 14 * (0.5f + 0.5f * std::sin(time_ * (7.0f + b * 2.3f) + b * 1.7f));
        Rect(x + w - 60 + b * 9, ry + 46 - hgt, 5, hgt, over > 0.5f ? kAccent : kAccent, 1.5f);
      }
    }
  });
  const Song& s = songs_[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    py += Wrapped(a_.display, 44, px, py, pw, kText, s.playlist, 1.15f) + 22;
    py = Facts(px, py, pw,
               {{"FILE", s.file.filename().string()},
                {"FORMAT", Upper(s.file.extension().string().substr(1)) + (s.playable ? "" : " (convert to MP3)")},
                {"SIZE", Size(s.bytes)}});
    py += 20;
    Wrapped(a_.body, 21, px, py, pw, White(0.6f),
            "In the game: Create An Entrance > Music > USER PLAYLIST, and My WWE > Jukebox > My Music.");
  });
}

void Hub::DrawMovies() {
  PageTitle("MOVIES", movies_.empty() ? std::string("Entrance movies you add show up here.")
                                      : std::to_string(movies_.size()) + " entrance movies for USER MOVIES.");
  if (movies_.empty()) {
    Empty("No movies yet",
          "Make entrance movies with the PC launcher's Movie Maker, then copy the .bik files over FTP into "
          "/data/homebrew/PPSA92011/Custom Movies/. Pick them in Create An Entrance > Finalize > Movie > USER "
          "MOVIES.");
    return;
  }
  const int n = int(movies_.size());
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) rows.push_back({i});
  const float x = kListX, w = kListW;
  List(rows, sel, 76, [&](int i, float ry, float over) {
    const Movie& m = movies_[size_t(i)];
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 24, m.name, w - 200));
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over),
         (m.id ? "Movie " + std::to_string(m.id) : std::string("New: numbered at the next start")) + "  \xC2\xB7  " +
             Size(m.bytes));
  });
  const Movie& m = movies_[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    py += Wrapped(a_.display, 44, px, py, pw, kText, m.name, 1.15f) + 22;
    py = Facts(px, py, pw,
               {{"MOVIE", m.id ? std::to_string(m.id) : "Numbered at the next start"},
                {"FILE", m.file.filename().string()},
                {"SIZE", Size(m.bytes)}});
    py += 20;
    Wrapped(a_.body, 21, px, py, pw, White(0.6f),
            "Pick it in Create An Entrance > Finalize > Movie > USER MOVIES. New movies come from the PC launcher's "
            "Movie Maker (320x320 Bink).");
  });
}

void Hub::DrawSaves() {
  PageTitle("SAVES", std::to_string(save_files_.size()) + " save files, " + std::to_string(backups_.size()) +
                         (backups_.size() == 1 ? " backup." : " backups."));
  const int nb = int(backups_.size()), nf = int(save_files_.size());
  const int n = 1 + nb + nf;
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, n - 1);
  std::vector<Row> rows;
  rows.push_back({-1, "BACKUPS"});
  for (int i = 0; i <= nb; ++i) rows.push_back({i});
  if (nf) {
    rows.push_back({-1, "YOUR SAVES  " + std::to_string(nf)});
    for (int i = 0; i < nf; ++i) rows.push_back({1 + nb + i});
  }
  const float x = kListX, w = kListW;
  List(rows, sel, 76, [&](int i, float ry, float over) {
    std::string name, sub;
    if (i == 0) {
      name = "Back Up Now", sub = "Copies your saves into SaveBackups";
      // (a plus sign)
      const ImU32 c = Mix(White(0.9f), kInk, over);
      Rect(x + w - 24 - 26, ry + 34, 26, 3, c, 1.5f);
      Rect(x + w - 24 - 14.5f, ry + 22.5f, 3, 26, c, 1.5f);
    } else if (i <= nb) {
      const Backup& b = backups_[size_t(i - 1)];
      name = b.name, sub = std::to_string(b.files) + (b.files == 1 ? " file" : " files") + "  \xC2\xB7  " + Size(b.bytes);
    } else {
      const SaveFile& f = save_files_[size_t(i - 1 - nb)];
      name = SaveKind(f.name), sub = f.name + "  \xC2\xB7  " + Size(f.bytes) + "  \xC2\xB7  " + Date(f.modified);
    }
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 24, name, w - 120));
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), Fit(a_.body, 18, sub, w - 120));
  });
  Panel([&](float px, float py, float pw) {
    if (sel == 0) {
      Text(a_.display, 44, px, py, kText, "Back Up Now");
      Wrapped(a_.body, 22, px, py + 72, pw, White(0.65f),
              "Copies everything in Saves - your game save, Created Superstars, Paint Tool logos, replays - into "
              "SaveBackups, named by the date and time.");
    } else if (sel <= nb) {
      const Backup& b = backups_[size_t(sel - 1)];
      py += Wrapped(a_.display, 44, px, py, pw, kText, b.name, 1.15f) + 22;
      py = Facts(px, py, pw, {{"FILES", std::to_string(b.files)}, {"SIZE", Size(b.bytes)}});
      Wrapped(a_.body, 21, px, py + 20, pw, White(0.6f),
              "Restoring puts these saves back. The saves you have now are backed up first.");
    } else {
      const SaveFile& f = save_files_[size_t(sel - 1 - nb)];
      py += Wrapped(a_.display, 44, px, py, pw, kText, SaveKind(f.name), 1.15f) + 22;
      Facts(px, py, pw, {{"FILE", f.name}, {"SIZE", Size(f.bytes)}, {"CHANGED", Date(f.modified)}});
    }
  });
}

}  // namespace svr2011::hub
