// WWE SmackDown vs. Raw 2011 - the PS5 hub's Install and Paint Tool pages
// (see hub_ui.h), and the list to pick from that both use.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "hub_style.h"
#include "hub_ui.h"

namespace svr2011::hub {

namespace fs = std::filesystem;

// An installer job on its thread (one at a time).
struct Hub::Task {
  std::thread thread;
  install::Progress p;
  bool ok = false;  // (a mod: installed)
  std::string message;
  void Cancel() { p.cancel = true; }
  ~Task() {
    p.cancel = true;  // (the hub closing: stops at the next piece)
    if (thread.joinable()) thread.join();
  }
};

namespace {

// Where a file is, for people: the app folder, a USB drive...
std::string Where(const fs::path& file, const fs::path& game) {
  const std::string dir = file.parent_path().string(), g = game.string();
  if (dir == g || dir.rfind(g + "/", 0) == 0) {
    return dir == g ? "The app folder" : "The app folder: " + dir.substr(g.size() + 1);
  }
  if (dir.rfind("/mnt/usb", 0) == 0) return "USB drive " + std::to_string(dir.size() > 8 ? dir[8] - '0' + 1 : 1);
  if (dir.rfind("/mnt/ext", 0) == 0) return "Extended storage";
  return dir;
}

bool Picture(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = char(std::tolower(static_cast<unsigned char>(c)));
  return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga" || e == ".gif";
}

std::string LogoName(int page, int k) {
  return page == 0 ? "Logo " + std::to_string(k + 1) : "Page " + std::to_string(page + 1) + " logo " + std::to_string(k + 1);
}

constexpr const char* kSteps[] = {"Game files from the disc", "Shaders and button icons", "Bundled mods", "Folders"};

}  // namespace

// ------------------------------------------------------------------- jobs

void Hub::LoadInstall(bool scan) {
  state_ = install::Look(paths_.game);
  if (scan) found_ = install::Scan(paths_.game);
}

bool Hub::Busy() {
  if (!task_) return false;
  Play(Sound::kError);
  Toast("Wait for the current job to finish");
  return true;
}

void Hub::StartInstall(const fs::path& image, const fs::path& port, std::vector<std::string> only) {
  if (Busy()) return;
  auto t = std::make_shared<Task>();
  install::Plan plan{paths_.game, image, port, std::move(only)};
  task_kind_ = 1;
  task_file_ = image;
  task_title_ = !image.empty() ? image.filename().string() : port.filename().string();
  task_speed_ = 0, task_last_ = 0, task_in_ = 0;
  Task* raw = t.get();
  t->thread = std::thread([raw, plan] { install::Install(plan, raw->p); });
  task_ = std::move(t);
}

void Hub::StartVerify() {
  if (Busy()) return;
  auto t = std::make_shared<Task>();
  task_kind_ = 2;
  task_file_.clear();
  task_title_ = "Every game file, compared with the disc's";
  task_speed_ = 0, task_last_ = 0, task_in_ = 0;
  Task* raw = t.get();
  const fs::path game = paths_.game;
  t->thread = std::thread([raw, game] { install::Verify(game, raw->p); });
  task_ = std::move(t);
}

void Hub::StartModInstall(const fs::path& svrmod) {
  if (Busy()) return;
  auto t = std::make_shared<Task>();
  task_kind_ = 3;
  task_file_ = svrmod;
  task_title_ = svrmod.filename().string();
  Task* raw = t.get();
  const fs::path game = paths_.game;
  t->thread = std::thread([raw, game, svrmod] {
    raw->ok = install::InstallMod(game, svrmod, false, &raw->message);
    raw->p.finished = true;
  });
  task_ = std::move(t);
}

// The bundled mods, at the first start (the game's files come from the app
// store, which doesn't unpack them).
void Hub::StartBundled() {
  if (task_) return;
  auto t = std::make_shared<Task>();
  task_kind_ = 4;
  task_file_.clear();
  task_title_ = "The bundled mods";
  Task* raw = t.get();
  const fs::path game = paths_.game;
  t->thread = std::thread([raw, game] { install::InstallBundledMods(game, raw->p); });
  task_ = std::move(t);
}

std::string Hub::BundledStatus() {
  if (!task_ || task_kind_ != 4) return {};
  const uint64_t done = task_->p.done, total = task_->p.total;
  return total ? "Setting up the bundled mods: " + std::to_string(std::min(done, total)) + " of " + std::to_string(total)
               : std::string("Setting up the bundled mods");
}

void Hub::StopTask() {
  if (task_) task_->Cancel();
}

void Hub::PollTask() {
  if (!task_) return;
  // the speed, smoothed
  const uint64_t done = task_->p.done;
  if (dt_ > 0 && done >= task_last_ && task_last_) {
    const float now = float(done - task_last_) / dt_;
    task_speed_ = task_speed_ <= 0 ? now : task_speed_ + (now - task_speed_) * (1 - std::exp(-dt_ * 1.5f));
  }
  task_last_ = done ? done : 1;
  if (!task_->p.finished) return;
  // (a question or a list open: its answer isn't taken over - the outcome waits)
  if (confirm_ || choice_ || kb_.open) return;
  std::shared_ptr<Task> t = std::move(task_);
  if (t->thread.joinable()) t->thread.join();
  const int kind = task_kind_;
  task_kind_ = 0;
  if (kind == 4) {  // the bundled mods
    LoadMods();
    if (t->p.mods_installed) Toast(t->p.result);
    return;
  }
  if (kind == 3) {  // a mod
    if (t->ok) {
      std::error_code ec;
      fs::remove(task_file_, ec);  // (it's in Mods now)
      LoadMods();
      Play(Sound::kOn);
    } else {
      LoadDrops();
      Play(Sound::kError);
    }
    Toast(t->message);
    return;
  }
  LoadInstall(true);
  if (kind == 2) {  // a check
    install::Progress& p = t->p;
    verify_result_ = p.result;
    std::vector<std::string> bad = p.damaged;
    bad.insert(bad.end(), p.missing.begin(), p.missing.end());
    const install::DiscImage* image = nullptr;
    for (const install::DiscImage& d : found_.images)
      if (d.state == install::DiscImage::kThisGame) image = image ? image : &d;
    if (!p.ok || bad.empty() || p.other_edition) {
      Play(bad.empty() && p.ok ? Sound::kOn : Sound::kError);
      Toast(p.result);
    } else if (image) {
      Play(Sound::kError);
      const fs::path file = image->file;
      Confirm("Repair the game files?",
              p.result + " They can make the game crash while it loads. Copy them again from " +
                  file.filename().string() + "?",
              "Repair", [this, file, bad] { StartInstall(file, {}, bad); });
    } else {
      Play(Sound::kError);
      Notice("Some game files need repairing",
             p.result + " Put your disc image back in the app folder (or on a USB drive), then check again to repair "
                        "them.");
    }
    return;
  }
  // an install
  install::Progress& p = t->p;
  LoadMods();
  LoadRoster();
  if (!p.ok) {
    Play(Sound::kError);
    Notice(p.cancel ? "Install stopped" : "The install didn't finish", p.result);
    return;
  }
  Play(Sound::kStart);
  std::error_code ec;
  const fs::path image = task_file_;
  if (state_.Installed() && !image.empty() && fs::exists(image, ec) && t->p.damaged.empty()) {
    const uint64_t bytes = fs::file_size(image, ec);
    Confirm("The game is installed",
            p.result + " Delete the disc image to free " + Size(bytes) +
                "? Keep it if you'd like to check or repair the game files later.",
            "Delete It", [this, image] {
              std::error_code e2;
              fs::remove(image, e2);
              Toast(e2 ? "The disc image couldn't be deleted" : "Disc image deleted");
              LoadInstall(true);
            });
  } else {
    Toast(p.result);
  }
}

// --------------------------------------------------------------- install page

namespace {
struct InstallRow {
  int kind;  // 0 a disc image, 1 the PC port's files, 2 check the game files
  int index;
};
}  // namespace

static std::vector<InstallRow> InstallRowsOf(const install::Found& f, const install::State& s) {
  std::vector<InstallRow> rows;
  for (int i = 0; i < int(f.images.size()); ++i) rows.push_back({0, i});
  for (int i = 0; i < int(f.ports.size()); ++i) rows.push_back({1, i});
  if (s.present) rows.push_back({2, 0});
  return rows;
}

int Hub::InstallItems() const { return int(InstallRowsOf(found_, state_).size()); }

void Hub::InstallActivate(int dir) {
  if (dir == 2) {  // look again
    LoadInstall(true);
    Play(Sound::kSelect);
    const size_t n = found_.images.size();
    Toast(n ? std::to_string(n) + (n == 1 ? " disc image found" : " disc images found") : "No disc image found");
    return;
  }
  const auto rows = InstallRowsOf(found_, state_);
  const int sel = item_[tab_];
  if (sel < 0 || sel >= int(rows.size())) return;
  const InstallRow r = rows[size_t(sel)];
  if (dir == 1) {  // delete the file
    if (r.kind == 2 || Busy()) return;
    const fs::path file = r.kind == 0 ? found_.images[size_t(r.index)].file : found_.ports[size_t(r.index)].where;
    std::error_code ec;
    if (fs::is_directory(file, ec)) {
      Play(Sound::kError);
      Toast("Folders are left alone: delete it over FTP");
      return;
    }
    Play(Sound::kSelect);
    Confirm("Delete this file?", file.filename().string() + " is deleted from the console, freeing " +
                                     Size(fs::file_size(file, ec)) + ".",
            "Delete", [this, file] {
              std::error_code e2;
              fs::remove(file, e2);
              Toast(e2 ? "It couldn't be deleted" : file.filename().string() + " deleted");
              LoadInstall(true);
              item_[tab_] = 0;
            });
    return;
  }
  if (Busy()) return;
  if (r.kind == 2) {
    Play(Sound::kSelect);
    StartVerify();
    return;
  }
  if (r.kind == 1) {
    Play(Sound::kSelect);
    StartInstall({}, found_.ports[size_t(r.index)].where);
    return;
  }
  const install::DiscImage& d = found_.images[size_t(r.index)];
  if (d.state != install::DiscImage::kThisGame) {
    Play(Sound::kError);
    Toast(d.note.empty() ? "This disc image can't be used" : d.note);
    return;
  }
  Play(Sound::kSelect);
  StartInstall(d.file, found_.ports.empty() ? fs::path() : found_.ports.front().where);
}

float Hub::InstallHints(float r) {
  if (task_ && task_kind_ != 3) return r;
  const auto rows = InstallRowsOf(found_, state_);
  const int sel = item_[tab_];
  r = Hint(r, kSquare, "Look Again");
  if (sel < 0 || sel >= int(rows.size())) return r;
  const InstallRow row = rows[size_t(sel)];
  if (row.kind != 2) r = Hint(r, kTriangle, "Delete File");
  r = Hint(r, kCross, row.kind == 2 ? "Check" : row.kind == 1 ? "Install These" : state_.Installed() ? "Install Again" : "Install");
  return r;
}

void Hub::DrawInstall() {
  if (task_ && task_kind_ != 3) {
    DrawInstallProgress();
    return;
  }
  std::string sub;
  if (state_.Installed()) {
    sub = "The game is installed";
    sub += state_.shaders ? "" : ", but the PC port's shaders are missing";
    sub += state_.bundled ? " - with " + std::to_string(state_.bundled) + " bundled mods." : ".";
  } else if (state_.present) {
    sub = std::to_string(state_.required - state_.present) + " of the game's " + std::to_string(state_.required) +
          " files are missing (" + Size(state_.missing_bytes) + ").";
  } else {
    sub = "Set the game up from your own disc image. It takes a few minutes.";
  }
  PageTitle("INSTALL", sub);
  const auto rows = InstallRowsOf(found_, state_);
  if (rows.empty()) {
    // (the app ships with the PC port's shaders, button icons and bundled mods: then only the disc is needed)
    Empty("Add your disc image",
          state_.shaders
              ? "Copy your SmackDown vs. Raw 2011 Xbox 360 disc image (.iso) over FTP into /data/homebrew/PPSA92011/, "
                "or put it on a USB drive. Then press Square to look again."
              : "Copy your SmackDown vs. Raw 2011 Xbox 360 disc image (.iso) over FTP into /data/homebrew/PPSA92011/, "
                "or put it on a USB drive. Put the PC port's newest release zip (SvR2011-PC-v....zip, from the "
                "Releases page of github.com/KaikoClanworth1/wwe-svr2011-pc) beside it, as downloaded: it has the "
                "shaders, button icons and bundled mods. Then press Square to look again.");
    return;
  }
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, int(rows.size()) - 1);
  std::vector<Row> list;
  for (int i = 0; i < int(rows.size()); ++i) {
    if (i == 0 || rows[size_t(i)].kind != rows[size_t(i) - 1].kind)
      list.push_back({-1, rows[size_t(i)].kind == 0 ? "DISC IMAGES" : rows[size_t(i)].kind == 1 ? "PC PORT FILES" : "GAME FILES"});
    list.push_back({i});
  }
  const float x = kListX, w = kListW;
  auto status = [&](float ry, float over, const std::string& st, bool lit) {
    const float sx = x + w - 24 - TextWidth(a_.body_bold, 18, st);
    if (lit) dl_->AddCircleFilled(P(sx - 14, ry + 35), 4.5f * s_, kAccent, 16);
    Text(a_.body_bold, 18, sx, ry + 24, Mix(White(lit ? 0.9f : 0.5f), Black(0.7f), over), st);
  };
  List(list, sel, 76, [&](int i, float ry, float over) {
    const InstallRow r = rows[size_t(i)];
    std::string title, line, st;
    bool lit = false;
    if (r.kind == 0) {
      const install::DiscImage& d = found_.images[size_t(r.index)];
      title = d.file.stem().string();
      line = Size(d.bytes) + "  \xC2\xB7  " + Where(d.file, paths_.game);
      lit = d.state == install::DiscImage::kThisGame;
      st = lit ? (state_.Installed() ? "Installed" : "Ready") : d.state == install::DiscImage::kOtherGame ? "Other game" : "Can't be used";
      if (lit && state_.Installed()) lit = false;
    } else if (r.kind == 1) {
      const install::PortFiles& pf = found_.ports[size_t(r.index)];
      title = "SmackDown vs. Raw Recomp" + (pf.version.empty() ? std::string() : " v" + pf.version);
      line = std::to_string(pf.shaders) + " shader files, " + std::to_string(pf.mods) + " bundled mods  \xC2\xB7  " +
             pf.where.filename().string();
      st = state_.shaders ? "Installed" : "Ready";
      lit = !state_.shaders;
    } else {
      title = "Check Game Files";
      line = verify_result_.empty() ? std::string("Compares every file with the disc's") : verify_result_;
    }
    Text(a_.body_bold, 24, x + 24, ry + 9, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 24, title, w - 260));
    Text(a_.body, 18, x + 24, ry + 41, Mix(White(0.42f), Black(0.55f), over), Fit(a_.body, 18, line, w - 260));
    if (!st.empty()) status(ry, over, st, lit);
  });
  const InstallRow r = rows[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    if (r.kind == 0) {
      const install::DiscImage& d = found_.images[size_t(r.index)];
      py += Wrapped(a_.display, 44, px, py, pw, kText, d.file.stem().string(), 1.15f) + 22;
      const bool ok = d.state == install::DiscImage::kThisGame;
      py = Facts(px, py, pw,
                 {{"GAME", ok ? "SmackDown vs. Raw 2011" : d.note},
                  {"SIZE", Size(d.bytes)},
                  {"WHERE", Where(d.file, paths_.game)},
                  {"INSTALLS", ok ? std::to_string(d.game_files) + " files, " + Size(d.game_bytes) : "-"}});
      if (ok)
        Wrapped(a_.body, 21, px, py + 10, pw, White(0.55f),
                state_.Installed()
                    ? "The game is installed. Installing again only copies the files that are missing."
                    : state_.shaders
                          ? "The game's files are copied from the disc. The shaders, button icons and bundled mods "
                            "are already in the app."
                    : found_.ports.empty()
                          ? "The PC port's release zip isn't here yet: the game needs its shaders. Add it now, or "
                            "install it on its own afterwards."
                          : "The PC port's shaders, button icons and bundled mods are installed with it.");
    } else if (r.kind == 1) {
      const install::PortFiles& pf = found_.ports[size_t(r.index)];
      py += Wrapped(a_.display, 44, px, py, pw, kText, "PC Port Files", 1.15f) + 22;
      py = Facts(px, py, pw,
                 {{"VERSION", pf.version.empty() ? "-" : pf.version},
                  {"SHADERS", std::to_string(pf.shaders) + " files"},
                  {"BUTTONS", pf.icons ? "PlayStation button icons" : "-"},
                  {"MODS", std::to_string(pf.mods) + " bundled"},
                  {"FROM", pf.where.filename().string()}});
      Wrapped(a_.body, 21, px, py + 10, pw, White(0.55f),
              "From the PC port's release: what the game needs on top of the disc. New bundled mods come switched off; "
              "turn on the ones you want on the Mods page.");
    } else {
      py += Wrapped(a_.display, 44, px, py, pw, kText, "Check Game Files", 1.15f) + 22;
      py = Facts(px, py, pw,
                 {{"FILES", std::to_string(state_.present) + " of " + std::to_string(state_.required)},
                  {"SHADERS", state_.shaders ? "Installed" : "Missing"},
                  {"BUTTONS", state_.icons ? "Installed" : "-"}});
      Wrapped(a_.body, 21, px, py + 10, pw, White(0.55f),
              "Reads every game file and compares it with the disc's. A damaged file can make the game crash while it "
              "loads: if one is found, it can be copied again from your disc image.");
    }
  });
}

