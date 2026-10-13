// WWE SmackDown vs. Raw 2011 - the PS5 hub's installer (see hub_install.h).
// The disc image's XDVDFS file system is read as the PC launcher reads it
// (launcher/svr2011_launcher.c), the check is launcher/verify.c's, and mods
// install as launcher/mods_tab.c installs them.
#include "hub_install.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miniz.h"

namespace svr2011::hub::install {

namespace {

constexpr uint32_t kTitleId = 0x5451085D;  // the game's (default.xex execution info)
constexpr size_t kChunk = 8u << 20;

// ------------------------------------------------------------------ helpers

uint32_t g_crc[8][256];

void BuildCrc() {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int j = 0; j < 8; ++j) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i)
    for (int j = 1; j < 8; ++j) g_crc[j][i] = (g_crc[j - 1][i] >> 8) ^ g_crc[0][g_crc[j - 1][i] & 0xFF];
}

void CrcInit() {
  static std::once_flag once;
  std::call_once(once, BuildCrc);
}

// zlib's CRC-32, 8 bytes at a time. `c`: the CRC so far (0 to start).
uint32_t Crc(uint32_t c, const uint8_t* p, size_t n) {
  c = ~c;
  while (n >= 8) {
    const uint32_t a = c ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
    const uint32_t b = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
    c = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^ g_crc[4][a >> 24] ^
        g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^ g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
    p += 8, n -= 8;
  }
  while (n--) c = g_crc[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
  return ~c;
}

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool EndsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && Lower(s.substr(s.size() - n)) == tail;
}

std::string ErrnoText(int e) { return std::strerror(e); }

// A file opened for reading at any offset.
class Reader {
 public:
  explicit Reader(const fs::path& p) : fd_(::open(p.c_str(), O_RDONLY)) {}
  ~Reader() {
    if (fd_ >= 0) ::close(fd_);
  }
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  bool ok() const { return fd_ >= 0; }
  bool Read(uint64_t off, void* buf, size_t n) const {
    auto* p = static_cast<uint8_t*>(buf);
    while (n) {
      const ssize_t got = ::pread(fd_, p, n, off_t(off));
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) return false;
      p += got, off += uint64_t(got), n -= size_t(got);
    }
    return true;
  }
  uint64_t Size() const {
    struct stat st {};
    return fd_ >= 0 && ::fstat(fd_, &st) == 0 ? uint64_t(st.st_size) : 0;
  }

 private:
  int fd_ = -1;
};

// A file being written: <name>.part, renamed to <name> when complete -
// or (`direct`: inside a folder that is itself moved into place when done,
// a mod's) straight to <name>, which saves a rename per file.
class Writer {
 public:
  explicit Writer(fs::path dst, bool direct = false)
      : dst_(std::move(dst)), part_(direct ? dst_ : fs::path(dst_.string() + ".part")), direct_(direct) {
    std::error_code ec;
    if (!direct || !fs::is_directory(dst_.parent_path(), ec)) fs::create_directories(dst_.parent_path(), ec);
    fd_ = ::open(part_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) error_ = "couldn't create " + dst_.filename().string() + " (" + ErrnoText(errno) + ")";
  }
  ~Writer() {
    if (fd_ >= 0) ::close(fd_);
    if (!done_) ::unlink(part_.c_str());
  }
  bool Write(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n && fd_ >= 0) {
      const ssize_t put = ::write(fd_, p, n);
      if (put < 0 && errno == EINTR) continue;
      if (put <= 0) {
        error_ = "couldn't write " + dst_.filename().string() + " (" + ErrnoText(put < 0 ? errno : ENOSPC) +
                 "). Is the storage full?";
        return false;
      }
      p += put, n -= size_t(put);
    }
    return fd_ >= 0;
  }
  bool Finish() {
    if (fd_ < 0) return false;
    const int rc = ::close(fd_);
    fd_ = -1;
    if (rc != 0) {
      error_ = "couldn't write " + dst_.filename().string() + " (" + ErrnoText(errno) + ")";
      return false;
    }
    if (!direct_) {
      std::error_code ec;
      fs::rename(part_, dst_, ec);
      if (ec) {
        error_ = "couldn't rename " + part_.filename().string() + " (" + ec.message() + ")";
        return false;
      }
    }
    done_ = true;
    return true;
  }
  const std::string& error() const { return error_; }

 private:
  fs::path dst_, part_;
  bool direct_ = false;
  int fd_ = -1;
  bool done_ = false;
  std::string error_;
};

// A file's CRC-32 (0 if it can't be read).
uint32_t FileCrc(const fs::path& p);

uint64_t FileSize(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? uint64_t(-1) : uint64_t(n);
}

uint32_t FileCrc(const fs::path& p) {
  CrcInit();
  Reader f(p);
  if (!f.ok()) return 0;
  std::vector<uint8_t> buf(4u << 20);
  uint32_t c = 0;
  const uint64_t size = f.Size();
  for (uint64_t at = 0; at < size;) {
    const size_t n = size_t(std::min<uint64_t>(size - at, buf.size()));
    if (!f.Read(at, buf.data(), n)) return 0;
    c = Crc(c, buf.data(), n);
    at += n;
  }
  return c;
}

// ----------------------------------------------------------- the game files

// The disc's files: path, size, CRC-32, the CRC after the port's change (0:
// none), flags (launcher/game_files_crc.inc, made by tools/make_game_file_crcs.py).
constexpr uint32_t VERIFY_MENU_TABLE = 1;  // menu.pac: its menu table (the port adds rows) is left out
struct GameFile {
  const wchar_t* path;
  uint64_t size;
  uint32_t crc, crc_ported, flags;
};
const GameFile kFiles[] = {
#include "../../launcher/game_files_crc.inc"
};
constexpr uint64_t kMenuTableAt = 0x25F000, kMenuTableSize = 0x6800;  // (game_files.cpp kMflo, kMfloSlot)

