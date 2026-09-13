// The four measurements behind namchain/glitch.h. See that header for the shape of the API and
// plan.md §10.3 for why each rule exists.
#include "namchain/glitch.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace namchain
{
namespace glitch
{
namespace
{

constexpr double kPi = 3.14159265358979323846;

double db_to_linear(double db)
{
  return std::pow(10.0, db / 20.0);
}

double linear_to_db(double linear)
{
  return linear > 0.0 ? 20.0 * std::log10(linear) : -std::numeric_limits<double>::infinity();
}

size_t ms_to_frames(double milliseconds, double rate)
{
  if (milliseconds <= 0.0)
    return 0;
  return static_cast<size_t>(milliseconds * rate / 1000.0 + 0.5);
}

/// A half-open frame range.
struct Region
{
  size_t first = 0;
  size_t last = 0;

  bool empty() const { return last <= first; }
  size_t size() const { return last > first ? last - first : 0; }
};

bool contains(const std::vector<Region>& regions, size_t frame)
{
  for (const Region& r : regions)
    if (frame >= r.first && frame < r.last)
      return true;
  return false;
}

/// Sort and coalesce, so that a rule that reports one finding per window does not report twelve for
/// twelve edits that happen to overlap.
std::vector<Region> coalesce(std::vector<Region> regions)
{
  std::sort(regions.begin(), regions.end(),
            [](const Region& a, const Region& b) { return a.first < b.first; });
  std::vector<Region> merged;
  for (const Region& r : regions)
  {
    if (r.empty())
      continue;
    if (!merged.empty() && r.first <= merged.back().last)
      merged.back().last = std::max(merged.back().last, r.last);
    else
      merged.push_back(r);
  }
  return merged;
}

/// The second difference d2[n] = |v[n] - 2 v[n-1] + v[n-2]|, and the frame it peaks at.
struct Peak
{
  double value = 0.0;
  size_t frame = 0;
};

Peak max_second_difference(const float* v, size_t frames, const std::vector<Region>& regions, bool inside)
{
  Peak peak;
  for (size_t n = 2; n < frames; ++n)
  {
    if (contains(regions, n) != inside)
      continue;
    const double d2 = std::abs(static_cast<double>(v[n]) - 2.0 * static_cast<double>(v[n - 1]) +
                               static_cast<double>(v[n - 2]));
    if (d2 > peak.value)
    {
      peak.value = d2;
      peak.frame = n;
    }
  }
  return peak;
}

/// Second-order Butterworth, applied forward and then backward.
///
/// Zero phase on purpose. Rule 3 compares high-frequency energy between two signals, and a single
/// pass would delay one relative to the other, turning a phase difference into an apparent energy
/// difference -- and the two oracles are not guaranteed to be time-aligned with the render to the
/// sample once a transition has moved state around.
struct Biquad
{
  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a1 = 0.0;
  double a2 = 0.0;

  static Biquad high_pass(double corner_hz, double rate)
  {
    const double w0 = 2.0 * kPi * corner_hz / rate;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752440); // Q = 1/sqrt(2) is Butterworth
    const double a0 = 1.0 + alpha;

    Biquad f;
    f.b0 = (1.0 + cos_w0) / 2.0 / a0;
    f.b1 = -(1.0 + cos_w0) / a0;
    f.b2 = (1.0 + cos_w0) / 2.0 / a0;
    f.a1 = -2.0 * cos_w0 / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
  }

  /// One causal pass, in place.
  void apply(std::vector<double>& x) const
  {
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (double& sample : x)
    {
      const double in = sample;
      const double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
      x2 = x1;
      x1 = in;
      y2 = y1;
      y1 = out;
      sample = out;
    }
  }

  std::vector<double> forward_backward(const float* input, size_t frames) const
  {
    std::vector<double> out(static_cast<size_t>(frames));
    for (size_t i = 0; i < frames; ++i)
      out[i] = static_cast<double>(input[i]);
    apply(out);
    std::reverse(out.begin(), out.end());
    apply(out);
    std::reverse(out.begin(), out.end());
    return out;
  }
};

double rms(const std::vector<double>& x, const Region& region)
{
  if (region.empty())
    return 0.0;
  double sum = 0.0;
  for (size_t i = region.first; i < region.last; ++i)
    sum += x[i] * x[i];
  return std::sqrt(sum / static_cast<double>(region.size()));
}

/// Rule 4's measurement: the largest disagreement with an oracle over a settled region, and where.
Peak max_abs_difference(const float* a, const float* b, const Region& region)
{
  Peak peak;
  for (size_t i = region.first; i < region.last; ++i)
  {
    const double difference = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
    if (difference > peak.value)
    {
      peak.value = difference;
      peak.frame = i;
    }
  }
  return peak;
}

} // namespace

Report analyse(const float* y, const float* before, const float* after, size_t frames,
               const std::vector<Edit>& edits, const Options& options, std::string& error)
{
  Report report;
  report.frames = frames;
  report.edits = edits.size();

  // An error is never a pass: the report says so itself rather than relying on every caller to
  // remember to read `error` as well.
  const auto reject = [&report, &error](std::string message) {
    error = std::move(message);
    report.ok = false;
    return report;
  };

  if (y == nullptr || before == nullptr || after == nullptr)
    return reject("glitch::analyse needs three signals");
  if (frames == 0)
    return reject("glitch::analyse needs at least one frame");
  if (!(options.sample_rate > 0.0))
    return reject("glitch::analyse needs a positive sample rate");

  // Rule 4's guard, computed here because it is also what makes an edit judgeable at all: an edit
  // whose transition reaches the edge of the span leaves nothing settled beside it.
  const size_t guard = ms_to_frames(options.guard_ms, options.sample_rate);
  for (size_t i = 0; i < edits.size(); ++i)
  {
    if (edits[i].start > edits[i].end)
      return reject("glitch::analyse: edit " + std::to_string(i) + " ends before it starts");
    if (i > 0 && edits[i].start < edits[i - 1].end)
      return reject("glitch::analyse: edits " + std::to_string(i - 1) + " and " + std::to_string(i) +
                    " overlap; a timeline is a sequence");
    if (edits[i].end > frames)
      return reject("glitch::analyse: edit " + std::to_string(i) + " reaches frame " +
                    std::to_string(edits[i].end) + " of a " + std::to_string(frames) + "-frame span");
    if (edits[i].start < guard || edits[i].end + guard > frames)
      return reject("glitch::analyse: edit " + std::to_string(i) + " at frames " + std::to_string(edits[i].start) +
                    ".." + std::to_string(edits[i].end) + " leaves no settled region inside a " +
                    std::to_string(frames) + "-frame span; widen the span to at least the guard (" +
                    std::to_string(guard) + " frames) either side");
  }

  // -------------------------------------------------------------------------------------------
  // 1. Continuity. Every edit may move the second difference in [start - 2, end + 2), because d2[n]
  //    reads n-2 through n. The reference is what the two oracles produce everywhere else: if the
  //    signal itself never turns faster than that, anything that turns faster is the transition.
  // -------------------------------------------------------------------------------------------
  std::vector<Region> touched;
  touched.reserve(edits.size());
  for (const Edit& edit : edits)
    touched.push_back(Region{edit.start > 2 ? edit.start - 2 : 0, std::min(frames, edit.end + 2)});
  const std::vector<Region> touch_regions = coalesce(touched);

  Peak reference = max_second_difference(before, frames, touch_regions, /*inside=*/false);
  reference.value = std::max(reference.value, max_second_difference(after, frames, touch_regions, false).value);
  Peak worst_edit = max_second_difference(y, frames, touch_regions, /*inside=*/true);

  bool reference_from_render = false;
  if (reference.value <= 0.0)
    // A silent span says nothing about continuity. Falling back to the render's own behaviour
    // outside the windows at least catches a step against the render's own baseline, and the flag
    // below records that the oracles did not provide the reference.
    {
      reference = max_second_difference(y, frames, touch_regions, false);
      reference_from_render = true;
    }

  if (!edits.empty() && reference.value > 0.0)
  {
    report.continuity_evaluated = true;
    report.reference_d2 = reference.value;
    report.max_edit_d2 = worst_edit.value;
    const double limit = options.kappa * reference.value;
    if (worst_edit.value > limit * (1.0 + 1e-12))
    {
      Finding finding;
      finding.rule = "continuity";
      finding.frame = worst_edit.frame;
      finding.measured = worst_edit.value;
      finding.limit = limit;
      finding.detail = "second difference " + std::to_string(worst_edit.value) + " inside an edit against a limit of " +
                       std::to_string(limit) + " (kappa " + std::to_string(options.kappa) + " x " +
                       std::to_string(reference.value) + " " +
                       (reference_from_render ? "from the render itself)" : "from the oracles)");
      report.findings.push_back(std::move(finding));
    }
  }

  // -------------------------------------------------------------------------------------------
  // 2. Dropout. A run of digital black in the render, where an oracle says there should be sound.
  //    Not restricted to the edit windows: a dropout is a dropout wherever it happens, and the span
  //    is already built around an edit.
  //
  //    The oracle is asked about the run as a whole, not sample by sample. A 1 kHz sine crosses zero
  //    a thousand times a second, so a per-sample test breaks a half-second dropout into ~1 ms pieces
  //    -- one per zero crossing -- and then passes every one of them, which is a detector that cannot
  //    see the failure it exists for. A zero crossing of the oracle is not the oracle going silent.
  // -------------------------------------------------------------------------------------------
  const double dropout_linear = db_to_linear(options.dropout_db);
  const size_t dropout_limit = ms_to_frames(options.dropout_max_ms, options.sample_rate);

  // Prefix count of the frames in which an oracle has energy, so "does an oracle have energy between
  // a and b" is one subtraction.
  std::vector<size_t> oracle_energy(frames + 1, 0);
  for (size_t i = 0; i < frames; ++i)
  {
    const bool has_energy = std::abs(static_cast<double>(before[i])) > dropout_linear ||
                            std::abs(static_cast<double>(after[i])) > dropout_linear;
    oracle_energy[i + 1] = oracle_energy[i] + (has_energy ? 1u : 0u);
  }

  size_t worst_run = 0;
  size_t worst_run_start = 0;
  size_t run_start = 0;
  bool in_run = false;
  for (size_t i = 0; i < frames; ++i)
  {
    const bool black = std::abs(static_cast<double>(y[i])) <= dropout_linear;
    if (black && !in_run)
    {
      in_run = true;
      run_start = i;
    }
    else if (!black && in_run)
    {
      in_run = false;
      if (i - run_start > worst_run && oracle_energy[i] > oracle_energy[run_start])
      {
        worst_run = i - run_start;
        worst_run_start = run_start;
      }
    }
  }
  if (in_run && frames - run_start > worst_run && oracle_energy[frames] > oracle_energy[run_start])
  {
    worst_run = frames - run_start;
    worst_run_start = run_start;
  }
  report.max_dropout_ms = static_cast<double>(worst_run) / options.sample_rate * 1000.0;
  if (worst_run > dropout_limit)
  {
    Finding finding;
    finding.rule = "dropout";
    finding.frame = worst_run_start;
    finding.measured = report.max_dropout_ms;
    finding.limit = options.dropout_max_ms;
    finding.detail = "digital black for " + std::to_string(report.max_dropout_ms) + " ms where an oracle has energy (" +
                     std::to_string(worst_run) + " frames)";
    report.findings.push_back(std::move(finding));
  }

  // -------------------------------------------------------------------------------------------
  // 3. Spectral. A transition that is continuous but not band-limited -- a hard switch, a step in a
  //    derivative -- puts high-frequency energy where neither oracle has any.
  // -------------------------------------------------------------------------------------------
  if (!edits.empty())
  {
    const size_t hf_window = ms_to_frames(options.hf_window_ms, options.sample_rate);
    std::vector<Region> hf_regions;
    hf_regions.reserve(edits.size());
    for (const Edit& edit : edits)
      hf_regions.push_back(Region{edit.start > hf_window ? edit.start - hf_window : 0,
                                  std::min(frames, edit.end + hf_window)});
    hf_regions = coalesce(std::move(hf_regions));

    const Biquad high_pass = Biquad::high_pass(options.hf_highpass_hz, options.sample_rate);
    const std::vector<double> y_hf = high_pass.forward_backward(y, frames);
    const std::vector<double> before_hf = high_pass.forward_backward(before, frames);
    const std::vector<double> after_hf = high_pass.forward_backward(after, frames);

    double worst_excess = 0.0;
    bool measured = false;
    for (const Region& region : hf_regions)
    {
      const double oracle_hf = std::max(rms(before_hf, region), rms(after_hf, region));
      if (oracle_hf <= 0.0)
        continue; // nothing to compare against here; an unmeasurable window is not a pass
      const double excess = linear_to_db(rms(y_hf, region) / oracle_hf);
      measured = true;
      if (excess > worst_excess)
        worst_excess = excess;
      if (excess > options.hf_allowance_db)
      {
        Finding finding;
        finding.rule = "hf";
        finding.frame = region.first;
        finding.measured = excess;
        finding.limit = options.hf_allowance_db;
        finding.detail = "high-frequency energy " + std::to_string(excess) + " dB above the louder oracle over frames " +
                         std::to_string(region.first) + ".." + std::to_string(region.last);
        report.findings.push_back(std::move(finding));
      }
    }
    if (measured)
      report.max_hf_excess_db = worst_excess;
  }

  // -------------------------------------------------------------------------------------------
  // 4. Convergence. A smooth interpolation between the two chains is not a transition; the render
  //    has to actually be the old chain before the edit and the new chain after it.
  // -------------------------------------------------------------------------------------------
  if (!edits.empty())
  {
    double worst_abs = 0.0;
    for (size_t i = 0; i < edits.size(); ++i)
    {
      const Edit& edit = edits[i];

      // Between the previous edit and this one the render must still be the chain it started as;
      // between this edit and the next it must already be the chain it became. Back-to-back edits
      // leave no room for one of those, which is expected and simply is not judged.
      const size_t settled_from = i == 0 ? 0 : edits[i - 1].end + guard;
      const size_t settled_to = i + 1 == edits.size() ? frames : edits[i + 1].start - guard;

      const Region with_before{settled_from, edit.start - guard};
      const Region with_after{edit.end + guard, settled_to};

      const Peak before_peak = with_before.empty() ? Peak{} : max_abs_difference(y, before, with_before);
      const Peak after_peak = with_after.empty() ? Peak{} : max_abs_difference(y, after, with_after);
      if (!with_before.empty())
        worst_abs = std::max(worst_abs, before_peak.value);
      if (!with_after.empty())
        worst_abs = std::max(worst_abs, after_peak.value);

      const double limit = db_to_linear(options.convergence_db);
      const bool before_failed = !with_before.empty() && before_peak.value > limit;
      const bool after_failed = !with_after.empty() && after_peak.value > limit;
      if (before_failed || after_failed)
      {
        // Report whichever settled region is worse, so the frame in the finding and the side named
        // in the detail are the same measurement.
        const bool use_before = before_failed && (!after_failed || before_peak.value >= after_peak.value);
        const Peak& worst = use_before ? before_peak : after_peak;
        const Region& region = use_before ? with_before : with_after;

        Finding finding;
        finding.rule = "convergence";
        finding.frame = worst.frame;
        finding.measured = linear_to_db(worst.value);
        finding.limit = options.convergence_db;
        finding.detail = std::string("render is not the ") + (use_before ? "previous" : "next") +
                         " chain at frame " + std::to_string(worst.frame) + ": worst |difference| " +
                         std::to_string(worst.value) + " (" + std::to_string(linear_to_db(worst.value)) +
                         " dBFS) over " + std::to_string(region.size()) + " settled frames";
        report.findings.push_back(std::move(finding));
      }
    }
    // -1000 is the sentinel for "nothing to compare, or bit-identical": a linear difference of zero
    // has no dBFS value, and JSON has no infinity to carry it.
    report.worst_convergence_dbfs = worst_abs > 0.0 ? linear_to_db(worst_abs) : -1000.0;
  }

  report.ok = report.findings.empty();
  return report;
}

} // namespace glitch
} // namespace namchain
