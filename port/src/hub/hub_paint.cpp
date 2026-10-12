// WWE SmackDown vs. Raw 2011 - the PS5 hub's Paint Tool logos (see
// hub_paint.h). The file format, as reverse-engineered for the PC launcher
// (the game's loader sub_827B3630, its checksums sub_827B35C8 / sub_827B3118).
// Big-endian:
//   0        u32 magic 0x02A78E9A, u32 version 3
//   8+k*S    slot k (S = 0x604CC); offsets below are from k*S:
//     8..51      header (+36 used, +44 / +48 width / height = 256)
//     52         the logo: 256 x 256 pixels, A R G B bytes (the editor's canvas)
//     0x40034    the same as an 8-bit colour-mapped TGA (20-byte header,
//                256 A R G B palette entries, 65536 indices, top-down)
//     0x50448    the same as a DXT5 DDS (128-byte header, 64 KB), what the game shows
//     0x604C8    saved at: u16 year, month, day, hour, minute, second, weekday+1 (UTC)
//     0x604D0    u32 checksum: a sum of the slot's fields by type (SlotSum)
//   end-4    u32 magic + version + the 20 slot checksums
// The game refuses the whole file when a checksum is off.
#include "hub_paint.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#include "stb_image.h"

namespace svr2011::hub::paint {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kSlot = 0x604CCu, kBytes = 8u + kSlots * kSlot + 4u, kMagic = 0x02A78E9Au;
constexpr uint32_t kCanvas = 52u, kTga = 0x40034u, kDds = 0x50448u, kStamp = 0x604C8u, kSum = 0x604D0u;

const uint8_t kUsedHeader[44] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x01, 0x00, 0x00,
                                 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00};
const uint8_t kEmptyHeader[44] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
const uint8_t kTgaHeader[20] = {0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00,
                                0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x08, 0x00, 0x04, 0x01};
uint8_t g_dds_header[128];

