// WWE SmackDown vs. Raw 2011 - the PS5 hub: the screen the app opens on,
// before the game (the PC launcher's tabs, driven by the controller).
//
// This part is plain ImGui + the standard library, so it also builds on a
// PC for previews (tools/hub_preview). ps5_hub.cpp puts it on the PS5's
// screen, reads the DualSense and starts the game on LAUNCH GAME.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "hub_install.h"
#include "hub_paint.h"
#include "imgui.h"

namespace svr2011::hub {

// The controller, as the hub sees it.
enum Button : uint32_t {
  kUp = 1u << 0,
  kDown = 1u << 1,
  kLeft = 1u << 2,
  kRight = 1u << 3,
  kCross = 1u << 4,
  kCircle = 1u << 5,
  kSquare = 1u << 6,
  kTriangle = 1u << 7,
  kL1 = 1u << 8,
  kR1 = 1u << 9,
  kOptions = 1u << 10,
  kR3 = 1u << 11,
};

struct Pad {
  uint32_t held = 0;     // down now
  uint32_t pressed = 0;  // down now, not before (with key repeat for the d-pad)
};

struct Image {
  ImTextureID id = 0;
  float width = 0, height = 0;
  explicit operator bool() const { return id != 0; }
};

struct Assets {
  ImFont* display = nullptr;    // Oswald Bold: titles, the big button, numbers
  ImFont* nav = nullptr;        // Oswald SemiBold: the tabs, section names
  ImFont* body = nullptr;       // Inter Regular: text
  ImFont* body_bold = nullptr;  // Inter SemiBold: labels, values
  Image logo;                 // the SmackDown vs. Raw Recomp logo
  Image hero;                 // the picture behind it all
  std::map<uint32_t, Image> buttons;  // the PS5's button pictures, by Button (kLeft | kRight: the d-pad)
};

struct Paths {
  std::filesystem::path game;    // the game folder (/app0 on the PS5)
  std::filesystem::path config;  // svr2011.toml
  std::filesystem::path saves;   // the saves folder
};

// The sounds the hub asks for (ps5_hub.cpp plays them).
enum class Sound { kMove, kSelect, kBack, kPage, kOn, kOff, kStart, kError };

// What the hub plays through (none: silent; previews in tools/hub_preview).
struct Audio {
  std::function<void(Sound)> sfx;
  // A song of the Music page: start (false if it can't be played) / stop / still playing.
  std::function<bool(const std::filesystem::path&)> preview;
  std::function<void()> stop_preview;
  std::function<bool()> previewing;
  // The menu music: R3 plays the next song; its volume (0..100, Settings).
  std::function<void()> next_song;
  std::function<void(int)> music_volume;
};

// The server: (method, url, JSON body, session token, answer out) -> HTTP
// status (0: no answer). Runs on a thread of the hub's (it blocks).
using Request = std::function<int(const std::string&, const std::string&, const std::string&, const std::string&,
                                  std::string*)>;

// A DLC package installed (ps5/dlc_install.cpp): (package file, the folder
// name it gets, progress, why not) -> done. None: the game installs it as it
// starts.
using DlcInstall = std::function<bool(const std::filesystem::path&, const std::string&, install::Progress&,
                                      std::string&)>;

// The pages, in the order of the top bar.
enum Tab {
  kHome, kSettingsTab, kModsTab, kRosterTab, kPacksTab, kMusicTab, kMoviesTab, kPaintTab, kSavesTab, kOnlineTab,
  kDlcTab, kAboutTab, kTabCount
};

// Pictures the hub makes while it runs (the Paint Tool's logos): RGBA to a
// texture, and a texture no longer needed (freed once no frame uses it).
struct Textures {
  std::function<Image(const uint8_t* rgba, int w, int h)> make;
  std::function<void(const Image&)> drop;
};

class Hub {
 public:
  void Init(const Paths& paths, const Assets& assets, std::string player, std::string version);
  void SetAudio(Audio audio) { audio_ = std::move(audio); }
  void SetRequest(Request request) { request_ = std::move(request); }
  void SetTextures(Textures textures) { textures_ = std::move(textures); }
  void SetDlcInstall(DlcInstall install) { dlc_install_ = std::move(install); }
  // The loading screen from START GAME to the game's first picture (the mods
  // being set up): the home page's picture and logo, with what's being done
  // where START GAME was. Needs only the assets (no pages are read).
  void InitStartup(const Assets& assets, std::string version) {
    a_ = assets;
    version_ = std::move(version);
  }
  // `fraction` < 0: no known end (a sweep); `alpha` fades it out over the game.
  void DrawStartup(const std::string& step, const std::string& detail, float fraction, float dt, float alpha);
  ~Hub();
  // Draws the hub over the whole display; true when the game should start.
  bool Draw(const Pad& pad, float dt);
  // From START GAME until Draw returns true: the hub fades out (and its music).
  bool Leaving() const { return leaving_; }
  // The menu music's volume, 0..100 (Settings: Menu Music).
  int MusicVolume() const;
  float LeaveProgress() const { return leave_; }