void Hub::DrawInstallProgress() {
  install::Progress& p = task_->p;
  const bool check = task_kind_ == 2;
  const int step = p.step;
  PageTitle(check ? "CHECKING" : "INSTALLING", task_title_);
  Ease(task_in_, 1, 6);
  const uint64_t done = p.done, total = p.total;
  const float frac = total ? float(double(done) / double(total)) : 0.0f;
  const float x = kListX, y = 300;
  if (!check)
    Text(a_.body_bold, 16, x, y, White(0.5f),
         "STEP " + std::to_string(step + 1) + " OF 4  \xC2\xB7  " + Upper(kSteps[std::clamp(step, 0, 3)]), 0, 3);
  char pct[16];
  std::snprintf(pct, sizeof(pct), "%d%%", int(frac * 100));
  Text(a_.display, 140, x - 6, y + 34, kText, pct);
  // the bar, with a sheen running along it
  const float bw = 860, by = y + 206;
  Rect(x, by, bw, 10, White(0.12f), 5);
  static float shown = 0;
  shown = shown > frac ? frac : shown + (frac - shown) * (1 - std::exp(-dt_ * 8));
  const float fw = std::max(10.0f, bw * shown);
  Rect(x, by, fw, 10, kAccent, 5);
  const float sheen = std::fmod(time_ * 0.6f, 1.4f) - 0.2f;
  const float sx = x + fw * sheen;
  if (sheen > 0 && sheen < 1)
    dl_->AddRectFilledMultiColor(P(std::max(x, sx - 60), by), P(std::min(x + fw, sx + 60), by + 10), White(0), White(0.35f),
                                 White(0.35f), White(0));
  std::string line;
  if (step == 2 && !check) {
    line = std::to_string(done) + " of " + std::to_string(total) + " mods";
  } else {
    line = install::Bytes(done) + " of " + install::Bytes(total);
    if (task_speed_ > 1 << 20) {
      char sp[32];
      std::snprintf(sp, sizeof(sp), "%.0f MB/s", task_speed_ / 1048576.0f);
      line += std::string("  \xC2\xB7  ") + sp;
      const float left = float(total - std::min(done, total)) / task_speed_;
      if (left > 1) {
        line += "  \xC2\xB7  ";
        line += left >= 90 ? "about " + std::to_string(int(left / 60 + 0.5f)) + " min left" : std::to_string(int(left) + 1) + " s left";
      }
    }
  }
  Text(a_.body_bold, 22, x, by + 34, White(0.85f), line);
  Text(a_.body, 19, x, by + 76, White(0.45f), Fit(a_.body, 19, p.File(), bw));
  // the steps, on the right
  Panel([&](float px, float py, float pw) {
    if (check) {
      Text(a_.body_bold, 15, px, py + 5, White(0.45f), "WHAT HAPPENS", 0, 3);
      Wrapped(a_.body, 22, px, py + 50, pw, White(0.7f),
              "Every game file is read and compared with the disc's checksums. It takes a minute or two; Circle stops "
              "it.");
      return;
    }
    Text(a_.body_bold, 15, px, py + 5, White(0.45f), "STEPS", 0, 3);
    py += 52;
    for (int k = 0; k < 4; ++k) {
      const bool now = k == step, past = k < step;
      const float cy = py + 14;
      if (past) {
        dl_->AddCircleFilled(P(px + 12, cy), 12 * s_, White(0.9f), 24);
        dl_->AddLine(P(px + 6, cy), P(px + 10.5f, cy + 5), kInk, 2.4f * s_);
        dl_->AddLine(P(px + 10.5f, cy + 5), P(px + 18, cy - 5), kInk, 2.4f * s_);
      } else if (now) {
        const float a0 = time_ * 5;
        dl_->PathArcTo(P(px + 12, cy), 11 * s_, a0, a0 + 4.2f, 24);
        dl_->PathStroke(kAccent, 0, 3 * s_);
      } else {
        dl_->AddCircle(P(px + 12, cy), 11 * s_, White(0.25f), 24, 1.6f * s_);
      }
      Text(now ? a_.body_bold : a_.body, 22, px + 44, py, now || past ? White(now ? 0.95f : 0.6f) : White(0.35f), kSteps[k]);
      py += 56;
    }
    Wrapped(a_.body, 19, px, py + 20, pw, White(0.45f),
            "You can look around the other pages meanwhile. If it stops, install again: it goes on where it stopped.");
  });
}

