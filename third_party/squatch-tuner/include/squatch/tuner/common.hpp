// SPDX-License-Identifier: MIT
// Squatch Tuner - shared constants and small helpers.
// Header-only, no allocation, no dependencies beyond <cmath>, <cstdint> and <array>.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace squatch::tuner {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

/// Compile-time capacity limits. Every buffer in the audio path is sized from these, so the
/// object size is fixed and nothing is allocated after construction.
struct Limits {
  static constexpr int kMaxDecimation = 8;
  static constexpr int kDecimTapsPerPhase = 16;                 // FIR taps per decimation phase
  static constexpr int kMaxWindow = 1024;                       // coarse analysis window (work rate)
  static constexpr int kMaxLag = kMaxWindow / 2;                // longest period the coarse stage finds
  static constexpr int kMaxPartials = 8;                        // partials the strobe bank tracks
  static constexpr int kMaxBoxcar = 512;                        // demodulator lowpass length (work rate)
  static constexpr int kMaxFitPoints = 512;                     // phase points in the slope window
};

inline float midiToHz(float midi, float a4 = 440.0f) { return a4 * std::exp2((midi - 69.0f) / 12.0f); }
inline float hzToMidi(float hz, float a4 = 440.0f) { return 69.0f + 12.0f * std::log2(hz / a4); }
inline float centsBetween(float hz, float refHz) { return 1200.0f * std::log2(hz / refHz); }

/// Wrap a phase to (-pi, pi]. The hot path (a difference of two wrapped phases, so within
/// (-2 pi, 2 pi)) is two compares; anything larger falls back to fmod.
inline float wrapPi(float p) {
  if (p > kPi) p -= kTwoPi;
  if (p <= -kPi) p += kTwoPi;
  if (p > kPi || p <= -kPi) {
    p = std::fmod(p + kPi, kTwoPi);
    p = (p < 0.0f ? p + kTwoPi : p) - kPi;
  }
  return p;
}

/// Nearest equal-tempered note and the offset from it.
struct NoteReading {
  int midi = 0;      // nearest MIDI note (69 = A4)
  float cents = 0;   // signed offset from that note
};

inline NoteReading nearestNote(float hz, float a4 = 440.0f) {
  const float m = hzToMidi(hz, a4);
  NoteReading r;
  r.midi = static_cast<int>(std::lround(m));
  r.cents = (m - static_cast<float>(r.midi)) * 100.0f;
  return r;
}

/// Parabolic vertex offset for three equally spaced samples; returns delta in [-1, 1].
inline float parabolicOffset(float ym1, float y0, float yp1) {
  const float den = ym1 - 2.0f * y0 + yp1;
  if (std::fabs(den) < 1e-12f) return 0.0f;
  const float d = 0.5f * (ym1 - yp1) / den;
  return d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
}

}  // namespace squatch::tuner
