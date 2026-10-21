// SPDX-License-Identifier: MIT
// Numerically controlled oscillator: a 32-bit phase accumulator and a cosine table.
// The reference frequency is exact bookkeeping (fs / 2^32 resolution, about 3e-6 Hz at 12 kHz),
// so the tuner's absolute accuracy never depends on float rounding in the oscillator.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

#include "common.hpp"

namespace squatch::tuner {

class CosTable {
 public:
  static constexpr int kBits = 11;
  static constexpr int kSize = 1 << kBits;

  static const CosTable& instance() {
    static const CosTable t;
    return t;
  }

  /// cos and sin of phase (full scale 2^32 = one turn), linear interpolation.
  void cosSin(uint32_t phase, float& c, float& s) const {
    c = lookup(phase);
    s = lookup(phase - 0x40000000u);  // sin(x) = cos(x - pi/2)
  }

 private:
  CosTable() {
    for (int i = 0; i <= kSize; ++i) t_[i] = static_cast<float>(std::cos(2.0 * 3.141592653589793 * i / kSize));
  }

  float lookup(uint32_t phase) const {
    const uint32_t idx = phase >> (32 - kBits);
    const float frac = static_cast<float>(phase & ((1u << (32 - kBits)) - 1u)) * (1.0f / (1u << (32 - kBits)));
    return t_[idx] + frac * (t_[idx + 1] - t_[idx]);
  }

  std::array<float, kSize + 1> t_{};
};

class Nco {
 public:
  void setFrequency(double hz, double sampleRate) {
    step_ = static_cast<uint32_t>(std::llround(hz / sampleRate * 4294967296.0));
    rate_ = sampleRate;
  }

  /// The frequency the oscillator actually runs at (after step quantisation).
  double frequency() const { return static_cast<double>(step_) * rate_ / 4294967296.0; }

  void reset() { phase_ = 0; }

  /// Current phase (2^32 = one turn); partial k of this reference is phase() * k.
  uint32_t phase() const { return phase_; }
  void advance() { phase_ += step_; }

  /// Returns cos/sin of the current phase, then advances one sample.
  void next(float& c, float& s) {
    CosTable::instance().cosSin(phase_, c, s);
    phase_ += step_;
  }

 private:
  uint32_t phase_ = 0;
  uint32_t step_ = 0;
  double rate_ = 1.0;
};

}  // namespace squatch::tuner
