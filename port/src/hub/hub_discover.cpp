// WWE SmackDown vs. Raw 2011 - the PS5 hub's Discovery (from the Mods page, Triangle): the
// community's mods on the online server, as the PC launcher's Mods > Discovery has them (its
// /api/mods), browsed with the controller: a grid of 24 a page (search, type, order), a mod's
// page with its pictures, and Download, which installs the mod as the Mods page does and
// deletes the file again. Uploads stay with the PC launcher.
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

constexpr int kColumns = 5;
constexpr float kPanelW2 = 1600, kPanelH2 = 900;
constexpr float kCardW = 280, kThumbH = 210, kCardGap = 24, kRowPitch = 300;
constexpr uint64_t kPiece = 4ull << 20;  // (a download, a range at a time)

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
  bool open = false;
  int view = 0;  // 0 the grid, 1 a mod's page
  float in = 0, view_in = 1;
  std::string query;
  int type = 0, sort = 0;
  int page = 0, total = 0, per_page = 24;
  std::vector<Entry> mods;
  int sel = 0;
  float scroll = 0, fx = -1, fy = -1;
  bool loading = false;
  std::string error;
  int gen = 0;  // (a newer page's answers replace an older one's)
  std::map<int, Image> thumbs;   // by mod id
  std::set<int> thumbs_asked;
  int pic = 0;                   // the mod page's picture shown
  int pics_of = 0;               // ... of this mod id
  std::map<int, Image> pics;     // its pictures, by number
  std::set<int> pics_missing;    // (the server has no such picture)
  std::set<int> installed;       // installed from here this time (ids)
  // what the threads send back, run on the hub's
  std::mutex m;
  std::vector<std::function<void(Hub&)>> done;
  void Post(std::function<void(Hub&)> f) {
    std::lock_guard l(m);
    done.push_back(std::move(f));
  }
};

bool Hub::DiscoverOpen() const { return dv_ && dv_->open; }

void Hub::OpenDiscover() {
  if (!dv_) dv_ = std::make_shared<Discover>();
  dv_->open = true;
  dv_->view = 0;
  if (dv_->mods.empty() && !dv_->loading) DiscoverLoad(0);
  Play(Sound::kSelect);
}

// Asks for a page of the list (with the search, type and order as they are).
void Hub::DiscoverLoad(int page) {
  Discover& d = *dv_;
  const int gen = ++d.gen;
  d.loading = true, d.error.clear();
  d.page = std::max(0, page);
  std::string path = "/api/mods?q=" + UrlEncode(d.query) + "&type=" + kTypes[d.type].key + "&sort=" +
                     kSorts[d.sort].key + "&page=" + std::to_string(d.page);
  if (!request_) {
    d.loading = false;
    d.error = "Discovery needs the console's network.";
    return;
  }
  std::shared_ptr<Discover> keep = dv_;
  std::thread([keep, gen, req = request_, url = ServerUrl() + path] {
    std::string answer;
    const int status = req("GET", url, "", "", &answer);
    keep->Post([gen, status, answer](Hub& hub) {
      Discover& d = *hub.dv_;
      if (gen != d.gen) return;
      d.loading = false;
      if (status != 200) {
        d.error = status ? "The server answered HTTP " + std::to_string(status) + "." :
                           "Couldn't reach the online server" + (answer.empty() ? std::string(".") : ": " + answer);
        return;
      }
      const auto top = Members(answer, answer.find('{'));
      d.total = int(Num(top, "total"));
      d.per_page = std::max<int>(1, int(Num(top, "per_page")));
      d.mods.clear();
      const auto it = top.find("mods");
      if (it != top.end()) {
        const std::string& a = it->second;
        for (size_t i = a.find('{'); i != std::string::npos && i < a.size(); i = a.find('{', SkipValue(a, i))) {
          const auto m = Members(a, i);
          Discover::Entry e;
          e.id = int(Num(m, "id"));
          e.name = Str(m, "name"), e.author = Str(m, "author"), e.version = Str(m, "version");
          e.types = TypeLabel(Strings(m, "types"));
          e.size = uint64_t(Num(m, "size")), e.downloads = Num(m, "downloads"), e.pics = Num(m, "pics");
          e.created = Num(m, "created"), e.updated = Num(m, "updated");
          if (e.id > 0) d.mods.push_back(std::move(e));
        }
      }
      d.sel = std::clamp(d.sel, 0, std::max(0, int(d.mods.size()) - 1));
      // (the old page's pictures go)
      std::set<int> keep_ids;
      for (const auto& e : d.mods) keep_ids.insert(e.id);
      for (auto t = d.thumbs.begin(); t != d.thumbs.end();) {
        if (keep_ids.count(t->first)) {
          ++t;
          continue;
        }
        if (hub.textures_.drop) hub.textures_.drop(t->second);
        t = d.thumbs.erase(t);
      }
      d.thumbs_asked.clear();
      for (const auto& e : d.mods)
        if (d.thumbs.count(e.id)) d.thumbs_asked.insert(e.id);
    });
  }).detach();
}

