// SPDX-License-Identifier: MIT
// Combine per-partial frequency measurements into one fundamental.
//
// A real string is stiff, so its partials are stretched: f_k = k f0 sqrt(1 + B k^2)
// (Fletcher & Rossing). With B = 1e-4, partial 4 is already 1.4 cents sharp of 4 * f0, so a
// tuner that averages harmonics without a stiffness term reads sharp. Using
// f_k / k ~= f0 + c k^2 (c = f0 B / 2), a two-parameter weighted least-squares fit returns the
// fundamental the string actually has, plus B. Weights are k^2 * information: dividing a partial
// by k divides its error by k, which is why upper partials are worth tracking at all.
#pragma once
#include <algorithm>
#include <cmath>

#include "strobe.hpp"

namespace squatch::tuner {

struct PartialFit {
  float f0 = 0.0f;
  float sigmaHz = 0.0f;        // standard error of f0 (optimistic: ignores correlated errors)
  float inharmonicity = 0.0f;  // B
  float spreadHz = 0.0f;       // weighted RMS of the partials' f0 estimates about the fit
  int used = 0;                // partials that passed the power gate
  bool valid = false;
};

struct FitOptions {
  bool fitInharmonicity = true;
  float minRelativePower = 1e-5f;  // ignore partials 50 dB below the strongest (weights do the rest)
  int minPoints = 8;               // phase samples (4 per period) a partial needs before it counts
  float maxInharmonicity = 2e-3f;
  bool lagCompensation = true;     // read each partial at the newest point, not the window centre
  float fixedInharmonicity = -1.0f;  // >= 0: use this B and fit f0 alone
};

namespace detail {
struct Sums {
  double w = 0, wk2 = 0, wk4 = 0, wy = 0, wyk2 = 0;
  void add(double weight, double k2, double y) {
    w += weight;
    wk2 += weight * k2;
    wk4 += weight * k2 * k2;
    wy += weight * y;
    wyk2 += weight * y * k2;
  }
};

inline float strongestPower(const PartialMeasure* m, int n) {
  float p = 0.0f;
  for (int k = 0; k < n; ++k) p = m[k].power > p ? m[k].power : p;
  return p;
}

/// A fitted line y_k = f0 + c k^2 and the total weight behind it.
struct Line {
  double f0 = 0.0, c = 0.0, w = 0.0;
};

/// How far the partials' own estimates of f0 (y_k minus the stiffness term) lie from the fitted
/// f0: the weighted RMS, in Hz. The fit's sigma comes from each partial's phase residual alone,
/// so two partials that each look clean but follow different sources (a sympathetic string, the
/// next note, hum) still claim a tiny sigma; this is what shows they disagree.
inline double spread(const PartialMeasure* ms, int n, double refHz, unsigned use, const Line& l) {
  double rss = 0.0;
  for (int k = 1; k <= n; ++k) {
    if (!(use & (1u << k))) continue;
    const double k2 = static_cast<double>(k) * k;
    const double r = refHz + ms[k - 1].offsetHz / static_cast<double>(k) - (l.f0 + l.c * k2);
    rss += k2 * ms[k - 1].information * r * r;
  }
  return l.w > 0.0 ? std::sqrt(rss / l.w) : 0.0;
}

/// Weighted least squares for y_k = f0 + c k^2 over the partials in `use`.
inline PartialFit solve(const PartialMeasure* ms, int n, double refHz, unsigned use, const FitOptions& o) {
  Sums s;
  PartialFit r;
  for (int k = 1; k <= n; ++k) {
    if (!(use & (1u << k))) continue;
    const double k2 = static_cast<double>(k) * k;
    s.add(k2 * ms[k - 1].information, k2, refHz + ms[k - 1].offsetHz / static_cast<double>(k));
    ++r.used;
  }
  if (r.used == 0 || s.w <= 0.0) return r;
  double f0 = s.wy / s.w, c = 0.0, var = 1.0 / s.w;
  const double det = s.w * s.wk4 - s.wk2 * s.wk2;
  const bool freeB = o.fitInharmonicity && r.used >= 3 && det > 1e-12 * s.w * s.wk4;
  if (o.fixedInharmonicity >= 0.0f) {
    c = 0.5 * o.fixedInharmonicity * f0;
  } else if (freeB) {
    var = s.wk4 / det;
    c = std::clamp((s.w * s.wyk2 - s.wk2 * s.wy) / det, 0.0, 0.5 * o.maxInharmonicity * f0);
  }
  f0 = (s.wy - c * s.wk2) / s.w;  // best f0 for that stiffness
  r.f0 = static_cast<float>(f0);
  r.sigmaHz = static_cast<float>(std::sqrt(var));
  r.inharmonicity = static_cast<float>(2.0 * c / f0);
  r.spreadHz = static_cast<float>(spread(ms, n, refHz, use, {f0, c, s.w}));
  r.valid = true;
  return r;
}

/// Robust weights (a Cauchy weight): a partial's weight falls as 1 / (1 + (r / 5 c)^2) with its
/// distance r from the model, so hum or a neighbouring string tens of cents off counts for almost
/// nothing. The scale is wide and the weight continuous on purpose. The old rule dropped the worst
/// partial once it was 1 c and chi^2 16 out, and real strings' partials often disagree by a few
/// cents (EGFxSet Bridge 4-14: partial 2 sits 4.5 c above partial 1 whatever B is), so it chose
/// one partial on some hops and the other on the rest, and the number flipped between two values
/// 5 c apart several times a second. A narrow scale does the same. At 5 c they are averaged.
inline void downweightOutliers(PartialMeasure* ms, int n, double refHz, unsigned use, const PartialFit& f) {
  for (int k = 1; k <= n; ++k) {
    if (!(use & (1u << k))) continue;
    PartialMeasure& m = ms[k - 1];
    const double model = k * f.f0 * (1.0 + 0.5 * f.inharmonicity * k * k);
    const double resid = refHz * k + m.offsetHz - model;  // Hz, partial k
    const double r0 = 2.9e-3 * model;                     // 5 cents
    m.information = static_cast<float>(m.information / (1.0 + (resid * resid) / (r0 * r0)));
  }
}
}  // namespace detail

/// m[k-1] describes partial k measured against a reference at k * refHz. Partials that are too
/// weak or too new are skipped; partials that disagree with the rest (hum, a neighbouring string)
/// are weighted down (detail::downweightOutliers, twice) and the fit is redone.
inline PartialFit fitPartials(const PartialMeasure* ms, int n, double refHz, const FitOptions& o) {
  const float gate = detail::strongestPower(ms, n) * o.minRelativePower;
  unsigned use = 0;
  std::array<PartialMeasure, Limits::kMaxPartials> w{};
  for (int k = 1; k <= n; ++k) {
    const PartialMeasure& m = ms[k - 1];
    w[static_cast<std::size_t>(k - 1)] = m;
    if (m.points >= o.minPoints && m.power >= gate && m.information > 0.0f) use |= 1u << k;
  }
  PartialFit r = detail::solve(ms, n, refHz, use, o);
  for (int pass = 0; pass < 2 && r.valid && r.used >= 3; ++pass) {
    detail::downweightOutliers(w.data(), n, refHz, use, r);
    r = detail::solve(w.data(), n, refHz, use, o);
  }
  return r;
}

/// The partials of one string agree on its f0 to within a cent or so (p99 0.3 to 1.0 c on clean
/// EGFxSet and owner notes). When they do not, some follow something else (hum as the note sinks
/// into it, a string ringing beside it), and f0 follows whichever partial the weights favour: it
/// slid 6 c on our open-E take from 11 s and hopped 20 c on Guitar-TECHS notes.
inline bool partialsAgree(const PartialFit& f, float maxCents) { return 1731.23f * f.spreadHz <= maxCents * f.f0; }  // 1200 / ln 2

inline PartialFit fitPartials(const StrobeBank& bank, const FitOptions& o) {
  std::array<PartialMeasure, Limits::kMaxPartials> m{};
  for (int k = 0; k < bank.partials(); ++k) m[k] = bank.measure(k, o.lagCompensation);
  return fitPartials(m.data(), bank.partials(), bank.refHz(), o);
}

}  // namespace squatch::tuner