const uint8_t* DdsHeader() {
  if (!g_dds_header[0]) {
    const uint8_t start[24] = {0x44, 0x44, 0x53, 0x20, 0x7C, 0x00, 0x00, 0x00, 0x07, 0x10, 0x08, 0x00,
                               0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    std::memcpy(g_dds_header, start, sizeof(start));
    g_dds_header[76] = 0x20;
    g_dds_header[80] = 0x04;
    std::memcpy(g_dds_header + 84, "DXT5", 4);
    g_dds_header[108] = 0x02;
    g_dds_header[109] = 0x10;
  }
  return g_dds_header;
}

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

uint8_t* Slot(uint8_t* f, int k) { return f + size_t(k) * kSlot; }
const uint8_t* Slot(const uint8_t* f, int k) { return f + size_t(k) * kSlot; }

// The game's slot checksum (sub_827B3118): every field of the slot, added as
// the type the game reads it as (signed bytes at +20..27).
uint32_t SlotSum(const uint8_t* f, int k) {
  const uint8_t *b = Slot(f, k), *t = b + kTga, *d = b + kDds, *e = b + kStamp;
  static const int words[] = {8, 12, 16, 28, 32, 36, 40, 44, 48};
  static const int tb[] = {0, 1, 2, 7, 16, 17}, th[] = {3, 5, 8, 10, 12, 14, 18};
  uint32_t s = 0;
  for (int i = 20; i < 28; ++i) s += uint32_t(int32_t(int8_t(b[i])));
  for (int w : words) s += Rd32(b + w);
  for (int i = 0; i < kSize * kSize; ++i) s += Rd32(b + kCanvas + 4 * i);
  for (int i : tb) s += t[i];
  for (int i : th) s += Rd16(t + i);
  for (int i = 0; i < 256; ++i) s += Rd32(t + 20 + 4 * i);
  for (int i = 0; i < kSize * kSize; ++i) s += t[0x414 + i];
  for (int i = 0; i < 32; ++i) s += Rd32(d + 4 * i);
  for (int j = 0; j < 2048; ++j) {
    const uint8_t* q = d + 128 + 32 * j;
    s += Rd16(q) + Rd16(q + 2) + Rd32(q + 4) + Rd16(q + 8) + Rd16(q + 10) + Rd32(q + 12) + Rd16(q + 16) +
         Rd16(q + 18) + Rd32(q + 20) + Rd16(q + 24) + Rd16(q + 26) + Rd32(q + 28);
  }
  s += Rd16(e);
  for (int i = 2; i < 8; ++i) s += e[i];
  return s;
}

void FixSums(uint8_t* f) {
  uint32_t total = Rd32(f) + Rd32(f + 4);
  for (int k = 0; k < kSlots; ++k) {
    const uint32_t s = SlotSum(f, k);
    Wr32(Slot(f, k) + kSum, s);
    total += s;
  }
  Wr32(f + kBytes - 4, total);
}

bool Valid(const uint8_t* f) {
  if (Rd32(f) != kMagic || Rd32(f + 4) != 3) return false;
  uint32_t total = Rd32(f) + Rd32(f + 4);
  for (int k = 0; k < kSlots; ++k) total += Rd32(Slot(f, k) + kSum);
  return total == Rd32(f + kBytes - 4);
}

bool UsedIn(const uint8_t* f, int k) { return Rd32(Slot(f, k) + 36) != 0; }

void ClearIn(uint8_t* f, int k) {
  uint8_t* b = Slot(f, k);
  std::memset(b + 8, 0, kStamp - 8);
  std::memcpy(b + 8, kEmptyHeader, 44);
}

fs::path PageFile(const fs::path& saves, int page, int k) {
  char name[32];
  std::snprintf(name, sizeof(name), "p%02d_s%02d.bin", page + 1, k + 1);
  return saves / ".paint" / name;
}

bool WriteFile(const fs::path& file, const uint8_t* data, size_t n) {
  const fs::path tmp = file.string() + ".new";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.write(reinterpret_cast<const char*>(data), std::streamsize(n))) return false;
    out.close();
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

// ---- the slot's three copies of the logo

// A 256-colour palette by median cut over the image's colours (A R G B), and
// each pixel's nearest entry.
void MakeTga(uint8_t* t, const uint32_t* argb) {
  struct Color {
    uint32_t argb, count;
  };
  struct Box {
    int lo, hi;
  };
  std::vector<Color> c(kSize * kSize);
  for (int i = 0; i < kSize * kSize; ++i) c[size_t(i)] = {argb[i], 1};
  std::sort(c.begin(), c.end(), [](const Color& a, const Color& b) { return a.argb < b.argb; });
  int n = 0;
  for (int i = 0; i < kSize * kSize; ++i) {  // unique colours with counts
    if (n && c[size_t(n) - 1].argb == c[size_t(i)].argb) ++c[size_t(n) - 1].count;
    else c[size_t(n++)] = c[size_t(i)];
  }
  Box box[256];
  int nb = 1;
  box[0] = {0, n};
  while (nb < 256) {  // split the widest box
    int best = -1, bestw = 0, bestch = 0;
    for (int i = 0; i < nb; ++i) {
      if (box[i].hi - box[i].lo < 2) continue;
      for (int j = 0; j < 32; j += 8) {
        int lo = 255, hi = 0;
        for (int k = box[i].lo; k < box[i].hi; ++k) {
          const int v = int(c[size_t(k)].argb >> j) & 0xFF;
          lo = std::min(lo, v), hi = std::max(hi, v);
        }
        if (hi - lo > bestw) bestw = hi - lo, best = i, bestch = j;
      }
    }
    if (best < 0) break;
    std::sort(c.begin() + box[best].lo, c.begin() + box[best].hi,
              [bestch](const Color& a, const Color& b) { return ((a.argb >> bestch) & 0xFF) < ((b.argb >> bestch) & 0xFF); });
    uint64_t total = 0, acc = 0;
    int mid = -1;
    for (int k = box[best].lo; k < box[best].hi; ++k) total += c[size_t(k)].count;
    for (int k = box[best].lo; k < box[best].hi - 1; ++k) {  // at the median by pixel count
      acc += c[size_t(k)].count;
      if (acc * 2 >= total) {
        mid = k + 1;
        break;
      }
    }
    if (mid < 0) mid = box[best].hi - 1;
    box[nb] = {mid, box[best].hi};
    box[best].hi = mid;
    ++nb;
  }
  uint32_t pal[256] = {};
  for (int i = 0; i < nb; ++i) {  // each box's average
    uint64_t s[4] = {}, w = 0;
    for (int k = box[i].lo; k < box[i].hi; ++k) {
      for (int j = 0; j < 4; ++j) s[j] += uint64_t((c[size_t(k)].argb >> (8 * j)) & 0xFF) * c[size_t(k)].count;
      w += c[size_t(k)].count;
    }
    if (w) pal[i] = uint32_t((s[3] / w) << 24 | (s[2] / w) << 16 | (s[1] / w) << 8 | (s[0] / w));
  }
  std::memcpy(t, kTgaHeader, 20);
  for (int i = 0; i < 256; ++i) Wr32(t + 20 + 4 * i, pal[i]);
  for (int i = 0; i < kSize * kSize; ++i) {  // nearest palette entry
    const uint32_t p = argb[i];
    int best = 0;
    uint32_t bd = 0xFFFFFFFF;
    for (int j = 0; j < nb; ++j) {
      const int da = int(p >> 24) - int(pal[j] >> 24), dr = int((p >> 16) & 0xFF) - int((pal[j] >> 16) & 0xFF),
                dg = int((p >> 8) & 0xFF) - int((pal[j] >> 8) & 0xFF), db = int(p & 0xFF) - int(pal[j] & 0xFF);
      const uint32_t dd = uint32_t(da * da + dr * dr + dg * dg + db * db);
      if (dd < bd) {
        bd = dd, best = j;
        if (!dd) break;
      }
    }
    t[0x414 + i] = uint8_t(best);
  }
}

uint16_t To565(int r, int g, int b) {
  return uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}
void From565(uint16_t c, int* r, int* g, int* b) {
  *r = ((c >> 11) & 31) * 255 / 31, *g = ((c >> 5) & 63) * 255 / 63, *b = (c & 31) * 255 / 31;
}

// DXT5, block by block (little-endian, as a PC DDS).
void MakeDds(uint8_t* d, const uint32_t* argb) {
  uint8_t* o = d + 128;
  std::memcpy(d, DdsHeader(), 128);
  for (int by = 0; by < kSize / 4; ++by)
    for (int bx = 0; bx < kSize / 4; ++bx, o += 16) {
      uint32_t px[16];
      int amin = 255, amax = 0;
      for (int i = 0; i < 16; ++i) {
        const uint32_t p = argb[(by * 4 + i / 4) * kSize + bx * 4 + i % 4];
        const int a = int(p >> 24);
        px[i] = p;
        amin = std::min(amin, a), amax = std::max(amax, a);
      }
      // alpha: 8 steps between max and min
      uint8_t apal[8];
      apal[0] = uint8_t(amax), apal[1] = uint8_t(amin);
      for (int i = 1; i < 7; ++i) apal[i + 1] = uint8_t(((7 - i) * amax + i * amin) / 7);
      uint64_t abits = 0;
      for (int i = 0; i < 16; ++i) {
        const int a = int(px[i] >> 24);
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; ++k)
          if (std::abs(a - apal[k]) < bd) bd = std::abs(a - apal[k]), best = k;
        abits |= uint64_t(best) << (3 * i);
      }
      o[0] = uint8_t(amax), o[1] = uint8_t(amin);
      for (int i = 0; i < 6; ++i) o[2 + i] = uint8_t(abits >> (8 * i));
      // colour: the block's two most different pixels, four-colour mode
      int best = -1, pa = 0, pb = 0;
      for (int a2 = 0; a2 < 16; ++a2)
        for (int b2 = a2 + 1; b2 < 16; ++b2) {
          const int dr = int((px[a2] >> 16) & 0xFF) - int((px[b2] >> 16) & 0xFF),
                    dg = int((px[a2] >> 8) & 0xFF) - int((px[b2] >> 8) & 0xFF), db = int(px[a2] & 0xFF) - int(px[b2] & 0xFF);
          const int dd = dr * dr + dg * dg + db * db;
          if (dd > best) best = dd, pa = a2, pb = b2;
        }
      uint16_t c0 = To565(int(px[pa] >> 16) & 0xFF, int(px[pa] >> 8) & 0xFF, int(px[pa]) & 0xFF);
      uint16_t c1 = To565(int(px[pb] >> 16) & 0xFF, int(px[pb] >> 8) & 0xFF, int(px[pb]) & 0xFF);
      if (c0 < c1) std::swap(c0, c1);
      uint32_t cbits = 0;
      if (c0 != c1) {
        int cr[4], cg[4], cb[4];
        From565(c0, &cr[0], &cg[0], &cb[0]);
        From565(c1, &cr[1], &cg[1], &cb[1]);
        cr[2] = (2 * cr[0] + cr[1]) / 3, cg[2] = (2 * cg[0] + cg[1]) / 3, cb[2] = (2 * cb[0] + cb[1]) / 3;
        cr[3] = (cr[0] + 2 * cr[1]) / 3, cg[3] = (cg[0] + 2 * cg[1]) / 3, cb[3] = (cb[0] + 2 * cb[1]) / 3;
        for (int i = 0; i < 16; ++i) {
          const int r = int(px[i] >> 16) & 0xFF, g = int(px[i] >> 8) & 0xFF, b = int(px[i]) & 0xFF;
          int bi = 0, bd = 1 << 30;
          for (int k = 0; k < 4; ++k) {
            const int dd = (r - cr[k]) * (r - cr[k]) + (g - cg[k]) * (g - cg[k]) + (b - cb[k]) * (b - cb[k]);
            if (dd < bd) bd = dd, bi = k;
          }
          cbits |= uint32_t(bi) << (2 * i);
        }
      }
      o[8] = uint8_t(c0), o[9] = uint8_t(c0 >> 8), o[10] = uint8_t(c1), o[11] = uint8_t(c1 >> 8);
      for (int i = 0; i < 4; ++i) o[12 + i] = uint8_t(cbits >> (8 * i));
    }
}

}  // namespace