// A mod's picture `n` (0: the first, the grid's), fetched once; `thumb`: for the grid.
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
    if (status == 200) rgba = Picture(answer, thumb ? int(kCardW) : 840, &w, &h);
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

// A mod's page: its description and file name, and its pictures.
void Hub::DiscoverOpenMod() {
  Discover& d = *dv_;
  if (d.sel < 0 || d.sel >= int(d.mods.size())) return;
  Discover::Entry& e = d.mods[size_t(d.sel)];
  d.view = 1, d.view_in = 0, d.pic = 0;
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
      for (Discover::Entry& e : hub.dv_->mods)
        if (e.id == id) {
          e.description = Str(m, "description");
          e.file_name = Str(m, "file_name");
          e.detail = true;
        }
    });
  }).detach();
}

// Download: the .svrmod a range at a time into the app's folder, then installed as the Mods
// page installs one, and the file deleted (task kind 7).
void Hub::DiscoverDownload() {
  Discover& d = *dv_;
  if (d.sel < 0 || d.sel >= int(d.mods.size())) return;
  if (!download_) {
    Play(Sound::kError);
    Toast("Downloads need the console's network");
    return;
  }
  if (Busy()) return;
  const Discover::Entry e = d.mods[size_t(d.sel)];
  auto t = std::make_shared<Task>();
  task_kind_ = 7;
  const fs::path folder = paths_.game / "Downloaded Mods";
  task_file_ = folder / SafeFile(e.file_name.empty() ? e.name : e.file_name);
  task_title_ = e.name;
  discover_task_id_ = e.id;
  Task* raw = t.get();
  t->p.total = e.size;
  const fs::path game = paths_.game, file = task_file_;
  const std::string url = ServerUrl() + "/api/mods/" + std::to_string(e.id) + "/file";
  t->thread = std::thread([raw, game, file, url, dl = download_] {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path part = file.string() + ".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    uint64_t at = 0, total = raw->p.total;
    int failures = 0;
    std::string why;
    while (!raw->p.cancel && (total == 0 || at < total)) {
      std::string body;
      uint64_t reported = 0;
      const uint64_t to = at + kPiece - 1;
      const int status = dl(url, at, total ? std::min(to, total - 1) : to, &body, &reported);
      if (status != 200 && status != 206) {
        if (++failures < 6) continue;  // (a dropped connection: the same range again)
        why = status ? "The server answered HTTP " + std::to_string(status) + "." : "The download stopped: " + body;
        break;
      }
      failures = 0;
      if (reported) total = reported, raw->p.total = reported;
      if (status == 200) at = 0, out.seekp(0);  // (no ranges: the whole file came)
      out.write(body.data(), std::streamsize(body.size()));
      at += body.size();
      raw->p.done = at;
      if (status == 200 || body.empty()) break;
    }
    out.close();
    if (raw->p.cancel) {
      fs::remove(part, ec);
      raw->message = "Download stopped";
    } else if (!why.empty() || (total && at < total) || !out) {
      fs::remove(part, ec);
      raw->message = why.empty() ? "The download didn't finish. Try again." : why;
    } else {
      fs::rename(part, file, ec);
      raw->p.SetFile(file.filename().string());
      std::string msg;
      raw->ok = !ec && install::InstallMod(game, file, false, &msg);
      raw->message = ec ? "The download couldn't be saved." : msg;
    }
    raw->p.finished = true;
  });
  task_ = std::move(t);
  press_ = 1;
  Play(Sound::kSelect);
}

