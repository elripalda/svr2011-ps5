// WWE SmackDown vs. Raw 2011 - the PS5 hub's Discover: the community's mods on the online server,
// as the PC launcher's Mods > Discovery has them (its /api/mods), browsed with the controller - a
// grid by kind (L1 / R1), search and order, that loads more as you scroll; a mod's page with its
// pictures. Downloads go in a queue (one at a time, streamed on their own thread, picking up
// where a dropped connection left off) and install themselves into My Mods, on or off as the
// player chose; the downloads widget (Triangle) and the Downloads page show them. Home's NEW IN
// DISCOVER comes from here too. Uploads stay with the PC launcher.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "hub_style.h"
#include "hub_task.h"
#include "hub_ui.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace svr2011::hub {

namespace fs = std::filesystem;

namespace {

// The grid: 4 across (24 a page: every row full), cards 16:9.
constexpr int kColumns = 4;
constexpr float kCardGap = 24, kCardW = (kListW + kPanelW + 72 - 3 * kCardGap) / 4, kArtH = kCardW * 9 / 16;
constexpr float kRowPitch = kArtH + 12 + 52 + 22, kGridTop = kTabsListY, kGridBottom = 990;
constexpr int kThumbPixels = 560, kPicturePixels = 1280;  // (sharp at 4K)

// The types and orders the server knows (its discovery.py), and how the page names them.
struct Named {
  const char* key;
  const char* label;
};
constexpr Named kTypes[] = {{"", "All Mods"}, {"superstar", "Superstars"}, {"arena", "Arenas"},
                            {"entrance", "Entrances"}, {"moveset", "Move-Sets"}};
constexpr Named kSorts[] = {{"popular", "Most Downloaded"}, {"new", "Newest"}, {"updated", "Recently Updated"},
                            {"name", "By Name"}};

std::string UrlEncode(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// ------------------------------------------------------------------ the server's JSON
//
// Small and known: an object of strings, numbers, arrays of strings, and (in a list) an array
// of such objects. A value's text is found by its key at the object's own level.

size_t SkipString(const std::string& j, size_t i) {  // i at the opening quote; returns past the closing one
  for (++i; i < j.size(); ++i) {
    if (j[i] == '\\') ++i;
    else if (j[i] == '"') return i + 1;
  }
  return j.size();
}

// Past the value starting at i (a string, number, literal, array or object).
size_t SkipValue(const std::string& j, size_t i) {
  if (i >= j.size()) return i;
  if (j[i] == '"') return SkipString(j, i);
  if (j[i] == '{' || j[i] == '[') {
    int depth = 0;
    for (; i < j.size(); ++i) {
      if (j[i] == '"') {
        i = SkipString(j, i) - 1;
        continue;
      }
      if (j[i] == '{' || j[i] == '[') ++depth;
      if (j[i] == '}' || j[i] == ']') {
        if (--depth == 0) return i + 1;
      }
    }
    return j.size();
  }
  while (i < j.size() && j[i] != ',' && j[i] != '}' && j[i] != ']') ++i;
  return i;
}

std::string Unescape(const std::string& s) {  // a JSON string's text, without its quotes
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out += s[i];
      continue;
    }
    const char c = s[++i];
    if (c == 'n') out += '\n';
    else if (c == 't') out += ' ';
    else if (c == 'r') continue;
    else if (c == 'u' && i + 4 < s.size()) {
      const unsigned cp = unsigned(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
      i += 4;
      if (cp < 0x80) out += char(cp);
      else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
      else out += char(0xE0 | (cp >> 12)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
    } else {
      out += c;
    }
  }
  return out;
}

// The members of the object at j[i] = '{': key -> raw value text.
std::map<std::string, std::string> Members(const std::string& j, size_t i) {
  std::map<std::string, std::string> out;
  if (i >= j.size() || j[i] != '{') return out;
  ++i;
  while (i < j.size()) {
    i = j.find_first_not_of(" \t\r\n,", i);
    if (i == std::string::npos || j[i] == '}') break;
    if (j[i] != '"') break;
    const size_t ke = SkipString(j, i);
    const std::string key = j.substr(i + 1, ke - i - 2);
    i = j.find(':', ke);
    if (i == std::string::npos) break;
    i = j.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos) break;
    const size_t ve = SkipValue(j, i);
    out[key] = j.substr(i, ve - i);
    i = ve;
  }
  return out;
}

std::string Str(const std::map<std::string, std::string>& m, const char* key) {
  const auto it = m.find(key);
  if (it == m.end() || it->second.size() < 2 || it->second[0] != '"') return {};
  return Unescape(it->second.substr(1, it->second.size() - 2));
}

int64_t Num(const std::map<std::string, std::string>& m, const char* key) {
  const auto it = m.find(key);
  return it == m.end() ? 0 : int64_t(std::strtod(it->second.c_str(), nullptr));
}

// An array of strings' values.
std::vector<std::string> Strings(const std::map<std::string, std::string>& m, const char* key) {
  std::vector<std::string> out;
  const auto it = m.find(key);
  if (it == m.end()) return out;
  const std::string& a = it->second;
  for (size_t i = 0; (i = a.find('"', i)) != std::string::npos;) {
    const size_t e = SkipString(a, i);
    out.push_back(Unescape(a.substr(i + 1, e - i - 2)));
    i = e;
  }
  return out;
}

std::string TypeLabel(const std::vector<std::string>& types) {
  std::string out;
  for (const std::string& t : types) {
    const char* l = t == "superstar" ? "Superstar" : t == "arena" ? "Arena" : t == "entrance" ? "Entrance"
                    : t == "moveset" ? "Move-Set" : nullptr;
    if (l) out += (out.empty() ? "" : " + ") + std::string(l);
  }
  return out.empty() ? "Mod" : out;
}

std::string Date(int64_t unix_seconds) {
  if (unix_seconds <= 0) return {};
  const std::time_t t = std::time_t(unix_seconds);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char out[32];
  std::strftime(out, sizeof(out), "%b %e, %Y", &tm);
  std::string s = out;
  for (size_t p; (p = s.find("  ")) != std::string::npos;) s.erase(p, 1);
  return s;
}

std::string Count(int64_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// A JPEG or PNG made `w` wide (aspect kept, box-filtered down): RGBA, or empty.
std::vector<uint8_t> Picture(const std::string& data, int w, int* out_w, int* out_h) {
  int sw = 0, sh = 0, n = 0;
  unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(data.data()), int(data.size()), &sw,
                                            &sh, &n, 4);
  if (!px || sw <= 0 || sh <= 0) {
    if (px) stbi_image_free(px);
    return {};
  }
  const int dw = std::min(w, sw), dh = std::max(1, sh * dw / sw);
  std::vector<uint8_t> out(size_t(dw) * dh * 4);
  for (int y = 0; y < dh; ++y) {
    const int y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
    for (int x = 0; x < dw; ++x) {
      const int x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
      uint32_t sum[4] = {};
      for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
          for (int c = 0; c < 4; ++c) sum[c] += px[(size_t(yy) * sw + xx) * 4 + c];
      const uint32_t k = uint32_t((y1 - y0) * (x1 - x0));
      for (int c = 0; c < 4; ++c) out[(size_t(y) * dw + x) * 4 + c] = uint8_t(sum[c] / k);
    }
  }
  stbi_image_free(px);
  *out_w = dw, *out_h = dh;
  return out;
}

std::string LowerCase(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A file name safe for the app's folder.
std::string SafeFile(std::string name) {
  for (char& c : name)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' ||
          c == ')'))
      c = '_';
  if (name.empty() || name[0] == '.') name = "mod" + name;
  if (name.size() < 8 || LowerCase(name.substr(name.size() - 7)) != ".svrmod") name += ".svrmod";
  return name;
}

}  // namespace

struct Hub::Discover {
  struct Entry {
    int id = 0;
    std::string name, author, version, types, description, file_name;
    uint64_t size = 0;
    int64_t downloads = 0, pics = 0, created = 0, updated = 0;
    bool detail = false;  // (its page asked for: the description and file name)
  };
  int view = 0;  // 0 the grid, 1 a mod's page (cur)
  float view_in = 1;
  std::string query;
  int sort = 0;
  int page = 0, total = 0, per_page = 24;
  std::vector<Entry> mods;
  int sel = 0;
  bool sort_focus = false;   // (Up from the grid's first row: the order)
  float scroll = 0, scroll_to = 0, fx = -1, fy = -1;
  bool loading = false, appending = false, asked = false;
  std::string error;
  int gen = 0;  // (a newer list's answers replace an older one's)
  std::vector<Entry> fresh;  // Home's NEW IN DISCOVER
  std::map<int, Image> thumbs;  // by mod id
  std::set<int> thumbs_asked;
  Entry cur;        // the mod page's
  int mod_focus = 0;  // the mod page: 0 its button, 1 "Turn it on when it's done"
  int pic = 0;      // the mod page's picture shown
  int pics_of = 0;  // ... of this mod id
  std::map<int, Image> pics;   // its pictures, by number
  std::set<int> pics_missing;  // (the server has no such picture)
  // what the threads send back, run on the hub's
  std::mutex m;
  std::vector<std::function<void(Hub&)>> done;
  void Post(std::function<void(Hub&)> f) {
    std::lock_guard l(m);
    done.push_back(std::move(f));
  }
};

// A download: waiting, going (on its thread), installing, then done (or stopped, or failed).
struct Hub::DownloadItem {
  enum State { kWaiting, kGoing, kInstalling, kDone, kFailed, kStopped };
  int id = 0;
  std::string name, types, author, version, file_name;
  uint64_t size = 0;
  int64_t pics = 0;
  bool auto_on = true;  // "Turn it on when it's done"
  std::function<void(const std::string&)> log;  // (the hub's log_: what it installed)
  std::atomic<int> state{kWaiting};
  std::atomic<uint64_t> done{0}, total{0};
  std::atomic<bool> cancel{false}, finished{false};
  std::atomic<bool> may_install{false};  // (the hub says when: not while another job changes Mods)
  std::thread thread;
  // set by its thread before `finished`
  bool ok = false;
  std::string message;
  fs::path folder;
  // the hub's
  bool on = false, seen = false;
  float speed = 0, sample_t = 0;
  uint64_t sample_done = 0;
  ~DownloadItem() {
    cancel = true;
    may_install = true;
    if (thread.joinable()) thread.join();
  }
};