bool Page::Read(const fs::path& saves, int page, std::string* why) {
  saves_ = saves;
  page_ = page;
  f_.assign(kBytes, 0);
  const fs::path pt = saves / "00PaintTool.pt";
  std::error_code ec;
  if (!fs::exists(pt, ec)) {
    *why = "There are no Paint Tool logos yet. Open Create Mode > Create A Superstar > Paint Tool in the game once, "
           "then come back.";
    f_.clear();
    return false;
  }
  if (page == 0) {
    std::ifstream in(pt, std::ios::binary);
    in.read(reinterpret_cast<char*>(f_.data()), std::streamsize(kBytes));
    if (in.gcount() != std::streamsize(kBytes) || in.peek() != EOF || !Valid(f_.data())) {
      *why = "The Paint Tool save (00PaintTool.pt) isn't one the game accepts.";
      f_.clear();
      return false;
    }
    return true;
  }
  // pages 2-10: one file per used slot (its raw bytes, as at 8 + k * kSlot)
  Wr32(f_.data(), kMagic);
  Wr32(f_.data() + 4, 3);
  for (int k = 0; k < kSlots; ++k) {
    std::ifstream in(PageFile(saves, page, k), std::ios::binary);
    std::streamsize got = 0;
    if (in) {
      in.read(reinterpret_cast<char*>(Slot(f_.data(), k) + 8), kSlot);
      got = in.gcount();
    }
    if (got != std::streamsize(kSlot) || !UsedIn(f_.data(), k)) ClearIn(f_.data(), k);
  }
  FixSums(f_.data());
  return true;
}

