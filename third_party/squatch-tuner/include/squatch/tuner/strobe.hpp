// SPDX-License-Identifier: MIT
// The strobe bank: a software strobe tuner.
//
// A mechanical strobe lights a spinning pattern with the string's own signal; the pattern
// stands still when the string matches the disc and drifts at a speed set by the error.
// Here, each partial k of the note is mixed with a reference at k * f_ref (complex
// demodulation), lowpassed, and its phase is tracked. Phase drift rate IS the frequency error
// of that partial, which is exactly what a strobe shows; the same numbers drive the display.
//
// Details that matter for accuracy:
//  - One 32-bit phase accumulator drives every partial (partial k uses k * phase), so the
//    references are exact and mutually coherent.
//  - The lowpass is two cascaded boxcars whose length is a whole number of reference periods.
//    A boxcar of one period has a null at every multiple of f_ref, which is where every other
//    partial (and every mirror image) lands after mixing.
//  - Phase is fitted with weights that saturate: p / (p + 1e-3 * peak). Points within ~30 dB of
//    the note's peak count equally (so the loud, pitch-glided attack cannot outvote the settled
//    note), and points far below it (noise before the pluck, the dying tail) fade out.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

#include "common.hpp"
#include "nco.hpp"
#include "phase_slope.hpp"

namespace squatch::tuner {

/// Running sum of the last `len` complex samples (a boxcar lowpass). The sum is rebuilt from
/// the buffer once per wrap so float rounding cannot drift.
class ComplexBoxcar {
 public:
  void configure(int len) {
    len_ = len < 1 ? 1 : (len > Limits::kMaxBoxcar ? Limits::kMaxBoxcar : len);
    reset();
  }

  void reset() {
    re_.fill(0.0f);
    im_.fill(0.0f);
    sr_ = si_ = 0.0f;
    pos_ = 0;
  }

  void push(float xr, float xi, float& outR, float& outI) {
    sr_ += xr - re_[pos_];
    si_ += xi - im_[pos_];
    re_[pos_] = xr;
    im_[pos_] = xi;
    if (++pos_ == len_) {
      pos_ = 0;
      rebuild();
    }
    outR = sr_;
    outI = si_;
  }

 private:
  void rebuild() {
    float a = 0.0f, b = 0.0f;
    for (int i = 0; i < len_; ++i) {
      a += re_[i];
      b += im_[i];
    }
    sr_ = a;
    si_ = b;
  }

  std::array<float, Limits::kMaxBoxcar> re_{}, im_{};
  float sr_ = 0.0f, si_ = 0.0f;
  int len_ = 1;
  int pos_ = 0;
};

/// One partial: mixer, lowpass, phase unwrap and slope fit.
class PartialChannel {
 public:
  void configure(int boxcarLen, int windowPoints) {
    a_.configure(boxcarLen);
    b_.configure(boxcarLen);
    fit_.setWindow(windowPoints);
    reset();
  }

  void reset() {
    a_.reset();
    b_.reset();
    fit_.reset();
    peak_ = 0.0f;
    prevPhase_ = 0.0f;
    zr_ = zi_ = 0.0f;
  }

  /// x times e^{-j theta}, where (c, s) = (cos theta, sin theta).
  void mix(float x, float c, float s) {
    float r1, i1;
    a_.push(x * c, -x * s, r1, i1);
    b_.push(r1, i1, zr_, zi_);
  }

  /// Take a phase sample from the filtered output (called every decimation step).
  void sample() {
    const float power = zr_ * zr_ + zi_ * zi_;
    if (power <= 0.0f) return;
    const float ph = std::atan2(zi_, zr_);
    const float dphase = wrapPi(ph - prevPhase_);
    prevPhase_ = ph;
    peak_ = power > peak_ ? power : peak_;
    fit_.add(dphase, power / (power + 1e-3f * peak_));
  }

  using Fit = PhaseSlopeWindow<Limits::kMaxFitPoints>;
  const Fit& fit() const { return fit_; }
  void setWindow(int points) { fit_.setWindow(points); }
  float phase() const { return prevPhase_; }
  float power() const { return zr_ * zr_ + zi_ * zi_; }