  struct Setting;

 private:
  struct Mod {
    std::filesystem::path folder;
    std::string type, id, name, author, version;
    uint64_t bytes = 0;
    bool enabled = true;
  };
  struct Dlc {
    std::filesystem::path path;
    std::string file, title;
    std::string name;  // the folder it installs as (a browser's " (1)" left out)
    uint64_t bytes = 0;
    bool installed = false;
    bool other = false;  // not SmackDown vs. Raw 2011's (or not DLC)
  };
  struct Character {
    uint32_t id = 0;
    std::string name, group, note;
    int category = 0;  // 0 default, 1 SMACKDOWN, 2 RAW, 3 NPC, 4 MODS
  };
  struct Pack {
    std::string name;
    uint64_t bytes = 0;
    uint32_t files = 0;
    int order = -1;  // its place in native_texture_packs (-1: off)
  };
  struct Song {
    std::string playlist;
    std::filesystem::path file;
    uint64_t bytes = 0;
    bool playable = true;
    bool folder = false;  // a playlist folder (its first song)
  };
  struct Movie {
    std::string name;
    std::filesystem::path file;
    uint64_t bytes = 0;
    int id = 0;
  };
  struct SaveFile {
    std::string name;
    uint64_t bytes = 0;
    int64_t modified = 0;  // unix time
  };
  struct Backup {
    std::string name;
    uint32_t files = 0;
    uint64_t bytes = 0;
  };
  // A row of a list: an item, or a heading over the next ones.
  struct Row {
    int item = -1;  // -1: a heading
    std::string heading;
    float y = 0;
  };

  void Load();
  void LoadMods();
  void LoadDlc();
  void LoadRoster();
  void LoadPacks();
  void LoadMusic();
  void LoadMovies();
  void LoadSaves();
  void LoadInstall(bool scan);  // what the game folder has (and, `scan`: the disc images etc. found)
  void LoadPaint();
  std::string Value(const std::string& key, const std::string& fallback) const;
  void Save(const std::string& key, const std::string& value);
  int SettingIndex(const Setting& r) const;
  void SaveRoster();
  void SavePacks();

  // pages
  void DrawBackdrop();
  void DrawTopBar();
  void DrawHints();
  void DrawConfirm();
  void DrawHome();
  void DrawSettings();
  void DrawMods();
  void DrawRoster();
  void DrawPacks();
  void DrawMusic();
  void DrawMovies();
  void DrawSaves();
  void DrawDlc();
  void DrawGetMods();  // the Mods page's GET MODS corner (a QR code to the mods Discord)
  void DrawQrBig();    // ... and big (Triangle)
  void DrawQr(float x, float y, float size);
  void DrawOnline();
  void DrawAbout();
  void DrawInstall();
  void DrawInstallProgress();
  void DrawPaint();
  void DrawChoice();
  void DrawKeyboard();
  bool KeyboardInput(uint32_t pressed);  // (true: the keyboard took the press)
  // Asks for text with the on-screen keyboard; `done` gets it (not on Cancel).
  void Ask(std::string title, std::string text, bool secret, std::function<void(const std::string&)> done);
  // Runs a server request on a thread; `then` runs on the hub's (Draw).
  void Call(std::string method, std::string path, std::string body, std::string token,
            std::function<void(int, const std::string&)> then);
  void PollJob();
  // the installer's jobs, on a thread (one at a time)
  void StartInstall(const std::filesystem::path& image, const std::filesystem::path& port,
                    std::vector<std::string> only = {});
  void StartVerify();
  void StartModInstall(const std::filesystem::path& svrmod);
  void StartBundled();
  std::string BundledStatus();  // the bundled mods being set up ("": not now)
  // DLC packages installed one after another; `then_start`: the game starts after.
  void StartDlcInstalls(std::vector<Dlc> list, bool then_start);
  std::string DlcStatus();  // the DLC being installed ("": not now)
  std::vector<Dlc> DlcToInstall() const;  // this game's packages not installed (and not failed this time)
  void DlcActivate();
  void DrawDlcProgress();
  void PollTask();
  void StopTask();
  int InstallItems() const;
  void InstallActivate(int dir);
  float InstallHints(float right);
  bool Busy();  // (a job running: says so)
  // the Paint Tool
  void PaintAction();
  void PaintImport(int slot, const std::filesystem::path& file);
  void PaintExport(int slot);
  void PaintExportAll();
  void PaintPage(int page, float dir);
  // A list to pick from (Cross picks, Circle cancels).
  void Choose(std::string title, std::vector<std::string> items, std::function<void(int)> done);
  void GoTo(int tab, float dir);
  void SignIn(bool create);
  void SignOut();
  std::string ServerUrl() const;
  void PageTitle(const char* title, const std::string& subtitle);
  // An empty page's message.
  void Empty(const char* title, const std::string& text);
  // A scrolled list with headings at the page's left: rows' items are
  // 0..n-1 in order; `draw` draws item i's row at y (`over`: how much the
  // focus highlight covers it, 0..1).
  void List(std::vector<Row>& rows, int sel, float rh, const std::function<void(int, float, float)>& draw);
  // The details panel at the right; `draw` from its top, faded in.
  void Panel(const std::function<void(float x, float y, float w)>& draw);
  // Label: value lines of the panel; returns the y after them.
  float Facts(float x, float y, float w, const std::vector<std::pair<std::string, std::string>>& facts);