bool Page::Write(std::string* why) {
  if (f_.size() != kBytes) return false;
  FixSums(f_.data());
  std::error_code ec;
  if (page_ == 0) {
    if (!WriteFile(saves_ / "00PaintTool.pt", f_.data(), f_.size())) {
      *why = "The Paint Tool save couldn't be written.";
      return false;
    }
    return true;
  }
  fs::create_directories(saves_ / ".paint", ec);
  for (int k = 0; k < kSlots; ++k) {
    const fs::path file = PageFile(saves_, page_, k);
    if (!UsedIn(f_.data(), k)) {
      fs::remove(file, ec);
      continue;
    }
    if (!WriteFile(file, Slot(f_.data(), k) + 8, kSlot)) {
      *why = "The logo couldn't be written (" + file.filename().string() + ").";
      return false;
    }
  }
  return true;
}

bool Page::Used(int k) const { return f_.size() == kBytes && UsedIn(f_.data(), k); }

int Page::UsedCount() const {
  int n = 0;
  for (int k = 0; k < kSlots; ++k) n += Used(k);
  return n;
}

// The canvas (full colour), unless it isn't the picture the game shows - then
// its 8-bit copy (a slot can hold another logo's canvas: seen on page 2).
void Page::Get(int k, uint8_t* rgba) const {
  const uint8_t *b = Slot(f_.data(), k), *c = b + kCanvas;
  const uint8_t *pal = b + kTga + 20, *idx = b + kTga + 0x414;
  int differ = 0;
  for (int i = 0; i < kSize * kSize; ++i)
    if (std::memcmp(c + 4 * i, pal + 4 * idx[i], 4) && ++differ > kSize * kSize / 16) {
      c = nullptr;
      break;
    }
  for (int i = 0; i < kSize * kSize; ++i) {
    const uint8_t* p = c ? c + 4 * i : pal + 4 * idx[i];  // (A R G B)
    rgba[4 * i] = p[1], rgba[4 * i + 1] = p[2], rgba[4 * i + 2] = p[3], rgba[4 * i + 3] = p[0];
  }
}