 private:
  ComplexBoxcar a_, b_;
  Fit fit_;
  float zr_ = 0.0f, zi_ = 0.0f;
  float peak_ = 0.0f;
  float prevPhase_ = 0.0f;
};

struct PartialMeasure {
  float offsetHz = 0.0f;   // measured frequency minus k * f_ref
  float information = 0.0f;  // 1 / variance of offsetHz (1/Hz^2), from the fit's own residual
  float power = 0.0f;
  int points = 0;
};

class StrobeBank {
 public:
  /// Tune the bank to a reference frequency. Resets all partial state.
  void start(float refHz, float workRate, int partials, float memorySeconds, float minBoxcarSeconds = 0.0f) {
    rate_ = workRate;
    nco_.setFrequency(refHz, workRate);
    nco_.reset();
    refHz_ = static_cast<float>(nco_.frequency());
    const float period = workRate / refHz_;
    // Whole periods, at least 64 samples and at least minBoxcarSeconds (a longer boxcar has
    // nulls between the harmonics too, which rejects mains hum under low notes, but fills slower).
    const float minLen = std::fmax(64.0f, minBoxcarSeconds * workRate);
    int periods = static_cast<int>(std::ceil(minLen / period));
    while (periods > 1 && period * static_cast<float>(periods) > Limits::kMaxBoxcar) --periods;
    const int len = static_cast<int>(std::lround(period * static_cast<float>(periods)));
    decim_ = len / 4 < 1 ? 1 : len / 4;
    const int maxK = static_cast<int>(0.45f * workRate / refHz_);
    partials_ = partials < maxK ? partials : (maxK < 1 ? 1 : maxK);
    if (partials_ > Limits::kMaxPartials) partials_ = Limits::kMaxPartials;
    maxPoints_ = static_cast<int>(memorySeconds * workRate / static_cast<float>(decim_));
    for (int k = 0; k < partials_; ++k) ch_[k].configure(len, 4);
    counter_ = 0;
    points_ = 0;
  }

  void push(float x) {
    float c1, s1;
    const uint32_t base = nco_.phase();
    for (int k = 0; k < partials_; ++k) {
      CosTable::instance().cosSin(base * static_cast<uint32_t>(k + 1), c1, s1);
      ch_[k].mix(x, c1, s1);
    }
    nco_.advance();
    if (++counter_ < decim_) return;
    counter_ = 0;
    ++points_;
    // The fit covers the later half of the note so far, up to the memory length: the attack
    // (pick noise, the steepest glide, the coarse stage's first guess) ages out quickly, and a
    // sustained note is still averaged over the full memory.
    const int window = points_ / 2 < maxPoints_ ? points_ / 2 : maxPoints_;
    for (int k = 0; k < partials_; ++k) {
      ch_[k].setWindow(window);
      ch_[k].sample();
    }
  }

  /// Frequency offset of partial k and its inverse variance. With lagCompensation the offset is
  /// taken at the newest point (see PhaseSlopeWindow::endSlope), otherwise at the window centre.
  PartialMeasure measure(int k, bool lagCompensation = true) const { return measure(k, lagCompensation ? 1.0f : 0.0f); }

  /// The same, taken `lead` of the way from the window centre (0) to the newest point (1).
  PartialMeasure measure(int k, float lead) const {
    const PartialChannel::Fit& f = ch_[k].fit();
    float slope = f.slope(), variance = 0.0f;
    if (lead > 0.0f) {
      slope = f.slopeAt(lead, variance);
    } else {
      const float r = f.residualVariance() > 1e-6f ? f.residualVariance() : 1e-6f;
      variance = f.information() > 0.0f ? r / f.information() : 1e30f;
    }
    return make(k, slope, variance);
  }

  /// Two measures of partial k from one parabola fit, at leads a and b (both > 0): what the
  /// tuner's steady and fast fits read each hop, for half the arithmetic.
  void measurePair(int k, float leadA, float leadB, PartialMeasure& a, PartialMeasure& b) const {
    PartialChannel::Fit::Reading ra, rb;
    ra.lead = leadA;
    rb.lead = leadB;
    ch_[k].fit().slopesAt(ra, rb);
    a = make(k, ra.slope, ra.variance);
    b = make(k, rb.slope, rb.variance);
  }

  /// Power in odd partials (1, 3, 5, ...) over power in even ones. Near zero means the
  /// reference is an octave below the string: its "odd partials" are empty.
  float oddEvenRatio() const {
    float odd = 0.0f, even = 0.0f;
    for (int k = 0; k < partials_; ++k) (k % 2 == 0 ? odd : even) += ch_[k].power();
    return even > 0.0f ? odd / even : 1.0f;
  }

  /// Phase of partial k against its reference, in turns (0..1). For the display.
  float phaseTurns(int k) const { return ch_[k].phase() / kTwoPi + 0.5f; }

  int partials() const { return partials_; }
  float refHz() const { return refHz_; }

 private:
  PartialMeasure make(int k, float slope, float variance) const {
    const float hzPerRadStep = rate_ / (kTwoPi * static_cast<float>(decim_));
    PartialMeasure m;
    m.offsetHz = slope * hzPerRadStep;
    m.information = 1.0f / (variance * hzPerRadStep * hzPerRadStep + 1e-30f);
    m.power = ch_[k].power();
    m.points = ch_[k].fit().points();
    return m;
  }

  Nco nco_;
  std::array<PartialChannel, Limits::kMaxPartials> ch_{};
  float rate_ = 12000.0f;
  float refHz_ = 0.0f;
  int partials_ = 1;
  int decim_ = 1;
  int counter_ = 0;
  int points_ = 0;
  int maxPoints_ = 64;
};

}  // namespace squatch::tuner