// Called every frame: what the threads finished.
void Hub::PollDiscover() {
  if (!dv_) return;
  std::vector<std::function<void(Hub&)>> done;
  {
    std::lock_guard l(dv_->m);
    done.swap(dv_->done);
  }
  for (auto& f : done) f(*this);
}

// A finished download (PollTask, kind 7).
void Hub::DiscoverDownloaded(bool ok, const std::string& message) {
  std::error_code ec;
  fs::remove(task_file_, ec);  // (installed in Mods now, or not at all)
  if (ok) {
    if (dv_) dv_->installed.insert(discover_task_id_);
    LoadMods();
    Play(Sound::kOn);
  } else if (message != "Download stopped") {
    Play(Sound::kError);
  }
  Toast(message);
  discover_task_id_ = 0;
}

void Hub::DiscoverInput(uint32_t p) {
  Discover& d = *dv_;
  const int n = int(d.mods.size());
  const bool downloading = task_ && task_kind_ == 7;
  if (d.view == 1) {  // a mod's page
    const Discover::Entry& e = d.mods[size_t(std::clamp(d.sel, 0, std::max(0, n - 1)))];
    int shown = 0;
    for (int i = 0; i < 4; ++i) shown += !d.pics_missing.count(i);
    if ((p & kLeft) && shown > 1) d.pic = (d.pic + 3) % 4, Play(Sound::kMove);
    if ((p & kRight) && shown > 1) d.pic = (d.pic + 1) % 4, Play(Sound::kMove);
    while (d.pics_missing.count(d.pic) && d.pics_missing.size() < 4) d.pic = (d.pic + 1) % 4;
    if (p & kCross) {
      if (downloading) {
        Play(Sound::kError);
        Toast(discover_task_id_ == e.id ? "It's downloading" : "Wait for the download to finish");
      } else {
        DiscoverDownload();
      }
    }
    if ((p & kSquare) && downloading) task_->Cancel(), Play(Sound::kBack);
    if (p & kCircle) d.view = 0, d.view_in = 0, Play(Sound::kBack);
    return;
  }
  if (p & kCircle) {
    d.open = false;
    Play(Sound::kBack);
    return;
  }
  const int pages = std::max(1, (d.total + d.per_page - 1) / d.per_page);
  if ((p & kR1) && d.page + 1 < pages) d.sel = 0, DiscoverLoad(d.page + 1), Play(Sound::kPage);
  if ((p & kL1) && d.page > 0) d.sel = 0, DiscoverLoad(d.page - 1), Play(Sound::kPage);
  if (p & kSquare) {  // search
    Ask("Search Mods", d.query, false, [this](const std::string& text) {
      dv_->query = text;
      dv_->sel = 0;
      DiscoverLoad(0);
    });
    return;
  }
  if (p & kTriangle) {  // the type and the order
    std::vector<std::string> items;
    for (const Named& t : kTypes) items.push_back(t.label);
    for (const Named& s : kSorts) items.push_back(std::string("Sort: ") + s.label);
    const int n_types = int(std::size(kTypes));
    for (int i = 0; i < int(items.size()); ++i)
      if (i == d.type || i == n_types + d.sort) items[size_t(i)] += "  \xE2\x80\xA2";
    Choose("Show", std::move(items), [this, n_types](int i) {
      if (i < n_types) dv_->type = i;
      else dv_->sort = i - n_types;
      dv_->sel = 0;
      DiscoverLoad(0);
    });
    choice_sel_ = d.type;
    return;
  }
  if (n == 0) return;
  const int before = d.sel;
  if (p & kLeft) d.sel = std::max(0, d.sel - 1);
  if (p & kRight) d.sel = std::min(n - 1, d.sel + 1);
  if (p & kUp) d.sel = d.sel >= kColumns ? d.sel - kColumns : d.sel;
  if (p & kDown) d.sel = std::min(n - 1, d.sel + kColumns);
  if (d.sel != before) Play(Sound::kMove);
  if (p & kCross) Play(Sound::kSelect), DiscoverOpenMod();
}

