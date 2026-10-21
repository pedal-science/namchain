// SPDX-License-Identifier: MIT
// Integer-factor FIR decimator (windowed-sinc lowpass). The tuner does all its work at
// roughly 12 kHz: guitar and bass partials worth tracking sit well below 5 kHz, and running at a
// quarter of 48 kHz cuts every downstream cost by four.
#pragma once
#include <array>
#include <cmath>

#include "common.hpp"

namespace squatch::tuner {

class Decimator {
 public:
  static constexpr int kMaxTaps = Limits::kMaxDecimation * Limits::kDecimTapsPerPhase;

  /// factor 1 passes samples through untouched.
  void configure(int factor) {
    factor_ = factor < 1 ? 1 : (factor > Limits::kMaxDecimation ? Limits::kMaxDecimation : factor);
    taps_ = factor_ == 1 ? 1 : factor_ * Limits::kDecimTapsPerPhase;
    designLowpass();
    reset();
  }

  void reset() {
    line_.fill(0.0f);
    pos_ = 0;
    phase_ = 0;
  }

  int factor() const { return factor_; }

  /// Feed one input sample. Returns true when `out` holds a new decimated sample.
  bool push(float x, float& out) {
    line_[pos_] = x;
    line_[pos_ + kMaxTaps] = x;
    pos_ = (pos_ + 1) % kMaxTaps;
    if (++phase_ < factor_) return false;
    phase_ = 0;
    const float* p = &line_[pos_ + kMaxTaps - taps_];
    float acc = 0.0f;
    for (int i = 0; i < taps_; ++i) acc += p[i] * h_[i];
    out = acc;
    return true;
  }

 private:
  // Blackman-windowed sinc, cutoff at 0.8 of the output Nyquist, unity DC gain.
  void designLowpass() {
    if (taps_ == 1) {
      h_[0] = 1.0f;
      return;
    }
    const float fc = 0.4f / static_cast<float>(factor_);  // cycles per input sample
    const float mid = 0.5f * static_cast<float>(taps_ - 1);
    float sum = 0.0f;
    for (int i = 0; i < taps_; ++i) {
      const float t = static_cast<float>(i) - mid;
      const float sinc = std::fabs(t) < 1e-6f ? 2.0f * fc : std::sin(kTwoPi * fc * t) / (kPi * t);
      const float w = static_cast<float>(i) / static_cast<float>(taps_ - 1);
      const float win = 0.42f - 0.5f * std::cos(kTwoPi * w) + 0.08f * std::cos(2.0f * kTwoPi * w);
      h_[i] = sinc * win;
      sum += h_[i];
    }
    for (int i = 0; i < taps_; ++i) h_[i] /= sum;
  }

  std::array<float, kMaxTaps> h_{};
  std::array<float, 2 * kMaxTaps> line_{};
  int factor_ = 1;
  int taps_ = 1;
  int pos_ = 0;
  int phase_ = 0;
};

}  // namespace squatch::tuner
