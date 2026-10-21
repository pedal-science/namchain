// SPDX-License-Identifier: MIT
// Weighted least-squares line fit of phase against time over a sliding window.
//
// A frequency offset is a phase that grows linearly with time, so the slope of unwrapped phase
// against time is the offset. At high SNR this is the maximum-likelihood frequency estimator,
// and its variance falls as T^-3 with the observation time T (Rife & Boorstyn 1974).
//
// The caller sets the window length (it may change as the note ages); points beyond it slide
// out. A sliding window has no tail: an exponential memory keeps a little of the sharp, pitch-glided attack for a
// long time, and a line fit gives those old points a lot of leverage. Sums are updated in O(1)
// per point and rebuilt from the stored points once per window, re-based to the oldest point,
// so rounding never accumulates. Points are equally spaced, so only phase and weight are stored.
//
// The fit also reports its residual phase variance. A partial that is clean has a tiny residual;
// one that is beating against hum, a neighbouring string or noise has a large one, and the
// partial fit weights each partial by its own measured inverse variance.
//
// Lag: a line fit reports the frequency at the window's centre, which trails a gliding note by
// half a window. endSlope() also fits a parabola (a glide is a phase with curvature), takes its
// slope at the newest point, and moves the linear answer towards it by d^2 / (d^2 + var d):
// fully when the curvature is significant, hardly at all when it is noise. slopeAt() does the same
// at a point part of the way from the centre to the newest point: less lag than the line, less
// variance than the end.
//
// Sums are double: points arrive only every quarter period (a few hundred per second per
// partial), so even soft-float doubles on an ESP32-S3 cost well under 1% of a core, and the
// residual needs the precision.
#pragma once
#include <array>

namespace squatch::tuner {

template <int Cap>
class PhaseSlopeWindow {
 public:
  /// Floor on the phase residual variance (rad^2, about 0.06 degrees rms), so a perfect fit
  /// cannot claim infinite confidence.
  static constexpr double kMinResidual = 1e-6;

  /// Longest window. It may shrink or grow between points; excess old points are dropped.
  void setWindow(int points) {
    window_ = points < 4 ? 4 : (points > Cap ? Cap : points);
    while (count_ > window_) removeOldest();
  }

  void reset() {
    s0_ = st_ = stt_ = sp_ = stp_ = spp_ = st3_ = st4_ = st2p_ = 0.0;
    t0_ = phase_ = 0.0;
    head_ = count_ = sinceRebuild_ = added_ = 0;
  }

  /// Add the next point, given the (wrapped) phase change since the previous one.
  void add(float dphase, float weight) {
    if (count_ == window_) removeOldest();
    phase_ += dphase;
    const int slot = (head_ + count_) % Cap;
    p_[slot] = phase_;
    w_[slot] = weight;
    accumulate(t0_ + count_, phase_, weight);
    ++count_;
    ++added_;
    if (++sinceRebuild_ >= window_) rebuild();
  }

  /// Phase slope in radians per step (positive = the signal is above the reference).
  float slope() const {
    const double den = s0_ * stt_ - st_ * st_;
    return den > 1e-20 * s0_ * s0_ ? static_cast<float>((s0_ * stp_ - st_ * sp_) / den) : 0.0f;
  }

  /// Weighted spread of the sample times: slope variance = residualVariance() / information().
  float information() const { return s0_ > 0.0 ? static_cast<float>(stt_ - st_ * st_ / s0_) : 0.0f; }

  /// Weighted mean squared phase residual about the fitted line (rad^2).
  float residualVariance() const {
    if (s0_ <= 0.0) return 0.0f;
    const double ttc = stt_ - st_ * st_ / s0_, tpc = stp_ - st_ * sp_ / s0_, ppc = spp_ - sp_ * sp_ / s0_;
    const double rss = ttc > 0.0 ? ppc - tpc * tpc / ttc : ppc;
    return rss > 0.0 ? static_cast<float>(rss / s0_) : 0.0f;
  }

  int points() const { return added_; }

  /// Slope at the newest point: the line fit, corrected towards the parabola's end slope in
  /// proportion to how significant that correction is. `variance` gets its variance (rad/step)^2.
  float endSlope(float& variance) const { return slopeAt(1.0f, variance); }

  /// The same at the point `lead` of the way from the window's weighted centre (0: the line fit's
  /// own point) to the newest point (1: endSlope).
  float slopeAt(float lead, float& variance) const { return evaluate(quadratic(), lead, variance); }

  /// A slope wanted at `lead`, and what it came to.
  struct Reading {
    float lead = 1.0f, slope = 0.0f, variance = 0.0f;
  };

  /// Two slopeAt() readings from one parabola fit (the tuner's steady and fast fits, each hop).
  void slopesAt(Reading& a, Reading& b) const {
    const Quad q = quadratic();
    a.slope = evaluate(q, a.lead, a.variance);
    b.slope = evaluate(q, b.lead, b.variance);
  }