float Hub::DiscoverHints(float r) {
  const Discover& d = *dv_;
  const bool downloading = task_ && task_kind_ == 7;
  if (d.view == 1) {
    r = Hint(r, kCircle, "Back");
    if (downloading) r = Hint(r, kSquare, "Stop Download");
    int shown = 0;
    for (int i = 0; i < 4; ++i) shown += !d.pics_missing.count(i);
    if (shown > 1) r = Hint(r, kLeft | kRight, "Pictures");
    if (!downloading) {
      const bool again = d.sel < int(d.mods.size()) && d.installed.count(d.mods[size_t(d.sel)].id);
      r = Hint(r, kCross, again ? "Download Again" : "Download & Install");
    }
    return r;
  }
  r = Hint(r, kCircle, "Close");
  const int pages = std::max(1, (d.total + d.per_page - 1) / d.per_page);
  if (pages > 1) r = Hint(r, kL1, "Page");
  r = Hint(r, kTriangle, "Show");
  r = Hint(r, kSquare, "Search");
  if (!d.mods.empty()) r = Hint(r, kCross, "Open");
  return r;
}

void Hub::DrawDiscover() {
  if (!dv_) return;
  Discover& d = *dv_;
  Ease(d.in, d.open ? 1.0f : 0.0f, 14);
  if (d.in < 0.01f) return;
  Ease(d.view_in, 1, 12);
  const float a = Smooth(d.in);
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  dl_->AddRectFilled(ImVec2(0, 0), size, Black(0.78f * a));
  const float w = kPanelW2, h = kPanelH2, x = (kW - w) * 0.5f, y = (kH - h) * 0.5f - 20 + (1 - a) * 30;
  const int v0 = dl_->VtxBuffer.Size;
  Rect(x, y, w, h, Rgba(22, 23, 27), 18);
  dl_->AddRectFilled(P(x, y), P(x + 6, y + h), kAccent, 18 * s_, ImDrawFlags_RoundCornersLeft);
  const int n = int(d.mods.size());
  const int pages = std::max(1, (d.total + d.per_page - 1) / d.per_page);
  const bool downloading = task_ && task_kind_ == 7;

  // the bottom line: a download going, or where the mods come from
  const float fy = y + h - 56;
  if (downloading) {
    const uint64_t done = task_->p.done, total = task_->p.total;
    const float frac = total ? float(double(done) / double(total)) : 0.0f;
    const std::string file = task_->p.File();
    const std::string line = (file.empty() ? "Downloading " + task_title_ + "  \xC2\xB7  " + Size(done) +
                                                 (total ? " of " + Size(total) : std::string())
                                           : "Installing " + task_title_ + "...");
    dl_->AddCircleFilled(P(x + 60, fy + 12), 4.5f * s_, WithAlpha(kAccent, 0.5f + 0.5f * std::sin(time_ * 6)), 16);
    Text(a_.body_bold, 19, x + 76, fy + 1, White(0.8f), Fit(a_.body_bold, 19, line, w - 520));
    Rect(x + w - 420, fy + 9, 360, 6, White(0.12f), 3);
    Rect(x + w - 420, fy + 9, std::max(6.0f, 360 * frac), 6, kAccent, 3);
  } else {
    Text(a_.body, 19, x + 54, fy + 1, White(0.4f),
         "The community's mods from the online server, as the PC launcher's Discovery has them. Upload yours from the "
         "PC launcher.");
  }

  if (d.view == 1 && n) {  // ----------------------------------------- a mod's page
    const Discover::Entry& e = d.mods[size_t(std::clamp(d.sel, 0, n - 1))];
    const float slide = (1 - Smooth(d.view_in)) * 30;
    const float px = x + 52 + slide, py = y + 52, pw = 840, ph = 630;
    Rect(px, py, pw, ph, White(0.05f), 12);
    if (auto it = d.pics.find(d.pic); it != d.pics.end() && it->second) {
      const Image& img = it->second;
      const float sc = std::min(pw / img.width, ph / img.height);
      const float iw = img.width * sc, ih = img.height * sc;
      dl_->AddImageRounded(img.id, P(px + (pw - iw) * 0.5f, py + (ph - ih) * 0.5f),
                           P(px + (pw + iw) * 0.5f, py + (ph + ih) * 0.5f), ImVec2(0, 0), ImVec2(1, 1), White(1), 12 * s_);
    } else if (!d.pics_missing.count(d.pic)) {
      Text(a_.body_bold, 20, px + pw * 0.5f, py + ph * 0.5f - 12, White(0.35f), "Loading the picture...", 0.5f);
    }
    // the pictures' dots
    int shown = 0;
    for (int i = 0; i < 4; ++i) shown += !d.pics_missing.count(i);
    if (shown > 1) {
      float dx = px + pw * 0.5f - (shown - 1) * 11;
      for (int i = 0; i < 4; ++i) {
        if (d.pics_missing.count(i)) continue;
        dl_->AddCircleFilled(P(dx, py + ph + 26), 4.5f * s_, i == d.pic ? kAccent : White(0.3f), 16);
        dx += 22;
      }
    }
    const float tx = px + pw + 52, tw = x + w - 52 - tx;
    float ty = py;
    Text(a_.body_bold, 15, tx, ty + 4, White(0.5f), Upper(e.types), 0, 3);
    ty += 34;
    ty += Wrapped(a_.display, 44, tx, ty, tw, kText, e.name, 1.12f) + 12;
    if (!e.author.empty()) ty += Wrapped(a_.body_bold, 21, tx, ty, tw, White(0.7f), "by " + e.author) + 18;
    std::vector<std::pair<std::string, std::string>> facts;
    if (!e.version.empty()) facts.push_back({"VERSION", e.version});
    facts.push_back({"SIZE", Size(e.size)});
    facts.push_back({"DOWNLOADS", std::to_string(e.downloads)});
    const int64_t when = e.updated ? e.updated : e.created;
    if (when) facts.push_back({e.updated ? "UPDATED" : "ADDED", Date(when)});
    ty = Facts(tx, ty, tw, facts) + 8;
    const float bottom = y + h - 90;
    if (downloading && discover_task_id_ == e.id) {
      const uint64_t done = task_->p.done, total = task_->p.total;
      const float frac = total ? float(double(done) / double(total)) : 0.0f;
      Text(a_.body_bold, 15, tx, bottom - 64, White(0.45f), task_->p.File().empty() ? "DOWNLOADING" : "INSTALLING", 0, 3);
      Text(a_.display, 40, tx + tw, bottom - 82, kText, std::to_string(int(frac * 100 + 0.5f)) + "%", 1);
      Rect(tx, bottom - 24, tw, 8, White(0.12f), 4);
      Rect(tx, bottom - 24, std::max(8.0f, tw * frac), 8, kAccent, 4);
    } else if (d.installed.count(e.id)) {
      dl_->AddCircleFilled(P(tx + 6, bottom - 22), 5 * s_, kAccent, 16);
      Text(a_.body_bold, 20, tx + 22, bottom - 34, White(0.85f), "Installed. Turn it on or off on the Mods page.");
    }
    const std::string desc = e.detail ? (e.description.empty() ? std::string("No description.") : e.description)
                                      : std::string("Loading...");
    dl_->PushClipRect(P(tx, ty), P(tx + tw, bottom - 100), true);
    Wrapped(a_.body, 20, tx, ty + 4, tw, White(0.65f), desc);
    dl_->PopClipRect();
    Fade(v0, a);
    return;
  }

  // ------------------------------------------------------------------ the grid
  Text(a_.display, 46, x + 52, y + 38, kText, "DISCOVER");
  std::string line = kTypes[d.type].label;
  line += std::string("  \xC2\xB7  ") + kSorts[d.sort].label;
  if (!d.query.empty()) line += "  \xC2\xB7  \xE2\x80\x9C" + d.query + "\xE2\x80\x9D";
  Text(a_.body_bold, 21, x + 54, y + 108, White(0.55f), Fit(a_.body_bold, 21, line, w - 520));
  if (!d.loading && d.error.empty())
    Text(a_.body_bold, 21, x + w - 52, y + 108, d.total ? WithAlpha(kAccent, 1) : White(0.4f),
         Count(d.total, "mod", "mods") + (pages > 1 ? "  \xC2\xB7  page " + std::to_string(d.page + 1) + " of " +
                                                         std::to_string(pages)
                                                   : std::string()),
         1);
  Rect(x + 52, y + 150, w - 104, 1, White(0.08f));
  const float gx = x + (w - (kColumns * kCardW + (kColumns - 1) * kCardGap)) * 0.5f, gy = y + 176;
  const float view_h = fy - 20 - gy;
  if (d.loading && n == 0) {
    Text(a_.body_bold, 24, x + w * 0.5f, gy + 200, White(0.45f), "Looking for mods...", 0.5f);
  } else if (!d.error.empty()) {
    Wrapped(a_.body, 24, x + 54, gy + 20, w - 108, White(0.6f),
            d.error + " Check that the PS5 is online, then press Triangle and pick a type to try again.");
  } else if (n == 0) {
    Wrapped(a_.body, 24, x + 54, gy + 20, w - 108, White(0.6f),
            d.query.empty() ? "No mods here yet." : "No mods match \xE2\x80\x9C" + d.query + "\xE2\x80\x9D. Press Square to search for something else.");
  } else {
    d.sel = std::clamp(d.sel, 0, n - 1);
    const int row = d.sel / kColumns, rows = (n + kColumns - 1) / kColumns;
    const float max_scroll = std::max(0.0f, rows * kRowPitch - kCardGap - view_h);
    const float want = std::clamp(row * kRowPitch - (view_h - kRowPitch) * 0.5f, 0.0f, max_scroll);
    d.scroll += (want - d.scroll) * (1 - std::exp(-dt_ * 14));
    const float fxw = gx + (d.sel % kColumns) * (kCardW + kCardGap), fyw = gy + row * kRowPitch;
    if (d.fx < 0 || d.in < 0.05f) d.fx = fxw, d.fy = fyw, d.scroll = want;
    d.fx += (fxw - d.fx) * (1 - std::exp(-dt_ * 20));
    d.fy += (fyw - d.fy) * (1 - std::exp(-dt_ * 20));
    dl_->PushClipRect(P(x, gy - 6), P(x + w, gy + view_h), true);
    // the focus: a ring round the card
    {
      const float rx = d.fx - 6, ry = d.fy - d.scroll - 6;
      dl_->AddRect(P(rx, ry), P(rx + kCardW + 12, ry + kRowPitch - kCardGap + 12), kText, 14 * s_, 0, 3 * s_);
    }
    for (int i = 0; i < n; ++i) {
      const Discover::Entry& e = d.mods[size_t(i)];
      const float cx = gx + (i % kColumns) * (kCardW + kCardGap), cy = gy + (i / kColumns) * kRowPitch - d.scroll;
      if (cy > gy + view_h || cy + kRowPitch < gy) continue;
      // (its picture: asked for once it's near)
      if (!d.thumbs_asked.count(e.id)) {
        d.thumbs_asked.insert(e.id);
        DiscoverPicture(e.id, e.pics, 0, true);
      }
      Rect(cx, cy, kCardW, kThumbH, White(0.06f), 10);
      if (auto it = d.thumbs.find(e.id); it != d.thumbs.end() && it->second) {
        const Image& img = it->second;
        const float sc = std::max(kCardW / img.width, kThumbH / img.height);
        const float uw = kCardW / (img.width * sc), vh = kThumbH / (img.height * sc);
        dl_->AddImageRounded(img.id, P(cx, cy), P(cx + kCardW, cy + kThumbH), ImVec2((1 - uw) * 0.5f, (1 - vh) * 0.5f),
                             ImVec2((1 + uw) * 0.5f, (1 + vh) * 0.5f), White(1), 10 * s_);
      }
      if (d.installed.count(e.id) || (downloading && discover_task_id_ == e.id)) {
        const char* tag = d.installed.count(e.id) ? "INSTALLED" : "DOWNLOADING";
        const float tw2 = TextWidth(a_.body_bold, 13, tag, 2) + 20;
        Rect(cx + 10, cy + 10, tw2, 26, Black(0.7f), 13);
        Text(a_.body_bold, 13, cx + 20, cy + 15, kAccent, tag, 0, 2);
      }
      Text(a_.body_bold, 20, cx + 2, cy + kThumbH + 12, White(i == d.sel ? 1.0f : 0.85f),
           Fit(a_.body_bold, 20, e.name, kCardW - 4));
      const std::string sub = e.types + (e.author.empty() ? std::string() : "  \xC2\xB7  " + e.author);
      Text(a_.body, 16, cx + 2, cy + kThumbH + 40, White(0.45f), Fit(a_.body, 16, sub, kCardW - 4));
    }
    dl_->PopClipRect();
    if (d.loading) Text(a_.body_bold, 18, x + w - 52, y + 52, White(0.45f), "Loading...", 1);
  }
  Fade(v0, a);
}

}  // namespace svr2011::hub