std::string PathOf(const GameFile& g) {
  std::string s;
  for (const wchar_t* w = g.path; *w; ++w) s += *w == L'\\' ? '/' : char(*w);
  return s;
}

// Files a game folder may lack: the disc's system update and dashboard art.
bool Optional(const std::string& path) { return path.rfind("$SystemUpdate/", 0) == 0 || path == "nxeart"; }

// ------------------------------------------------------------ the disc image

struct DiscEntry {
  std::string path;  // relative, '/'
  uint32_t sector = 0, size = 0;
  bool dir = false;
};

struct Disc {
  std::unique_ptr<Reader> f;
  uint64_t base = 0;  // the game partition's offset in the image
  std::vector<DiscEntry> entries;

  bool Read(uint64_t off, void* buf, size_t n) const { return f->Read(base + off, buf, n); }

  // One directory table: u16 left, u16 right, u32 sector, u32 size, u8
  // attributes, u8 name length, name; 4-byte aligned; 0xFFFF where an entry
  // would start pads to the next sector.
  bool Dir(uint32_t sector, uint32_t size, const std::string& prefix, int depth) {
    if (depth > 32 || size > (16u << 20) || entries.size() > 100000) return false;
    std::vector<uint8_t> buf(size);
    if (!Read(uint64_t(sector) * 2048, buf.data(), size)) return false;
    uint32_t off = 0;
    while (off + 14 <= size) {
      const uint8_t* e = buf.data() + off;
      const uint16_t left = Le16(e);
      const uint8_t attr = e[12], nlen = e[13];
      if (left == 0xFFFF || nlen == 0) {
        off = (off / 2048 + 1) * 2048;
        continue;
      }
      if (off + 14 + nlen > size) return false;
      const std::string name(reinterpret_cast<const char*>(e + 14), nlen);
      if (name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos ||
          name.find('\0') != std::string::npos)
        return false;  // (unsafe, or broken)
      DiscEntry d;
      d.path = prefix.empty() ? name : prefix + "/" + name;
      d.sector = Le32(e + 4);
      d.size = Le32(e + 8);
      d.dir = attr & 0x10;
      entries.push_back(d);
      if (d.dir && d.size && !Dir(d.sector, d.size, d.path, depth + 1)) return false;
      off = (off + 14 + nlen + 3) & ~3u;
    }
    return true;
  }

  const DiscEntry* Find(const std::string& path) const {
    for (const DiscEntry& e : entries)
      if (!e.dir && Lower(e.path) == Lower(path)) return &e;
    return nullptr;
  }

  // The title ID in default.xex's execution info (optional header 0x00040006).
  bool TitleId(const DiscEntry& x, uint32_t* id) const {
    uint8_t hdr[24], opt[64 * 8], exe[16];
    const uint64_t at = uint64_t(x.sector) * 2048;
    if (x.size < 24 || !Read(at, hdr, 24) || std::memcmp(hdr, "XEX2", 4)) return false;
    const uint32_t n = Be32(hdr + 20);
    if (n == 0 || n > 64 || 24 + n * 8 > x.size || !Read(at + 24, opt, n * 8)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      if (Be32(opt + i * 8) != 0x00040006u) continue;
      const uint32_t o = Be32(opt + i * 8 + 4);
      if (o + 16 > x.size || !Read(at + o, exe, 16)) return false;
      *id = Be32(exe + 12);
      return true;
    }
    return false;
  }
};

// Opens an image (Redump ISO or XISO) and lists its files; `why` if it can't.
bool OpenDisc(const fs::path& image, Disc* d, DiscImage::State* state, std::string* why) {
  d->f = std::make_unique<Reader>(image);
  if (!d->f->ok()) {
    *state = DiscImage::kNotADisc;
    *why = "It can't be opened (" + ErrnoText(errno) + ").";
    return false;
  }
  static const uint64_t kBases[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};
  for (uint64_t base : kBases) {
    d->base = base;
    uint8_t vd[2048];
    if (!d->Read(32 * 2048, vd, sizeof(vd)) || std::memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) continue;
    d->entries.clear();
    if (!d->Dir(Le32(vd + 20), Le32(vd + 24), "", 0)) {
      *state = DiscImage::kNotADisc;
      *why = "The image is damaged: its file list can't be read.";
      return false;
    }
    const DiscEntry* x = d->Find("default.xex");
    uint32_t tid = 0;
    if (!x || !d->TitleId(*x, &tid)) {
      *state = DiscImage::kOtherGame;
      *why = x ? "Its default.xex can't be read: the image may be incomplete." : "It isn't an Xbox 360 game disc.";
      return false;
    }
    if (tid != kTitleId) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "It's another game (title ID %08X).", tid);
      *state = DiscImage::kOtherGame;
      *why = buf;
      return false;
    }
    *state = DiscImage::kThisGame;
    return true;
  }
  *state = DiscImage::kNotADisc;
  *why = "It isn't an Xbox 360 disc image (no Xbox file system in it).";
  return false;
}

// ------------------------------------------------------------------ zip files

struct ZipEntry {
  std::string name;
  uint16_t method = 0;
  uint32_t crc = 0;
  uint64_t csize = 0, usize = 0, local = 0;
};

