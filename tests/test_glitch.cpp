// Tests for the glitch detector (plan.md §10.3).
//
// The detector is the instrument the transition machinery will be judged by, so the tests are built
// the other way round from usual: each one constructs a signal that is *wrong in exactly one way*
// and requires the corresponding rule to be the one that fires. A detector that reports "glitch" for
// everything is as useless as one that reports nothing, and only a case per rule can tell them apart.
//
// The oracles here are synthetic sines rather than renders. That is deliberate: this layer knows
// nothing about chains, and the four rules are properties of three signals, so the tests pin the
// arithmetic instead of the engine. What the engine's own transitions do to those signals is the
// torture render's job (§10.3).
#include <doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "namchain/glitch.h"

namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

/// 100 ms at 48 kHz: long enough that the default 5 ms guard leaves 95 ms of settled signal either
/// side of a 10 ms edit, which is what rule 4 needs to be judgeable at all.
constexpr size_t kFrames = 4800;
constexpr size_t kEditStart = 2400;
constexpr size_t kEditEnd = 2880; // 10 ms

std::vector<float> tone(double hz, double amplitude)
{
  std::vector<float> v(kFrames);
  for (size_t i = 0; i < kFrames; ++i)
    v[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
  return v;
}

/// A linear crossfade from `a` to `b` over [first, last), and exactly `b` from `last` on.
std::vector<float> crossfade(const std::vector<float>& a, const std::vector<float>& b, size_t first, size_t last)
{
  std::vector<float> y(kFrames);
  for (size_t i = 0; i < kFrames; ++i)
  {
    double gain = 0.0;
    if (i >= last)
      gain = 1.0;
    else if (i > first)
      gain = static_cast<double>(i - first) / static_cast<double>(last - first);
    y[i] = static_cast<float>((1.0 - gain) * static_cast<double>(a[i]) + gain * static_cast<double>(b[i]));
  }
  return y;
}

bool has_rule(const namchain::glitch::Report& report, const std::string& rule)
{
  for (const namchain::glitch::Finding& finding : report.findings)
    if (finding.rule == rule)
      return true;
  return false;
}

namchain::glitch::Options options()
{
  namchain::glitch::Options o;
  o.sample_rate = kRate;
  return o;
}

std::vector<namchain::glitch::Edit> one_edit()
{
  return {namchain::glitch::Edit{kEditStart, kEditEnd}};
}

} // namespace

TEST_CASE("a clean crossfade passes all four rules")
{
  const std::vector<float> a = tone(1000.0, 0.5);
  const std::vector<float> b = tone(1000.0, 0.25);
  const std::vector<float> y = crossfade(a, b, kEditStart, kEditEnd);

  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), a.data(), b.data(), kFrames, one_edit(), options(), error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK(report.ok);
  CHECK(report.findings.empty());
  CHECK(report.continuity_evaluated);
  CHECK(report.reference_d2 > 0.0);
  CHECK(report.max_edit_d2 <= report.reference_d2 * options().kappa);
  CHECK(report.max_dropout_ms == doctest::Approx(0.0));
  CHECK(report.max_hf_excess_db < options().hf_allowance_db);
  CHECK(report.worst_convergence_dbfs == doctest::Approx(-1000.0));
}

TEST_CASE("an unramped polarity flip trips continuity and nothing else")
{
  const std::vector<float> a = tone(1000.0, 0.5);
  std::vector<float> b(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    b[i] = -a[i];
  // No fade at all: the edit is instantaneous, which is exactly what rule 1 exists to catch.
  std::vector<float> y(a);
  for (size_t i = kEditStart; i < kFrames; ++i)
    y[i] = b[i];

  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), a.data(), b.data(), kFrames,
                              {namchain::glitch::Edit{kEditStart, kEditStart}}, options(), error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK_FALSE(report.ok);
  CHECK(has_rule(report, "continuity"));
  // Both oracles are exactly what the render is on its own side of the edit, so the smooth-interpolation
  // rule has nothing to complain about -- this is a genuine step, not a failure to converge.
  CHECK_FALSE(has_rule(report, "convergence"));
}

