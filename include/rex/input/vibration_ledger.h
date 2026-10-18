#pragma once
/**
 * @file        rex/input/vibration_ledger.h
 * @brief       Counts the guest's vibration requests at InputSystem::SetState.
 *
 * WWE All Stars recomp: tells "the game sends only zeros" apart from "the pad
 * refused" from one log line (.scratch/controller-vibration/spec.md).
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/kernel.h>

namespace rex::input {

/// Which log line: one SetState emits every 10 s, or the one at exit.
enum class VibrationLineKind { kPeriodic, kFinal };

inline std::string_view ToString(VibrationLineKind kind) {
  return kind == VibrationLineKind::kFinal ? "final" : "periodic";
}

/// The pad driver's answer as the log lines spell it.
inline std::string PadResultText(X_RESULT result) {
  if (result == X_ERROR_SUCCESS) {
    return "success";
  }
  if (result == X_ERROR_DEVICE_NOT_CONNECTED) {
    return "not_connected";
  }
  return fmt::format("0x{:08X}", static_cast<uint32_t>(result));
}

/// The player index the guest passed, before XamInputSetState pins 0xFF
/// ("any user") to user 0: 0-3, ff, or other.
enum class RawIndex : size_t { k0, k1, k2, k3, kAny, kOther, kCount };

inline RawIndex ToRawIndex(uint32_t raw_user) {
  const uint32_t low = raw_user & 0xFF;
  if (low == 0xFF) {
    return RawIndex::kAny;
  }
  return raw_user < 4 ? static_cast<RawIndex>(raw_user) : RawIndex::kOther;
}

inline std::string_view ToString(RawIndex index) {
  constexpr std::string_view kNames[] = {"0", "1", "2", "3", "ff", "other"};
  return kNames[static_cast<size_t>(index)];
}

struct VibrationIndexCount {
  uint64_t requests = 0;
  uint64_t nonzero = 0;
};

struct VibrationLedger {
  uint64_t requests = 0;  // every SetState call
  uint64_t nonzero = 0;   // ... with either motor above zero
  uint16_t peak_left = 0;
  uint16_t peak_right = 0;
  uint32_t last_user = 0;
  uint16_t last_left = 0;
  uint16_t last_right = 0;
  // What the real pads answered to the last request a pad took part in.
  // Requests that reach no pad (synthetic devices only, or a user with no pad)
  // leave it alone, so with no pad at all it reads "not connected".
  X_RESULT pad_result = X_ERROR_DEVICE_NOT_CONNECTED;
  // The same counts split by the raw player index, indexed by RawIndex.
  std::array<VibrationIndexCount, static_cast<size_t>(RawIndex::kCount)> by_raw_index{};

  const VibrationIndexCount& ForRawIndex(uint32_t raw_user) const {
    return by_raw_index[static_cast<size_t>(ToRawIndex(raw_user))];
  }

  /// `user` is the index after the pin, `raw_user` the one the guest passed.
  /// `pads` is the real pads' answer, or empty when no pad took part.
  void Record(uint32_t raw_user, uint32_t user, uint16_t left, uint16_t right,
              std::optional<X_RESULT> pads) {
    auto& by_index = by_raw_index[static_cast<size_t>(ToRawIndex(raw_user))];
    requests++;
    by_index.requests++;
    if (left || right) {
      nonzero++;
      by_index.nonzero++;
    }
    peak_left = std::max(peak_left, left);
    peak_right = std::max(peak_right, right);
    last_user = user;
    last_left = left;
    last_right = right;
    if (pads) {
      pad_result = *pads;
    }
  }

  /// True when the line would say something new since `logged`. A game that
  /// repeats the same request every frame only moves `requests`, which on its
  /// own is not news.
  bool ChangedSince(const VibrationLedger& logged) const {
    return (requests != 0 && logged.requests == 0) || nonzero != logged.nonzero ||
           peak_left != logged.peak_left || peak_right != logged.peak_right ||
           last_user != logged.last_user || last_left != logged.last_left ||
           last_right != logged.last_right || pad_result != logged.pad_result;
  }

  std::string Line(VibrationLineKind kind, std::string_view backend) const {
    std::string split;
    for (size_t i = 0; i < by_raw_index.size(); ++i) {
      split += fmt::format("{}{}:{}/{}", i ? "," : "", ToString(static_cast<RawIndex>(i)),
                           by_raw_index[i].requests, by_raw_index[i].nonzero);
    }
    return fmt::format(
        "vibration {}: backend={} requests={} nonzero={} peak_left={} peak_right={} "
        "last_user={} last_left={} last_right={} pad_result={} by_raw_index={}",
        ToString(kind), backend, requests, nonzero, peak_left, peak_right, last_user, last_left,
        last_right, PadResultText(pad_result), split);
  }
};

/// When to log the ledger: a periodic line at most every 10 s, and only when
/// it says something new since the last line logged.
class VibrationLog {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr Clock::duration kPeriod = std::chrono::seconds(10);

  /// Call after each request. True once per period; the first request starts
  /// the clock.
  bool PeriodicDue(Clock::time_point now) {
    if (next_ == Clock::time_point{}) {
      next_ = now + kPeriod;
      return false;
    }
    if (now < next_) {
      return false;
    }
    next_ = now + kPeriod;
    return true;
  }

  /// The line to log if `ledger` changed since the last one taken, which it
  /// then remembers.
  std::optional<std::string> TakeIfChanged(const VibrationLedger& ledger, VibrationLineKind kind,
                                           std::string_view backend) {
    if (!ledger.ChangedSince(logged_)) {
      return std::nullopt;
    }
    logged_ = ledger;
    return ledger.Line(kind, backend);
  }

 private:
  Clock::time_point next_{};
  VibrationLedger logged_;
};

/// One log line per non-zero request, and one for the zero that ends each
/// pulse with its length, per raw player index; capped so a game that rumbles
/// all match cannot flood the log.
class VibrationTrace {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr uint32_t kMaxLines = 300;

  /// Call for every request; returns the line to log, if any. `t` in the
  /// line counts from the first request.
  std::optional<std::string> Take(uint32_t raw_user, uint32_t user, uint16_t left, uint16_t right,
                                  std::optional<X_RESULT> pads, Clock::time_point now) {
    if (!started_) {
      started_ = true;
      start_ = now;
    }
    Pulse& pulse = pulses_[static_cast<size_t>(ToRawIndex(raw_user))];
    const bool nonzero = left || right;
    if (!nonzero && pulse.requests == 0) {
      return std::nullopt;  // at rest
    }
    if (lines_ > kMaxLines) {
      return std::nullopt;
    }
    if (lines_ == kMaxLines) {
      lines_++;
      return fmt::format("vibration trace: {} lines logged, later requests are only counted",
                         kMaxLines);
    }
    lines_++;
    const RawIndex index = ToRawIndex(raw_user);
    const std::string raw = index == RawIndex::kOther ? fmt::format("0x{:X}", raw_user)
                                                      : std::string(ToString(index));
    std::string line = fmt::format(
        "vibration request: t={:.3f}s raw_index={} user={} left={} right={} pad_result={}",
        std::chrono::duration<double>(now - start_).count(), raw, user, left, right,
        pads ? PadResultText(*pads) : std::string("no_pad"));
    if (nonzero) {
      if (pulse.requests++ == 0) {
        pulse.start = now;
      }
    } else {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - pulse.start);
      line += fmt::format(" pulse_end pulse_ms={} pulse_requests={}", ms.count(), pulse.requests);
      pulse = {};
    }
    return line;
  }

 private:
  struct Pulse {
    Clock::time_point start{};
    uint64_t requests = 0;  // non-zero requests since the pulse began
  };

  bool started_ = false;
  Clock::time_point start_{};
  uint32_t lines_ = 0;
  std::array<Pulse, static_cast<size_t>(RawIndex::kCount)> pulses_{};
};

}  // namespace rex::input