// The central directory (zip64 too).
bool ReadZip(const Reader& f, std::vector<ZipEntry>* out) {
  const uint64_t size = f.Size();
  if (size < 22) return false;
  const size_t tail = size_t(std::min<uint64_t>(size, 65557));
  std::vector<uint8_t> t(tail);
  if (!f.Read(size - tail, t.data(), tail)) return false;
  size_t eocd = std::string::npos;
  for (size_t i = tail - 22 + 1; i-- > 0;)
    if (Le32(&t[i]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd == std::string::npos) return false;
  uint64_t count = Le16(&t[eocd + 10]), cd_size = Le32(&t[eocd + 12]), cd_off = Le32(&t[eocd + 16]);
  if ((count == 0xFFFF || cd_off == 0xFFFFFFFF) && eocd >= 20 && Le32(&t[eocd - 20]) == 0x07064b50) {
    uint8_t z[56];
    if (!f.Read(Le64(&t[eocd - 20 + 8]), z, sizeof(z)) || Le32(z) != 0x06064b50) return false;
    count = Le64(z + 32), cd_size = Le64(z + 40), cd_off = Le64(z + 48);
  }
  if (cd_size > (64u << 20) || cd_off + cd_size > size) return false;
  std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
  if (!f.Read(cd_off, cd.data(), cd.size())) return false;
  size_t p = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (p + 46 > cd.size() || Le32(&cd[p]) != 0x02014b50) return false;
    ZipEntry e;
    e.method = Le16(&cd[p + 10]);
    e.crc = Le32(&cd[p + 16]);
    e.csize = Le32(&cd[p + 20]);
    e.usize = Le32(&cd[p + 24]);
    const size_t nl = Le16(&cd[p + 28]), xl = Le16(&cd[p + 30]), cl = Le16(&cd[p + 32]);
    e.local = Le32(&cd[p + 42]);
    if (p + 46 + nl + xl + cl > cd.size()) return false;
    e.name.assign(reinterpret_cast<const char*>(&cd[p + 46]), nl);
    std::replace(e.name.begin(), e.name.end(), '\\', '/');
    // zip64: the sizes and offset that didn't fit, in that order
    const size_t xend = p + 46 + nl + xl;
    for (size_t x = p + 46 + nl; x + 4 <= xend;) {
      const uint16_t id = Le16(&cd[x]), len = Le16(&cd[x + 2]);
      if (x + 4 + len > xend) break;
      if (id == 0x0001) {
        size_t q = x + 4;
        if (e.usize == 0xFFFFFFFF && q + 8 <= x + 4 + len) e.usize = Le64(&cd[q]), q += 8;
        if (e.csize == 0xFFFFFFFF && q + 8 <= x + 4 + len) e.csize = Le64(&cd[q]), q += 8;
        if (e.local == 0xFFFFFFFF && q + 8 <= x + 4 + len) e.local = Le64(&cd[q]);
      }
      x += 4 + len;
    }
    out->push_back(std::move(e));
    p += 46 + nl + xl + cl;
  }
  return true;
}

