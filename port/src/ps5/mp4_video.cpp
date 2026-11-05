// [ps5 port] An .mp4's H.264 video on the CPU (mp4_video.h).
//
// The file: its boxes read here - moov / trak (the first video track holding
// avc1 / avc3 with avcC) / mdia / mdhd (time scale, length) / minf / stbl:
// stts (decode times), ctts (display offsets), stsc + stco / co64 (chunks),
// stsz (sizes), stss (key pictures). Its samples go to FFmpeg's H.264 decoder
// in file order (length-prefixed NAL units, avcC as the extradata) and the
// pictures come back in display order; a picture's time is its display time
// less the first one's (an edit list's start: the B-frame delay).
#include "mp4_video.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace svr2011::ps5 {
namespace {

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
constexpr uint32_t Tag(const char (&t)[5]) {
  return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 | uint8_t(t[3]);
}

struct Box {
  uint32_t type;
  size_t begin, end;  // its payload
};

// The boxes in d[begin, end).
std::vector<Box> Boxes(const std::vector<uint8_t>& d, size_t begin, size_t end) {
  std::vector<Box> out;
  for (size_t at = begin; at + 8 <= end;) {
    uint64_t size = Be32(&d[at]);
    const uint32_t type = Be32(&d[at + 4]);
    size_t head = 8;
    if (size == 1) {
      if (at + 16 > end) break;
      size = Be64(&d[at + 8]), head = 16;
    } else if (size == 0) {
      size = end - at;
    }
    if (size < head || size > end - at) break;
    out.push_back({type, at + head, at + size_t(size)});
    at += size_t(size);
  }
  return out;
}

const Box* Find(const std::vector<Box>& boxes, uint32_t type) {
  for (const Box& b : boxes)
    if (b.type == type) return &b;
  return nullptr;
}

struct Sample {
  uint64_t offset = 0;
  uint32_t size = 0;
  int64_t dts = 0, pts = 0;
  bool key = true;
};

struct Track {
  uint32_t timescale = 0;
  uint64_t duration = 0;
  std::vector<uint8_t> avcc;
  std::vector<Sample> samples;
};

bool ReadTrack(const std::vector<uint8_t>& d, const Box& trak, Track* t, std::string* why) {
  const auto kids = Boxes(d, trak.begin, trak.end);
  const Box* mdia = Find(kids, Tag("mdia"));
  if (!mdia) return false;
  const auto m = Boxes(d, mdia->begin, mdia->end);
  const Box* hdlr = Find(m, Tag("hdlr"));
  if (!hdlr || hdlr->end - hdlr->begin < 12 || Be32(&d[hdlr->begin + 8]) != Tag("vide")) return false;
  const Box* mdhd = Find(m, Tag("mdhd"));
  const Box* minf = Find(m, Tag("minf"));
  if (!mdhd || !minf || mdhd->end - mdhd->begin < 24) return false;
  if (d[mdhd->begin] == 1) {
    if (mdhd->end - mdhd->begin < 32) return false;
    t->timescale = Be32(&d[mdhd->begin + 20]), t->duration = Be64(&d[mdhd->begin + 24]);
  } else {
    t->timescale = Be32(&d[mdhd->begin + 12]), t->duration = Be32(&d[mdhd->begin + 16]);
  }
  if (!t->timescale) return false;
  const auto mi = Boxes(d, minf->begin, minf->end);
  const Box* stbl = Find(mi, Tag("stbl"));
  if (!stbl) return false;
  const auto st = Boxes(d, stbl->begin, stbl->end);

  // stsd: the first entry, avc1 / avc3 (a VisualSampleEntry: its boxes from +78) with avcC.
  const Box* stsd = Find(st, Tag("stsd"));
  if (!stsd || stsd->end - stsd->begin < 8) return false;
  const auto entries = Boxes(d, stsd->begin + 8, stsd->end);
  if (entries.empty()) return false;
  const Box& e = entries[0];
  if (e.type == Tag("hvc1") || e.type == Tag("hev1")) {
    if (why) *why = "HEVC (H.265) video - only H.264 plays here";
    return false;
  }
  if (e.type != Tag("avc1") && e.type != Tag("avc3")) {
    if (why) *why = "not H.264 video";
    return false;
  }
  if (e.end - e.begin < 78) return false;
  const auto inside = Boxes(d, e.begin + 78, e.end);
  const Box* avcc = Find(inside, Tag("avcC"));
  if (!avcc || avcc->end - avcc->begin < 7) {
    if (why) *why = "H.264 without its avcC";
    return false;
  }
  t->avcc.assign(d.begin() + ptrdiff_t(avcc->begin), d.begin() + ptrdiff_t(avcc->end));

  // stsz: the sizes (one for all, or one each).
  const Box* stsz = Find(st, Tag("stsz"));
  if (!stsz || stsz->end - stsz->begin < 12) {
    if (why) *why = "no sample sizes (stsz)";
    return false;
  }
  const uint32_t same = Be32(&d[stsz->begin + 4]), n = Be32(&d[stsz->begin + 8]);
  if (!n || (!same && (stsz->end - stsz->begin - 12) / 4 < n)) return false;
  std::vector<uint32_t> sizes(n, same);
  if (!same)
    for (uint32_t i = 0; i < n; ++i) sizes[i] = Be32(&d[stsz->begin + 12 + 4 * size_t(i)]);

  // stco / co64: the chunks' places.
  std::vector<uint64_t> chunks;
  if (const Box* stco = Find(st, Tag("stco")); stco && stco->end - stco->begin >= 8) {
    const uint32_t c = Be32(&d[stco->begin + 4]);
    if ((stco->end - stco->begin - 8) / 4 < c) return false;
    for (uint32_t i = 0; i < c; ++i) chunks.push_back(Be32(&d[stco->begin + 8 + 4 * size_t(i)]));
  } else if (const Box* co64 = Find(st, Tag("co64")); co64 && co64->end - co64->begin >= 8) {
    const uint32_t c = Be32(&d[co64->begin + 4]);
    if ((co64->end - co64->begin - 8) / 8 < c) return false;
    for (uint32_t i = 0; i < c; ++i) chunks.push_back(Be64(&d[co64->begin + 8 + 8 * size_t(i)]));
  }
  // stsc: runs of chunks with the same sample count.
  const Box* stsc = Find(st, Tag("stsc"));
  if (chunks.empty() || !stsc || stsc->end - stsc->begin < 8) {
    if (why) *why = "no chunk table (stco / stsc)";
    return false;
  }
  const uint32_t runs = Be32(&d[stsc->begin + 4]);
  if ((stsc->end - stsc->begin - 8) / 12 < runs || !runs) return false;
  t->samples.resize(n);
  size_t s = 0;
  for (uint32_t r = 0; r < runs && s < n; ++r) {
    const uint8_t* p = &d[stsc->begin + 8 + 12 * size_t(r)];
    const uint64_t first = Be32(p), per = Be32(p + 4);
    const uint64_t next = r + 1 < runs ? Be32(p + 12) : chunks.size() + 1;
    for (uint64_t c = std::max<uint64_t>(first, 1); c < next && c <= chunks.size() && s < n; ++c) {
      uint64_t at = chunks[size_t(c - 1)];
      for (uint64_t k = 0; k < per && s < n; ++k, ++s) {
        t->samples[s].offset = at, t->samples[s].size = sizes[s];
        at += sizes[s];
      }
    }
  }
  t->samples.resize(s);
  if (t->samples.empty()) return false;

  // stts: decode times; ctts: display offsets (signed, as the version 0 ones are used too).
  if (const Box* stts = Find(st, Tag("stts")); stts && stts->end - stts->begin >= 8) {
    const uint32_t c = Be32(&d[stts->begin + 4]);
    int64_t dts = 0;
    size_t i = 0;
    for (uint32_t k = 0; k < c && (k + 1) * size_t(8) <= stts->end - stts->begin - 8; ++k) {
      const uint32_t count = Be32(&d[stts->begin + 8 + 8 * size_t(k)]), delta = Be32(&d[stts->begin + 12 + 8 * size_t(k)]);
      for (uint32_t j = 0; j < count && i < t->samples.size(); ++j, ++i) t->samples[i].dts = dts, dts += delta;
    }
    for (; i < t->samples.size(); ++i) t->samples[i].dts = dts;  // (a short table: the rest at its end)
  }
  for (Sample& x : t->samples) x.pts = x.dts;
  if (const Box* ctts = Find(st, Tag("ctts")); ctts && ctts->end - ctts->begin >= 8) {
    const uint32_t c = Be32(&d[ctts->begin + 4]);
    size_t i = 0;
    for (uint32_t k = 0; k < c && (k + 1) * size_t(8) <= ctts->end - ctts->begin - 8; ++k) {
      const uint32_t count = Be32(&d[ctts->begin + 8 + 8 * size_t(k)]);
      const int32_t offset = int32_t(Be32(&d[ctts->begin + 12 + 8 * size_t(k)]));
      for (uint32_t j = 0; j < count && i < t->samples.size(); ++j, ++i) t->samples[i].pts += offset;
    }
  }
  // stss: the key pictures (none: all are).
  if (const Box* stss = Find(st, Tag("stss")); stss && stss->end - stss->begin >= 8) {
    for (Sample& x : t->samples) x.key = false;
    const uint32_t c = Be32(&d[stss->begin + 4]);
    for (uint32_t k = 0; k < c && (k + 1) * size_t(4) <= stss->end - stss->begin - 8; ++k)
      if (const uint32_t i = Be32(&d[stss->begin + 8 + 4 * size_t(k)]); i >= 1 && i <= t->samples.size())
        t->samples[i - 1].key = true;
  }
  return true;
}

}  // namespace

