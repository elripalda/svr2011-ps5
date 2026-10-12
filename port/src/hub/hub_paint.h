// WWE SmackDown vs. Raw 2011 - the PS5 hub's Paint Tool page: the logos of
// CREATE A SUPERSTAR > PAINT TOOL (10 pages of 20), as the PC launcher's Paint
// Tool tab reads and writes them (launcher/svr2011_launcher.c, "Paint Tool").
//
// Page 1 is Saves/00PaintTool.pt (the game's file); pages 2-10 are the
// port's, Saves/.paint/pNN_sMM.bin (src/paint_pages.h).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svr2011::hub::paint {

constexpr int kPages = 10, kSlots = 20, kSize = 256;

class Page {
 public:
  // Reads page `page` (0-based) of the saves folder; false (and why) if it can't.
  bool Read(const std::filesystem::path& saves, int page, std::string* why);
  bool Write(std::string* why);  // (checksums fixed)
  bool Used(int k) const;
  // Logo k as 256 x 256 RGBA.
  void Get(int k, uint8_t* rgba) const;
  // Logo k from 256 x 256 RGBA, as the game would have saved it.
  void Put(int k, const uint8_t* rgba);
  void Clear(int k);
  // When logo k was saved ("" if not known).
  std::string Saved(int k) const;
  int UsedCount() const;

 private:
  std::filesystem::path saves_;
  int page_ = 0;
  std::vector<uint8_t> f_;  // the page as a whole Paint Tool file
};

// An image file (PNG, JPEG, BMP, TGA, GIF) fitted into 256 x 256 RGBA
// (its shape kept, centred, transparent around it).
bool LoadImage(const std::filesystem::path& file, std::vector<uint8_t>* rgba, std::string* why);

}  // namespace svr2011::hub::paint