// An entry's bytes, in pieces to `sink` (false from it: stop). Checks the CRC.
bool Unzip(const Reader& f, const ZipEntry& e, const std::function<bool(const uint8_t*, size_t)>& sink,
           std::string* why) {
  uint8_t lh[30];
  if (!f.Read(e.local, lh, 30) || Le32(lh) != 0x04034b50) {
    *why = "the zip is damaged (" + e.name + ")";
    return false;
  }
  uint64_t at = e.local + 30 + Le16(lh + 26) + Le16(lh + 28);
  uint32_t crc = 0;
  uint64_t out = 0;
  bool too_long = false;
  auto emit = [&](const uint8_t* p, size_t n) {
    if (n > e.usize - out) {  // (more than it says it holds: damaged - don't write on forever)
      too_long = true;
      return false;
    }
    crc = Crc(crc, p, n);
    out += n;
    return sink(p, n);
  };
  if (e.method == 0) {
    std::vector<uint8_t> buf(std::min<uint64_t>(e.csize, kChunk));
    for (uint64_t left = e.csize; left;) {
      const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
      if (!f.Read(at, buf.data(), n)) {
        *why = "the zip can't be read (" + e.name + ")";
        return false;
      }
      if (!emit(buf.data(), n)) {
        if (too_long) *why = "the zip is damaged (" + e.name + ")";
        return false;
      }
      at += n, left -= n;
    }
  } else if (e.method == 8) {
    auto inf = std::make_unique<tinfl_decompressor>();
    tinfl_init(inf.get());
    std::vector<uint8_t> in(1u << 18), dict(TINFL_LZ_DICT_SIZE);
    size_t in_have = 0, in_pos = 0, dict_ofs = 0;
    uint64_t left = e.csize;
    for (;;) {
      if (in_pos == in_have && left) {
        in_have = size_t(std::min<uint64_t>(left, in.size()));
        if (!f.Read(at, in.data(), in_have)) {
          *why = "the zip can't be read (" + e.name + ")";
          return false;
        }
        at += in_have, left -= in_have, in_pos = 0;
      }
      size_t in_bytes = in_have - in_pos, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
      const tinfl_status st = tinfl_decompress(inf.get(), in.data() + in_pos, &in_bytes, dict.data(),
                                               dict.data() + dict_ofs, &out_bytes, left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
      in_pos += in_bytes;
      if (out_bytes && !emit(dict.data() + dict_ofs, out_bytes)) {
        if (too_long) *why = "the zip is damaged (" + e.name + ")";
        return false;
      }
      dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
      if (st == TINFL_STATUS_DONE) break;
      if (st < TINFL_STATUS_DONE || (st == TINFL_STATUS_NEEDS_MORE_INPUT && !left && in_pos == in_have)) {
        *why = "the zip is damaged (" + e.name + ")";
        return false;
      }
    }
  } else {
    *why = e.name + " is packed in a way the hub can't unpack (method " + std::to_string(e.method) + ")";
    return false;
  }
  if (out != e.usize || crc != e.crc) {
    *why = "the zip is damaged (" + e.name + " doesn't match its checksum)";
    return false;
  }
  return true;
}

// A safe relative path inside a folder (no "..", nothing absolute).
bool SafeName(const std::string& name) {
  if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return false;
  size_t a = 0;
  while (a <= name.size()) {
    const size_t b = std::min(name.find('/', a), name.size());
    if (name.compare(a, b - a, "..") == 0 && b - a == 2) return false;
    a = b + 1;
  }
  return true;
}

bool UnzipTo(const Reader& f, const ZipEntry& e, const fs::path& dst, Progress* p, std::string* why,
             bool direct = false) {
  Writer w(dst, direct);
  if (!w.error().empty()) {
    *why = w.error();
    return false;
  }
  const bool ok = Unzip(
      f, e,
      [&](const uint8_t* d, size_t n) {
        if (p && p->cancel) return false;
        if (p) p->done += n;
        return w.Write(d, n);
      },
      why);
  if (!ok) {
    if (why->empty()) *why = !w.error().empty() ? w.error() : "stopped";
    return false;
  }
  if (!w.Finish()) {
    *why = w.error();
    return false;
  }
  return true;
}

// The parts of the PC release the console needs: <its folder>/<entry>.
const char* const kPortParts[] = {"native_shaders/", "pad_icons/", "Bundled Mods/"};

// Which part an entry is in (-1: none); `rel` gets its path from the part's folder on.
int PortPart(const std::string& name, std::string* rel) {
  // (the zip's files are at its top, or in one folder: "SvR2011-PC-v2.0.6/native_shaders/...")
  for (int k = 0; k < 3; ++k) {
    const std::string part = kPortParts[k];
    size_t at = name.rfind(part, 0) == 0 ? 0 : std::string::npos;
    if (at == std::string::npos) {
      const size_t slash = name.find('/');
      if (slash != std::string::npos && name.compare(slash + 1, part.size(), part) == 0) at = slash + 1;
    }
    if (at == std::string::npos || name.size() == at + part.size() || name.back() == '/') continue;
    *rel = name.substr(at);
    return k;
  }
  return -1;
}

std::string VersionIn(const std::string& name) {
  for (size_t i = 0; i + 1 < name.size(); ++i) {
    if ((name[i] == 'v' || name[i] == 'V') && std::isdigit(static_cast<unsigned char>(name[i + 1]))) {
      size_t j = i + 1;
      while (j < name.size() && (std::isdigit(static_cast<unsigned char>(name[j])) || name[j] == '.')) ++j;
      std::string v = name.substr(i + 1, j - i - 1);
      while (!v.empty() && v.back() == '.') v.pop_back();
      if (v.find('.') != std::string::npos) return v;
    }
  }
  return "";
}

// ------------------------------------------------------------------- mods

struct Manifest {
  std::string type, id, name, version, author;
};

Manifest ParseManifest(const std::string& text) {
  Manifest m;
  size_t a = 0;
  while (a < text.size()) {
    size_t b = text.find('\n', a);
    if (b == std::string::npos) b = text.size();
    std::string line = text.substr(a, b - a);
    a = b + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    auto trim = [](std::string& s) {
      s.erase(0, s.find_first_not_of(" \t\r\xEF\xBB\xBF"));
      s.erase(s.find_last_not_of(" \t\r") + 1);
    };
    trim(k), trim(v);
    if (k == "type") m.type = v;
    else if (k == "id") m.id = v;
    else if (k == "name") m.name = v;
    else if (k == "version") m.version = v;
    else if (k == "author") m.author = v;
  }
  return m;
}

Manifest ReadManifest(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return ParseManifest(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

// a > b as dotted versions ("1.10" > "1.9").
bool Newer(const std::string& a, const std::string& b) {
  const char *x = a.c_str(), *y = b.c_str();
  while (*x || *y) {
    char *xe, *ye;
    const long p = std::strtol(x, &xe, 10), q = std::strtol(y, &ye, 10);
    if (p != q) return p > q;
    x = xe, y = ye;
    if (*x == '.') ++x;
    if (*y == '.') ++y;
    if ((*x && !std::isdigit(static_cast<unsigned char>(*x))) || (*y && !std::isdigit(static_cast<unsigned char>(*y))))
      break;
  }
  return false;
}

// The Mods/ folders, one per type (as the PC launcher keeps them).
const char* const kModFolders[] = {"Arenas", "Superstars", "Signs", "Media", "Backstage", "Moves", "MatchTypes"};

}  // namespace

bool WritePng(const fs::path& file, const uint8_t* rgba, int w, int h) {
  size_t len = 0;
  void* png = tdefl_write_image_to_png_file_in_memory_ex(rgba, w, h, 4, w * 4, &len, 6, false, nullptr, 0, nullptr, 0);
  if (!png) return false;
  Writer out(file);
  const bool ok = out.Write(png, len) && out.Finish();
  mz_free(png);
  return ok;
}

std::vector<fs::directory_entry> ListDir(const fs::path& dir) {
  std::vector<fs::directory_entry> out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec))
    out.push_back(*it);
  return out;
}