namespace {

using Item = Hub::DownloadItem;

// The downloads in the order they're shown: the one going, those waiting (in turn), then the
// finished ones, newest first.
template <class List>
std::vector<Item*> Ordered(const List& all) {
  std::vector<Item*> out;
  for (const auto& d : all)
    if (d->state == Item::kGoing || d->state == Item::kInstalling) out.push_back(d.get());
  for (const auto& d : all)
    if (d->state == Item::kWaiting) out.push_back(d.get());
  for (auto it = all.rbegin(); it != all.rend(); ++it)
    if ((*it)->state >= Item::kDone) out.push_back(it->get());
  return out;
}

std::string Seconds(float s) {
  if (s < 1) return "a moment left";
  if (s < 60) return std::to_string(int(s + 0.5f)) + " s left";
  const int m = int(s / 60 + 0.5f);
  return m < 60 ? std::to_string(m) + " min left" : std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min left";
}

std::string Speed(float bytes_per_s) {
  char out[32];
  std::snprintf(out, sizeof(out), "%.1f MB/s", double(bytes_per_s) / double(1 << 20));
  return out;
}

// A placeholder's colours for a mod without its picture yet (from its id: always the same).
std::pair<ImU32, ImU32> ArtColours(int id) {
  static const ImU32 hi[] = {Rgba(209, 58, 67), Rgba(60, 100, 214), Rgba(226, 116, 46), Rgba(47, 154, 102),
                             Rgba(123, 79, 192), Rgba(37, 165, 179), Rgba(201, 154, 46), Rgba(106, 109, 121)};
  static const ImU32 lo[] = {Rgba(62, 11, 16), Rgba(18, 31, 74), Rgba(74, 26, 8), Rgba(14, 51, 34),
                             Rgba(43, 23, 71), Rgba(11, 52, 56), Rgba(61, 42, 8), Rgba(20, 21, 25)};
  const unsigned k = unsigned(id) * 2654435761u >> 29;
  return {hi[k & 7], lo[k & 7]};
}

}  // namespace

bool Hub::DiscoverOpen() const { return dv_ && dv_->view == 1; }

void Hub::DiscoverEnsure() {
  if (!dv_) dv_ = std::make_shared<Discover>();
  if (!dv_->asked) DiscoverLoad(0);
}

namespace {
Hub::Discover::Entry EntryOf(const std::map<std::string, std::string>& m) {
  Hub::Discover::Entry e;
  e.id = int(Num(m, "id"));
  e.name = Str(m, "name"), e.author = Str(m, "author"), e.version = Str(m, "version");
  e.types = TypeLabel(Strings(m, "types"));
  e.size = uint64_t(Num(m, "size")), e.downloads = Num(m, "downloads"), e.pics = Num(m, "pics");
  e.created = Num(m, "created"), e.updated = Num(m, "updated");
  return e;
}

std::vector<Hub::Discover::Entry> EntriesOf(const std::string& answer, int* total, int* per_page) {
  std::vector<Hub::Discover::Entry> out;
  const auto top = Members(answer, answer.find('{'));
  if (total) *total = int(Num(top, "total"));
  if (per_page) *per_page = std::max<int>(1, int(Num(top, "per_page")));
  const auto it = top.find("mods");
  if (it == top.end()) return out;
  const std::string& a = it->second;
  for (size_t i = a.find('{'); i != std::string::npos && i < a.size(); i = a.find('{', SkipValue(a, i))) {
    Hub::Discover::Entry e = EntryOf(Members(a, i));
    if (e.id > 0) out.push_back(std::move(e));
  }
  return out;
}
}  // namespace

// Asks for a page of the list (the search, kind and order as they are); `append`: the next page,
// added under the ones there (the grid scrolled near its end).
void Hub::DiscoverLoad(int page, bool append) {
  if (!dv_) dv_ = std::make_shared<Discover>();
  Discover& d = *dv_;
  const int gen = append ? d.gen : ++d.gen;
  d.asked = true;
  d.loading = true, d.appending = append, d.error.clear();
  if (!append) d.sel = 0, d.scroll = d.scroll_to = 0, d.fy = -1, d.sort_focus = false;
  const int type = std::clamp(sub_[kDiscoverTab], 0, int(std::size(kTypes)) - 1);
  const std::string path = "/api/mods?q=" + UrlEncode(d.query) + "&type=" + kTypes[type].key + "&sort=" +
                           kSorts[d.sort].key + "&page=" + std::to_string(std::max(0, page));
  if (!request_) {
    d.loading = false;
    d.error = "Discover needs the console's network.";
    return;
  }
  std::shared_ptr<Discover> keep = dv_;
  std::thread([keep, gen, page, append, req = request_, url = ServerUrl() + path] {
    std::string answer;
    const int status = req("GET", url, "", "", &answer);
    keep->Post([gen, page, append, status, answer](Hub& hub) {
      Discover& d = *hub.dv_;
      if (gen != d.gen) return;
      d.loading = false, d.appending = false;
      if (status != 200) {
        if (!append)
          d.error = status ? "The server answered HTTP " + std::to_string(status) + "."
                           : "Couldn't reach the online server" + (answer.empty() ? std::string(".") : ": " + answer);
        return;
      }
      int total = 0, per_page = 24;
      std::vector<Discover::Entry> got = EntriesOf(answer, &total, &per_page);
      d.total = total, d.per_page = per_page, d.page = page;
      if (!append) d.mods.clear();
      std::set<int> have;
      for (const auto& e : d.mods) have.insert(e.id);
      for (auto& e : got)
        if (!have.count(e.id)) d.mods.push_back(std::move(e));
      d.sel = std::clamp(d.sel, 0, std::max(0, int(d.mods.size()) - 1));
      // (pictures nothing shows any more go: the list's, Home's and the downloads' stay)
      std::set<int> keep_ids;
      for (const auto& e : d.mods) keep_ids.insert(e.id);
      for (const auto& e : d.fresh) keep_ids.insert(e.id);
      for (const auto& q : hub.downloads_) keep_ids.insert(q->id);
      for (auto t = d.thumbs.begin(); t != d.thumbs.end();) {
        if (keep_ids.count(t->first)) {
          ++t;
          continue;
        }
        if (hub.textures_.drop) hub.textures_.drop(t->second);
        d.thumbs_asked.erase(t->first);
        t = d.thumbs.erase(t);
      }
    });
  }).detach();
}

// Home's NEW IN DISCOVER: the four newest mods (quietly nothing when the console is offline).
void Hub::DiscoverFresh() {
  if (!request_) return;
  if (!dv_) dv_ = std::make_shared<Discover>();
  std::shared_ptr<Discover> keep = dv_;
  std::thread([keep, req = request_, url = ServerUrl() + "/api/mods?q=&type=&sort=new&page=0"] {
    std::string answer;
    if (req("GET", url, "", "", &answer) != 200) return;
    std::vector<Discover::Entry> got = EntriesOf(answer, nullptr, nullptr);
    if (got.size() > 4) got.resize(4);
    keep->Post([got = std::move(got)](Hub& hub) { hub.dv_->fresh = got; });
  }).detach();
}

// A mod's picture `n` (0: the first, the cards'), fetched once; `thumb`: for a card.
void Hub::DiscoverPicture(int id, int64_t version, int n, bool thumb) {
  if (!request_ || !textures_.make) return;
  std::shared_ptr<Discover> keep = dv_;
  const std::string url =
      ServerUrl() + "/api/mods/" + std::to_string(id) + "/img/" + std::to_string(n) + "?v=" + std::to_string(version);
  std::thread([keep, req = request_, url, id, n, thumb] {
    std::string answer;
    const int status = req("GET", url, "", "", &answer);
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (status == 200) rgba = Picture(answer, thumb ? kThumbPixels : kPicturePixels, &w, &h);
    keep->Post([id, n, thumb, status, rgba = std::move(rgba), w, h](Hub& hub) {
      Discover& d = *hub.dv_;
      if (rgba.empty()) {
        if (!thumb && d.pics_of == id && status == 404) d.pics_missing.insert(n);
        return;
      }
      const Image img = hub.textures_.make(rgba.data(), w, h);
      if (thumb) {
        if (d.thumbs.count(id) && hub.textures_.drop) hub.textures_.drop(d.thumbs[id]);
        d.thumbs[id] = img;
      } else if (d.pics_of == id) {
        d.pics[n] = img;
      } else if (hub.textures_.drop) {
        hub.textures_.drop(img);
      }
    });
  }).detach();
}

// A mod's page: its description and file name, and its pictures (index: of the list; -1: cur as set).
void Hub::DiscoverOpenMod(int index) {
  Discover& d = *dv_;
  if (index >= 0) {
    if (index >= int(d.mods.size())) return;
    d.cur = d.mods[size_t(index)];
  }
  Discover::Entry& e = d.cur;
  d.view = 1, d.view_in = 0, d.pic = 0, d.mod_focus = 0;
  if (d.pics_of != e.id) {
    for (auto& [n, img] : d.pics)
      if (textures_.drop) textures_.drop(img);
    d.pics.clear(), d.pics_missing.clear();
    d.pics_of = e.id;
    for (int n = 0; n < 4; ++n) DiscoverPicture(e.id, e.pics, n, false);
  }
  if (e.detail || !request_) return;
  std::shared_ptr<Discover> keep = dv_;
  const int id = e.id;
  std::thread([keep, req = request_, url = ServerUrl() + "/api/mods/" + std::to_string(id), id] {
    std::string answer;
    const int status = req("GET", url, "", "", &answer);
    keep->Post([id, status, answer](Hub& hub) {
      if (status != 200) return;
      const auto m = Members(answer, answer.find('{'));
      auto fill = [&](Discover::Entry& e) {
        if (e.id != id) return;
        e.description = Str(m, "description");
        e.file_name = Str(m, "file_name");
        e.detail = true;
      };
      for (Discover::Entry& e : hub.dv_->mods) fill(e);
      fill(hub.dv_->cur);
    });
  }).detach();
}

