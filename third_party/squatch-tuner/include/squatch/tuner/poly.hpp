// SPDX-License-Identifier: MIT
// Polyphonic strum check: strum all strings, see which are out.
//
// The tuning is known, so each string gets its own strobe channels at the partials of its
// target pitch, and the phase drift of each channel is that string's error. TC Electronic's
// patent (US 8,338,683) describes the same premise with band-pass filters and per-band
// monophonic detectors; here the "band" is a demodulator whose boxcar is long (85 ms, nulls
// every 11.7 Hz) so strings a few tens of hertz apart do not leak into each other. It runs at
// 6 kHz, which covers the 2nd partial of E4 and halves the cost.
//
// Each string always uses its fundamental, and adds its 2nd partial when that clears every
// partial of every other string by 10 Hz and agrees with the fundamental to 8 c (an absent
// partial reads a neighbour's leakage). Standard
// tuning has collisions no filter can undo in a strum: every partial of B3 sits 0.3 Hz (2 c)
// from a multiple of E2's 3rd partial, and E2's 4th, A2's 3rd and E4 coincide. So B3 (and E4)
// read with some pull from the low E when both are out; see RESULTS.md for how much.
// Readings are not stiffness-corrected (one or two partials cannot support that fit), so a
// 2nd partial adds up to ~1 cent of bias: enough to say which string is out and by how much.
#pragma once
#include "decimator.hpp"
#include "nco.hpp"
#include "strobe.hpp"

namespace squatch::tuner {

struct PolyConfig {
  float sampleRate = 48000.0f;
  float workRate = 6000.0f;
  int strings = 6;
  std::array<float, 8> tuning{82.4069f, 110.0f, 146.832f, 195.998f, 246.942f, 329.628f, 0.0f, 0.0f};
  float boxcarSeconds = 0.085f;
  float memorySeconds = 0.5f;
  float minRelativeLevel = 0.01f;  // a string 20 dB below the loudest is reported as not sounding
};

struct PolyPartial;

struct PolyString {
  bool valid = false;
  float cents = 0.0f;       // offset from the target pitch
  float uncertainty = 0.0f;  // standard error, cents
  float level = 0.0f;
};

class PolyTuner {
 public:
  static constexpr int kMaxStrings = 8;
  static constexpr int kPartialsPerString = 2;

  void configure(const PolyConfig& c) {
    cfg_ = c;
    dec_.configure(static_cast<int>(c.sampleRate / c.workRate + 0.5f));
    rate_ = c.sampleRate / static_cast<float>(dec_.factor());
    (void)CosTable::instance();
    restart();
  }

  /// Start a new strum: clears every channel. Call on a strum onset (or let the owner decide).
  void restart() {
    const int len = static_cast<int>(cfg_.boxcarSeconds * rate_);
    decim_ = len / 4 < 1 ? 1 : len / 4;
    const int window = static_cast<int>(cfg_.memorySeconds * rate_ / static_cast<float>(decim_));
    for (int s = 0; s < cfg_.strings; ++s) {
      nco_[s].setFrequency(cfg_.tuning[s], rate_);
      nco_[s].reset();
      for (int k = 0; k < kPartialsPerString; ++k) {
        use_[s][k] = k == 0 ? tuningOk(s) : partialIsClean(s, k + 1);
        ch_[s][k].configure(len, window);
      }
    }
    counter_ = points_ = 0;
  }

  void process(const float* in, int n) {
    for (int i = 0; i < n; ++i) {
      float y;
      if (dec_.push(in[i], y)) pushWork(y);
    }
  }

  PolyString reading(int s) const;
  PolyPartial partial(int s, int k) const;
  float loudest() const;
  bool usesPartial(int s, int k) const { return use_[s][k - 1]; }

 private:
  bool tuningOk(int s) const { return cfg_.tuning[s] > 0.0f && cfg_.tuning[s] < 0.45f * rate_; }

  bool partialIsClean(int s, int k) const {
    const float f = cfg_.tuning[s] * static_cast<float>(k);
    if (f > 0.45f * rate_) return false;
    for (int o = 0; o < cfg_.strings; ++o) {
      for (int j = 1; o != s && j <= 6; ++j) {
        if (std::fabs(f - cfg_.tuning[o] * static_cast<float>(j)) < 10.0f) return false;
      }
    }
    return true;
  }