std::string Bytes(uint64_t n) {
  char buf[32];
  if (n >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", double(n) / double(1ull << 30));
  else if (n >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.0f MB", double(n) / double(1ull << 20));
  else std::snprintf(buf, sizeof(buf), "%.0f KB", double(n) / 1024.0);
  return buf;
}

// ------------------------------------------------------------------- scan

namespace {

void CheckImage(DiscImage& img) {
  Disc d;
  DiscImage::State st = DiscImage::kUnchecked;
  if (!OpenDisc(img.file, &d, &st, &img.note)) {
    img.state = st;
    return;
  }
  img.state = DiscImage::kThisGame;
  for (const DiscEntry& e : d.entries)
    if (!e.dir && !Optional(e.path)) img.game_bytes += e.size, ++img.game_files;
}

bool CheckPortZip(PortFiles& pf) {
  Reader f(pf.where);
  std::vector<ZipEntry> entries;
  if (!f.ok() || !ReadZip(f, &entries)) return false;
  for (const ZipEntry& e : entries) {
    std::string rel;
    const int part = PortPart(e.name, &rel);
    if (part == 0) ++pf.shaders;
    else if (part == 1) ++pf.icons;
    else if (part == 2 && EndsWith(rel, ".svrmod")) ++pf.mods;
  }
  return pf.shaders > 0;
}

bool CheckPortFolder(PortFiles& pf) {
  std::error_code ec;
  int* counts[] = {&pf.shaders, &pf.icons, &pf.mods};
  for (int k = 0; k < 3; ++k) {
    std::string part = kPortParts[k];
    part.pop_back();
    for (const auto& e : ListDir(pf.where / part))
      if (e.is_regular_file(ec)) ++*counts[k], pf.bytes += e.file_size(ec);
  }
  return pf.shaders > 0;
}

// Folders the game itself makes or keeps: not looked in.
bool Skipped(const std::string& name) {
  static const std::set<std::string> kSkip = {
      "pac", "sound", "movies", "mods", "saves", "userdata", "logs", "hub", "native_shaders", "pad_icons",
      "dlc", "music", "custom movies", "texture packs", "savebackups", "sce_sys", "sce_module", "paint tool",
      "bundled mods", "$systemupdate", "cache", "shader_cache", "dumped textures"};
  return name.empty() || name[0] == '.' || kSkip.count(Lower(name));
}

}  // namespace

Found Scan(const fs::path& game) {
  Found found;
  std::vector<fs::path> places = {game};
#if defined(__PROSPERO__)
  // (the internal storage, and USB / extended storage drives)
  places.push_back("/data");
  for (int i = 0; i < 8; ++i) places.push_back("/mnt/usb" + std::to_string(i));
  places.push_back("/mnt/ext0");
  places.push_back("/mnt/ext1");
#endif
  std::set<std::pair<std::string, uint64_t>> seen;  // (name, size): the same file reached twice
  std::error_code ec;
  auto look = [&](const fs::path& dir, int depth, auto& self) -> void {
    std::vector<fs::path> subdirs;
    for (const auto& e : ListDir(dir)) {
      const fs::path& p = e.path();
      const std::string name = p.filename().string();
      if (e.is_directory(ec)) {
        if (Skipped(name)) continue;
        if (fs::is_directory(p / "native_shaders", ec) && !fs::equivalent(p, game, ec)) {
          PortFiles pf;
          pf.where = p;
          pf.version = VersionIn(name);
          if (CheckPortFolder(pf)) found.ports.push_back(pf);
          continue;
        }
        if (depth == 0 && subdirs.size() < 64) subdirs.push_back(p);
        continue;
      }
      if (!e.is_regular_file(ec)) continue;
      const uint64_t size = e.file_size(ec);
      if (!seen.insert({name, size}).second) continue;
      if (EndsWith(name, ".iso") || EndsWith(name, ".xiso")) {
        DiscImage img;
        img.file = p;
        img.bytes = size;
        CheckImage(img);
        found.images.push_back(std::move(img));
      } else if (EndsWith(name, ".zip")) {
        PortFiles pf;
        pf.where = p;
        pf.zip = true;
        pf.bytes = size;
        pf.version = VersionIn(name);
        if (CheckPortZip(pf)) found.ports.push_back(pf);
      } else if (EndsWith(name, ".svrmod") && (depth == 0 || Lower(p.parent_path().filename().string()) == "mods")) {
        found.mods.push_back(p);
      }
    }
    for (const fs::path& s : subdirs) self(s, depth + 1, self);
  };
  for (const fs::path& place : places) {
    if (!fs::is_directory(place, ec)) continue;
    look(place, 0, look);
  }
  // (mods dropped in Mods/ itself)
  for (const auto& e : ListDir(game / "Mods")) {
    const std::string name = e.path().filename().string();
    if (e.is_regular_file(ec) && EndsWith(name, ".svrmod") && seen.insert({name, e.file_size(ec)}).second)
      found.mods.push_back(e.path());
  }
  // the usable images first, the newest port release first
  std::stable_sort(found.images.begin(), found.images.end(),
                   [](const DiscImage& a, const DiscImage& b) { return a.state < b.state; });
  std::stable_sort(found.ports.begin(), found.ports.end(),
                   [](const PortFiles& a, const PortFiles& b) { return Newer(a.version, b.version); });
  std::sort(found.mods.begin(), found.mods.end());
  return found;
}

std::vector<fs::path> FindMods(const fs::path& game) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const fs::path& dir : {game, game / "Mods"})
    for (const auto& e : ListDir(dir))
      if (e.is_regular_file(ec) && EndsWith(e.path().filename().string(), ".svrmod")) out.push_back(e.path());
  std::sort(out.begin(), out.end());
  return out;
}

bool PeekMod(const fs::path& svrmod, ModInfo* info) {
  CrcInit();
  Reader f(svrmod);
  std::vector<ZipEntry> entries;
  if (!f.ok() || !ReadZip(f, &entries)) return false;
  for (const ZipEntry& e : entries) {
    const size_t slash = e.name.find('/');
    if (e.name != "manifest.txt" && (slash == std::string::npos || e.name.substr(slash + 1) != "manifest.txt")) continue;
    if (e.usize > 64 * 1024) return false;
    std::string text, why;
    if (!Unzip(f, e, [&](const uint8_t* d, size_t n) {
          text.append(reinterpret_cast<const char*>(d), n);
          return true;
        }, &why))
      return false;
    const Manifest m = ParseManifest(text);
    *info = {m.type, m.id, m.name, m.version, m.author};
    return !m.type.empty();
  }
  return false;
}

State Look(const fs::path& game) {
  State s;
  for (const GameFile& g : kFiles) {
    const std::string path = PathOf(g);
    if (Optional(path)) continue;
    ++s.required;
    if (FileSize(game / path) == g.size) ++s.present;
    else s.missing_bytes += g.size;
  }
  std::error_code ec;
  for (const auto& e : ListDir(game / "native_shaders"))
    if (EndsWith(e.path().filename().string(), ".pak") || EndsWith(e.path().filename().string(), ".spv")) s.shaders = true;
  s.icons = fs::exists(game / "pad_icons" / "pad_icons.txt", ec);
  std::ifstream bundled(game / "Mods" / ".bundled");
  for (std::string line; std::getline(bundled, line);) s.bundled += !line.empty();
  return s;
}