// --------------------------------------------------------------- paint tool

void Hub::LoadPaint() {
  paint_ok_ = paint_.Read(paths_.saves, paint_page_, &paint_why_);
  std::vector<uint8_t> rgba(size_t(paint::kSize) * paint::kSize * 4);
  for (int k = 0; k < paint::kSlots; ++k) {
    if (paint_tex_[k] && textures_.drop) textures_.drop(paint_tex_[k]);
    paint_tex_[k] = {};
    if (!paint_ok_ || !paint_.Used(k) || !textures_.make) continue;
    paint_.Get(k, rgba.data());
    paint_tex_[k] = textures_.make(rgba.data(), paint::kSize, paint::kSize);
  }
}

void Hub::PaintPage(int page, float dir) {
  paint_page_ = page;
  paint_slide_ = 0;
  paint_dir_ = dir;
  LoadPaint();
}

void Hub::PaintAction() {
  if (!paint_ok_) return;
  const int k = std::clamp(item_[kPaintTab], 0, paint::kSlots - 1);
  auto pick = [this, k] {
    std::vector<fs::path> pics;
    std::error_code ec;
    for (const auto& e : install::ListDir(paths_.game / "Paint Tool"))
      if (e.is_regular_file(ec) && Picture(e.path())) pics.push_back(e.path());
    std::sort(pics.begin(), pics.end());
    if (pics.empty()) {
      Play(Sound::kError);
      Notice("No pictures yet",
             "Copy PNG or JPEG pictures over FTP into /data/homebrew/PPSA92011/Paint Tool/, then choose the logo "
             "again. Square pictures fit best; others are fitted in, with clear space around them.");
      return;
    }
    std::vector<std::string> names;
    for (const fs::path& p : pics) names.push_back(p.filename().string());
    Choose("Choose a Picture", names, [this, k, pics](int i) { PaintImport(k, pics[size_t(i)]); });
  };
  Play(Sound::kSelect);
  if (!paint_.Used(k)) {
    pick();
    return;
  }
  Choose(LogoName(paint_page_, k), {"Replace With a Picture", "Save as PNG", "Delete"}, [this, k, pick](int i) {
    if (i == 0) pick();
    else if (i == 1) PaintExport(k);
    else
      Confirm("Delete this logo?", LogoName(paint_page_, k) + " is removed from the Paint Tool.", "Delete", [this, k] {
        std::string why;
        paint_.Clear(k);
        if (!paint_.Write(&why)) Play(Sound::kError), Toast(why);
        else Play(Sound::kOff), Toast(LogoName(paint_page_, k) + " deleted");
        LoadPaint();
      });
  });
}