  void Navigate(const Pad& pad);
  int Items() const;
  void Activate(int dir);
  void Play(Sound s) {
    if (audio_.sfx) audio_.sfx(s);
  }
  void Toast(std::string text) {
    toast_ = std::move(text);
    toast_time_ = 3;
  }
  void Confirm(std::string title, std::string text, std::string yes, std::function<void()> action) {
    confirm_title_ = std::move(title), confirm_text_ = std::move(text), confirm_yes_ = std::move(yes);
    confirm_ = std::move(action);
    confirm_in_ = 0;
  }
  // A message (Cross closes it).
  void Notice(std::string title, std::string text) { Confirm(std::move(title), std::move(text), "OK", [] {}); }
  void BackUpSaves(const std::string& why);
  void RestoreSaves(const Backup& b);

  Paths paths_;
  Assets a_;
  Audio audio_;
  std::string player_, version_;
  // layout: virtual 1920 x 1080, scaled to the display
  float s_ = 1, ox_ = 0, oy_ = 0;
  ImDrawList* dl_ = nullptr;
  float time_ = 0, dt_ = 0;

  int tab_ = 0;
  int item_[16] = {};  // the focused row on each page
  bool launch_ = false;
  std::string toast_;
  float toast_time_ = 0;
  // a question (restore a backup?): Cross does it, Circle doesn't
  std::function<void()> confirm_;
  std::string confirm_title_, confirm_text_, confirm_yes_;
  float confirm_in_ = 0;
  bool qr_open_ = false;  // GET MODS shown big
  float qr_in_ = 0;
  // animation (eased toward their targets)
  float tab_x_ = -1, tab_w_ = 0, focus_y_ = -1, shade_ = 0.3f, page_in_ = 1;
  float page_dir_ = 1;          // the page slides in from this side (+1: the right)
  float panel_in_ = 1;          // the details panel, after the focus moves
  int panel_item_ = -1;
  float value_in_ = 1, value_dir_ = 0;  // a value, after it changes
  float press_ = 0;             // a flash on the focused thing when it's pressed
  float hero_x_ = -1;           // the picture drifts sideways with the pages
  bool leaving_ = false;        // START GAME: fading out
  float leave_ = 0;
  float scroll_f_ = 0, scroll_px_ = 0;  // the page's list: its scroll, shown and wanted
  std::map<int, float> knobs_;  // switches: (page << 16 | item) -> 0 off .. 1 on
  int previewing_ = -1;         // the Music page's song playing (-1: none)

  // the on-screen keyboard
  struct Keyboard {
    bool open = false, secret = false, shift = false;
    std::string title, text;
    int row = 1, col = 0;  // (col in key units)
    float in = 0, fx = -1, fy = -1, press = 0;
    std::function<void(const std::string&)> done;
  } kb_;
  // a request to the server
  struct Job;
  std::shared_ptr<Job> job_;
  Request request_;
  std::string online_msg_;  // the last sign-in's outcome