// ---------------------------------------------------------------- install

namespace {

bool Fail(Progress& p, std::string why) {
  p.ok = false;
  p.result = std::move(why);
  return false;
}

bool CopyDisc(const Plan& plan, Progress& p, int* copied) {
  Disc d;
  DiscImage::State st;
  std::string why;
  p.SetFile("Reading the disc image");
  if (!OpenDisc(plan.image, &d, &st, &why)) return Fail(p, plan.image.filename().string() + ": " + why);
  std::set<std::string> only;
  for (const std::string& o : plan.only) only.insert(Lower(o));
  std::vector<const DiscEntry*> files;
  uint64_t need = 0;
  std::error_code ec;
  for (const DiscEntry& e : d.entries) {
    if (Optional(e.path) || (e.dir && e.path.rfind("$SystemUpdate", 0) == 0)) continue;
    if (e.dir) {
      if (only.empty()) fs::create_directories(plan.game / e.path, ec);
      continue;
    }
    if (!only.empty() && !only.count(Lower(e.path))) continue;
    if (only.empty() && FileSize(plan.game / e.path) == e.size) continue;  // (there: an install that stopped goes on)
    files.push_back(&e);
    need += e.size;
  }
  const fs::space_info space = fs::space(plan.game, ec);
  if (!ec && space.available < need + (64ull << 20))
    return Fail(p, "Not enough free space: the game needs " + Bytes(need + (64ull << 20)) + " and there is " +
                       Bytes(space.available) + ".");
  std::sort(files.begin(), files.end(), [](const DiscEntry* a, const DiscEntry* b) { return a->sector < b->sector; });
  p.total = need;
  p.done = 0;
  std::vector<uint8_t> buf(kChunk);
  for (const DiscEntry* e : files) {
    p.SetFile(e->path);
    Writer w(plan.game / e->path);
    if (!w.error().empty()) return Fail(p, w.error());
    uint64_t off = uint64_t(e->sector) * 2048;
    for (uint64_t left = e->size; left;) {
      if (p.cancel) return Fail(p, "Stopped. The files copied so far are kept: install again to go on.");
      const size_t n = size_t(std::min<uint64_t>(left, kChunk));
      if (!d.Read(off, buf.data(), n)) return Fail(p, "The disc image can't be read at " + e->path + ": it may be incomplete.");
      if (!w.Write(buf.data(), n)) return Fail(p, w.error());
      off += n, left -= n;
      p.done += n;
    }
    if (!w.Finish()) return Fail(p, w.error());
    ++*copied;
  }
  return true;
}

bool CopyPort(const Plan& plan, Progress& p) {
  std::error_code ec;
  if (fs::is_directory(plan.port, ec)) {
    std::vector<std::pair<fs::path, fs::path>> copies;
    uint64_t total = 0;
    for (const char* part : kPortParts) {
      std::string name = part;
      name.pop_back();
      for (const auto& e : ListDir(plan.port / name)) {
        if (!e.is_regular_file(ec)) continue;
        const fs::path dst = plan.game / name / e.path().filename();
        // (the same file there already: by size, then content - a new release's file can be the same size)
        if (FileSize(dst) == e.file_size(ec) && FileCrc(dst) == FileCrc(e.path())) continue;
        copies.push_back({e.path(), dst});
        total += e.file_size(ec);
      }
    }
    p.total = total;
    p.done = 0;
    std::vector<uint8_t> buf(kChunk);
    for (const auto& [src, dst] : copies) {
      p.SetFile(dst.parent_path().filename().string() + "/" + dst.filename().string());
      Reader r(src);
      Writer w(dst);
      if (!r.ok() || !w.error().empty()) return Fail(p, !w.error().empty() ? w.error() : "couldn't read " + src.string());
      const uint64_t size = r.Size();
      for (uint64_t off = 0; off < size;) {
        if (p.cancel) return Fail(p, "Stopped.");
        const size_t n = size_t(std::min<uint64_t>(size - off, kChunk));
        if (!r.Read(off, buf.data(), n) || !w.Write(buf.data(), n)) return Fail(p, w.error().empty() ? "couldn't read " + src.string() : w.error());
        off += n;
        p.done += n;
      }
      if (!w.Finish()) return Fail(p, w.error());
    }
    return true;
  }
  Reader f(plan.port);
  std::vector<ZipEntry> entries;
  if (!f.ok() || !ReadZip(f, &entries)) return Fail(p, plan.port.filename().string() + " can't be read as a zip.");
  std::vector<std::pair<const ZipEntry*, std::string>> todo;
  uint64_t total = 0;
  for (const ZipEntry& e : entries) {
    std::string rel;
    if (PortPart(e.name, &rel) < 0 || !SafeName(rel)) continue;
    if (FileSize(plan.game / rel) == e.usize && FileCrc(plan.game / rel) == e.crc) continue;
    todo.push_back({&e, rel});
    total += e.usize;
  }
  p.total = total;
  p.done = 0;
  for (const auto& [e, rel] : todo) {
    p.SetFile(rel);
    std::string why;
    if (!UnzipTo(f, *e, plan.game / rel, &p, &why)) return Fail(p, p.cancel ? std::string("Stopped.") : plan.port.filename().string() + ": " + why);
  }
  return true;
}

// The port's mods (Bundled Mods/*.svrmod), as the PC launcher installs them:
// each once, and again only when its file changes - Mods/.bundled keeps
// "name|size|..." of those installed - so one the player removed stays
// removed. New ones come switched off, except match types.
void InstallBundled(const fs::path& game, Progress& p) {
  const fs::path list = game / "Mods" / ".bundled";
  std::vector<std::string> known;
  {
    std::ifstream in(list);
    for (std::string line; std::getline(in, line);)
      if (!line.empty()) known.push_back(line);
  }
  std::error_code ec;
  std::vector<fs::path> bundles;
  for (const auto& e : ListDir(game / "Bundled Mods"))
    if (e.is_regular_file(ec) && EndsWith(e.path().filename().string(), ".svrmod")) bundles.push_back(e.path());
  std::sort(bundles.begin(), bundles.end());
  p.total = bundles.size();
  p.done = 0;
  bool changed = false;
  for (const fs::path& b : bundles) {
    ++p.done;
    if (p.cancel) break;
    const std::string name = b.filename().string();
    // (2.0.5: The Hurricane is the playable manager on the M tile already)
    if (Lower(name) == "hurricane.svrmod") continue;
    // "name|size|c<crc>"; the PC launcher's lines are "name|size|<time>" (its
    // own copy of this file, by size: the same file)
    const std::string base = name + "|" + std::to_string(FileSize(b)) + "|";
    char crc[16];
    std::snprintf(crc, sizeof(crc), "c%08x", FileCrc(b));
    const std::string key = base + crc;
    bool same = false, fresh = true;
    for (const std::string& k : known) {
      if (k == key || (k.rfind(base, 0) == 0 && k.size() > base.size() && k[base.size()] != 'c')) same = true;
      if (k.rfind(name + "|", 0) == 0) fresh = false;
    }
    if (same) continue;
    p.SetFile(name);
    std::string msg;
    fs::path folder;
    bool existed_before = false;
    if (!InstallMod(game, b, true, &msg, &folder, &existed_before)) continue;
    ++p.mods_installed;
    if (fresh && !existed_before && folder.parent_path().filename() != "MatchTypes" && !fs::exists(folder / "disabled", ec))
      std::ofstream(folder / "disabled") << "";
    known.erase(std::remove_if(known.begin(), known.end(), [&](const std::string& k) { return k.rfind(name + "|", 0) == 0; }),
                known.end());
    known.push_back(key);
    changed = true;
  }
  if (changed) {
    fs::create_directories(list.parent_path(), ec);
    std::ofstream out(list, std::ios::trunc);
    for (const std::string& k : known) out << k << "\n";
  }
}

void MakeFolders(const fs::path& game) {
  std::error_code ec;
  for (const char* m : kModFolders) fs::create_directories(game / "Mods" / m, ec);
  for (const char* d : {"DLC", "Music", "Custom Movies", "Texture Packs", "Paint Tool", "Saves", "UserData"})
    fs::create_directories(game / d, ec);
}

}  // namespace

