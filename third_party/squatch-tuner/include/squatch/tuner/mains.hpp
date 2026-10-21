// SPDX-License-Identifier: MIT
// Mains hum, as the tuner refuses it.
//
// Mains hum is periodic, and on a Hi-Z input it read as B1 -49 c: from power-on (99% of an idle
// take), between strings, and as a dying note's successor. It swings 20 dB as the hand touches
// and leaves the strings (-75 to -55 dBFS measured), which the onset test takes for a pluck, so
// the floor cannot always hold it back, and the coarse stage places it up to 2% off (58.9 Hz). So
// it is judged by the fine fit: a note within 8 c of 50 or 60 Hz or their 2nd and 3rd harmonics
// never reads, and after 8 hops there it ends. No tuned string lies that close (the nearest, D3,
// is 37 c from 150 Hz); a string passing through one of these pitches while being tuned is hidden
// for those 16 cents only. Hum that wanders further (a Hi-Z input's: 55 to 67 Hz) is refused at a
// note's tail by successor.hpp instead.
#pragma once
#include <cmath>

#include "common.hpp"

namespace squatch::tuner {

inline bool atMains(float hz, float withinCents) {
  for (float mains : {50.0f, 60.0f}) {
    for (int k = 1; k <= 3; ++k) {
      if (std::fabs(centsBetween(hz, mains * static_cast<float>(k))) < withinCents) return true;
    }
  }
  return false;
}

}  // namespace squatch::tuner