  std::vector<std::pair<std::string, std::string>> config_;  // top-level key = value
  std::vector<Mod> mods_;
  struct Drop {  // a .svrmod file to install
    std::filesystem::path file;
    install::ModInfo info;
    uint64_t bytes = 0;
  };
  std::vector<Drop> drops_;
  void LoadDrops();
  // Find Mods (hub_browse.cpp): .svrmod files anywhere - USB drives, extended
  // storage, the console's folders - browsed with the controller and
  // installed from where they are (the files are kept).
  struct Browser {
    struct Entry {
      enum Kind { kPlace, kFolder, kMod } kind = kFolder;
      std::string label, sub;
      std::filesystem::path path;
      install::ModInfo info;
      uint64_t bytes = 0;
    };
    bool open = false;
    std::filesystem::path dir;  // (empty: the places)
    std::vector<Entry> entries;
    int sel = 0, mods = 0;      // (mods: the .svrmod files listed)
    float in = 0, scroll = 0, focus = 0;
  } br_;
  void OpenBrowser();
  void BrowseTo(const std::filesystem::path& dir, const std::filesystem::path& focus = {});
  void BrowserInput(uint32_t pressed);
  void DrawBrowser();
  float BrowserHints(float right);
  // 0 not installed, 1 installed (this version), 2 installed (another version)
  int ModStatus(const install::ModInfo& info) const;
  // Installs .svrmod files where they are, one after another (task kind 5).
  void StartModInstalls(std::vector<std::filesystem::path> files);
  std::string ModInstallsStatus();  // ("Installing 2 of 5: ..."; "": not now)
  std::vector<Dlc> dlc_;
  std::vector<Character> roster_;
  std::vector<Pack> packs_;
  std::vector<Song> songs_;
  std::vector<Movie> movies_;
  std::vector<SaveFile> save_files_;
  std::vector<Backup> backups_;
  int saves_ = 0;
  // install
  install::Found found_;
  install::State state_;
  struct Task;
  std::shared_ptr<Task> task_;
  float task_speed_ = 0;
  uint64_t task_last_ = 0;
  float task_in_ = 0;
  int task_kind_ = 0;  // 1 install, 2 check, 3 a mod, 4 the bundled mods (at the first start), 5 mods found (Find Mods),
                       // 6 DLC
  DlcInstall dlc_install_;
  size_t dlc_count_ = 0;          // the DLC packages of the install running
  bool dlc_then_start_ = false;   // START GAME asked for it: the game starts after
  std::set<std::string> dlc_failed_;  // (not tried again by START GAME this time)
  std::filesystem::path task_file_;
  std::string task_title_;
  std::string verify_result_;  // the last check's outcome
  // the Paint Tool
  paint::Page paint_;
  bool paint_ok_ = false;
  std::string paint_why_;
  int paint_page_ = 0;
  Image paint_tex_[paint::kSlots];
  float paint_slide_ = 1, paint_dir_ = 0;
  float paint_fx_ = -1, paint_fy_ = -1;  // the focus ring (eased)
  Textures textures_;
  // a list to pick from
  std::string choice_title_;
  std::vector<std::string> choice_items_;
  int choice_sel_ = 0;
  float choice_in_ = 0, choice_scroll_ = 0;
  std::function<void(int)> choice_;
  uint64_t played_seconds_ = 0;
  uint32_t sessions_ = 0;

  // helpers (virtual coordinates)
  ImVec2 P(float x, float y) const { return ImVec2(ox_ + x * s_, oy_ + y * s_); }
  float Ease(float& v, float target, float speed = 16);
  float& Knob(int item, bool on);
  void Text(ImFont* f, float size, float x, float y, ImU32 col, const std::string& t, float align = 0,
            float spacing = 0);
  float TextWidth(ImFont* f, float size, const std::string& t, float spacing = 0) const;
  // `t` cut to fit `width` (with "...").
  std::string Fit(ImFont* f, float size, const std::string& t, float width) const;
  float Wrapped(ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t,
                float line = 1.4f);
  void Rect(float x, float y, float w, float h, ImU32 col, float r = 0);
  void Glyph(uint32_t button, float cx, float cy, ImU32 col);  // a PlayStation button
  float Hint(float right, uint32_t button, const char* label);  // returns its left edge
  void Switch(float x, float y, float on, bool focused);  // on: 0 off .. 1 on (animated)
  void Fade(int first_vertex, float alpha);               // fades what was drawn since
};

}  // namespace svr2011::hub