void Install(const Plan& plan, Progress& p) {
  CrcInit();
  p.ok = true;
  int copied = 0;
  bool good = true;
  if (!plan.image.empty()) {
    p.step = 0;
    good = CopyDisc(plan, p, &copied);
  }
  if (good && !plan.port.empty() && plan.only.empty()) {
    p.step = 1;
    good = CopyPort(plan, p);
  }
  if (good && plan.only.empty()) {
    p.step = 2;
    InstallBundled(plan.game, p);
    if (p.cancel) good = Fail(p, "Stopped.");
  }
  if (good) {
    p.step = 3;
    MakeFolders(plan.game);
    p.ok = true;
    if (!plan.only.empty()) {
      p.result = std::to_string(copied) + " game file" + (copied == 1 ? "" : "s") + " copied again from the disc image.";
    } else {
      const State s = Look(plan.game);
      p.result = s.Installed() ? "The game is installed." : "Done, but " + std::to_string(s.required - s.present) +
                                                              " game files are still missing.";
      if (!s.shaders) p.result += " The PC port's shaders are still needed (its release zip).";
    }
  }
  p.SetFile("");
  p.finished = true;
}

bool BundledPending(const fs::path& game) {
  std::vector<std::string> known;
  {
    std::ifstream in(game / "Mods" / ".bundled");
    for (std::string line; std::getline(in, line);)
      if (!line.empty()) known.push_back(line);
  }
  std::error_code ec;
  for (const auto& e : ListDir(game / "Bundled Mods")) {
    const std::string name = e.path().filename().string();
    if (!e.is_regular_file(ec) || !EndsWith(name, ".svrmod") || Lower(name) == "hurricane.svrmod") continue;
    const std::string base = name + "|" + std::to_string(FileSize(e.path())) + "|";
    if (std::none_of(known.begin(), known.end(), [&](const std::string& k) { return k.rfind(base, 0) == 0; }))
      return true;
  }
  return false;
}

void InstallBundledMods(const fs::path& game, Progress& p) {
  CrcInit();
  p.ok = true;
  p.step = 2;
  InstallBundled(game, p);
  MakeFolders(game);
  p.result = std::to_string(p.mods_installed) + (p.mods_installed == 1 ? " bundled mod" : " bundled mods") +
             " set up.";
  p.SetFile("");
  p.finished = true;
}