 private:
  struct Quad {
    double suu = 0, sup = 0, sigma2 = 0, b = 0, c = 0, ue = 0, ibb = 0, icc = 0, ibc = 0;
    bool ok = false;
  };

  float evaluate(const Quad& q, float lead, float& variance) const {
    if (!q.ok) {
      const float r = residualVariance() > kMinResidual ? residualVariance() : static_cast<float>(kMinResidual);
      variance = information() > 0.0f ? r / information() : 1e30f;
      return slope();
    }
    const double ue = lead * q.ue, endSlope = q.b + 2 * q.c * ue;
    const double varEnd = q.sigma2 * (q.ibb + 4 * ue * q.ibc + 4 * ue * ue * q.icc);
    const double lin = q.sup / q.suu, varLin = q.sigma2 / q.suu;
    const double d = endSlope - lin, varD = varEnd > varLin ? varEnd - varLin : 0.0;
    const double shrink = d * d / (d * d + varD + 1e-30);
    variance = static_cast<float>(varLin + shrink * shrink * varD);
    return static_cast<float>(lin + shrink * d);
  }

  // Weighted quadratic fit about the weighted mean time: what evaluate() needs for its slope and
  // that slope's variance at any point (ue: from the mean time to the newest point). See endSlope().
  Quad quadratic() const {
    Quad q;
    if (s0_ <= 0.0 || count_ < 6) return q;
    const double m = st_ / s0_, m2 = m * m;
    q.suu = stt_ - st_ * m;
    const double s3 = st3_ - 3 * m * stt_ + 2 * m2 * st_;
    const double s4 = st4_ - 4 * m * st3_ + 6 * m2 * stt_ - 3 * m2 * m * st_;
    q.sup = stp_ - m * sp_;
    const double suup = st2p_ - 2 * m * stp_ + m2 * sp_;
    // Normal equations [s0 0 suu; 0 suu s3; suu s3 s4] (a b c) = (sp sup suup).
    const double det = s0_ * (q.suu * s4 - s3 * s3) - q.suu * q.suu * q.suu;
    if (q.suu <= 0.0 || det <= 1e-12 * s0_ * q.suu * s4) return q;
    const double a = (sp_ * (q.suu * s4 - s3 * s3) + q.suu * (q.sup * s3 - q.suu * suup)) / det;
    q.b = (s0_ * (q.sup * s4 - s3 * suup) - q.suu * (q.suu * q.sup) + q.suu * s3 * sp_) / det;
    q.c = (s0_ * (q.suu * suup - s3 * q.sup) - q.suu * q.suu * sp_) / det;
    const double rss = spp_ - a * sp_ - q.b * q.sup - q.c * suup;
    q.sigma2 = rss / s0_ > kMinResidual ? rss / s0_ : kMinResidual;
    q.ue = (t0_ + count_ - 1) - m;
    // var(b + 2 c ue) = sigma2 * g' M^-1 g with g = (0, 1, 2 ue); cofactors of the symmetric M.
    q.ibb = (s0_ * s4 - q.suu * q.suu) / det;
    q.icc = (s0_ * q.suu) / det;
    q.ibc = -(s0_ * s3) / det;
    q.ok = true;
    return q;
  }

  void accumulate(double t, double p, double w) {
    s0_ += w;
    st_ += w * t;
    stt_ += w * t * t;
    sp_ += w * p;
    stp_ += w * t * p;
    spp_ += w * p * p;
    st3_ += w * t * t * t;
    st4_ += w * t * t * t * t;
    st2p_ += w * t * t * p;
  }

  void removeOldest() {
    accumulate(t0_, p_[head_], -w_[head_]);
    head_ = (head_ + 1) % Cap;
    t0_ += 1.0;
    --count_;
  }

  // Re-base time and phase to the oldest point and recompute the sums exactly.
  void rebuild() {
    const double p0 = p_[head_];
    s0_ = st_ = stt_ = sp_ = stp_ = spp_ = st3_ = st4_ = st2p_ = 0.0;
    for (int n = 0; n < count_; ++n) {
      const int i = (head_ + n) % Cap;
      p_[i] -= p0;
      accumulate(n, p_[i], w_[i]);
    }
    phase_ -= p0;
    t0_ = 0.0;
    sinceRebuild_ = 0;
  }

  std::array<double, Cap> p_{};
  std::array<float, Cap> w_{};
  double s0_ = 0, st_ = 0, stt_ = 0, sp_ = 0, stp_ = 0, spp_ = 0, st3_ = 0, st4_ = 0, st2p_ = 0;
  double t0_ = 0.0, phase_ = 0.0;
  int window_ = 64, head_ = 0, count_ = 0, sinceRebuild_ = 0, added_ = 0;
};

}  // namespace squatch::tuner