struct Mp4Video::Impl {
  FILE* file = nullptr;
  Track track;
  int64_t first_pts = 0;
  double seconds = 0;
  AVCodecContext* ctx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;
  size_t next = 0;
  bool draining = false;
  int bad = 0;

  ~Impl() {
    avcodec_free_context(&ctx);
    av_frame_free(&frame);
    av_packet_free(&packet);
    if (file) std::fclose(file);
  }
};

Mp4Video::Mp4Video() = default;
Mp4Video::~Mp4Video() = default;

bool Mp4Video::Open(const std::filesystem::path& mp4, std::string* why) {
  impl_.reset();
  auto m = std::make_unique<Impl>();
  const auto fail = [&](const char* text) {
    if (why && why->empty()) *why = text;
    return false;
  };
  m->file = std::fopen(mp4.string().c_str(), "rb");
  if (!m->file) return fail("can't open it");
  std::fseek(m->file, 0, SEEK_END);
  const long long length = std::ftell(m->file);
  // The top level: moov, wherever it is (before or after the media).
  std::vector<uint8_t> moov;
  for (long long at = 0; at + 8 <= length;) {
    uint8_t h[16];
    std::fseek(m->file, long(at), SEEK_SET);
    if (std::fread(h, 1, 8, m->file) != 8) break;
    uint64_t size = Be32(h);
    long long head = 8;
    if (size == 1) {
      if (std::fread(h + 8, 1, 8, m->file) != 8) break;
      size = Be64(h + 8), head = 16;
    } else if (size == 0) {
      size = uint64_t(length - at);
    }
    if (size < uint64_t(head) || size > uint64_t(length - at)) break;
    if (Be32(h + 4) == Tag("moov")) {
      if (size - head > (64u << 20)) return fail("its index (moov) is too big");
      moov.resize(size_t(size - head));
      if (std::fread(moov.data(), 1, moov.size(), m->file) != moov.size()) return fail("can't read its index (moov)");
      break;
    }
    at += (long long)size;
  }
  if (moov.empty()) return fail("not an .mp4 (no moov)");
  bool found = false;
  for (const Box& b : Boxes(moov, 0, moov.size()))
    if (b.type == Tag("trak") && ReadTrack(moov, b, &m->track, why)) {
      found = true;
      break;
    }
  if (!found) return fail("no H.264 video track");
  Track& t = m->track;
  m->first_pts = std::numeric_limits<int64_t>::max();
  int64_t last = 0;
  for (const Sample& s : t.samples) m->first_pts = std::min(m->first_pts, s.pts), last = std::max(last, s.pts);
  m->seconds = t.duration ? double(t.duration) / t.timescale : double(last - m->first_pts) / t.timescale;
  if (t.samples.size() > 1 && !t.duration)
    m->seconds += double(last - m->first_pts) / t.timescale / double(t.samples.size() - 1);  // (the last picture's own)

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) return fail("no H.264 decoder in this build");
  m->ctx = avcodec_alloc_context3(codec);
  m->frame = av_frame_alloc();
  m->packet = av_packet_alloc();
  if (!m->ctx || !m->frame || !m->packet) return fail("out of memory");
  m->ctx->extradata = static_cast<uint8_t*>(av_mallocz(t.avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!m->ctx->extradata) return fail("out of memory");
  std::memcpy(m->ctx->extradata, t.avcc.data(), t.avcc.size());
  m->ctx->extradata_size = int(t.avcc.size());
  m->ctx->thread_count = 1;  // (a titantron's movie is small: one thread, below the game's)
  m->ctx->pkt_timebase = AVRational{1, int(std::min<uint32_t>(t.timescale, 0x7FFFFFFF))};
  if (avcodec_open2(m->ctx, codec, nullptr) < 0) return fail("the H.264 decoder didn't take it");
  impl_ = std::move(m);
  return true;
}

