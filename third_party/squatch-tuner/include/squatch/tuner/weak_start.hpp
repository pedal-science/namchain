// SPDX-License-Identifier: MIT
// A second way to start a note, for strings ringing together.
//
// A hard open-E pluck sets the other five open strings ringing within 3 to 6 dB of it. The coarse
// stage's clarity then stays at 0.2 to 0.8, under the 0.8 a start needs, and nothing was shown for
// 8 s. Such mixtures wander (a hard pluck in our takes gave 49, 73, 184 and 82 Hz in its first two
// seconds), so only a pitch that holds is taken: a candidate the coarse stage keeps returning at
// kClarity or more for a net kHops. Its sub-octave (MPM's usual slip) counts for it, its octave
// replaces it, and anything else costs it a hop and takes its place once it has none left. The
// tuner decides the rest (level, hum, the lowest pitch) before it starts the note.
#pragma once
#include <algorithm>
#include <cmath>

#include "common.hpp"

namespace squatch::tuner {

class WeakStart {
 public:
  static constexpr float kClarity = 0.45f;  // a weak start's coarse clarity
  static constexpr int kHops = 24;          // ... held for this many coarse checks, net (0.13 s)

  void reset() {
    hz_ = 0.0f;
    hops_ = 0;
  }

  /// A coarse check with no candidate for it (unclear, or too quiet): the candidate loses a hop.
  void miss() { hops_ = std::max(hops_ - 1, 0); }

  /// A coarse pitch at kClarity or more. True once the candidate has held for kHops.
  bool offer(float hz) {
    const float same = std::fabs(centsBetween(hz, hz_)), below = std::fabs(centsBetween(2.0f * hz, hz_));
    const float above = std::fabs(centsBetween(hz, 2.0f * hz_));
    if (hz_ <= 0.0f || (same > 25.0f && below > 25.0f && above > 25.0f)) {
      if (--hops_ <= 0) {
        hz_ = hz;
        hops_ = 1;
      }
      return false;
    }
    if (same <= 25.0f) hz_ += 0.1f * (hz - hz_);
    if (above <= 25.0f) hz_ = hz;
    hops_ = std::min(hops_ + 1, kHops);
    return hops_ >= kHops;
  }

  float hz() const { return hz_; }

  /// `hz` within `cents` of `ref`, or its octave within `cents` of it (the sub-octave slip).
  static bool near(float hz, float ref, float cents) {
    return std::fabs(centsBetween(hz, ref)) <= cents || std::fabs(centsBetween(2.0f * hz, ref)) <= cents;
  }

 private:
  float hz_ = 0.0f;
  int hops_ = 0;
};

}  // namespace squatch::tuner