TEST_CASE("a dropout trips the gap rule")
{
  const std::vector<float> a = tone(1000.0, 0.5);
  std::vector<float> y = a;
  // 5 ms of digital black where the oracle has plenty of signal: five times the tolerated run.
  for (size_t i = kEditStart; i < kEditStart + 240; ++i)
    y[i] = 0.0f;

  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), a.data(), a.data(), kFrames, one_edit(), options(), error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK_FALSE(report.ok);
  CHECK(has_rule(report, "dropout"));
  CHECK(report.max_dropout_ms > options().dropout_max_ms);
  CHECK(report.max_dropout_ms == doctest::Approx(5.0).epsilon(0.02));
}

TEST_CASE("silence is not a dropout where the oracle is silent too")
{
  const std::vector<float> silence(kFrames, 0.0f);
  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(silence.data(), silence.data(), silence.data(), kFrames, one_edit(), options(), error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK(report.ok);
  CHECK(report.max_dropout_ms == doctest::Approx(0.0));
  // There is no second difference anywhere to compare against, so continuity is not "passed" -- it
  // is unmeasurable, and the report is required to say which.
  CHECK_FALSE(report.continuity_evaluated);
}

TEST_CASE("a hard switch trips the spectral rule, which is a measurement of its own")
{
  // A polarity inversion at frame 2400 of a 1 kHz tone at 48 kHz: exactly 50 whole periods, so the
  // switch lands on a zero crossing. The waveform's *value* is continuous there and its slope
  // reverses -- a corner, not a jump.
  //
  // Rules 1 and 3 both object to that, and they are meant to: a corner is a second-difference spike
  // and it is broadband, and the plan asks for four overlapping measurements rather than four disjoint
  // ones. So this case raises kappa until rule 1 is satisfied, which leaves the spectral result as the
  // only thing that can produce a finding -- that is what makes it a test of rule 3 rather than a
  // second test of rule 1.
  const std::vector<float> a = tone(1000.0, 0.5);
  std::vector<float> b(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    b[i] = -a[i];
  std::vector<float> y(a);
  for (size_t i = kEditStart; i < kFrames; ++i)
    y[i] = b[i];

  namchain::glitch::Options o = options();
  o.kappa = 50.0;

  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), a.data(), b.data(), kFrames, one_edit(), o, error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK_FALSE(report.ok);
  CHECK(has_rule(report, "hf"));
  CHECK(report.max_hf_excess_db > o.hf_allowance_db);
  CHECK_FALSE(has_rule(report, "continuity"));
  CHECK_FALSE(has_rule(report, "convergence"));
  CHECK_FALSE(has_rule(report, "dropout"));
}

TEST_CASE("the same corner is a continuity failure at the default kappa")
{
  // The other half of the previous case, with nothing relaxed: the second-difference rule sees the
  // slope reversal that the spectral measurement also sees. This pins the overlap between the two
  // rules instead of leaving it as a comment.
  const std::vector<float> a = tone(1000.0, 0.5);
  std::vector<float> b(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    b[i] = -a[i];
  std::vector<float> y(a);
  for (size_t i = kEditStart; i < kFrames; ++i)
    y[i] = b[i];

  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), a.data(), b.data(), kFrames, one_edit(), options(), error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK(has_rule(report, "continuity"));
  CHECK(report.max_edit_d2 > report.reference_d2 * options().kappa);
}

TEST_CASE("a smooth interpolation that never settles trips convergence")
{
  const std::vector<float> a = tone(1000.0, 0.5);
  const std::vector<float> b = tone(1000.0, 0.25);
  // The edit window is respected, the fade is smooth, nothing drops out and nothing steps -- but the
  // render is still drifting between the two chains long after the transition should have finished.
  std::vector<float> y(kFrames);
  for (size_t i = 0; i < kFrames; ++i)
  {
    const double gain = static_cast<double>(i) / static_cast<double>(kFrames - 1);
    y[i] = static_cast<float>((1.0 - gain) * static_cast<double>(a[i]) + gain * static_cast<double>(b[i]));
  }

  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), a.data(), b.data(), kFrames, one_edit(), options(), error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK_FALSE(report.ok);
  CHECK(has_rule(report, "convergence"));
  CHECK(report.worst_convergence_dbfs > options().convergence_db);
}

