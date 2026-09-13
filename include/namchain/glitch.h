// The glitch detector: four measurements that turn "it does not click" into a number.
//
// plan.md §10.3 makes this the headline test, and it is written before the transition machinery it
// judges, because a transition you cannot measure is a transition you cannot claim. The shape of the
// API follows from that order: this header knows nothing about ramps, plans or patch protocols. It
// takes three signals over one span -- the render with an edit applied, and the two oracles it must
// agree with on either side -- plus where the edits are, and answers four questions about them.
//
//   y       the engine's output, edits applied
//   before  the same span rendered through the chain as it was before the edit
//   after   the same span rendered through the chain as it is after the edit
//
// Four rules, each of which a real transition bug trips (§10.3):
//
//   1. Continuity   a step, a polarity flip, a mute, an unramped parameter all show up as a second
//                   difference larger than anything the signal itself produces.
//   2. Dropout      a transition that passes through silence (or drops a block) leaves a run of
//                   digital black where the oracle has energy, or changes the sample count.
//   3. Spectral     a transition that is continuous but not band-limited (a hard switch, a
//                   discontinuity in a derivative) puts energy above 5 kHz that neither oracle has.
//   4. Convergence  the render must actually be the old chain before the edit and the new chain
//                   after it, not merely a smooth interpolation between them.
//
// The span is analysed, not the whole file, because a glitch is local: `namchain-render` renders a
// short window around each edit and hands it here, which keeps memory bounded on a 500-edit torture
// render and lets the oracle renders be local too. The caller is responsible for the one thing a
// span cannot see: that the render's total frame count equals the input's (rule 2's "no repeats or
// insertions"), which is checked where the file is written.
//
// Everything here is ordinary floating point on a control thread. None of it is RT code.
#ifndef NAMCHAIN_GLITCH_H
#define NAMCHAIN_GLITCH_H

#include <cstddef>
#include <string>
#include <vector>

namespace namchain
{
namespace glitch
{

/// One edit inside the analysed span, in frames.
///
/// `end` is one past the last frame the transition is allowed to touch, so a crossfade of F frames
/// starting at t is `{t, t + F}`. The detector never assumes `end - start` takes a particular value:
/// a legal edit may be instantaneous (`end == start`), which is exactly the case rule 1 exists to
/// catch.
struct Edit
{
  size_t start = 0;
  size_t end = 0;
};

struct Options
{
  double sample_rate = 48000.0;

  /// Rule 1. A second difference inside an edit window may be this many times the largest one the
  /// oracles produce outside every window. 2 is §10.3's value and is deliberately not 1: a fade
  /// changes the signal's slope on purpose, so the reference is a floor, not an equality.
  double kappa = 2.0;

  /// Rule 2, in dBFS, and how long a run of it is tolerated.
  double dropout_db = -90.0;
  double dropout_max_ms = 1.0;

  /// Rule 3: a zero-phase high pass at this corner, RMS compared over a window this wide either
  /// side of each edit, tolerated this far above the louder oracle.
  double hf_highpass_hz = 5000.0;
  double hf_window_ms = 10.0;
  double hf_allowance_db = 3.0;

  /// Rule 4: the largest |y - oracle| allowed inside the settled regions, in dBFS, and how far
  /// either side of an edit those regions begin.
  double convergence_db = -60.0;
  double guard_ms = 5.0;
};

/// One failed (or borderline reported) measurement, with the number behind it.
struct Finding
{
  std::string rule;   ///< "continuity", "dropout", "hf", "convergence"
  size_t frame = 0;   ///< where in the span, for the first offending sample
  double measured = 0.0;
  double limit = 0.0;
  std::string detail; ///< human-readable, with units
};

struct Report
{
  bool ok = true;
  size_t frames = 0;
  size_t edits = 0;
  std::vector<Finding> findings;

  /// Rule 1's numbers: the continuity reference taken from the oracles outside the edit windows, and
  /// the largest second difference found inside one.
  double reference_d2 = 0.0;
  double max_edit_d2 = 0.0;
  /// False when the span contains no signal to derive a continuity reference from -- a silent span
  /// is not a passing one, it is an unmeasurable one, and saying so is the point.
  bool continuity_evaluated = false;

  /// Rule 2: the longest run of digital black where an oracle has energy, in milliseconds.
  double max_dropout_ms = 0.0;
  /// Rule 3: the worst excess of high-frequency energy over the louder oracle, in dB. Never
  /// negative; 0 means the render never exceeded the oracles, or that no window had an oracle with
  /// energy in it to compare against.
  double max_hf_excess_db = 0.0;
  /// Rule 4: the worst settled disagreement with either oracle, in dBFS. -1000 is the sentinel for
  /// "bit-identical, or nothing settled to compare" -- a difference of exactly zero has no dBFS
  /// value and JSON has no infinity to carry one.
  double worst_convergence_dbfs = -1000.0;
};

/// Analyse one span.
///
/// `y`, `before` and `after` must each hold `frames` samples of the same span, every edit must lie
/// inside it with `start <= end`, and edits must appear in order and not overlap -- a timeline is a
/// sequence, and silently sorting one would hide a caller's bug. An edit whose transition reaches
/// the edge of the span cannot be judged: the detector would be reading the middle of a fade as if
/// it were settled, so a span that does not leave a settled region beside every edit is an error
/// rather than a pass. Edits that are back to back *inside* the span are legal; the region between
/// them is simply too short to judge, and is skipped rather than failed.
///
/// On a rule that cannot be evaluated for lack of signal (a silent span, an oracle that is exactly
/// zero where the edit is), the report says so through `continuity_evaluated` and the per-rule
/// numbers instead of inventing a verdict. An error never leaves `ok` true.
Report analyse(const float* y, const float* before, const float* after, size_t frames,
               const std::vector<Edit>& edits, const Options& options, std::string& error);

} // namespace glitch
} // namespace namchain

#endif // NAMCHAIN_GLITCH_H
