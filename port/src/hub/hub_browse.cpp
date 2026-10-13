// WWE SmackDown vs. Raw 2011 - the PS5 hub's Find Mods (see hub_ui.h): a
// browser for .svrmod files on USB drives, extended storage and the
// console's own folders, driven with the controller. Mods are installed
// from where they are, and their files are kept.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "hub_style.h"
#include "hub_ui.h"

namespace svr2011::hub {

namespace fs = std::filesystem;

namespace {

std::string LowerCase(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool IsSvrmod(const fs::path& p) { return LowerCase(p.extension().string()) == ".svrmod"; }

// The places to start from: the ones that are there.
std::vector<std::pair<std::string, fs::path>> Places(const fs::path& game) {
  std::vector<std::pair<std::string, fs::path>> out;
  std::error_code ec;
  for (int i = 0; i < 8; ++i) {
    const fs::path p = "/mnt/usb" + std::to_string(i);
    if (fs::is_directory(p, ec)) out.push_back({"USB drive " + std::to_string(i + 1), p});
  }
  for (int i = 0; i < 2; ++i) {
    const fs::path p = "/mnt/ext" + std::to_string(i);
    if (fs::is_directory(p, ec)) out.push_back({i ? "Extended storage 2" : "Extended storage", p});
  }
  out.push_back({"This app's folder", game});
  if (fs::is_directory("/data", ec)) out.push_back({"Console storage", "/data"});
  return out;
}

// What's a mod's type, for people.
std::string TypeName(const std::string& type) {
  const std::string t = LowerCase(type);
  return t == "superstar" ? "Superstar" : t == "arena" ? "Arena" : t == "backstage" ? "Backstage" : t == "moves" ? "Move pack"
         : t == "signs" ? "Crowd signs" : t == "media" ? "Media" : t == "matchtype" ? "Match type" : type;
}

}  // namespace

int Hub::ModStatus(const install::ModInfo& info) const {
  if (info.id.empty()) return 0;
  for (const Mod& m : mods_)
    if (LowerCase(m.id) == LowerCase(info.id)) return m.version == info.version ? 1 : 2;
  return 0;
}

void Hub::OpenBrowser() {
  br_ = Browser{};
  br_.open = true;
  // (straight into the only USB drive, when there's one)
  const auto places = Places(paths_.game);
  int usb = 0;
  fs::path drive;
  for (const auto& [label, p] : places)
    if (p.string().rfind("/mnt/usb", 0) == 0) ++usb, drive = p;
  BrowseTo(usb == 1 ? drive : fs::path());
  Play(Sound::kSelect);
}

void Hub::BrowseTo(const fs::path& dir, const fs::path& focus) {
  br_.dir = dir;
  br_.entries.clear();
  br_.mods = 0;
  if (dir.empty()) {
    for (const auto& [label, p] : Places(paths_.game)) {
      Browser::Entry e;
      e.kind = Browser::Entry::kPlace;
      e.label = label;
      e.sub = p.string();
      e.path = p;
      br_.entries.push_back(std::move(e));
    }
  } else {
    std::vector<Browser::Entry> folders, mods;
    std::error_code ec;
    for (const auto& d : install::ListDir(dir)) {
      const std::string name = d.path().filename().string();
      if (name.empty() || name[0] == '.' || name[0] == '$') continue;  // (hidden, $RECYCLE.BIN...)
      if (d.is_directory(ec)) {
        Browser::Entry e;
        e.kind = Browser::Entry::kFolder;
        e.label = name;
        e.path = d.path();
        folders.push_back(std::move(e));
      } else if (IsSvrmod(d.path()) && d.is_regular_file(ec)) {
        Browser::Entry e;
        e.kind = Browser::Entry::kMod;
        e.path = d.path();
        e.bytes = d.file_size(ec);
        install::PeekMod(d.path(), &e.info);
        e.label = e.info.name.empty() ? d.path().stem().string() : e.info.name;
        mods.push_back(std::move(e));
      }
      if (folders.size() + mods.size() >= 4000) break;  // (a huge folder: the first ones)
    }
    auto by_name = [](const Browser::Entry& a, const Browser::Entry& b) {
      return LowerCase(a.label) < LowerCase(b.label);
    };
    std::sort(folders.begin(), folders.end(), by_name);
    std::sort(mods.begin(), mods.end(), by_name);
    br_.mods = int(mods.size());
    // the mods first (what you came for), then the folders
    for (auto& e : mods) br_.entries.push_back(std::move(e));
    for (auto& e : folders) br_.entries.push_back(std::move(e));
  }
  br_.sel = 0;
  for (size_t i = 0; i < br_.entries.size(); ++i)
    if (!focus.empty() && br_.entries[i].path == focus) br_.sel = int(i);
  br_.scroll = br_.focus = float(br_.sel);
}

void Hub::BrowserInput(uint32_t p) {
  const int n = int(br_.entries.size());
  if (n) {
    const int before = br_.sel;
    if (p & kUp) br_.sel = (br_.sel + n - 1) % n;
    if (p & kDown) br_.sel = (br_.sel + 1) % n;
    if (p & kL1) br_.sel = std::max(0, br_.sel - 8);  // (a page at a time)
    if (p & kR1) br_.sel = std::min(n - 1, br_.sel + 8);
    if (br_.sel != before) Play(Sound::kMove);
  }
  if ((p & kCross) && n && br_.in > 0.6f) {
    const Browser::Entry& e = br_.entries[size_t(br_.sel)];
    if (e.kind == Browser::Entry::kMod) {
      if (e.info.type.empty()) {
        Play(Sound::kError);
        Toast(e.path.filename().string() + " can't be read: it isn't an SvR 2011 mod");
      } else if (!Busy()) {
        press_ = 1;
        Play(Sound::kSelect);
        StartModInstalls({e.path});
      }
    } else {
      const fs::path to = e.path;
      Play(Sound::kSelect);
      BrowseTo(to);
    }
    return;
  }
  if ((p & kTriangle) && br_.mods > 1) {  // every mod in this folder not installed yet (or another version)
    std::vector<fs::path> files;
    for (const Browser::Entry& e : br_.entries)
      if (e.kind == Browser::Entry::kMod && !e.info.type.empty() && ModStatus(e.info) != 1) files.push_back(e.path);
    if (files.empty()) {
      Play(Sound::kError);
      Toast("All the mods here are installed");
      return;
    }
    if (Busy()) return;
    press_ = 1;
    Play(Sound::kSelect);
    StartModInstalls(std::move(files));
    return;
  }
  if (p & kCircle) {
    Play(Sound::kBack);
    if (br_.dir.empty()) {
      br_.open = false;
      LoadDrops();
      return;
    }
    // up a folder; from a place's top, back to the places
    for (const auto& [label, place] : Places(paths_.game)) {
      if (br_.dir == place) {
        BrowseTo({}, place);
        return;
      }
    }
    const fs::path here = br_.dir;
    BrowseTo(here.parent_path(), here);
  }
}

float Hub::BrowserHints(float r) {
  r = Hint(r, kCircle, br_.dir.empty() ? "Close" : "Back");
  if (br_.mods > 1) r = Hint(r, kTriangle, "Install All Here");
  if (!br_.entries.empty()) {
    const Browser::Entry& e = br_.entries[size_t(std::clamp(br_.sel, 0, int(br_.entries.size()) - 1))];
    r = Hint(r, kCross, e.kind == Browser::Entry::kMod ? (ModStatus(e.info) ? "Install Again" : "Install") : "Open");
  }
  return r;
}

void Hub::DrawBrowser() {
  Ease(br_.in, br_.open ? 1.0f : 0.0f, 14);
  if (br_.in < 0.01f) return;
  const float a = Smooth(br_.in);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.7f * a));
  const int n = int(br_.entries.size()), shown = 8;
  const float w = 1240, rh = 70, x = (kW - w) * 0.5f;
  const float ly0 = 196;  // (the list's top, from the panel's)
  const float h = ly0 + shown * rh + 84;
  const float y = (kH - h) * 0.5f - 20 + (1 - a) * 30;
  const int v0 = dl_->VtxBuffer.Size;
  Rect(x, y, w, h, Rgba(22, 23, 27), 18);
  dl_->AddRectFilled(P(x, y), P(x + 6, y + h), kAccent, 18 * s_, ImDrawFlags_RoundCornersLeft);
  Text(a_.display, 46, x + 52, y + 38, kText, "FIND MODS");
  // where we are
  std::string where = "Choose where your .svrmod files are";
  if (!br_.dir.empty()) {
    where = br_.dir.string();
    for (const auto& [label, place] : Places(paths_.game)) {
      const std::string ps = place.string();
      if (where == ps) where = label;
      else if (where.rfind(ps + "/", 0) == 0) where = label + "  \xC2\xBB  " + where.substr(ps.size() + 1);
      else continue;
      break;
    }
    // (the path's folders separated as the place's)
    for (size_t i = 0; (i = where.find('/', i)) != std::string::npos;) where.replace(i, 1, "  \xC2\xBB  ");
  }
  Text(a_.body_bold, 21, x + 54, y + 108, White(0.55f), Fit(a_.body_bold, 21, where, w - 300));
  if (!br_.dir.empty()) {
    const std::string count = br_.mods ? std::to_string(br_.mods) + (br_.mods == 1 ? " mod here" : " mods here")
                                       : std::string("No mods here");
    Text(a_.body_bold, 21, x + w - 52, y + 108, br_.mods ? WithAlpha(kAccent, 1) : White(0.4f), count, 1);
  }
  Rect(x + 52, y + 150, w - 104, 1, White(0.08f));
  const float ly = y + ly0;
  if (n == 0) {
    Wrapped(a_.body, 24, x + 54, ly + 20, w - 108, White(0.6f),
            br_.dir.empty() ? "No drives or folders to look in."
                            : "Nothing here: no .svrmod files or folders. Press Circle to go back.");
  } else {
    br_.sel = std::clamp(br_.sel, 0, n - 1);
    const float target = float(std::clamp(br_.sel - shown / 2, 0, std::max(0, n - shown)));
    br_.scroll += (target - br_.scroll) * (1 - std::exp(-dt_ * 16));
    br_.focus += (float(br_.sel) - br_.focus) * (1 - std::exp(-dt_ * 20));
    if (br_.in < 0.05f) br_.focus = float(br_.sel), br_.scroll = target;
    dl_->PushClipRect(P(x, ly - 2), P(x + w, ly + shown * rh), true);
    Rect(x + 36, ly + (br_.focus - br_.scroll) * rh, w - 72, rh - 6, kText, 10);
    for (int i = 0; i < n; ++i) {
      const float ry = ly + (float(i) - br_.scroll) * rh;
      if (ry < ly - rh || ry > ly + shown * rh) continue;
      const float over = std::clamp(1 - std::fabs(br_.focus - float(i)), 0.0f, 1.0f);
      const Browser::Entry& e = br_.entries[size_t(i)];
      const float ix = x + 62, iy = ry + rh * 0.5f - 3;
      // its icon: a folder / a drive, or a mod (a red tile)
      if (e.kind == Browser::Entry::kMod) {
        Rect(ix, iy - 15, 30, 30, WithAlpha(kAccent, e.info.type.empty() ? 0.35f : 1.0f), 6);
        Text(a_.display, 18, ix + 15, iy - 13, White(0.95f), "M", 0.5f);
      } else {
        const ImU32 c = Mix(White(0.55f), Black(0.55f), over);
        if (e.kind == Browser::Entry::kPlace) {
          Rect(ix, iy - 10, 32, 20, c, 4);
          dl_->AddCircleFilled(P(ix + 25, iy), 2.5f * s_, Mix(Rgba(22, 23, 27), kText, over), 10);
        } else {
          Rect(ix, iy - 12, 13, 6, c, 2);
          Rect(ix, iy - 8, 32, 22, c, 3);
        }
      }
      const float tx = x + 116, tw = w - 116 - 300;
      if (e.kind == Browser::Entry::kMod) {
        Text(a_.body_bold, 23, tx, ry + 9, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 23, e.label, tw));
        std::string sub = e.info.type.empty() ? std::string("Can't be read") : TypeName(e.info.type);
        if (!e.info.version.empty()) sub += "  \xC2\xB7  v" + e.info.version;
        sub += "  \xC2\xB7  " + Size(e.bytes) + "  \xC2\xB7  " + e.path.filename().string();
        Text(a_.body, 17, tx, ry + 39, Mix(White(0.42f), Black(0.55f), over), Fit(a_.body, 17, sub, tw));
        // its state
        const bool working = task_ && task_kind_ == 5 && (task_file_ == e.path || task_file_.empty());
        const int st = ModStatus(e.info);
        const char* label = working ? "Installing..." : st == 1 ? "Installed" : st == 2 ? "Other version installed" : "";
        if (*label) {
          const float sx = x + w - 62 - TextWidth(a_.body_bold, 18, label);
          if (working || st != 1)
            dl_->AddCircleFilled(P(sx - 14, ry + 33), 4.5f * s_,
                                 WithAlpha(kAccent, 0.6f + 0.4f * std::sin(time_ * (working ? 9 : 3))), 16);
          Text(a_.body_bold, 18, sx, ry + 22, Mix(White(st == 1 && !working ? 0.5f : 0.9f), Black(0.7f), over), label);
        }
      } else {
        Text(a_.body_bold, 23, tx, ry + (e.sub.empty() ? 21 : 9), Mix(White(0.94f), kInk, over),
             Fit(a_.body_bold, 23, e.label, tw));
        if (!e.sub.empty())
          Text(a_.body, 17, tx, ry + 39, Mix(White(0.42f), Black(0.55f), over), Fit(a_.body, 17, e.sub, tw));
        // (a chevron: it opens)
        const ImU32 cc = Mix(White(0.35f), Black(0.5f), over);
        const ImVec2 chev[3] = {P(x + w - 74, ry + rh * 0.5f - 13), P(x + w - 64, ry + rh * 0.5f - 3), P(x + w - 74, ry + rh * 0.5f + 7)};
        dl_->AddPolyline(chev, 3, cc, ImDrawFlags_None, 2.5f * s_);
      }
    }
    dl_->PopClipRect();
    if (n > shown) {
      const float bh = shown * rh * shown / n, by = ly + (shown * rh - bh) * br_.scroll / float(n - shown);
      Rect(x + w - 24, by, 3, bh - 6, White(0.4f), 1.5f);
    }
  }
  // the bottom line: an install going, or how it works
  const float fy = y + h - 56;
  if (const std::string st = ModInstallsStatus(); !st.empty()) {
    dl_->AddCircleFilled(P(x + 60, fy + 12), 4.5f * s_, WithAlpha(kAccent, 0.5f + 0.5f * std::sin(time_ * 6)), 16);
    Text(a_.body_bold, 19, x + 76, fy + 1, White(0.75f), Fit(a_.body_bold, 19, st, w - 140));
  } else {
    Text(a_.body, 19, x + 54, fy + 1, White(0.4f),
         "Mods are installed from where they are: the .svrmod files stay. Copy them onto a USB drive from your PC.");
  }
  Fade(v0, a);
}

}  // namespace svr2011::hub