  void pushWork(float y) {
    for (int s = 0; s < cfg_.strings; ++s) {
      const uint32_t base = nco_[s].phase();
      for (int k = 0; k < kPartialsPerString; ++k) {
        float c, sn;
        CosTable::instance().cosSin(base * static_cast<uint32_t>(k + 1), c, sn);
        if (use_[s][k]) ch_[s][k].mix(y, c, sn);
      }
      nco_[s].advance();
    }
    if (++counter_ < decim_) return;
    counter_ = 0;
    ++points_;
    const int window = static_cast<int>(cfg_.memorySeconds * rate_ / static_cast<float>(decim_));
    for (int s = 0; s < cfg_.strings; ++s) {
      for (int k = 0; k < kPartialsPerString; ++k) {
        if (!use_[s][k]) continue;
        ch_[s][k].setWindow(points_ / 2 < window ? points_ / 2 : window);
        ch_[s][k].sample();
      }
    }
  }

  PolyConfig cfg_{};
  Decimator dec_;
  float rate_ = 12000.0f;
  int decim_ = 1, counter_ = 0, points_ = 0;
  std::array<Nco, kMaxStrings> nco_{};
  std::array<std::array<PartialChannel, kPartialsPerString>, kMaxStrings> ch_{};
  std::array<std::array<bool, kPartialsPerString>, kMaxStrings> use_{};
};

struct PolyPartial {
  double hz = 0.0, w = 0.0;  // implied fundamental and its inverse variance
  float power = 0.0f;
};

inline float PolyTuner::loudest() const {
  float l = 0.0f;
  for (int o = 0; o < cfg_.strings; ++o) {
    for (int k = 0; k < kPartialsPerString; ++k) l = std::fmax(l, use_[o][k] ? ch_[o][k].power() : 0.0f);
  }
  return l;
}

inline PolyString PolyTuner::reading(int s) const {
  PolyString r;
  std::array<PolyPartial, kPartialsPerString> p{};
  for (int k = 0; k < kPartialsPerString; ++k) p[k] = partial(s, k);
  // A 2nd partial that disagrees with the fundamental by more than 8 c is leakage: keep the
  // stronger of the two.
  if (p[0].w > 0.0 && p[1].w > 0.0 && std::fabs(1200.0 * std::log2(p[1].hz / p[0].hz)) > 8.0) {
    p[p[0].power >= p[1].power ? 1 : 0].w = 0.0;
  }
  const double sw = p[0].w + p[1].w;
  if (sw <= 0.0) return r;
  const double hz = (p[0].w * p[0].hz + p[1].w * p[1].hz) / sw;
  for (const PolyPartial& q : p) r.level = q.w > 0.0 ? std::fmax(r.level, q.power) : r.level;
  r.cents = static_cast<float>(1200.0 * std::log2(hz / cfg_.tuning[s]));
  r.uncertainty = static_cast<float>(1731.23 * std::sqrt(1.0 / sw) / cfg_.tuning[s]);
  r.valid = r.level >= cfg_.minRelativeLevel * loudest() && r.uncertainty < 2.0f;
  return r;
}

inline PolyPartial PolyTuner::partial(int s, int k) const {
  PolyPartial p;
  if (!use_[s][k] || ch_[s][k].fit().points() < 8) return p;
  const PartialChannel::Fit& f = ch_[s][k].fit();
  const double hzPerRadStep = rate_ / (kTwoPi * static_cast<float>(decim_));
  const double resid = std::fmax(f.residualVariance(), 1e-6f);
  const double varHz = resid / std::fmax(f.information(), 1e-20f) * hzPerRadStep * hzPerRadStep;
  const double kk = static_cast<double>(k + 1);
  p.hz = cfg_.tuning[s] + f.slope() * hzPerRadStep / kk;
  p.w = kk * kk / varHz;  // variance of offset/k is varHz / k^2
  p.power = ch_[s][k].power();
  return p;
}

}  // namespace squatch::tuner
