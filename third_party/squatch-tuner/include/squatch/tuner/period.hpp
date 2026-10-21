// SPDX-License-Identifier: MIT
// Coarse period detectors on a block of samples:
//  - Mpm: McLeod & Wyvill's normalised square difference function (NSDF), "A smarter way to
//    find pitch" (ICMC 2005), key-maximum picking with threshold k, parabolic interpolation.
//  - Yin: de Cheveigne & Kawahara's cumulative-mean-normalised difference (JASA 2002),
//    absolute threshold, parabolic interpolation.
// Both are direct O(window * lag) loops: at the ~12 kHz work rate that is cheap enough for a
// Cortex-M7 or an ESP32-S3, and it keeps the code free of an FFT dependency.
#pragma once
#include <algorithm>
#include <array>

#include "common.hpp"

namespace squatch::tuner {

struct PeriodEstimate {
  float period = 0.0f;   // in samples at the rate the block was taken
  float clarity = 0.0f;  // 0..1, how periodic the block is (NSDF peak, or 1 - YIN dip)
  bool valid = false;
};

struct PeriodRange {
  int window = 0;  // samples in the analysis block
  int minLag = 2;  // shortest period searched
  int maxLag = 0;  // longest period searched (< window)
};

template <int MaxLag = Limits::kMaxLag>
class MpmT {
 public:
  void configure(const PeriodRange& r, float keyThreshold = 0.9f) {
    r_ = r;
    k_ = keyThreshold;
  }

  PeriodEstimate analyze(const float* x) {
    computeNsdf(x);
    return pickPeak();
  }

  const float* nsdf() const { return nsdf_.data(); }

 private:
  void computeNsdf(const float* x) {
    const int w = r_.window;
    float m = 0.0f;
    for (int j = 0; j < w; ++j) m += 2.0f * x[j] * x[j];
    for (int tau = 0; tau <= r_.maxLag + 1; ++tau) {
      float r = 0.0f;
      for (int j = 0; j < w - tau; ++j) r += x[j] * x[j + tau];
      nsdf_[tau] = m > 1e-20f ? 2.0f * r / m : 0.0f;
      m -= x[tau] * x[tau] + x[w - 1 - tau] * x[w - 1 - tau];
    }
  }

  // Key maxima: the highest point of each positive lobe after the first negative crossing.
  // The first one within k of the highest is the period (McLeod & Wyvill's rule).
  PeriodEstimate pickPeak() const {
    std::array<int, 64> keys{};
    const int nkeys = keyMaxima(keys);
    float best = 0.0f;
    for (int i = 0; i < nkeys; ++i) best = std::max(best, nsdf_[keys[i]]);
    for (int i = 0; i < nkeys; ++i) {
      if (nsdf_[keys[i]] >= k_ * best) return refine(keys[i]);
    }
    return {};
  }

  int keyMaxima(std::array<int, 64>& keys) const {
    int n = 0, tau = 1;
    while (tau <= r_.maxLag && nsdf_[tau] > 0.0f) ++tau;  // leave the zero-lag lobe
    while (tau <= r_.maxLag && n < 64) {
      while (tau <= r_.maxLag && nsdf_[tau] <= 0.0f) ++tau;
      const int peak = lobePeak(tau);
      if (peak >= r_.minLag) keys[n++] = peak;
    }
    return n;
  }

  // Highest point of the positive lobe starting at tau; leaves tau just past the lobe.
  int lobePeak(int& tau) const {
    int peak = -1;
    for (; tau <= r_.maxLag && nsdf_[tau] > 0.0f; ++tau) {
      if (peak < 0 || nsdf_[tau] > nsdf_[peak]) peak = tau;
    }
    return peak;
  }

  PeriodEstimate refine(int t) const {
    PeriodEstimate e;
    const float d = parabolicOffset(nsdf_[t - 1], nsdf_[t], nsdf_[t + 1]);
    e.period = static_cast<float>(t) + d;
    e.clarity = nsdf_[t];
    e.valid = true;
    return e;
  }

  PeriodRange r_{};
  float k_ = 0.9f;
  std::array<float, MaxLag + 3> nsdf_{};
};

template <int MaxLag = Limits::kMaxLag>
class YinT {
 public:
  void configure(const PeriodRange& r, float threshold = 0.15f) {
    r_ = r;
    thr_ = threshold;
  }

  PeriodEstimate analyze(const float* x) {
    computeCmnd(x);
    return pickDip();
  }

 private:
  void computeCmnd(const float* x) {
    const int w = r_.window - r_.maxLag - 1;  // integration length
    float running = 0.0f;
    d_[0] = 1.0f;
    for (int tau = 1; tau <= r_.maxLag + 1; ++tau) {
      float s = 0.0f;
      for (int j = 0; j < w; ++j) {
        const float diff = x[j] - x[j + tau];
        s += diff * diff;
      }
      running += s;
      d_[tau] = running > 1e-20f ? s * static_cast<float>(tau) / running : 1.0f;
    }
  }

  PeriodEstimate pickDip() const {
    int best = -1;
    for (int tau = r_.minLag; tau <= r_.maxLag; ++tau) {
      if (d_[tau] < thr_) {
        while (tau + 1 <= r_.maxLag && d_[tau + 1] < d_[tau]) ++tau;
        best = tau;
        break;
      }
    }
    if (best < 0) return {};
    PeriodEstimate e;
    e.period = static_cast<float>(best) + parabolicOffset(d_[best - 1], d_[best], d_[best + 1]);
    e.clarity = 1.0f - d_[best];
    e.valid = true;
    return e;
  }

  PeriodRange r_{};
  float thr_ = 0.15f;
  std::array<float, MaxLag + 3> d_{};
};

using Mpm = MpmT<>;
using Yin = YinT<>;

}  // namespace squatch::tuner