void Hub::PaintImport(int k, const fs::path& file) {
  std::vector<uint8_t> rgba;
  std::string why;
  if (!paint::LoadImage(file, &rgba, &why)) {
    Play(Sound::kError);
    Toast(why);
    return;
  }
  paint_.Put(k, rgba.data());
  if (!paint_.Write(&why)) {
    Play(Sound::kError);
    Toast(why);
  } else {
    Play(Sound::kOn);
    Toast(LogoName(paint_page_, k) + ": " + file.filename().string());
  }
  LoadPaint();
}

void Hub::PaintExport(int k) {
  std::vector<uint8_t> rgba(size_t(paint::kSize) * paint::kSize * 4);
  paint_.Get(k, rgba.data());
  const fs::path out = paths_.game / "Paint Tool" / "Exported" / (LogoName(paint_page_, k) + ".png");
  if (install::WritePng(out, rgba.data(), paint::kSize, paint::kSize)) {
    Play(Sound::kOn);
    Toast("Saved as Paint Tool/Exported/" + out.filename().string());
  } else {
    Play(Sound::kError);
    Toast("The PNG couldn't be saved");
  }
}

void Hub::PaintExportAll() {
  if (!paint_ok_) return;
  std::vector<uint8_t> rgba(size_t(paint::kSize) * paint::kSize * 4);
  int saved = 0, failed = 0;
  for (int page = 0; page < paint::kPages; ++page) {
    paint::Page pg;
    std::string why;
    if (!pg.Read(paths_.saves, page, &why)) continue;
    for (int k = 0; k < paint::kSlots; ++k) {
      if (!pg.Used(k)) continue;
      pg.Get(k, rgba.data());
      const fs::path out = paths_.game / "Paint Tool" / "Exported" / (LogoName(page, k) + ".png");
      (install::WritePng(out, rgba.data(), paint::kSize, paint::kSize) ? saved : failed)++;
    }
  }
  Play(saved && !failed ? Sound::kOn : Sound::kError);
  Toast(saved ? std::to_string(saved) + " logos saved in Paint Tool/Exported" : failed ? "The logos couldn't be saved"
                                                                                    : "There are no logos to save");
}

