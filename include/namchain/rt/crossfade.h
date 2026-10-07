#pragma once

// rt::Crossfade -- equal-power crossfade between two mono signals.
//
// The primitive the structural transition of plan.md §5.3 is built from. When a rewire, add/remove,
// scene recall or model swap changes the plan, the engine runs the old and the new plan together for
// `fade_ms` and crossfades their chain outputs before retiring the old one.
//
// **Equal-power, not linear.** Two signals that are not sample-identical sum with their powers, not
// their amplitudes: a linear crossfade (w, 1-w) has w^2 + (1-w)^2 = 0.5 at the midpoint, a 3 dB dip
// in the middle of every transition, which is audible on sustained material. The weights here are
// cos/sin of a quarter turn, so `from^2 + to^2 == 1` across the whole fade. That identity is the
// property the test checks, and it is the reason the shape is what it is rather than what looks
// reasonable.
//
// **But that identity is only the whole story when the two signals are uncorrelated.** The power of
// g1*a + g2*b is g1^2 + g2^2 + 2*rho*g1*g2, which equal power takes to be 1: true at rho = 0, and
// **+3 dB at the midpoint at rho = 1**. A fade between two signals carrying nearly the same audio --
// a bypass of a linear node, an EQ or delay ramp, a Lite/Full handover -- adds energy instead of
// holding it, and that is a thump rather than a click: continuous, no dropout, no excess high
// frequency, and it converges, so none of the glitch detector's four rules can see it. Measured
// 2026-10-07 at +3.0 dB through this class, through `nc.xfade` and through a plan-level transition
// (bench-results/e2-fade-thump.txt), while two *different* rigs (rho -0.7 to +0.5) are the
// uncorrelated case equal power is right for.
//
// So the weights are chosen for the correlation the two signals actually have: `start` takes a rho,
// and the default measures it from the first block it is given. The table for each rho is built in
// the constructor -- five of them, a few hundred floats -- so `start` stays O(1) on the render path
// and the measurement is one block of multiply-adds per fade. Tests: tests/test_crossfade.cpp, which
// measures the excess through this class, through `nc.xfade` and through a plan-level transition. The table divides both weights by
// sqrt(1 + 2*rho*g1*g2), which is equal power at rho = 0 and constant amplitude at rho = 1. rho is
// clamped to [0, 1]: a negative estimate would divide by less than one and *amplify* the fade.
//
// The table is built in the constructor (control thread). `start` and `mix` are render-path calls:
// no allocation, no lock, no syscall.
#include <vector>

namespace namchain
{
namespace rt
{

class Crossfade
{
public:
  /// Pass this as \p rho to `start` to have the correlation measured from the first block mixed.
  static constexpr double kMeasureCorrelation = -1.0;
  /// \param max_frames The longest fade the table covers. A longer `start` is clamped, not refused:
  ///        a fade is a safety mechanism, and refusing one at runtime would be worse than shortening
  ///        it. Sized once, on the control thread.
  explicit Crossfade(int max_frames);

  Crossfade(const Crossfade&) = delete;
  Crossfade& operator=(const Crossfade&) = delete;

  /// Render path: begin a fade over \p frames, clamped to `[1, max_frames]`.
  ///
  /// \param rho The correlation of the two signals about to be mixed, in [0, 1], or
  ///        \ref kMeasureCorrelation (the default) to measure it from the first block. Measuring is
  ///        what the engine's call sites want: what a transition joins is not known when the fade
  ///        starts -- a bypass of a NAM model is two unrelated signals and a bypass of a gain is the
  ///        same audio twice -- and the block is in hand before the output is written, so nothing is
  ///        mixed with the wrong law.
  void start(int frames, double rho = kMeasureCorrelation) noexcept;

  bool active() const noexcept { return remaining_ > 0; }
  int remaining() const noexcept { return remaining_; }
  /// The fade length currently in force, in frames; 0 before the first `start`.
  int length() const noexcept { return length_; }
  /// The correlation in force: what `start` was given, or what the first block measured.
  double correlation() const noexcept { return rho_; }
  int max_frames() const noexcept { return max_frames_; }

  /// Render path: mix \p a (fading out) and \p b (fading in) into \p out.
  ///
  /// Once the fade closes inside the block the tail is \p b alone, so a caller does not need a
  /// separate post-fade path: the same call that finishes the fade also produces the settled signal.
  void mix(const float* a, const float* b, float* out, int frames) noexcept;

  /// Abandon the fade with the new signal at full weight (used when a transition is interrupted).
  void finish() noexcept
  {
    position_ = length_;
    remaining_ = 0;
  }

private:
  std::vector<float> from_; ///< equal-power weight for the fading-out signal, by position
  std::vector<float> to_;   ///< equal-power weight for the fading-in signal
  int max_frames_ = 0;
  int length_ = 0;
  int position_ = 0;
  int remaining_ = 0;
  float rho_ = 0.0f;       ///< what the weights are normalised for, in [0, 1]
  bool measure_ = false;   ///< the next `mix` measures the correlation from its block
};

} // namespace rt
} // namespace namchain