TEST_CASE("no edits means nothing to judge")
{
  const std::vector<float> a = tone(1000.0, 0.5);
  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(a.data(), a.data(), a.data(), kFrames, {}, options(), error);

  CHECK(error.empty());
  CHECK(report.ok);
  CHECK(report.edits == 0);
  CHECK_FALSE(report.continuity_evaluated);
}

TEST_CASE("the detector refuses a span it cannot judge")
{
  const std::vector<float> a = tone(1000.0, 0.5);
  std::string error;
  namchain::glitch::Options o = options();

  SUBCASE("no frames")
  {
    const namchain::glitch::Report report =
      namchain::glitch::analyse(a.data(), a.data(), a.data(), 0, {}, o, error);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(report.ok);
  }

  SUBCASE("an edit that starts before it ends")
  {
    error.clear();
    const namchain::glitch::Report report = namchain::glitch::analyse(
      a.data(), a.data(), a.data(), kFrames, {namchain::glitch::Edit{500, 400}}, o, error);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(report.ok);
  }

  SUBCASE("edits out of order")
  {
    error.clear();
    const namchain::glitch::Report report = namchain::glitch::analyse(
      a.data(), a.data(), a.data(), kFrames,
      {namchain::glitch::Edit{2000, 2100}, namchain::glitch::Edit{1000, 1100}}, o, error);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(report.ok);
  }

  SUBCASE("an edit flush against the start of the span")
  {
    error.clear();
    const namchain::glitch::Report report = namchain::glitch::analyse(
      a.data(), a.data(), a.data(), kFrames, {namchain::glitch::Edit{10, 100}}, o, error);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(report.ok);
  }

  SUBCASE("an edit flush against the end of the span")
  {
    error.clear();
    const namchain::glitch::Report report = namchain::glitch::analyse(
      a.data(), a.data(), a.data(), kFrames, {namchain::glitch::Edit{kFrames - 100, kFrames - 10}}, o, error);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(report.ok);
  }

  SUBCASE("an edit past the end of the span")
  {
    error.clear();
    const namchain::glitch::Report report = namchain::glitch::analyse(
      a.data(), a.data(), a.data(), kFrames, {namchain::glitch::Edit{kFrames + 10, kFrames + 20}}, o, error);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(report.ok);
  }
}

TEST_CASE("back-to-back edits inside the span are legal, and the gap between them is not judged")
{
  const std::vector<float> a = tone(1000.0, 0.5);
  const std::vector<float> b = tone(1000.0, 0.25);
  // Two 10 ms fades that meet exactly. There is no settled region between them, so rule 4 has
  // nothing to say there -- but nothing about that is an error, and both outer regions still are.
  // The render is continuous throughout, so a detector that failed this would be failing for its own
  // convenience rather than on the audio.
  std::vector<float> y = crossfade(a, b, 1800, 2400);
  for (size_t i = 2400; i < 3000; ++i)
    y[i] = b[i];

  std::string error;
  const namchain::glitch::Report report = namchain::glitch::analyse(
    y.data(), a.data(), b.data(), kFrames,
    {namchain::glitch::Edit{1800, 2400}, namchain::glitch::Edit{2400, 3000}}, options(), error);

  MESSAGE(error);
  CHECK(error.empty());
  CHECK(report.ok);
  CHECK(report.findings.empty());
  CHECK(report.worst_convergence_dbfs == doctest::Approx(-1000.0));
}