void Hub::DrawPaint() {
  PageTitle("PAINT TOOL", paint_ok_ ? "Page " + std::to_string(paint_page_ + 1) + " of " + std::to_string(paint::kPages) +
                                          "  \xC2\xB7  " + std::to_string(paint_.UsedCount()) + " of 20 logos used"
                                    : std::string("The logos you paint in Create A Superstar."));
  if (!paint_ok_) {
    Empty("No logos yet", paint_why_);
    return;
  }
  constexpr float kCell = 136, kGap = 16;
  const float gx = kListX, gy = kListY;
  const float gw = 5 * kCell + 4 * kGap;
  // the pages, as dots
  for (int i = 0; i < paint::kPages; ++i) {
    const float cx = gx + gw - (paint::kPages - 1 - i) * 20 - 5;
    if (i == paint_page_) dl_->AddCircleFilled(P(cx, 226), 5 * s_, kAccent, 16);
    else dl_->AddCircleFilled(P(cx, 226), 3.5f * s_, White(0.3f), 16);
  }
  Ease(paint_slide_, 1, 10);
  const int sel = item_[tab_] = std::clamp(item_[tab_], 0, paint::kSlots - 1);
  const float shift = (1 - Smooth(paint_slide_)) * 70 * paint_dir_;
  const int v0 = dl_->VtxBuffer.Size;
  for (int k = 0; k < paint::kSlots; ++k) {
    const float cx = gx + shift + (k % 5) * (kCell + kGap), cy = gy + (k / 5) * (kCell + kGap);
    Rect(cx, cy, kCell, kCell, White(paint_.Used(k) ? 0.07f : 0.04f), 12);
    if (paint_tex_[k]) {
      dl_->AddImage(paint_tex_[k].id, P(cx + 8, cy + 8), P(cx + kCell - 8, cy + kCell - 8));
    } else if (!paint_.Used(k)) {
      const float m = kCell * 0.5f;
      Rect(cx + m - 12, cy + m - 1.5f, 24, 3, White(0.18f), 1.5f);
      Rect(cx + m - 1.5f, cy + m - 12, 3, 24, White(0.18f), 1.5f);
    }
  }
  Fade(v0, 0.3f + 0.7f * Smooth(paint_slide_));
  // the focus: a ring that glides between the logos
  const float fx = gx + (sel % 5) * (kCell + kGap), fy = gy + (sel / 5) * (kCell + kGap);
  Ease(paint_fx_, fx, 20);
  Ease(paint_fy_, fy, 20);
  const float g = 5 - press_ * 3;
  dl_->AddRect(P(paint_fx_ - g, paint_fy_ - g), P(paint_fx_ + kCell + g, paint_fy_ + kCell + g), kText, (12 + g) * s_, 0,
               3 * s_);
  Panel([&](float px, float py, float pw) {
    const float big = 300;
    // (a checkerboard behind it shows the clear parts)
    for (int yy = 0; yy < 10; ++yy)
      for (int xx = 0; xx < 10; ++xx)
        Rect(px + xx * 30, py + yy * 30, 30, 30, White((xx + yy) % 2 ? 0.05f : 0.09f));
    if (paint_tex_[sel]) dl_->AddImage(paint_tex_[sel].id, P(px, py), P(px + big, py + big));
    else Text(a_.body_bold, 20, px + big * 0.5f, py + big * 0.5f - 12, White(0.4f), "Empty", 0.5f);
    py += big + 34;
    Text(a_.display, 40, px, py, kText, LogoName(paint_page_, sel));
    py += 66;
    const std::string saved = paint_.Saved(sel);
    if (paint_.Used(sel) && !saved.empty()) py = Facts(px, py, pw, {{"SAVED", saved}});
    Wrapped(a_.body, 19, px, py + 6, pw, White(0.5f),
            "Pictures to use go in /data/homebrew/PPSA92011/Paint Tool/. Saved PNGs go in its Exported folder. "
            "Left and right past the edges turn the page.");
  });
}

