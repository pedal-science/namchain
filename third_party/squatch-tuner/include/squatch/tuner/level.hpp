// SPDX-License-Identifier: MIT
// Level bookkeeping for the tuner, once per 64-sample hop (5.3 ms at 12 kHz): the hop level,
// a 43 ms level, a rising-energy test for plucks, and the room's noise floor.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace squatch::tuner {

class LevelTracker {
 public:
  void reset() {
    energy_.fill(0.0f);
    index_ = 0;
    sinceRise_ = 0;
    rising_ = false;
    level_ = level43_ = 0.0f;
    floor_ = -1.0f;
    hopsSeen_ = 0;
    quietest_ = 0.0f;
    idleSeen_ = false;
    idleRun_ = 0;
    spanMin_.fill(0.0f);
    spanNow_ = 0.0f;
    spanHops_ = 0;
  }

  /// One hop: its energy (sum of squares over `hopLen` samples) and whether a note is ringing.
  void hop(float energy, int hopLen, bool tracking) {
    level_ = std::sqrt(energy / static_cast<float>(hopLen));
    energy_[index_++ & 7] = energy;
    rising_ = detectRise();
    float sum8 = 0.0f;
    for (float e : energy_) sum8 += e;
    level43_ = std::sqrt(sum8 / (8.0f * static_cast<float>(hopLen)));
    updateFloor(tracking, hopLen);
  }

  float level() const { return level_; }      // this hop's RMS
  float level43() const { return level43_; }  // RMS over the last 8 hops (43 ms)
  float floor() const { return floor_; }      // hum + noise RMS, once known
  bool floorKnown() const { return floor_ > 0.0f; }
  bool rising() const { return rising_; }     // a pluck-like jump in energy on this hop

 private:
  // The energy of the last 4 hops (21 ms) is 8x (+9 dB) that of the 4 before, and no rise in
  // the last 8 hops. Shorter windows flicker with the waveform of a low note (E1: 24 ms period).
  bool detectRise() {
    float recent = 0.0f, before = 0.0f;
    for (unsigned i = 0; i < 4; ++i) {
      recent += energy_[(index_ - 1 - i) & 7];
      before += energy_[(index_ - 5 - i) & 7];
    }
    const bool rise = recent > 8.0f * before && sinceRise_ > 8;
    sinceRise_ = rise ? 0 : std::min(sinceRise_ + 1, 1 << 20);
    return rise;
  }

  // Minimum statistics over the 43 ms level (a single hop dips inside one period of a low
  // note): the floor is the quietest 43 ms heard in the last kSpans one-second spans, so it
  // drops at once to anything quieter and rises only as quiet moments age out, 8 s later. The
  // spans advance only while no note is tracked, so a ringing note can never become its own
  // floor. It used to creep up by +0.17 dB per idle hop instead, which on a DI guitar was fatal:
  // a pluck the note finder could not lock yet (a hard low-E attack: MPM clarity 0.3 to 0.7) was
  // "idle", the floor reached it within half a second, and once a loud note had been heard
  // nothing within 10 dB of the floor may start one, so the tuner went deaf until the strings
  // were silent. Hum that appears later is learned within 8 s.
  //
  // The floor is unknown until 8 hops in a row have been idle and steady (hum and noise are; a
  // pluck, or a recording that starts mid-note, is not). "Idle" means no note is tracked AND the
  // analysis window has filled: before that the note finder has not run, so a string ringing at
  // power-on would otherwise pass for the room. In a room something is almost always ringing,
  // so "idle and steady" may never come: after kBootstrapHops (1 s) with no floor, the quietest
  // 43 ms level heard while idle becomes the floor. A note that rings from power-on never
  // counts, so it cannot become its own floor; until the floor is known every level counts as
  // above it.
  void updateFloor(bool tracking, int hopLen) {
    if (index_ < 8) return;
    idleRun_ = tracking ? 0 : idleRun_ + 1;
    if (floor_ < 0.0f) {
      bootstrapFloor(hopLen);
      if (floor_ > 0.0f) startSpans();
      return;
    }
    // Digital silence (a volume knob at zero, a cable pulled, an interface muting) is not the
    // room: learned, it would count everything above -140 dB as signal, hum included, for 8 s.
    if (level43_ > kSilence) spanNow_ = std::fmin(spanNow_, level43_);
    if (!tracking && ++spanHops_ >= kBootstrapHops) nextSpan();
    float f = spanNow_;
    for (float m : spanMin_) f = std::fmin(f, m);
    floor_ = std::fmax(f, 1e-7f);
  }

  void startSpans() {
    spanMin_.fill(floor_);
    spanNow_ = floor_;
    spanHops_ = 0;
  }

  // A new span starts from the current level, or from the last span's minimum in silence.
  void nextSpan() {
    spanMin_[spanIndex_++ % kSpans] = spanNow_;
    spanNow_ = std::fmax(level43_, spanNow_);
    spanHops_ = 0;
  }

  void bootstrapFloor(int hopLen) {
    const bool idle = idleRun_ >= 8;
    if (idle && level43_ > kSilence) {
      quietest_ = idleSeen_ ? std::fmin(quietest_, level43_) : level43_;
      idleSeen_ = true;
    }
    if (idle && steady() && level43_ > kSilence) {
      floor_ = std::fmax(std::sqrt(minEnergy() / static_cast<float>(hopLen)), 1e-7f);
    } else if (++hopsSeen_ >= kBootstrapHops && idleSeen_) {
      floor_ = std::fmax(quietest_, 1e-7f);
    }
  }

  float minEnergy() const {
    float e = energy_[0];
    for (float v : energy_) e = std::fmin(e, v);
    return e;
  }

  // The last 8 hops' energies lie within 6 dB of each other.
  bool steady() const {
    float hi = 0.0f;
    for (float v : energy_) hi = std::fmax(hi, v);
    return hi <= 4.0f * minEnergy();
  }

  static constexpr int kBootstrapHops = 188;  // 1 s of 64-sample hops at 12 kHz
  static constexpr float kSilence = 1e-6f;    // -120 dBFS: below any real input's noise
  static constexpr int kSpans = 8;            // the floor looks back 8 one-second spans
  std::array<float, 8> energy_{};
  unsigned index_ = 0;
  int hopsSeen_ = 0;
  float quietest_ = 0.0f;
  bool idleSeen_ = false;
  int idleRun_ = 0;
  std::array<float, kSpans> spanMin_{};
  float spanNow_ = 0.0f;
  int spanHops_ = 0;
  unsigned spanIndex_ = 0;
  int sinceRise_ = 0;
  bool rising_ = false;
  float level_ = 0.0f, level43_ = 0.0f, floor_ = -1.0f;
};

}  // namespace squatch::tuner
