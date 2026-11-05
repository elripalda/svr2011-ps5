// [ps5 port] An .mp4's H.264 video decoded on the CPU, for hd_movies.cpp: Windows and Android
// have the system's decoder, the PS5 has none a homebrew app can use - mods' entrance movies
// (movie.mp4, often with no .bik beside it) played the base's movie. The H.264 decoder is
// FFmpeg's, in the runtime's libavcodec (its PS5 config); the .mp4 is read here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace svr2011::ps5 {

// One decoded picture: 8-bit planes Y, Cb, Cr; the chroma planes are the size
// shifted right by chroma_x / chroma_y (4:2:0: 1, 1).
struct VideoPicture {
  const uint8_t* plane[3] = {};
  int pitch[3] = {};
  int width = 0, height = 0;
  int chroma_x = 1, chroma_y = 1;
  double time = 0;  // seconds from the first picture shown
};

class Mp4Video {
 public:
  Mp4Video();
  ~Mp4Video();
  Mp4Video(const Mp4Video&) = delete;
  Mp4Video& operator=(const Mp4Video&) = delete;

  // The first H.264 video track. false (and why): no such track, or no decoder.
  bool Open(const std::filesystem::path& mp4, std::string* why = nullptr);
  double Seconds() const;  // its length
  // The next picture in display order (valid until the next call). false: the end, or the file stopped decoding.
  bool Next(VideoPicture* out);
  void Rewind();  // from the first picture again

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace svr2011::ps5