void Verify(const fs::path& game, Progress& p) {
  CrcInit();
  uint64_t total = 0;
  for (const GameFile& g : kFiles) total += g.size;
  p.total = total;
  p.done = 0;
  std::vector<uint8_t> buf(4u << 20);
  int present = 0;
  bool stopped = false;
  for (const GameFile& g : kFiles) {
    const std::string path = PathOf(g);
    Reader f(game / path);
    if (!f.ok()) {
      if (!Optional(path)) ++p.checked, p.missing.push_back(path);
      p.done += g.size;
      continue;
    }
    ++p.checked, ++present;
    p.SetFile(path);
    const uint64_t size = f.Size();
    if (size != g.size) {
      if (!Optional(path)) p.damaged.push_back(path);
      p.done += g.size;
      continue;
    }
    uint32_t c = 0;
    for (uint64_t at = 0; at < size;) {
      if (p.cancel) {
        stopped = true;
        break;
      }
      const size_t n = size_t(std::min<uint64_t>(size - at, buf.size()));
      if (!f.Read(at, buf.data(), n)) break;
      if (g.flags & VERIFY_MENU_TABLE) {  // (the menu table's bytes are left out)
        const uint64_t lo = kMenuTableAt, hi = kMenuTableAt + kMenuTableSize, a = at, b = at + n;
        if (b <= lo || a >= hi) {
          c = Crc(c, buf.data(), n);
        } else {
          if (a < lo) c = Crc(c, buf.data(), size_t(lo - a));
          if (b > hi) c = Crc(c, buf.data() + (hi - a), size_t(b - hi));
        }
      } else {
        c = Crc(c, buf.data(), n);
      }
      at += n;
      p.done += n;
    }
    if (stopped) break;
    if (c != g.crc && (!g.crc_ported || c != g.crc_ported) && !Optional(path)) p.damaged.push_back(path);
  }
  // most of the files different: another edition of the game, not damage
  p.other_edition = present >= 20 && int(p.damaged.size()) > present / 2;
  p.ok = !stopped;
  if (stopped) p.result = "The check was stopped.";
  else if (p.other_edition) p.result = "Most game files differ from the ones the hub knows: another edition of the game.";
  else if (p.damaged.empty() && p.missing.empty()) p.result = "All " + std::to_string(p.checked) + " game files are OK.";
  else
    p.result = std::to_string(p.damaged.size()) + " damaged and " + std::to_string(p.missing.size()) +
               " missing game file" + (p.damaged.size() + p.missing.size() == 1 ? "" : "s") + ".";
  p.SetFile("");
  p.finished = true;
}

bool InstallMod(const fs::path& game, const fs::path& svrmod, bool bundled, std::string* message, fs::path* folder,
                bool* existed) {
  CrcInit();
  Reader f(svrmod);
  std::vector<ZipEntry> entries;
  if (!f.ok() || !ReadZip(f, &entries)) {
    *message = svrmod.filename().string() + " isn't a mod (it can't be unpacked).";
    return false;
  }
  // its files are at the zip's top, or in one folder
  std::string prefix;
  const ZipEntry* manifest = nullptr;
  for (const ZipEntry& e : entries) {
    const size_t slash = e.name.find('/');
    if (e.name == "manifest.txt" || (slash != std::string::npos && e.name.substr(slash + 1) == "manifest.txt" && !manifest)) {
      manifest = &e;
      prefix = e.name == "manifest.txt" ? "" : e.name.substr(0, slash + 1);
      if (prefix.empty()) break;
    }
  }
  std::string text, why;
  if (!manifest || !Unzip(f, *manifest, [&](const uint8_t* d, size_t n) {
        text.append(reinterpret_cast<const char*>(d), n);
        return true;
      }, &why)) {
    *message = svrmod.filename().string() + " has no manifest.txt: it isn't an SvR 2011 mod.";
    return false;
  }
  Manifest m = ParseManifest(text);
  if (m.type.empty() || m.id.empty()) {
    *message = svrmod.filename().string() + "'s manifest.txt names no type and id.";
    return false;
  }
  auto has = [&](const char* name) {
    for (const ZipEntry& e : entries)
      if (Lower(e.name) == Lower(prefix + name)) return true;
    return false;
  };
  const std::string type = Lower(m.type);
  std::string kind;
  if (type == "signs") kind = "Signs";
  else if (type == "media") kind = "Media";
  else if (type == "matchtype") kind = "MatchTypes";
  else if (type == "moves") {
    if (!has("pack.txt")) {
      *message = "That move pack is incomplete (it needs pack.txt).";
      return false;
    }
    kind = "Moves";
  } else if (type == "arena" || type == "backstage" || type == "superstar") {
    const bool arena = type != "superstar", backstage = type == "backstage";
    if (!has(arena ? "arena.pac" : "ch.pac") || (arena && !backstage && !has("banner.dds"))) {
      *message = arena ? "That arena mod is incomplete (it needs arena.pac and banner.dds)."
                       : "That superstar mod is incomplete (it needs ch.pac).";
      return false;
    }
    kind = backstage ? "Backstage" : arena ? "Arenas" : "Superstars";
  } else {
    *message = "\"" + (m.name.empty() ? m.id : m.name) + "\" is a " + m.type + " mod, a kind this version can't install.";
    return false;
  }
  std::string id = m.id;
  for (char& c : id)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) c = '_';
  if (id[0] == '.') id[0] = '_';  // (never ".", ".." or hidden: the folder is replaced)
  std::error_code ec;
  const fs::path dst = game / "Mods" / kind / id, tmp = game / "Mods" / ".incoming";
  if (folder) *folder = dst;
  if (existed) *existed = fs::exists(dst, ec);
  if (bundled && fs::exists(dst / "manifest.txt", ec) && Newer(ReadManifest(dst / "manifest.txt").version, m.version))
    return true;  // (the player's copy is newer than the port's: kept)
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp, ec);
  for (const ZipEntry& e : entries) {
    if (e.name.empty() || e.name.back() == '/' || e.name.compare(0, prefix.size(), prefix)) continue;
    const std::string rel = e.name.substr(prefix.size());
    if (!SafeName(rel)) continue;
    if (!UnzipTo(f, e, tmp / rel, nullptr, &why, true)) {  // (.incoming is moved into place whole)
      fs::remove_all(tmp, ec);
      *message = svrmod.filename().string() + ": " + why;
      return false;
    }
  }
  const bool was_off = fs::exists(dst / "disabled", ec);  // (a new version keeps the old one's on / off)
  fs::remove_all(dst, ec);
  fs::create_directories(dst.parent_path(), ec);
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove_all(tmp, ec);
    *message = "The mod couldn't be put in Mods/" + kind + ".";
    return false;
  }
  if (was_off) std::ofstream(dst / "disabled") << "";
  const std::string shown = m.name.empty() ? m.id : m.name;
  *message = kind == "Arenas"       ? shown + " is installed: it's on the arena select pages."
             : kind == "Superstars" ? shown + " is installed: under the M tile of the character select."
             : kind == "MatchTypes" ? shown + " is installed: it's in the game's match types."
                                    : shown + " is installed.";
  return true;
}

}  // namespace svr2011::hub::install