double Mp4Video::Seconds() const { return impl_ ? impl_->seconds : 0; }

bool Mp4Video::Next(VideoPicture* out) {
  Impl* m = impl_.get();
  if (!m) return false;
  for (;;) {
    const int got = avcodec_receive_frame(m->ctx, m->frame);
    if (got == 0) {
      const AVFrame* f = m->frame;
      int cx = 1, cy = 1;
      switch (f->format) {
        case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: break;
        case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: cy = 0; break;
        case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P: cx = cy = 0; break;
        default: return false;  // (10-bit, 4:0:0: none of the Mod Maker's)
      }
      for (int p = 0; p < 3; ++p) out->plane[p] = f->data[p], out->pitch[p] = f->linesize[p];
      out->width = f->width, out->height = f->height, out->chroma_x = cx, out->chroma_y = cy;
      int64_t pts = f->best_effort_timestamp;
      if (pts == AV_NOPTS_VALUE) pts = f->pts;
      out->time = pts == AV_NOPTS_VALUE ? 0 : std::max(0.0, double(pts - m->first_pts) / m->track.timescale);
      return true;
    }
    if (got != AVERROR(EAGAIN) || m->draining) return false;  // (the end, or an error)
    if (m->next >= m->track.samples.size()) {
      avcodec_send_packet(m->ctx, nullptr);  // (the pictures still held back come out)
      m->draining = true;
      continue;
    }
    const Sample& s = m->track.samples[m->next++];
    if (av_new_packet(m->packet, int(s.size)) < 0) return false;
    std::fseek(m->file, long(s.offset), SEEK_SET);
    if (std::fread(m->packet->data, 1, s.size, m->file) != s.size) {
      av_packet_unref(m->packet);
      return false;
    }
    m->packet->pts = s.pts, m->packet->dts = s.dts;
    if (s.key) m->packet->flags |= AV_PKT_FLAG_KEY;
    const int sent = avcodec_send_packet(m->ctx, m->packet);
    av_packet_unref(m->packet);
    if (sent < 0 && sent != AVERROR(EAGAIN) && ++m->bad > 30) return false;  // (a broken sample: skipped)
  }
}

void Mp4Video::Rewind() {
  if (!impl_) return;
  avcodec_flush_buffers(impl_->ctx);
  impl_->next = 0, impl_->draining = false;
}

}  // namespace svr2011::ps5