void Page::Put(int k, const uint8_t* rgba) {
  uint8_t* b = Slot(f_.data(), k);
  std::vector<uint32_t> argb(kSize * kSize);
  std::memcpy(b + 8, kUsedHeader, 44);
  for (int i = 0; i < kSize * kSize; ++i) {
    const uint8_t* p = rgba + 4 * i;
    argb[size_t(i)] = uint32_t(p[3]) << 24 | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
    Wr32(b + kCanvas + 4 * i, argb[size_t(i)]);
  }
  MakeTga(b + kTga, argb.data());
  MakeDds(b + kDds, argb.data());
  const std::time_t now = std::time(nullptr);
  std::tm t{};
  gmtime_r(&now, &t);
  const int year = t.tm_year + 1900;
  b[kStamp] = uint8_t(year >> 8), b[kStamp + 1] = uint8_t(year);
  b[kStamp + 2] = uint8_t(t.tm_mon + 1), b[kStamp + 3] = uint8_t(t.tm_mday);
  b[kStamp + 4] = uint8_t(t.tm_hour), b[kStamp + 5] = uint8_t(t.tm_min);
  b[kStamp + 6] = uint8_t(t.tm_sec), b[kStamp + 7] = uint8_t(t.tm_wday + 1);
}

void Page::Clear(int k) { ClearIn(f_.data(), k); }

std::string Page::Saved(int k) const {
  if (!Used(k)) return "";
  const uint8_t* e = Slot(f_.data(), k) + kStamp;
  const int year = int(Rd16(e));
  if (year < 2000 || e[2] < 1 || e[2] > 12) return "";
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%d %s %d", e[3], kMonths[e[2] - 1], year);
  return buf;
}

bool LoadImage(const fs::path& file, std::vector<uint8_t>* rgba, std::string* why) {
  int w = 0, h = 0, n = 0;
  unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &n, 4);
  if (!px || w <= 0 || h <= 0) {
    if (px) stbi_image_free(px);
    *why = file.filename().string() + " can't be read as a picture (PNG, JPEG, BMP, TGA or GIF).";
    return false;
  }
  // fitted into 256 x 256, its shape kept
  int sw, sh;
  if (w >= h) sw = kSize, sh = std::max(1, int((int64_t(h) * kSize + w / 2) / w));
  else sh = kSize, sw = std::max(1, int((int64_t(w) * kSize + h / 2) / h));
  const int x0 = (kSize - sw) / 2, y0 = (kSize - sh) / 2;
  rgba->assign(size_t(kSize) * kSize * 4, 0);
  // each pixel: the average of the source pixels under it (alpha-weighted, so
  // no dark edges), or a bilinear sample when the picture is smaller
  const float fx = float(w) / float(sw), fy = float(h) / float(sh);
  auto src = [&](int x, int y) { return px + (size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)) * 4; };
  for (int y = 0; y < sh; ++y)
    for (int x = 0; x < sw; ++x) {
      float acc[4] = {}, wsum = 0;
      if (fx > 1 || fy > 1) {
        const int ax = int(x * fx), bx = std::max(ax + 1, int((x + 1) * fx));
        const int ay = int(y * fy), by = std::max(ay + 1, int((y + 1) * fy));
        for (int yy = ay; yy < by; ++yy)
          for (int xx = ax; xx < bx; ++xx) {
            const unsigned char* p = src(xx, yy);
            const float a = p[3] / 255.0f;
            for (int c = 0; c < 3; ++c) acc[c] += p[c] * a;
            acc[3] += a;
            wsum += 1;
          }
      } else {
        const float sx = (x + 0.5f) * fx - 0.5f, sy = (y + 0.5f) * fy - 0.5f;
        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
        const float tx = sx - ix, ty = sy - iy;
        const float wts[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
        const unsigned char* ps[4] = {src(ix, iy), src(ix + 1, iy), src(ix, iy + 1), src(ix + 1, iy + 1)};
        for (int k = 0; k < 4; ++k) {
          const float a = ps[k][3] / 255.0f * wts[k];
          for (int c = 0; c < 3; ++c) acc[c] += ps[k][c] * a;
          acc[3] += a;
          wsum += wts[k];
        }
      }
      uint8_t* o = rgba->data() + (size_t(y0 + y) * kSize + x0 + x) * 4;
      const float alpha = wsum > 0 ? acc[3] / wsum : 0;
      for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(acc[3] > 0 ? acc[c] / acc[3] : 0.0f, 0.0f, 255.0f) + 0.5f);
      o[3] = uint8_t(std::clamp(alpha * 255.0f, 0.0f, 255.0f) + 0.5f);
    }
  stbi_image_free(px);
  return true;
}

}  // namespace svr2011::hub::paint
