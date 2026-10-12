// WWE SmackDown vs. Raw 2011 - the PS5 hub's installer (the PC launcher's
// Install tab, for a console): the game's files from the player's own Xbox
// 360 disc image, the PC port's shaders, button icons and bundled mods from
// its release zip, and the folders the game and the hub use.
//
// Plain C++ (std::filesystem + POSIX reads): it also runs on a PC, for tests.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace svr2011::hub::install {

namespace fs = std::filesystem;

// A disc image found (.iso / .xiso).
struct DiscImage {
  fs::path file;
  uint64_t bytes = 0;
  enum State { kUnchecked, kThisGame, kOtherGame, kNotADisc } state = kUnchecked;
  std::string note;        // why it can't be used
  uint64_t game_bytes = 0;  // what an install copies from it
  int game_files = 0;
};

// The PC port's release: its zip (SvR2011-PC-v*.zip) or the folder it was unpacked to.
struct PortFiles {
  fs::path where;
  bool zip = false;
  uint64_t bytes = 0;  // (the zip's / the folder's)
  std::string version;  // from the name: "2.0.6"
  int shaders = 0, icons = 0, mods = 0;  // files of each part in it
};

struct Found {
  std::vector<DiscImage> images;
  std::vector<PortFiles> ports;
  std::vector<fs::path> mods;  // .svrmod files to install (Mods/, the app folder)
};

// Looks for disc images, the PC release and mods: in the game folder (and the
// folders in it), and on the console's USB drives. Disc images are checked.
Found Scan(const fs::path& game);

// Just the .svrmod files (in Mods/ and the game folder).
std::vector<fs::path> FindMods(const fs::path& game);

// What a .svrmod says it is (its manifest.txt); false if it can't be read.
struct ModInfo {
  std::string type, id, name, version, author;
};
bool PeekMod(const fs::path& svrmod, ModInfo* info);

// What the game folder has.
struct State {
  int required = 0, present = 0;  // the disc's files (by name and size)
  uint64_t missing_bytes = 0;
  bool shaders = false, icons = false;
  int bundled = 0;  // bundled mods installed
  bool Installed() const { return required && present == required; }
};
State Look(const fs::path& game);

// A job's progress, shared with the hub (atomics, and the strings under `m`).
struct Progress {
  std::atomic<uint64_t> done{0}, total{0};
  std::atomic<int> step{0};  // the part running (Plan's order: 0 disc, 1 port files, 2 mods, 3 folders)
  std::atomic<bool> cancel{false}, finished{false};
  std::mutex m;
  std::string file;  // being copied / checked
  // when finished
  bool ok = false;
  std::string result;
  // a check: the files that differ from the disc (relative, '/')
  std::vector<std::string> damaged, missing;
  bool other_edition = false;
  int checked = 0;
  int mods_installed = 0;
  void SetFile(std::string f) {
    std::lock_guard l(m);
    file = std::move(f);
  }
  std::string File() {
    std::lock_guard l(m);
    return file;
  }
};

struct Plan {
  fs::path game;
  fs::path image;  // the disc image ("": none)
  fs::path port;   // the PC release, zip or folder ("": none)
  std::vector<std::string> only;  // only these disc files (a repair), else all
};
// Runs a plan (on a thread of the caller's); `p.finished` when done.
void Install(const Plan& plan, Progress& p);

// The bundled mods (Bundled Mods/*.svrmod) not installed yet - a quick look
// by name and size, against Mods/.bundled.
bool BundledPending(const fs::path& game);
// Installs them (as an install does: each once, new ones switched off but
// match types) and makes the folders the game uses; `p.finished` when done.
void InstallBundledMods(const fs::path& game, Progress& p);

// Checks every game file against the disc's sizes and CRC-32s.
void Verify(const fs::path& game, Progress& p);

// Installs one mod (.svrmod, a zip of a mod folder) into Mods/<Type>/<id>.
// `bundled`: one of the port's (never an older version over a newer one).
// The installed folder in *folder.
bool InstallMod(const fs::path& game, const fs::path& svrmod, bool bundled, std::string* message,
                fs::path* folder = nullptr, bool* existed = nullptr);

// An RGBA picture as a PNG file.
bool WritePng(const fs::path& file, const uint8_t* rgba, int w, int h);

// A folder's entries (none if it can't be read). Unlike a range-for over
// directory_iterator, it never throws: an error part-way (a USB drive pulled
// out) ends the listing.
std::vector<fs::directory_entry> ListDir(const fs::path& dir);

// "6.6 GB" etc. for messages.
std::string Bytes(uint64_t n);

}  // namespace svr2011::hub::install