int Hub::HomeFreshCount() const { return dv_ ? int(dv_->fresh.size()) : 0; }

void Hub::OpenFresh(int k) {
  if (!dv_ || k < 0 || k >= int(dv_->fresh.size())) return;
  GoTo(kDiscoverTab, 1);
  rail_focus_ = false;
  dv_->cur = dv_->fresh[size_t(k)];
  DiscoverOpenMod(-1);
}

void Hub::PollDiscover() {
  if (!dv_) return;
  std::vector<std::function<void(Hub&)>> done;
  {
    std::lock_guard l(dv_->m);
    done.swap(dv_->done);
  }
  for (auto& f : done) f(*this);
}

// Installed already: a mod of that name in My Mods.
bool Hub::DiscoverInstalled(const std::string& name) const {
  const std::string want = LowerCase(name);
  for (const Mod& m : mods_)
    if (LowerCase(m.name) == want) return true;
  return false;
}

// ------------------------------------------------------------------ the download queue

std::shared_ptr<Hub::DownloadItem> Hub::DownloadOf(int id) const {
  for (auto it = downloads_.rbegin(); it != downloads_.rend(); ++it)
    if ((*it)->id == id) return *it;
  return nullptr;
}

bool Hub::DownloadStatus(float* frac, int* waiting, int* ready) const {
  bool going = false;
  *frac = 0, *waiting = 0, *ready = 0;
  for (const auto& d : downloads_) {
    const int st = d->state;
    if (st == DownloadItem::kGoing || st == DownloadItem::kInstalling) {
      going = true;
      const uint64_t t = d->total ? d->total.load() : d->size;
      *frac = st == DownloadItem::kInstalling ? 1.0f : t ? float(double(d->done) / double(t)) : 0.0f;
    }
    if (st == DownloadItem::kWaiting) ++*waiting;
    if (st == DownloadItem::kDone && !d->seen) ++*ready;
  }
  return going;
}

void Hub::StopDownloads() {
  for (const auto& q : downloads_)
    if (q->state <= DownloadItem::kGoing) q->cancel = true, q->state = q->state == DownloadItem::kWaiting ? DownloadItem::kStopped : q->state.load();
}

// Adds a mod to the queue (index: of the list; -1: the mod page's).
void Hub::QueueDownload(int index) {
  Discover& d = *dv_;
  const Discover::Entry& e = index < 0 ? d.cur : d.mods[size_t(index)];
  if (!download_) {
    Play(Sound::kError);
    Toast("Downloads need the console's network");
    return;
  }
  if (auto q = DownloadOf(e.id); q && q->state <= DownloadItem::kInstalling) {
    Play(Sound::kError);
    Toast(q->state == DownloadItem::kWaiting ? e.name + " is next in line" : e.name + " is downloading");
    return;
  }
  auto q = std::make_shared<DownloadItem>();
  q->id = e.id, q->name = e.name, q->types = e.types, q->author = e.author, q->version = e.version;
  q->file_name = e.file_name, q->size = e.size, q->pics = e.pics;
  q->auto_on = Value("discover_turn_on", "true") == "true";
  downloads_.push_back(q);
  if (!d.thumbs.count(e.id) && !d.thumbs_asked.count(e.id)) d.thumbs_asked.insert(e.id), DiscoverPicture(e.id, e.pics, 0, true);
  int ahead = 0;
  for (const auto& o : downloads_) ahead += o != q && o->state <= DownloadItem::kInstalling;
  press_ = 1;
  Play(Sound::kSelect);
  Toast(ahead ? e.name + " is in line: it starts after the one going" : "Downloading " + e.name + ". Keep browsing.");
}

