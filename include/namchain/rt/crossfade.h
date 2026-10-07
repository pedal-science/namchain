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
// **+3 dB at the midpoint at rho = 1**. So a fade between two signals that carry nearly the same
// audio -- a Lite/Full handover, a bypass toggle's wet and dry, a model swap within a slot -- adds
// energy instead of holding it, and it is a thump rather than a click: continuous, no dropout, no
// excess high frequency and it converges, so none of the glitch detector's four rules can see it.
// Measured 2026-10-07 on 104 of them: +2.6 to +3.0 dB on every model, where two *different* rigs
// (rho -0.7 to +0.5) are the uncorrelated case this law is right for. The fix is to divide both
// weights by sqrt(1 + 2*rho*g1*g2) with rho clamped to [0, 1] -- one extra sqrt per table entry,
// since the table is already built here in `start()` -- and it wants its own commit and transition
// tests rather than being slipped in. bench-results/e2-fade-thump.txt is the measurement.
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
  /// \param max_frames The longest fade the table covers. A longer `start` is clamped, not refused:
  ///        a fade is a safety mechanism, and refusing one at runtime would be worse than shortening
  ///        it. Sized once, on the control thread.
  explicit Crossfade(int max_frames);

  Crossfade(const Crossfade&) = delete;
  Crossfade& operator=(const Crossfade&) = delete;

  /// Render path: begin a fade over \p frames, clamped to `[1, max_frames]`.
  void start(int frames) noexcept;

  bool active() const noexcept { return remaining_ > 0; }
  int remaining() const noexcept { return remaining_; }
  /// The fade length currently in force, in frames; 0 before the first `start`.
  int length() const noexcept { return length_; }
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
  std::vector<float> from_; ///< weight for the fading-out signal, indexed by position
  std::vector<float> to_;   ///< weight for the fading-in signal
  int max_frames_ = 0;
  int length_ = 0;
  int position_ = 0;
  int remaining_ = 0;
};

} // namespace rt
} // namespace namchain
