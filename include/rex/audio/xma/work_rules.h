#pragma once
/**
 * What an XMA context does after one decode pass, as a pure rule so it can be
 * tested without guest memory or an XMA bitstream.
 *
 * Titles' XMA libraries read output_buffer_valid == 0 as "the hardware stopped
 * because the ring is full" and count the whole ring as readable, and valid
 * == 1 with equal read and write offsets as "empty". So the bit may only be
 * cleared when the ring is full (Canary xma_context_new.cc, commit 505697f980).
 * Like the hardware, a context keeps decoding without a new kick while it has
 * input and room, until the title locks it; a decoder that cannot progress
 * with input still valid marks the input exhausted, so the title finishes the
 * stream instead of waiting for PCM that never comes.
 *
 * @modified    WWE All Stars recomp, 2026 - XMA output-valid rule; frame drain
 *              and progress break from Xenia Canary (BSD-3)
 */

namespace rex::audio::xma {

struct PassOutcome {
  bool ring_full;        // the output ring holds capacity blocks of PCM
  bool room_for_decode;  // free blocks for at least one more decode step
  bool input_valid;      // an input buffer is still valid after the pass
  bool progressed;       // the pass wrote PCM or consumed input
  // A frame runs into the next input buffer, which the title has not marked
  // valid yet (error_status 4): waiting on the title, not stalled.
  bool waiting_for_next_buffer = false;
  // A decoded frame is only partly in the ring (the ring ran out of room).
  bool frame_pending = false;
};

struct PassDecision {
  bool output_valid;     // what output_buffer_valid is left at
  bool keep_decoding;    // stay enabled: decoded again on the worker's next
                         // sweep, without a kick, once there is room or input
  bool input_exhausted;  // clear both input buffers' valid bits
};

constexpr PassDecision DecideAfterPass(const PassOutcome& o) {
  const bool stalled =
      o.input_valid && o.room_for_decode && !o.progressed && !o.waiting_for_next_buffer;
  return PassDecision{
      .output_valid = !o.ring_full,
      .keep_decoding = (o.input_valid || o.frame_pending) && !stalled,
      .input_exhausted = stalled,
  };
}

struct LoopState {
  bool room_for_decode;  // free blocks for one more decode step
  bool input_valid;      // an input buffer is still valid
  bool frame_pending;    // a decoded frame is only partly in the ring
  bool waiting_for_next_buffer;  // error_status 4 (see PassOutcome)
  bool progressed = true;  // this round wrote PCM or moved the input
};

// Whether a decode pass goes round its loop again. A frame already decoded is
// delivered in full while there is room, even when the input has run dry or
// waits for the next buffer: the title polls for those samples and may never
// issue the kick that would deliver them. A round that moved nothing ends
// the pass, as another would do the same. Ported from Xenia Canary
// (xma_context_new.cc, commit 6bef1fbed1 and its decoder-progress break,
// BSD-3).
constexpr bool KeepPassGoing(const LoopState& s) {
  if (!s.room_for_decode || !s.progressed) {
    return false;
  }
  return s.frame_pending || (s.input_valid && !s.waiting_for_next_buffer);
}

}  // namespace rex::audio::xma