namespace {

// A download, on its own thread: streamed to "<game>/Downloaded Mods/<file>.part" (a dropped
// connection picks up where it stopped), then installed into Mods/ and the file deleted.
void RunDownload(Item* q, Download dl, std::string url, fs::path game) {
  std::error_code ec;
  const fs::path dir = game / "Downloaded Mods";
  fs::create_directories(dir, ec);
  const fs::path file = dir / SafeFile(q->file_name.empty() ? q->name : q->file_name);
  const fs::path part = file.string() + ".part";
  std::FILE* out = std::fopen(part.string().c_str(), "wb");
  std::string why;
  uint64_t at = 0;
  if (!out) why = "The download couldn't be saved on the console.";
  for (int tries = 0; out && !q->cancel; ++tries) {
    bool first = true, write_failed = false;
    std::string err;
    const int status = dl(url, at, [&](int st, uint64_t total, const char* data, size_t size) {
      if (q->cancel) return false;
      if (first) {
        first = false;
        if (st == 200 && at > 0) {  // (the server sent it all: from the top)
          std::fclose(out);
          out = std::fopen(part.string().c_str(), "wb");
          at = 0;
          if (!out) return write_failed = true, false;
        }
        if (total) q->total = total;
      }
      if (size && std::fwrite(data, 1, size, out) != size) return write_failed = true, false;
      at += size;
      q->done = at;
      return true;
    }, &err);
    if (write_failed) {
      why = "The console's storage didn't take the file (is it full?).";
      break;
    }
    const uint64_t total = q->total;
    if ((status == 200 || status == 206 || status == 416) && total && at >= total) break;  // (all there)
    if (status == 200 && !total && !q->cancel) break;  // (no size said: what came is all)
    if (status >= 400 && status != 416) {
      why = status == 404 ? "That mod isn't on the server any more." : "The server answered HTTP " + std::to_string(status) + ".";
      break;
    }
    if (tries >= 6) {
      why = err.empty() ? "The connection kept dropping. Try again." : "The download stopped: " + err;
      break;
    }
    for (int i = 0; i < 10 * (tries + 1) && !q->cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (out) std::fclose(out);
  if (q->cancel) {
    fs::remove(part, ec);
    q->message = "Download stopped";
    q->finished = true;
    return;
  }
  if (!why.empty()) {
    fs::remove(part, ec);
    q->message = why;
    if (q->log) q->log("download: " + q->name + " stopped - " + why);
    q->finished = true;
    return;
  }
  fs::rename(part, file, ec);
  if (ec) {
    fs::remove(part, ec);
    q->message = "The download couldn't be saved on the console.";
    q->finished = true;
    return;
  }
  q->state = Item::kInstalling;
  while (!q->may_install && !q->cancel) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::string msg;
  fs::path folder;
  q->ok = !q->cancel && install::InstallMod(game, file, false, &msg, &folder);
  fs::remove(file, ec);
  if (q->ok) {
    // on or off as the player chose ("Turn it on when it's done")
    if (q->auto_on) fs::remove(folder / "disabled", ec);
    else std::ofstream(folder / "disabled") << "";
    q->folder = folder;
  }
  q->message = q->ok ? std::string() : (msg.empty() ? std::string("It didn't install.") : msg);
  if (q->log) {
    if (q->ok) {
      // (the names under a folder: by their text - std::filesystem::relative gave nothing on the PS5)
      const auto under = [](const fs::path& p, const fs::path& base) {
        const std::string a = p.generic_string(), b = base.generic_string() + "/";
        return a.compare(0, b.size(), b) == 0 ? a.substr(b.size()) : p.filename().string();
      };
      std::string files;
      int n = 0;
      for (const auto& e : fs::recursive_directory_iterator(folder, ec))
        if (e.is_regular_file(ec) && ++n <= 24) files += (files.empty() ? "" : ", ") + under(e.path(), folder);
      q->log("download: " + q->name + " " + q->version + " installed in " + under(folder, game) +
             (q->auto_on ? " (on)" : " (off)") + " - " + std::to_string(n) + " files: " + files);
    } else {
      q->log("download: " + q->name + " didn't install - " + q->message);
    }
  }
  q->finished = true;
}

}  // namespace

void Hub::PollDownloads() {
  DownloadItem* going = nullptr;
  for (const auto& sp : downloads_) {
    DownloadItem& q = *sp;
    if (q.finished && q.thread.joinable()) {
      q.thread.join();
      const bool stopped = q.cancel;
      q.state = q.ok ? DownloadItem::kDone : stopped ? DownloadItem::kStopped : DownloadItem::kFailed;
      if (q.ok) {
        q.on = q.auto_on;
        fresh_mods_.insert(q.folder.string());
        LoadMods();
        Play(Sound::kOn);
        Toast(q.name + (q.on ? " is in My Mods, on" : " is in My Mods, ready to turn on"));
      } else if (!stopped) {
        Play(Sound::kError);
        Toast(q.name + ": " + q.message);
      }
    }
    if (q.state == DownloadItem::kGoing || q.state == DownloadItem::kInstalling) going = &q;
  }
  if (going) {  // its speed, measured every half second
    DownloadItem& q = *going;
    q.sample_t += dt_;
    if (q.sample_t >= 0.5f) {
      const uint64_t now = q.done;
      const float rate = float(double(now - std::min(now, q.sample_done)) / q.sample_t);
      q.speed = q.speed <= 0 ? rate : q.speed * 0.6f + rate * 0.4f;
      q.sample_done = now, q.sample_t = 0;
    }
    // (it installs when no other job is changing Mods)
    if (q.state == DownloadItem::kInstalling && !q.may_install && !task_) q.may_install = true;
    return;
  }
  for (const auto& sp : downloads_) {  // the next one
    DownloadItem& q = *sp;
    if (q.state != DownloadItem::kWaiting) continue;
    q.state = DownloadItem::kGoing;
    q.done = 0, q.total = 0, q.cancel = false, q.finished = false, q.may_install = false;
    q.ok = false, q.message.clear(), q.speed = 0, q.sample_t = 0, q.sample_done = 0;
    q.log = log_;
    q.thread = std::thread(RunDownload, &q, download_, ServerUrl() + "/api/mods/" + std::to_string(q.id) + "/file",
                           paths_.game);
    break;
  }
}

// Cross on a download: a finished one turns its mod on or off; a stopped or failed one goes
// again. Square (`stop`): a waiting one leaves the line, the one going stops, a finished one is
// taken off the list (its mod stays in My Mods).
void Hub::DownloadAction(DownloadItem& q, bool stop) {
  const int st = q.state;
  if (stop) {
    if (st == DownloadItem::kGoing) {
      q.cancel = true;
      Play(Sound::kBack);
      Toast("Stopping " + q.name);
      return;
    }
    if (st == DownloadItem::kInstalling) {
      Play(Sound::kError);
      Toast(q.name + " is installing: it's nearly done");
      return;
    }
    Play(Sound::kBack);
    downloads_.erase(std::remove_if(downloads_.begin(), downloads_.end(), [&](const auto& sp) { return sp.get() == &q; }),
                     downloads_.end());
    return;
  }
  if (st == DownloadItem::kDone) {
    q.seen = true;
    for (Mod& m : mods_)
      if (m.folder == q.folder) {
        press_ = 1;
        ToggleMod(m);
        q.on = m.enabled;
        return;
      }
    Play(Sound::kError);
    Toast("It's not in My Mods any more");
    return;
  }
  if (st == DownloadItem::kFailed || st == DownloadItem::kStopped) {
    q.state = DownloadItem::kWaiting;
    q.done = 0;
    Play(Sound::kSelect);
    Toast(q.name + " is back in line");
    return;
  }
  Play(Sound::kError);
  Toast(st == DownloadItem::kWaiting ? q.name + " is next in line" : q.name + " is downloading");
}

// ------------------------------------------------------------------ drawing: pieces

namespace {
// Two lines for a download: what it's doing.
std::string DownloadLine(const Item& q) {
  const int st = q.state;
  const uint64_t t = q.total ? q.total.load() : q.size;
  if (st == Item::kGoing) {
    std::string s = Size(q.done) + " of " + Size(t);
    if (q.speed > 1024) s += "  \xC2\xB7  " + Speed(q.speed) + "  \xC2\xB7  " + Seconds(float(double(t - std::min(t, q.done.load())) / q.speed));
    return s;
  }
  if (st == Item::kInstalling) return "Installing into My Mods...";
  if (st == Item::kWaiting) return "Next  \xC2\xB7  " + Size(q.size);
  if (st == Item::kDone) return q.on ? "In My Mods, on" : "In My Mods, off";
  if (st == Item::kStopped) return "Stopped";
  return q.message.empty() ? std::string("It didn't finish") : q.message;
}
}  // namespace

// The downloads pill at the top right: the one going (its ring and percent), or how many are
// waiting or ready. Returns its left edge.
float Hub::DownloadsWidget(float right, float y) {
  float frac = 0;
  int waiting = 0, ready = 0;
  const bool going = DownloadStatus(&frac, &waiting, &ready);
  std::string label = going ? std::to_string(int(frac * 100 + 0.5f)) + "%"
                            : ready ? std::to_string(ready) + " ready" : waiting ? std::to_string(waiting) + " waiting"
                                                                                 : "Downloads";
  const float h = 56, w = 64 + TextWidth(a_.body_bold, 19, label) + 46;
  const float x = right - w;
  Glow(x, y + 8, w, h, h * 0.5f, Black(0.5f), 18);
  GradRect(x, y, w, h, Rgba(250, 250, 252), Rgba(228, 230, 235), h * 0.5f);
  if (going) {
    Progress(x + 30, y + h * 0.5f, 15, frac, 4, Black(0.12f), false);
    Icon(kIconDownload, x + 30, y + h * 0.5f, 16, kDark, 2.4f);
  } else {
    Icon(kIconDownload, x + 30, y + h * 0.5f, 22, kDark, 2.2f);
  }
  TextMid(a_.body_bold, 19, x + 56, y + h * 0.5f, kDark, label);
  // (Triangle opens it: its button, dark on the white)
  if (auto it = a_.buttons.find(kTriangle); it != a_.buttons.end() && it->second)
    dl_->AddImage(it->second.id, P(right - 34 - 16, y + h * 0.5f - 16), P(right - 34 + 16, y + h * 0.5f + 16),
                  ImVec2(0, 0), ImVec2(1, 1), Black(0.7f));
  else
    Glyph(kTriangle, right - 34, y + h * 0.5f, Black(0.7f));
  return x;
}

// ------------------------------------------------------------------ the Discover page

void Hub::DrawDiscover() {
  DiscoverEnsure();
  Discover& d = *dv_;
  Ease(d.view_in, 1, 12);
  // a picture for every card near the screen
  auto want_thumb = [&](const Discover::Entry& e) {
    if (!d.thumbs.count(e.id) && !d.thumbs_asked.count(e.id)) d.thumbs_asked.insert(e.id), DiscoverPicture(e.id, e.pics, 0, true);
  };
  // a card's art: the picture (cropped to its shape) or its colours and name
  auto art = [&](int id, const std::string& name, float x, float y, float w, float h, float r) {
    if (auto it = d.thumbs.find(id); it != d.thumbs.end() && it->second) {
      const Image& img = it->second;
      const float ia = img.width / img.height, ta = w / h;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (ia > ta) uv0.x = (1 - ta / ia) * 0.5f, uv1.x = 1 - uv0.x;
      else uv0.y = (1 - ia / ta) * 0.5f, uv1.y = 1 - uv0.y;
      dl_->AddImageRounded(img.id, P(x, y), P(x + w, y + h), uv0, uv1, White(1), r * s_);
    } else {
      const auto [hi, lo] = ArtColours(id);
      GradRect(x, y, w, h, hi, lo, r);
      GradRect(x, y, w * 0.6f, h, White(0), White(0.08f), r, true);
      Wrapped(a_.display, std::min(30.0f, h * 0.17f), x + 16, y + h - 16 - std::min(30.0f, h * 0.17f) * 2.2f, w - 32,
              White(0.9f), Upper(name), 1.1f);
    }
    GradRect(x, y + h * 0.45f, w, h * 0.55f, Black(0), Black(0.55f), r);
    dl_->AddRect(P(x, y), P(x + w, y + h), White(0.10f), r * s_, 0, 1.0f * s_);
  };

  if (d.view == 1) {  // ------------------------------------------------ a mod's page
    const Discover::Entry& e = d.cur;
    const float slide = (1 - Smooth(d.view_in)) * 30;
    const int v0 = dl_->VtxBuffer.Size;
    ox_ += slide * s_;
    // where it sits
    {
      float bx = kPageX;
      const std::string parts[] = {"DISCOVER", Upper(e.types), Upper(e.name)};
      for (int i = 0; i < 3; ++i) {
        const std::string t = i == 2 ? Fit(a_.body_bold, 14, parts[i], 520) : parts[i];
        TextMid(a_.body_bold, 14, bx, 80, i == 2 ? kText : White(0.5f), t, 0, 3);
        bx += TextWidth(a_.body_bold, 14, t, 3) + 14;
        if (i < 2) TextMid(a_.body_bold, 16, bx, 80, White(0.35f), "/"), bx += 24;
      }
    }
    DownloadsWidget(kPageR, 52);
    // the pictures
    const float px = kPageX, py = 140, pw = 860, ph = 484;
    Glow(px, py + 20, pw, ph, 18, Black(0.7f), 50);
    const ImU32 tint = ArtColours(e.id).first;
    Glow(px, py, pw, ph, 18, WithAlpha(tint, 0.25f), 70);
    if (auto it = d.pics.find(d.pic); it != d.pics.end() && it->second) {
      const Image& img = it->second;
      const float ia = img.width / img.height, ta = pw / ph;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (ia > ta) uv0.x = (1 - ta / ia) * 0.5f, uv1.x = 1 - uv0.x;
      else uv0.y = (1 - ia / ta) * 0.5f, uv1.y = 1 - uv0.y;
      dl_->AddImageRounded(img.id, P(px, py), P(px + pw, py + ph), uv0, uv1, White(1), 18 * s_);
      dl_->AddRect(P(px, py), P(px + pw, py + ph), White(0.12f), 18 * s_, 0, 1.0f * s_);
    } else {
      art(e.id, e.name, px, py, pw, ph, 18);
      if (!d.pics_missing.count(d.pic)) {
        const float a0 = time_ * 5;
        dl_->PathArcTo(P(px + pw - 40, py + 40), 14 * s_, a0, a0 + 4.2f, 24);
        dl_->PathStroke(White(0.8f), 0, 3 * s_);
      }
    }
    int shown = 0;
    for (int i = 0; i < 4; ++i) shown += !d.pics_missing.count(i);
    if (shown > 1) {  // which picture: "1 of 4"
      const std::string t = std::to_string(d.pic + 1) + " of " + std::to_string(shown);
      const float cw = TextWidth(a_.body_bold, 16, t) + 62;
      GradRect(px + pw - 18 - cw, py + 18, cw, 38, Black(0.62f), Black(0.7f), 19);
      Glyph(kLeft | kRight, px + pw - 18 - cw + 24, py + 37, White(0.9f));
      TextMid(a_.body_bold, 16, px + pw - 18 - cw + 46, py + 37, kText, t);
    }
    // the pictures, small (as many as there are, sharing the width), the one shown ringed
    const int many = std::max(1, shown);
    const float tw = (pw - (many - 1) * 16) / many, th = std::min(tw * 9 / 16, (pw - 3 * 16) / 4 * 9 / 16), ty = py + ph + 18;
    for (int i = 0, slot = 0; i < 4; ++i) {
      if (d.pics_missing.count(i) || shown < 2) continue;
      const float tx = px + (slot++) * (tw + 16);
      if (auto it = d.pics.find(i); it != d.pics.end() && it->second) {
        const Image& img = it->second;
        const float ia = img.width / img.height, ta = tw / th;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (ia > ta) uv0.x = (1 - ta / ia) * 0.5f, uv1.x = 1 - uv0.x;
        else uv0.y = (1 - ia / ta) * 0.5f, uv1.y = 1 - uv0.y;
        dl_->AddImageRounded(img.id, P(tx, ty), P(tx + tw, ty + th), uv0, uv1, White(i == d.pic ? 1.0f : 0.55f), 10 * s_);
      } else {
        const auto [hi, lo] = ArtColours(e.id);
        GradRect(tx, ty, tw, th, WithAlpha(hi, i == d.pic ? 1.0f : 0.5f), WithAlpha(lo, i == d.pic ? 1.0f : 0.5f), 10);
      }
      if (i == d.pic) dl_->AddRect(P(tx - 4, ty - 4), P(tx + tw + 4, ty + th + 4), White(0.95f), 13 * s_, 0, 2.5f * s_);
    }
    // what it says about itself
    const float dy = shown > 1 ? ty + th + 28 : py + ph + 30;
    Text(a_.body_bold, 13, px, dy, White(0.45f), "ABOUT THIS MOD", 0, 3.2f);
    const std::string desc = e.detail ? (e.description.empty() ? std::string("No description.") : e.description)
                                      : std::string("Loading...");
    dl_->PushClipRect(P(px, dy + 26), P(px + pw, 1000), true);
    Wrapped(a_.body, 19, px, dy + 30, pw, White(0.68f), desc, 1.45f);
    dl_->PopClipRect();

    // the right: what it is, and the download
    const float rx = 1356, rw = kPageR - rx;
    float ry = 140;
    {
      const std::string chip = Upper(e.types);
      const float cw = TextWidth(a_.body_bold, 13, chip, 1.8f) + 26;
      Glass(rx, ry, cw, 30, 15, 1.1f);
      TextMid(a_.body_bold, 13, rx + 13, ry + 15, kText, chip, 0, 1.8f);
      ry += 48;
    }
    ry += Wrapped(a_.display, 48, rx, ry, rw, kText, e.name, 1.08f) + 10;
    std::string by = e.author.empty() ? std::string() : "by " + e.author;
    if (!e.version.empty()) by += (by.empty() ? "" : "  \xC2\xB7  ") + std::string("Version ") + e.version;
    if (!by.empty()) Text(a_.body_bold, 18, rx, ry, White(0.65f), Fit(a_.body_bold, 18, by, rw)), ry += 38;
    dl_->AddLine(P(rx, ry), P(rx + rw, ry), White(0.08f), 1.0f * s_);
    ry += 18;
    {
      const std::pair<std::string, std::string> facts[] = {
          {"SIZE", Size(e.size)},
          {"DOWNLOADS", std::to_string(e.downloads)},
          {"ADDED", Date(e.created)},
          {"UPDATED", e.updated && e.updated != e.created ? Date(e.updated) : std::string("-")}};
      for (int i = 0; i < 4; ++i) {
        const float fx = rx + (i % 2) * (rw * 0.5f), fy = ry + (i / 2) * 64;
        Text(a_.body_bold, 12, fx, fy, White(0.45f), facts[i].first, 0, 2.6f);
        Text(a_.body_bold, 20, fx, fy + 22, White(0.9f), facts[i].second.empty() ? "-" : facts[i].second);
      }
      ry += 128 + 4;
    }
    dl_->AddLine(P(rx, ry), P(rx + rw, ry), White(0.08f), 1.0f * s_);

    // the download: a button, or how it's going, and "Turn it on when it's done"
    const std::shared_ptr<DownloadItem> q = DownloadOf(e.id);
    const int st = q ? q->state.load() : -1;
    const bool installed = (q && st == DownloadItem::kDone) || DiscoverInstalled(e.name);
    const float cardy = 728, cardh = 232;
    Glass(rx, cardy, rw, cardh, 20, 1.1f);
    if (st == DownloadItem::kGoing || st == DownloadItem::kInstalling) Glow(rx, cardy, rw, cardh, 20, WithAlpha(kAccent, 0.25f), 30);
    const float bx = rx + 18, bw = rw - 36;
    float by2 = cardy + 18;
    const bool focus0 = d.mod_focus == 0, focus1 = d.mod_focus == 1;
    if (st == DownloadItem::kGoing || st == DownloadItem::kInstalling) {
      const uint64_t t = q->total ? q->total.load() : q->size;
      const float frac = st == DownloadItem::kInstalling ? 1.0f : t ? float(double(q->done) / double(t)) : 0.0f;
      Text(a_.body_bold, 13, bx, by2 + 4, kAccent, st == DownloadItem::kInstalling ? "INSTALLING" : "DOWNLOADING", 0, 3);
      Text(a_.display, 50, bx + bw, by2 - 6, kText, std::to_string(int(frac * 100 + 0.5f)) + "%", 1);
      Text(a_.body, 16, bx, by2 + 30, White(0.62f), Fit(a_.body, 16, DownloadLine(*q), bw - 120));
      by2 += 74;
      Rect(bx, by2, bw, 10, White(0.10f), 5);
      Glow(bx, by2, std::max(10.0f, bw * frac), 10, 5, WithAlpha(kAccent, 0.8f), 14);
      GradRect(bx, by2, std::max(10.0f, bw * frac), 10, kAccentHi, kAccentLo, 5, true);
      by2 += 30;
    } else {
      // the button: Download & Install / Waiting / Turn On, Off / Try Again
      std::string label = "Download & Install";
      int icon = kIconDownload;
      if (st == DownloadItem::kWaiting) label = "Next in Line", icon = kIconDownload;
      else if (st == DownloadItem::kFailed || st == DownloadItem::kStopped) label = "Try Again";
      else if (installed) {
        bool on = q ? q->on : false;
        if (!q) {
          for (const Mod& m : mods_)
            if (LowerCase(m.name) == LowerCase(e.name)) on = m.enabled;
        }
        label = on ? "Installed, On" : "Installed, Off";
        icon = kIconCheck;
      }
      const float bh = 64;
      if (focus0 && !rail_focus_) {
        const float push = press_ * 4;
        FocusFill(bx + push, by2 + push, bw - push * 2, bh - push * 2, 14);
        Ring(bx + push, by2 + push, bw - push * 2, bh - push * 2, 14);
      } else {
        GradRect(bx, by2, bw, bh, White(0.12f), White(0.05f), 14);
      }
      const float lw = TextWidth(a_.body_bold, 21, label) + 34;
      Icon(icon, bx + (bw - lw) * 0.5f + 11, by2 + bh * 0.5f, 22, kText, 2.4f);
      TextMid(a_.body_bold, 21, bx + (bw - lw) * 0.5f + 34, by2 + bh * 0.5f, kText, label);
      by2 += bh + 12;
      if (st == DownloadItem::kFailed && q)
        Wrapped(a_.body, 15, bx, by2 - 4, bw, Rgba(255, 140, 146), q->message), by2 += 20;
      else if (installed)
        Text(a_.body, 15, bx, by2 - 4, White(0.55f), "Cross turns it on or off. It's in My Mods."), by2 += 20;
    }
    // "Turn it on when it's done"
    {
      const bool auto_on = Value("discover_turn_on", "true") == "true";
      const float sh = 56, sy = cardy + cardh - 18 - sh;
      if (focus1 && !rail_focus_) FocusFill(bx, sy, bw, sh, 12, 0.7f), Ring(bx, sy, bw, sh, 12);
      else GradRect(bx, sy, bw, sh, White(0.06f), White(0.03f), 12);
      TextMid(a_.body_bold, 18, bx + 18, sy + sh * 0.5f, kText, "Turn it on when it's done");
      auto& knob = knobs_[kDiscoverTab << 16 | 0x7FFF];
      Ease(knob, auto_on ? 1.0f : 0.0f, 14);
      Switch(bx + bw - 16 - 56, sy + sh * 0.5f - 16, knob, focus1 && !rail_focus_);
    }
    ox_ -= slide * s_;
    Fade(v0, 0.2f + 0.8f * Smooth(d.view_in));
    return;
  }

  // ------------------------------------------------------------------ the grid
  PageTitle("DISCOVER", "The community's mods. One press downloads and installs them.");
  {  // search, and the downloads widget
    const float wx = DownloadsWidget(kPageR, 52);
    const float sw = 330, sx = wx - 16 - sw, sy = 52, sh = 56;
    Glass(sx, sy, sw, sh, sh * 0.5f, 1.1f);
    Icon(kIconSearch, sx + 30, sy + sh * 0.5f, 20, White(0.75f), 2.2f);
    const std::string q = d.query.empty() ? std::string("Search mods") : "\"" + d.query + "\"";
    TextMid(a_.body, 19, sx + 54, sy + sh * 0.5f, d.query.empty() ? White(0.5f) : kText, Fit(a_.body, 19, q, sw - 110));
    Glyph(kSquare, sx + sw - 30, sy + sh * 0.5f, White(0.8f));
  }
  {
    std::vector<std::pair<std::string, int>> tabs;
    for (const Named& t : kTypes) tabs.push_back({Upper(t.label), -1});
    PageTabs(tabs, std::clamp(sub_[kDiscoverTab], 0, int(tabs.size()) - 1));
    // the order (Up from the first row)
    const std::string s = kSorts[d.sort].label;
    const float cw = TextWidth(a_.body_bold, 17, s) + 64, cx = kPageR - cw, cy = 172;
    if (d.sort_focus && !rail_focus_) FocusFill(cx, cy, cw, 44, 22, 0.8f), Ring(cx, cy, cw, 44, 22);
    else Glass(cx, cy, cw, 44, 22, 1.0f);
    TextMid(a_.body_bold, 17, cx + 20, cy + 22, kText, s);
    Icon(kIconDown, cx + cw - 24, cy + 22, 16, kText, 2.6f);
  }
  list_y_ = kTabsListY;
  const int n = int(d.mods.size());
  if (n == 0) {
    if (d.loading) {  // cards that shimmer while the list comes
      for (int i = 0; i < 8; ++i) {
        const float cx = kPageX + (i % kColumns) * (kCardW + kCardGap), cy = kGridTop + (i / kColumns) * kRowPitch;
        const float a = 0.05f + 0.03f * std::sin(time_ * 4 - i * 0.5f);
        GradRect(cx, cy, kCardW, kArtH, White(a), White(a * 0.5f), 14);
        Rect(cx, cy + kArtH + 16, kCardW * 0.7f, 16, White(a), 8);
        Rect(cx, cy + kArtH + 42, kCardW * 0.45f, 12, White(a * 0.7f), 6);
      }
      return;
    }
    Empty(d.error.empty() ? "No mods found" : "Discover can't load",
          d.error.empty() ? (d.query.empty() ? std::string("Nothing of this kind on the server yet.")
                                             : "Nothing matches \"" + d.query + "\". Square searches again.")
                          : d.error + " Cross tries again.");
    return;
  }
  d.sel = std::clamp(d.sel, 0, n - 1);
  const int rows = (n + kColumns - 1) / kColumns, srow = d.sel / kColumns;
  // more as the focus gets near the end
  if (!d.loading && n < d.total && srow >= rows - 2) DiscoverLoad(d.page + 1, true);
  // scrolled to keep the focused row whole
  {
    const float top = srow * kRowPitch, bottom = top + kRowPitch - 22;
    const float view = kGridBottom - kGridTop - 40;
    if (top < d.scroll_to) d.scroll_to = top;
    if (bottom > d.scroll_to + view) d.scroll_to = bottom - view;
    d.scroll_to = std::max(0.0f, d.scroll_to);
    Ease(d.scroll, d.scroll_to, 14);
  }
  const float fx = kPageX + (d.sel % kColumns) * (kCardW + kCardGap), fy = kGridTop + srow * kRowPitch - d.scroll;
  Ease(d.fx, fx, 20);
  Ease(d.fy, fy, 20);
  const int v0 = dl_->VtxBuffer.Size;
  dl_->PushClipRect(P(0, kGridTop - 40), P(kW, kGridBottom + 30), true);
  const bool on_grid = !rail_focus_ && !d.sort_focus;
  // the focus's glow under the card it's on
  if (on_grid) Glow(d.fx - 6, d.fy - 6, kCardW + 12, kArtH + 12, 18, WithAlpha(kAccent, 0.9f), 34);
  for (int i = 0; i < n; ++i) {
    const Discover::Entry& e = d.mods[size_t(i)];
    const float cx = kPageX + (i % kColumns) * (kCardW + kCardGap), cy = kGridTop + (i / kColumns) * kRowPitch - d.scroll;
    if (cy > kGridBottom + 40 || cy + kRowPitch < kGridTop - 60) continue;
    want_thumb(e);
    const bool focus = on_grid && i == d.sel;
    const float grow = focus ? 4 + press_ * -3 : 0;
    const float ax = cx - grow, ay = cy - grow, aw = kCardW + grow * 2, ah = kArtH + grow * 2;
    art(e.id, e.name, ax, ay, aw, ah, 14);
    // its state: going (a ring), next, installed
    const std::shared_ptr<DownloadItem> q = DownloadOf(e.id);
    const int st = q ? q->state.load() : -1;
    std::string chip;
    if (st == DownloadItem::kGoing || st == DownloadItem::kInstalling) {
      const uint64_t t = q->total ? q->total.load() : q->size;
      const float frac = st == DownloadItem::kInstalling ? 1.0f : t ? float(double(q->done) / double(t)) : 0.0f;
      dl_->AddRectFilled(P(ax, ay), P(ax + aw, ay + ah), Black(0.55f), 14 * s_);
      Progress(ax + aw * 0.5f, ay + ah * 0.5f - 12, 38, frac, 7, White(0.16f), true);
      TextMid(a_.display, 24, ax + aw * 0.5f, ay + ah * 0.5f - 12, kText, std::to_string(int(frac * 100 + 0.5f)) + "%", 0.5f);
      TextMid(a_.body_bold, 14, ax + aw * 0.5f, ay + ah * 0.5f + 46, White(0.85f),
              st == DownloadItem::kInstalling ? "Installing" : q->speed > 1024 ? Speed(q->speed) : "Starting", 0.5f);
      Glow(ax, ay + ah - 4, aw * frac, 4, 2, WithAlpha(kAccent, 0.8f), 8);
      Rect(ax, ay + ah - 4, aw * frac, 4, kAccent, 2);
    } else if (st == DownloadItem::kWaiting) {
      chip = "NEXT";
    } else if (st == DownloadItem::kDone || DiscoverInstalled(e.name)) {
      chip = "INSTALLED";
    }
    if (!chip.empty()) {
      const float cw = TextWidth(a_.body_bold, 12, chip, 1.6f) + (chip == "INSTALLED" ? 44 : 26);
      GradRect(ax + 12, ay + 12, cw, 28, Black(0.7f), Black(0.78f), 14);
      float tx = ax + 25;
      if (chip == "INSTALLED") Icon(kIconCheck, tx + 6, ay + 26, 14, kText, 3), tx += 18;
      TextMid(a_.body_bold, 12, tx, ay + 26, kText, chip, 0, 1.6f);
    }
    if (focus) Ring(ax - 4, ay - 4, aw + 8, ah + 8, 17);
    // its name, who made it, its size and downloads
    TextMid(a_.body_bold, 20, cx, cy + kArtH + 24, focus ? kText : White(0.9f), Fit(a_.body_bold, 20, e.name, kCardW));
    const std::string cnt = e.downloads >= 1000 ? std::to_string(e.downloads / 1000) + "." + std::to_string(e.downloads / 100 % 10) + "K"
                                                : std::to_string(e.downloads);
    const float cntw = TextWidth(a_.body, 15, cnt);
    TextMid(a_.body, 15, cx + kCardW, cy + kArtH + 50, White(0.5f), cnt, 1);
    Icon(kIconDownload, cx + kCardW - cntw - 12, cy + kArtH + 50, 14, White(0.5f), 2.2f);
    TextMid(a_.body, 15, cx, cy + kArtH + 50, White(0.5f),
            Fit(a_.body, 15, (e.author.empty() ? e.types : e.author) + "  \xC2\xB7  " + Size(e.size), kCardW - cntw - 34));
  }
  if (d.loading && d.appending) {  // the next ones coming
    const float ly = kGridTop + rows * kRowPitch - d.scroll + 10;
    const float a0 = time_ * 5;
    dl_->PathArcTo(P(kPageX + 1360 * 0.5f, ly + 14), 14 * s_, a0, a0 + 4.2f, 24);
    dl_->PathStroke(White(0.7f), 0, 3 * s_);
  }
  dl_->PopClipRect();
  // the grid fades out at its top and bottom edges
  for (int i = v0; i < dl_->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl_->VtxBuffer[i];
    const float y = (v.pos.y - oy_) / s_;
    float k = 1;
    if (y > kGridBottom - 110) k = std::clamp((kGridBottom + 10 - y) / 120.0f, 0.0f, 1.0f);
    if (y < kGridTop - 14) k = std::min(k, std::clamp((y - (kGridTop - 44)) / 30.0f, 0.0f, 1.0f));
    if (k < 1) v.col = WithAlpha(v.col, k);
  }
}

void Hub::DiscoverInput(uint32_t p) {
  DiscoverEnsure();
  Discover& d = *dv_;
  if (p & kTriangle) {  // the downloads widget
    dl_open_ = true, dl_sel_ = 0, dl_in_ = 0;
    Play(Sound::kSelect);
    return;
  }
  if (d.view == 1) {  // a mod's page
    int shown = 0;
    for (int i = 0; i < 4; ++i) shown += !d.pics_missing.count(i);
    if ((p & kLeft) && shown > 1) {
      do d.pic = (d.pic + 3) % 4;
      while (d.pics_missing.count(d.pic));
      Play(Sound::kMove);
    }
    if ((p & kRight) && shown > 1) {
      do d.pic = (d.pic + 1) % 4;
      while (d.pics_missing.count(d.pic));
      Play(Sound::kMove);
    }
    if (p & (kUp | kDown)) {
      d.mod_focus = (p & kDown) ? 1 : 0;
      Play(Sound::kMove);
    }
    if (p & kCross) {
      if (d.mod_focus == 1) {  // "Turn it on when it's done" (for this and the next downloads)
        const bool on = Value("discover_turn_on", "true") != "true";
        Save("discover_turn_on", on ? "true" : "false");
        for (const auto& q : downloads_)
          if (q->state <= DownloadItem::kGoing) q->auto_on = on;
        Play(on ? Sound::kOn : Sound::kOff);
        return;
      }
      const std::shared_ptr<DownloadItem> q = DownloadOf(d.cur.id);
      if (q && (q->state == DownloadItem::kDone || q->state >= DownloadItem::kFailed)) {
        DownloadAction(*q, false);
      } else if (!q && DiscoverInstalled(d.cur.name)) {
        for (Mod& m : mods_)
          if (LowerCase(m.name) == LowerCase(d.cur.name)) {
            press_ = 1;
            ToggleMod(m);
            break;
          }
      } else {
        QueueDownload(-1);
      }
    }
    if (p & kSquare) {
      if (const std::shared_ptr<DownloadItem> q = DownloadOf(d.cur.id); q && q->state <= DownloadItem::kInstalling)
        DownloadAction(*q, true);
    }
    if (p & kCircle) d.view = 0, d.view_in = 0, Play(Sound::kBack);
    return;
  }
  if (p & kCircle) {
    if (d.sort_focus) d.sort_focus = false;
    else rail_focus_ = true;
    Play(Sound::kBack);
    return;
  }
  if (p & kSquare) {  // search
    Play(Sound::kSelect);
    Ask("Search Mods", d.query, false, [this](const std::string& text) {
      dv_->query = text;
      DiscoverLoad(0);
    });
    return;
  }
  const int n = int(d.mods.size());
  if (d.sort_focus) {  // the order
    if (p & kDown) d.sort_focus = false, Play(Sound::kMove);
    if (p & kLeft) d.sort_focus = false, rail_focus_ = true, Play(Sound::kBack);
    if (p & kCross) {
      std::vector<std::string> items;
      for (const Named& s : kSorts) items.push_back(s.label);
      items[size_t(d.sort)] += "  \xC2\xB7  now";
      Choose("Order", std::move(items), [this](int i) {
        dv_->sort = i;
        DiscoverLoad(0);
      });
      choice_sel_ = d.sort;
    }
    return;
  }
  if (n == 0) {
    if (p & kLeft) rail_focus_ = true, Play(Sound::kBack);
    if (p & kUp) d.sort_focus = true, Play(Sound::kMove);
    if ((p & kCross) && !d.loading) DiscoverLoad(0), Play(Sound::kSelect);
    return;
  }
  const int before = d.sel;
  if (p & kLeft) {
    if (d.sel % kColumns == 0) {
      rail_focus_ = true;
      Play(Sound::kBack);
      return;
    }
    --d.sel;
  }
  if ((p & kRight) && d.sel + 1 < n && d.sel % kColumns < kColumns - 1) ++d.sel;
  if (p & kUp) {
    if (d.sel < kColumns) d.sort_focus = true, Play(Sound::kMove);
    else d.sel -= kColumns;
  }
  if (p & kDown) {
    if (d.sel + kColumns < n) d.sel += kColumns;
    else if (d.sel / kColumns < (n - 1) / kColumns) d.sel = n - 1;  // (the last row isn't full)
  }
  if (d.sel != before) Play(Sound::kMove);
  if (p & kCross) press_ = 1, Play(Sound::kSelect), DiscoverOpenMod(d.sel);
}

float Hub::DiscoverHints(float r) {
  const Discover& d = *dv_;
  if (d.view == 1) {
    r = Hint(r, kCircle, "Back");
    r = Hint(r, kTriangle, "Downloads");
    if (const auto q = DownloadOf(d.cur.id); q && q->state <= DownloadItem::kInstalling) r = Hint(r, kSquare, "Stop");
    int shown = 0;
    for (int i = 0; i < 4; ++i) shown += !d.pics_missing.count(i);
    if (shown > 1) r = Hint(r, kLeft | kRight, "Pictures");
    r = Hint(r, kCross, d.mod_focus == 1 ? "Change" : "Choose");
    return r;
  }
  r = Hint(r, kTriangle, "Downloads");
  r = Hint(r, kSquare, "Search");
  r = Hint(r, kL1, "Kind");
  if (d.sort_focus) r = Hint(r, kCross, "Order");
  else if (!d.mods.empty()) r = Hint(r, kCross, "Open");
  return r;
}

// ------------------------------------------------------------------ the downloads widget, opened

void Hub::DrawPopover() {
  Ease(dl_in_, dl_open_ ? 1.0f : 0.0f, 16);
  if (dl_in_ < 0.01f) return;
  const float a = Smooth(dl_in_);
  const std::vector<Item*> list = Ordered(downloads_);
  const int shown = std::min(5, int(list.size()));
  dl_sel_ = std::clamp(dl_sel_, 0, std::max(0, int(list.size()) - 1));
  const int first = std::clamp(dl_sel_ - shown + 1, 0, std::max(0, int(list.size()) - shown));
  const int v0 = dl_->VtxBuffer.Size;
  const float w = 560, rh = 98, x = kPageR - w, y = 120 + (1 - a) * 12;
  const float h = 64 + std::max(1, shown) * rh + 54;
  GlassPanel(x, y, w, h, 22);
  // (a point up to the widget)
  dl_->AddTriangleFilled(P(kPageR - 52, y - 9), P(kPageR - 61, y + 1), P(kPageR - 43, y + 1), Rgba(32, 33, 42, 246));
  Text(a_.body_bold, 13, x + 26, y + 24, White(0.5f), "DOWNLOADS", 0, 3.2f);
  {
    int going = 0, waiting = 0, done = 0;
    for (const Item* q : list) {
      const int st = q->state;
      going += st == Item::kGoing || st == Item::kInstalling;
      waiting += st == Item::kWaiting;
      done += st >= Item::kDone;
    }
    std::string sum;
    if (going) sum += std::to_string(going) + " going";
    if (waiting) sum += (sum.empty() ? "" : "  \xC2\xB7  ") + std::to_string(waiting) + " waiting";
    if (done) sum += (sum.empty() ? "" : "  \xC2\xB7  ") + std::to_string(done) + " finished";
    Text(a_.body, 15, x + w - 26, y + 22, White(0.5f), sum, 1);
  }
  float ry = y + 58;
  if (list.empty()) {
    Wrapped(a_.body, 19, x + 26, ry + 18, w - 52, White(0.62f), "Nothing yet. Open a mod on Discover and press Cross: it downloads here and lands in My Mods.");
  }
  for (int k = first; k < first + shown; ++k) {
    Item& q = *list[size_t(k)];
    const bool f = k == dl_sel_;
    const float rx = x + 12, rw = w - 24;
    if (f) FocusFill(rx, ry, rw, rh - 8, 14, 0.8f), Ring(rx, ry, rw, rh - 8, 14);
    else GradRect(rx, ry, rw, rh - 8, White(0.05f), White(0.025f), 14);
    // its picture
    const float tw = 112, th = 63, tx = rx + 12, ty = ry + (rh - 8 - th) * 0.5f;
    if (auto it = dv_->thumbs.find(q.id); it != dv_->thumbs.end() && it->second) {  // (a download came from Discover: dv_ is there)
      const Image& img = it->second;
      const float ia = img.width / img.height, ta = tw / th;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (ia > ta) uv0.x = (1 - ta / ia) * 0.5f, uv1.x = 1 - uv0.x;
      else uv0.y = (1 - ia / ta) * 0.5f, uv1.y = 1 - uv0.y;
      dl_->AddImageRounded(img.id, P(tx, ty), P(tx + tw, ty + th), uv0, uv1, White(1), 8 * s_);
    } else {
      const auto [hi, lo] = ArtColours(q.id);
      GradRect(tx, ty, tw, th, hi, lo, 8);
    }
    const int st = q.state;
    const float nx = tx + tw + 16, nw = rx + rw - 16 - nx;
    std::string right;
    if (st == Item::kGoing || st == Item::kInstalling) {
      const uint64_t t = q.total ? q.total.load() : q.size;
      right = st == Item::kInstalling ? "100%" : std::to_string(t ? int(double(q.done) * 100 / double(t) + 0.5) : 0) + "%";
    }
    const float rwid = right.empty() ? 0 : TextWidth(a_.display, 20, right) + 12;
    float action_w = 0;
    if (st == Item::kDone || st >= Item::kFailed) {
      const std::string act = st == Item::kDone ? (q.on ? "Turn Off" : "Turn On") : "Try Again";
      action_w = TextWidth(a_.body_bold, 15, act) + 50;
      const float axp = rx + rw - 14 - action_w, ayp = ry + (rh - 8) * 0.5f - 18;
      GradRect(axp, ayp, action_w, 36, f ? White(1) : White(0.95f), f ? White(0.9f) : White(0.85f), 18);
      if (auto it = a_.buttons.find(kCross); it != a_.buttons.end() && it->second)
        dl_->AddImage(it->second.id, P(axp + 8, ayp + 4), P(axp + 36, ayp + 32), ImVec2(0, 0), ImVec2(1, 1), Black(0.75f));
      TextMid(a_.body_bold, 15, axp + 38, ayp + 18, kDark, act);
      action_w += 10;
    }
    TextMid(a_.body_bold, 19, nx, ry + 30, kText, Fit(a_.body_bold, 19, q.name, nw - rwid - action_w));
    if (!right.empty()) TextMid(a_.display, 20, rx + rw - 16, ry + 30, kText, right, 1);
    if (st == Item::kGoing || st == Item::kInstalling) {
      const uint64_t t = q.total ? q.total.load() : q.size;
      const float frac = st == Item::kInstalling ? 1.0f : t ? float(double(q.done) / double(t)) : 0.0f;
      Rect(nx, ry + 50, nw, 6, f ? Black(0.25f) : White(0.12f), 3);
      Rect(nx, ry + 50, std::max(6.0f, nw * frac), 6, f ? White(1) : kAccent, 3);
      TextMid(a_.body, 14, nx, ry + 72, f ? White(0.85f) : White(0.55f), Fit(a_.body, 14, DownloadLine(q), nw));
    } else {
      TextMid(a_.body, 15, nx, ry + 60, f ? White(0.85f) : (st == Item::kFailed ? Rgba(255, 140, 146) : White(0.58f)),
              Fit(a_.body, 15, DownloadLine(q), nw - action_w));
    }
    if (st == Item::kDone) q.seen = true;
    ry += rh;
  }
  Text(a_.body, 15, x + 26, y + h - 38, White(0.45f), "Downloads keep going on every page, and each mod lands in My Mods.");
  Fade(v0, a);
}

void Hub::PopoverInput(uint32_t p) {
  const std::vector<Item*> list = Ordered(downloads_);
  if (p & (kTriangle | kCircle)) {
    dl_open_ = false;
    Play(Sound::kBack);
    return;
  }
  if (list.empty()) return;
  const int before = dl_sel_;
  if (p & kUp) dl_sel_ = std::max(0, dl_sel_ - 1);
  if (p & kDown) dl_sel_ = std::min(int(list.size()) - 1, dl_sel_ + 1);
  if (dl_sel_ != before) Play(Sound::kMove);
  dl_sel_ = std::clamp(dl_sel_, 0, int(list.size()) - 1);
  if (p & kCross) DownloadAction(*list[size_t(dl_sel_)], false);
  if (p & kSquare) DownloadAction(*list[size_t(dl_sel_)], true);
}

float Hub::DownloadsHints(float r) {
  const std::vector<Item*> list = Ordered(downloads_);
  if (dl_open_) r = Hint(r, kCircle, "Close");  // (the Downloads page: DrawHints has Back)
  if (list.empty()) return r;
  const int sel = std::clamp(dl_open_ ? dl_sel_ : item_[kDownloadsTab], 0, int(list.size()) - 1);
  const int st = list[size_t(sel)]->state;
  r = Hint(r, kSquare, st == Item::kGoing ? "Stop" : st == Item::kWaiting ? "Take Out" : st == Item::kInstalling ? "Stop" : "Clear");
  if (st == Item::kDone) r = Hint(r, kCross, list[size_t(sel)]->on ? "Turn Off" : "Turn On");
  else if (st >= Item::kFailed) r = Hint(r, kCross, "Try Again");
  return r;
}

// ------------------------------------------------------------------ the Downloads page

void Hub::DrawDownloads() {
  const std::vector<Item*> list = Ordered(downloads_);
  int going = 0, waiting = 0;
  for (const Item* q : list) going += q->state == Item::kGoing || q->state == Item::kInstalling, waiting += q->state == Item::kWaiting;
  PageTitle("DOWNLOADS", list.empty() ? std::string("Mods you download from Discover show up here.")
                                      : "One at a time, each installed into My Mods when it's done." +
                                            std::string(waiting ? " " + std::to_string(waiting) + " waiting." : ""));
  if (list.empty()) {
    Empty("Nothing downloading",
          "Open a mod on Discover and press Cross. It downloads here, installs itself, and shows up in My Mods - on, "
          "or ready to turn on.");
    return;
  }
  const int n = int(list.size());
  const int sel = item_[kDownloadsTab] = std::clamp(item_[kDownloadsTab], 0, n - 1);
  std::vector<Row> rows;
  for (int i = 0; i < n; ++i) {
    const int st = list[size_t(i)]->state;
    const int group = st <= Item::kInstalling ? (st == Item::kWaiting ? 1 : 0) : 2;
    const int prev = i ? (list[size_t(i) - 1]->state <= Item::kInstalling ? (list[size_t(i) - 1]->state == Item::kWaiting ? 1 : 0) : 2) : -1;
    if (group != prev) rows.push_back({-1, group == 0 ? "DOWNLOADING" : group == 1 ? "WAITING" : "FINISHED"});
    rows.push_back({i});
  }
  const float x = kListX, w = kListW;
  List(rows, sel, 96, [&](int i, float ry, float over) {
    Item& q = *list[size_t(i)];
    const bool f = i == sel && !rail_focus_;
    const float th = 63, tw = 112, tx = x + 16, ty = ry + (90 - th) * 0.5f;
    if (auto it = dv_->thumbs.find(q.id); it != dv_->thumbs.end() && it->second) {  // (a download came from Discover: dv_ is there)
      const Image& img = it->second;
      const float ia = img.width / img.height, ta = tw / th;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (ia > ta) uv0.x = (1 - ta / ia) * 0.5f, uv1.x = 1 - uv0.x;
      else uv0.y = (1 - ia / ta) * 0.5f, uv1.y = 1 - uv0.y;
      dl_->AddImageRounded(img.id, P(tx, ty), P(tx + tw, ty + th), uv0, uv1, White(1), 8 * s_);
    } else {
      const auto [hi, lo] = ArtColours(q.id);
      GradRect(tx, ty, tw, th, hi, lo, 8);
    }
    const float nx = tx + tw + 20, nw = x + w - 24 - nx;
    const int st = q.state;
    TextMid(a_.body_bold, 21, nx, ry + 30, Mix(White(0.94f), kInk, over), Fit(a_.body_bold, 21, q.name, nw - 120));
    if (st == Item::kGoing || st == Item::kInstalling) {
      const uint64_t t = q.total ? q.total.load() : q.size;
      const float frac = st == Item::kInstalling ? 1.0f : t ? float(double(q.done) / double(t)) : 0.0f;
      TextMid(a_.display, 22, x + w - 24, ry + 30, kText, std::to_string(int(frac * 100 + 0.5f)) + "%", 1);
      Rect(nx, ry + 50, nw, 6, f ? Black(0.25f) : White(0.12f), 3);
      Rect(nx, ry + 50, std::max(6.0f, nw * frac), 6, f ? White(1) : kAccent, 3);
      TextMid(a_.body, 15, nx, ry + 72, Mix(White(0.55f), White(0.85f), over), Fit(a_.body, 15, DownloadLine(q), nw));
    } else {
      TextMid(a_.body, 16, nx, ry + 60, Mix(st == Item::kFailed ? Rgba(255, 140, 146) : White(0.55f), White(0.85f), over),
              Fit(a_.body, 16, DownloadLine(q), nw));
      if (st == Item::kDone) {
        auto& knob = knobs_[kDownloadsTab << 16 | (q.id & 0x7FFF)];
        Ease(knob, q.on ? 1.0f : 0.0f, 14);
        Switch(x + w - 24 - 56, ry + 29, knob, f);
      }
    }
    if (st == Item::kDone) q.seen = true;
  });
  Item& q = *list[size_t(sel)];
  Panel([&](float px, float py, float pw) {
    const int st = q.state;
    const uint64_t t = q.total ? q.total.load() : q.size;
    const float frac = st == Item::kInstalling || st == Item::kDone ? 1.0f : t ? float(double(q.done) / double(t)) : 0.0f;
    // a big ring: how far it got
    const float cx = px + pw * 0.5f, cy = py + 130, rr = 104;
    Glow(cx - rr, cy - rr, rr * 2, rr * 2, rr, WithAlpha(kAccent, st == Item::kGoing ? 0.25f : 0.0f), 40);
    Progress(cx, cy, rr, st == Item::kFailed || st == Item::kStopped ? 0.0f : frac, 12, White(0.10f), st == Item::kGoing || st == Item::kInstalling);
    const bool ended = st == Item::kFailed || st == Item::kStopped;
    TextMid(a_.display, 54, cx, cy - 8, kText,
            st == Item::kWaiting ? "NEXT" : ended ? (st == Item::kFailed ? "!" : "II") : std::to_string(int(frac * 100 + 0.5f)) + "%", 0.5f);
    TextMid(a_.body_bold, 14, cx, cy + 40, White(0.6f),
            st == Item::kGoing ? "DOWNLOADING" : st == Item::kInstalling ? "INSTALLING" : st == Item::kDone ? "IN MY MODS"
            : st == Item::kWaiting ? "WAITING" : st == Item::kFailed ? "DIDN'T FINISH" : "STOPPED",
            0.5f, 2.6f);
    py += 280;
    py += Wrapped(a_.display, 36, px, py, pw, kText, q.name, 1.1f) + 14;
    std::vector<std::pair<std::string, std::string>> facts = {{"KIND", q.types}, {"SIZE", Size(t ? t : q.size)}};
    if (!q.author.empty()) facts.push_back({"BY", q.author});
    if (st == Item::kGoing && q.speed > 1024) facts.push_back({"SPEED", Speed(q.speed)});
    py = Facts(px, py, pw, facts);
    Wrapped(a_.body, 17, px, py + 6, pw, White(0.55f),
            st == Item::kDone ? (q.on ? "On: the game loads it when it starts. Cross turns it off." : "Off. Cross turns it on.")
            : st == Item::kWaiting ? "It starts when the one going is done. Square takes it out of the line."
            : st == Item::kGoing ? "It keeps going on every page. Square stops it."
            : st == Item::kInstalling ? "Nearly done: it's going into My Mods."
                                      : "Cross tries again. Square clears it from the list.");
  });
}

void Hub::DownloadsInput(uint32_t p) {
  const std::vector<Item*> list = Ordered(downloads_);
  if (p & (kCircle | kLeft)) {
    rail_focus_ = true;
    Play(Sound::kBack);
    return;
  }
  if (list.empty()) return;
  int& sel = item_[kDownloadsTab];
  const int before = sel;
  if (p & kUp) sel = std::max(0, sel - 1);
  if (p & kDown) sel = std::min(int(list.size()) - 1, sel + 1);
  if (sel != before) Play(Sound::kMove);
  sel = std::clamp(sel, 0, int(list.size()) - 1);
  if (p & kCross) DownloadAction(*list[size_t(sel)], false);
  if (p & kSquare) DownloadAction(*list[size_t(sel)], true);
}

// ------------------------------------------------------------------ Home's NEW IN DISCOVER

void Hub::DrawHomeFresh(float y, bool on_page, float in) {
  if (!dv_ || dv_->fresh.empty()) return;
  Discover& d = *dv_;
  (void)in;
  Rect(kPageX, y + 9, 18, 2, kAccent, 1);
  Glow(kPageX, y + 9, 18, 2, 1, WithAlpha(kAccent, 0.8f), 6);
  Text(a_.body_bold, 13, kPageX + 30, y + 2, White(0.55f), "NEW IN DISCOVER", 0, 3.2f);
  Text(a_.body, 16, kPageR, y + 1, White(0.45f), "From the online server", 1);
  const float cy = y + 36, ah = 168;
  for (int k = 0; k < int(d.fresh.size()); ++k) {
    const Discover::Entry& e = d.fresh[size_t(k)];
    if (!d.thumbs.count(e.id) && !d.thumbs_asked.count(e.id)) d.thumbs_asked.insert(e.id), DiscoverPicture(e.id, e.pics, 0, true);
    const float cx = kPageX + k * (kCardW + kCardGap);
    const bool focus = on_page && home_sel_ == 2 + k;
    const float grow = focus ? 4 : 0;
    const float ax = cx - grow, ay = cy - grow, aw = kCardW + grow * 2, ahh = ah + grow * 2;
    if (focus) Glow(ax, ay, aw, ahh, 16, WithAlpha(kAccent, 0.9f), 30);
    Glow(ax, ay + 12, aw, ahh, 16, Black(0.5f), 24);
    if (auto it = d.thumbs.find(e.id); it != d.thumbs.end() && it->second) {
      const Image& img = it->second;
      const float ia = img.width / img.height, ta = aw / ahh;
      ImVec2 uv0(0, 0), uv1(1, 1);
      if (ia > ta) uv0.x = (1 - ta / ia) * 0.5f, uv1.x = 1 - uv0.x;
      else uv0.y = (1 - ia / ta) * 0.5f, uv1.y = 1 - uv0.y;
      dl_->AddImageRounded(img.id, P(ax, ay), P(ax + aw, ay + ahh), uv0, uv1, White(1), 14 * s_);
    } else {
      const auto [hi, lo] = ArtColours(e.id);
      GradRect(ax, ay, aw, ahh, hi, lo, 14);
    }
    GradRect(ax, ay + ahh * 0.45f, aw, ahh * 0.55f, Black(0), Black(0.55f), 14);
    dl_->AddRect(P(ax, ay), P(ax + aw, ay + ahh), White(0.10f), 14 * s_);
    const std::string chip = Upper(e.types);
    const float cw = TextWidth(a_.body_bold, 11, chip, 1.6f) + 22;
    GradRect(ax + 12, ay + 12, cw, 26, Black(0.55f), Black(0.65f), 13);
    TextMid(a_.body_bold, 11, ax + 23, ay + 25, kText, chip, 0, 1.6f);
    if (focus) Ring(ax - 4, ay - 4, aw + 8, ahh + 8, 17);
    TextMid(a_.body_bold, 19, cx, cy + ah + 24, focus ? kText : White(0.9f), Fit(a_.body_bold, 19, e.name, kCardW));
    TextMid(a_.body, 15, cx, cy + ah + 48, White(0.5f),
            Fit(a_.body, 15, (e.author.empty() ? std::string("Community") : e.author) + "  \xC2\xB7  " + Size(e.size), kCardW));
  }
}

}  // namespace svr2011::hub