// ------------------------------------------------------------- choose one

void Hub::Choose(std::string title, std::vector<std::string> items, std::function<void(int)> done) {
  if (items.empty()) return;
  choice_title_ = std::move(title);
  choice_items_ = std::move(items);
  choice_sel_ = 0;
  choice_scroll_ = 0;
  choice_in_ = 0;
  choice_ = std::move(done);
}

void Hub::DrawChoice() {
  Ease(choice_in_, choice_ ? 1.0f : 0.0f, 14);
  if (choice_in_ < 0.01f || choice_items_.empty()) return;
  const float a = Smooth(choice_in_);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.6f * a));
  const int n = int(choice_items_.size()), shown = std::min(n, 7);
  const float w = 760, rh = 58, x = (kW - w) * 0.5f;
  const float h = 124 + shown * rh + 36;
  const float y = (kH - h) * 0.5f + (1 - a) * 30;
  const int v0 = dl_->VtxBuffer.Size;
  Rect(x, y, w, h, Rgba(24, 25, 29), 16);
  dl_->AddRectFilled(P(x, y), P(x + 6, y + h), kAccent, 16 * s_, ImDrawFlags_RoundCornersLeft);
  Text(a_.display, 40, x + 48, y + 40, kText, choice_title_);
  // (the list scrolls to keep the focused item in view)
  const float target = float(std::clamp(choice_sel_ - shown / 2, 0, n - shown));
  choice_scroll_ += (target - choice_scroll_) * (1 - std::exp(-dt_ * 16));
  const float ly = y + 116;
  dl_->PushClipRect(P(x, ly - 2), P(x + w, ly + shown * rh), true);
  static float focus = 0;
  focus += (float(choice_sel_) - focus) * (1 - std::exp(-dt_ * 20));
  if (choice_in_ < 0.05f) focus = float(choice_sel_);
  Rect(x + 32, ly + (focus - choice_scroll_) * rh, w - 64, rh - 6, kText, 10);
  for (int i = 0; i < n; ++i) {
    const float ry = ly + (float(i) - choice_scroll_) * rh;
    if (ry < ly - rh || ry > ly + shown * rh) continue;
    const float over = std::clamp(1 - std::fabs(focus - float(i)), 0.0f, 1.0f);
    Text(a_.body_bold, 23, x + 56, ry + 13, Mix(White(0.9f), kInk, over), Fit(a_.body_bold, 23, choice_items_[size_t(i)], w - 120));
  }
  dl_->PopClipRect();
  if (n > shown) {
    const float bh = shown * rh * shown / n, by = ly + (shown * rh - bh) * choice_scroll_ / float(n - shown);
    Rect(x + w - 22, by, 3, bh - 6, White(0.4f), 1.5f);
  }
  Fade(v0, a);
}

}  // namespace svr2011::hub
