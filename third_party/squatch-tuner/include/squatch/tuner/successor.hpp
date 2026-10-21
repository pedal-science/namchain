// SPDX-License-Identifier: MIT
// What rings on after a note without a pluck of its own: a successor.
//
// At a note's tail a successor is almost never a new note. It is mains hum (on a Hi-Z input it
// wanders 55 to 67 Hz, outside atMains()), a sympathetic string or noise, and it used to be shown
// as a live A1..C2 or as whatever the other string was (our open A, G, B and E takes; 18%
// of EGFxSet's wrong-note frames). So a note that starts with no pluck while a plucked note rings,
// or after one until a quarter second of silence, must be within 20 dB of the loudest such note
// (legato: a hammer-on, a slide) and never below kLowestHz. A pluck (an onset) lifts both; a
// string ringing from power-on, or a test oscillator, has no note before it.
#pragma once
#include <algorithm>

namespace squatch::tuner {

class SuccessorGuard {
 public:
  static constexpr int kForgetHops = 47;      // 0.25 s of hops at 12 kHz
  static constexpr float kLowestHz = 70.0f;   // mains hum and its wander lie below

  void reset() {
    anchor_ = 0.0f;
    silent_ = 0;
  }

  /// Each hop: a pluck starts afresh; a ringing note's peak (43 ms level) is the anchor.
  void hop(bool onset, bool tracking, float notePeak) {
    if (onset) anchor_ = 0.0f;
    if (tracking) anchor_ = std::max(anchor_, notePeak);
  }

  /// A hop above the gate, or below it: a quarter second of silence forgets the anchor.
  void heard(bool sound) {
    silent_ = sound ? 0 : silent_ + 1;
    if (silent_ >= kForgetHops) {
      anchor_ = 0.0f;
      silent_ = kForgetHops;
    }
  }

  /// Whether a start at `hz` with the 43 ms level `level` and no pending pluck is refused.
  bool refuses(float hz, float level, bool pluck) const {
    if (pluck || anchor_ <= 0.0f) return false;
    return level < 0.1f * anchor_ || hz < kLowestHz;
  }

 private:
  float anchor_ = 0.0f;  // loudest note since the last pluck or silence
  int silent_ = 0;
};

}  // namespace squatch::tuner
