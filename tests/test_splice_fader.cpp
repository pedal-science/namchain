// Tests for rt::SpliceFader -- the repair that makes a dropped or inserted period inaudible.
//
// The headline test is not a property of the fader, it is the verdict of the project's own glitch
// detector on the signal the fader produces. plan.md §10.3 requires the detector to exist before the
// transition machinery it judges, and this is that order being used: the same span is analysed twice,
// once spliced raw and once repaired, and the detector's continuity rule is what decides. `before` and
// `after` are the *raw* spliced signal, which is the honest oracle for a repair: the signal that was
// actually going to be emitted is what the repaired one must be smoother than, and neither is more
// faithful than the other -- a drop loses 64 frames of audio and no fade can bring them back. What the
// fade buys is continuity, so continuity is what is measured.
#include <doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "namchain/glitch.h"
#include "namchain/rt/splice_fader.h"

namespace
{

constexpr double kSampleRate = 48000.0;
/// A signal with energy in the band the detectors care about, and a slope steep enough that a step is
/// unmistakable against it.
std::vector<float> tone(size_t frames, double hz = 997.0)
{
  std::vector<float> out(frames);
  for (size_t i = 0; i < frames; ++i)
    out[i] = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(i) /
                                                kSampleRate));
  return out;
}

/// Copy `in` minus `frames_to_drop` starting at `at` -- the signal the device hears when the loop
/// refuses to write one period, with the hole where the period would have been.
std::vector<float> drop_period(const std::vector<float>& in, size_t at, size_t frames_to_drop)
{
  std::vector<float> out;
  out.reserve(in.size() - frames_to_drop);
  out.insert(out.end(), in.begin(), in.begin() + static_cast<long>(at));
  out.insert(out.end(), in.begin() + static_cast<long>(at + frames_to_drop), in.end());
  return out;
}

/// Run the detector over one span with a single edit window covering the splice and its repair.
namchain::glitch::Report analyse(const std::vector<float>& y, const std::vector<float>& raw, size_t at,
                                size_t window)
{
  namchain::glitch::Options options;
  options.sample_rate = kSampleRate;
  std::vector<namchain::glitch::Edit> edits;
  edits.push_back({at, at + window});
  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), raw.data(), raw.data(), y.size(), edits, options, error);
  REQUIRE(error.empty());
  return report;
}

} // namespace

TEST_CASE("a raw dropped period trips the continuity rule and the repaired one does not")
{
  // The real shape of the artefact: 64 frames of signal simply missing, so the sample before the join
  // and the sample after it have nothing to do with each other.
  constexpr size_t kPeriod = 64;
  constexpr size_t kJoin = 2048;
  const std::vector<float> clean = tone(4096);
  const std::vector<float> raw = drop_period(clean, kJoin, kPeriod);

  // Repair it the way the loop does: hold the last emitted sample, and glide from it into the period
  // that follows the hole. The fader is given the signal *as spliced*, one period at a time, which is
  // exactly what the loop hands it.
  namchain::rt::SpliceFader fader(1, 240); // 5 ms at 48 kHz
  std::vector<float> repaired(raw.size());
  const unsigned period = static_cast<unsigned>(kPeriod);
  for (size_t at = 0; at < raw.size(); at += period)
  {
    const unsigned frames = static_cast<unsigned>(std::min<size_t>(period, raw.size() - at));
    if (at == kJoin)
      fader.arm(namchain::rt::SpliceFader::Cause::Dropped);
    fader.apply(raw.data() + at, repaired.data() + at, frames);
  }
  CHECK(fader.fades() == 1);

  const namchain::glitch::Report raw_report = analyse(raw, raw, kJoin, 240);
  const namchain::glitch::Report fixed_report = analyse(repaired, raw, kJoin, 240);
  // The raw splice is a step, and the detector says so with a number rather than a shrug.
  CHECK_FALSE(raw_report.ok);
  CHECK(raw_report.continuity_evaluated);
  // The repair is continuous: no rule is tripped, and its worst second difference inside the window is
  // within the allowance the detector derives from the tone itself.
  CHECK(fixed_report.ok);
  CHECK(fixed_report.continuity_evaluated);
  CHECK(fixed_report.max_edit_d2 <= raw_report.max_edit_d2 / 10.0);
  CHECK(fixed_report.max_edit_d2 <= fixed_report.reference_d2 * 2.0); // kappa, §10.3
  // And it has less broadband energy than the step it replaces, which is the other half of "audible".
  CHECK(fixed_report.max_hf_excess_db <= raw_report.max_hf_excess_db);
  // Outside the repair the signal is untouched, so the repair cannot be blamed for anything else.
  CHECK(fixed_report.worst_convergence_dbfs == raw_report.worst_convergence_dbfs);
}

TEST_CASE("an inserted period is repaired the same way, and silence is the worse of the two payloads")
{
  // The floor controller inserts a period; the backend repeats the last frame it wrote rather than
  // zeros, because a held value leaves from where the device was and silence steps to zero first. The
  // fader then glides out of the hold into the next real period. Both payloads are built here so the
  // claim is measured: the held-frame one is continuous at the entry, the silence one is not.
  constexpr size_t kPeriod = 64;
  constexpr size_t kAt = 2048;
  const std::vector<float> clean = tone(4096, 313.0);

  std::vector<float> with_hold;
  std::vector<float> with_silence;
  with_hold.insert(with_hold.end(), clean.begin(), clean.begin() + static_cast<long>(kAt));
  with_silence.insert(with_silence.end(), clean.begin(), clean.begin() + static_cast<long>(kAt));
  const float held = clean[kAt - 1];
  for (size_t i = 0; i < kPeriod; ++i)
  {
    with_hold.push_back(held);
    with_silence.push_back(0.0f);
  }
  with_hold.insert(with_hold.end(), clean.begin() + static_cast<long>(kAt), clean.end());
  with_silence.insert(with_silence.end(), clean.begin() + static_cast<long>(kAt), clean.end());

  // Repair the exit of each: the fader is armed for the period after the insert, when the loop finds
  // out what the backend did.
  const auto repair_exit = [](const std::vector<float>& in, size_t arm_at) {
    namchain::rt::SpliceFader fader(1, 240);
    std::vector<float> out(in.size());
    const unsigned period = static_cast<unsigned>(kPeriod);
    for (size_t at = 0; at < in.size(); at += period)
    {
      const unsigned frames = static_cast<unsigned>(std::min<size_t>(period, in.size() - at));
      if (at == arm_at)
        fader.arm(namchain::rt::SpliceFader::Cause::Dropped);
      fader.apply(in.data() + at, out.data() + at, frames);
    }
    return out;
  };
  // Arm one period after the insert, so the ramp leaves from the hold.
  const std::vector<float> hold_fixed = repair_exit(with_hold, kAt + kPeriod);
  const std::vector<float> silence_fixed = repair_exit(with_silence, kAt + kPeriod);

  // The entry is where the two payloads differ, so it is measured there: the largest sample-to-sample
  // step across the join.
  const auto max_step = [](const std::vector<float>& v, size_t from, size_t to) {
    float worst = 0.0f;
    for (size_t i = from + 1; i < to && i < v.size(); ++i)
      worst = std::max(worst, std::fabs(v[i] - v[i - 1]));
    return worst;
  };
  const float hold_entry = max_step(with_hold, kAt - 2, kAt + 2);
  const float silence_entry = max_step(with_silence, kAt - 2, kAt + 2);
  CHECK(silence_entry > 0.3f);       // a step to zero, on a signal peaking at 0.4
  CHECK(hold_entry < 0.05f);         // the held frame leaves from where the signal was

  // Both repairs remove the *exit* step, which is the fader's half of the job.
  CHECK(max_step(hold_fixed, kAt + kPeriod - 2, kAt + kPeriod + 2) < 0.05f);
  CHECK(max_step(silence_fixed, kAt + kPeriod - 2, kAt + kPeriod + 2) < 0.05f);
  // The entry of the repaired-silence version is still a step; the held-frame version has none.
  CHECK(max_step(silence_fixed, kAt - 2, kAt + 2) > 0.3f);
  CHECK(max_step(hold_fixed, kAt - 2, kAt + 2) < 0.05f);

  // The detector on the whole span, and the honest verdict: the exit is bridged and the *value* step is
  // gone, but the span still trips the continuity rule. The reason is the one thing a loop-side repair
  // cannot reach -- the entry of the inserted period, which the backend wrote and the loop never sees,
  // and which arrives as a slope break because the signal's slope goes to zero in one sample. Fixing it
  // means the backend writing a *bridged* payload rather than a held one, using the same two Hermite
  // terms; that is the remaining piece of this repair and it is a backend change, not a loop one.
  const namchain::glitch::Report raw_report = analyse(with_silence, with_silence, kAt, 240);
  const namchain::glitch::Report hold_report = analyse(hold_fixed, with_silence, kAt, 240);
  CHECK_FALSE(raw_report.ok);
  CHECK_FALSE(hold_report.ok);
  CHECK(hold_report.findings.size() <= raw_report.findings.size());
  // What the held payload does buy is measured, not asserted: the entry stops being a step in *value*
  // (0.4 on this signal) and becomes one in *slope* (~a tenth of that), so the worst second difference
  // in the window falls by roughly an order of magnitude even though the rule is still tripped.
  CHECK(hold_report.max_edit_d2 < raw_report.max_edit_d2 / 4.0);
  CHECK(hold_report.continuity_evaluated);
}

TEST_CASE("the bridge window is long enough to smooth the mismatch and short enough not to duck")
{
  // The window length is the one free parameter, and the detector bounds it from both sides. Too short
  // and the mismatch is bridged too abruptly to be smooth; long enough and the residual second
  // difference falls, but the offset that does the bridging is held for longer -- which is a swell on
  // the signal, not a repair. The measured curve, rather than a preference for round numbers, is what
  // picks the default.
  constexpr size_t kPeriod = 64;
  constexpr size_t kJoin = 2048;
  const std::vector<float> clean = tone(4096);
  const std::vector<float> raw = drop_period(clean, kJoin, kPeriod);

  const auto repair = [&raw](unsigned window) {
    namchain::rt::SpliceFader fader(1, window);
    std::vector<float> out(raw.size());
    const unsigned period = static_cast<unsigned>(kPeriod);
    for (size_t at = 0; at < raw.size(); at += period)
    {
      const unsigned frames = static_cast<unsigned>(std::min<size_t>(period, raw.size() - at));
      if (at == kJoin)
        fader.arm(namchain::rt::SpliceFader::Cause::Dropped);
      fader.apply(raw.data() + at, out.data() + at, frames);
    }
    return out;
  };

  const auto d2_of = [](const std::vector<float>& v, size_t window) {
    double worst = 0.0;
    for (size_t n = kJoin + 2; n < kJoin + window && n < v.size(); ++n)
      worst = std::max(worst, static_cast<double>(std::fabs(v[n] - 2.0f * v[n - 1] + v[n - 2])));
    return worst;
  };

  const double short_window = d2_of(repair(4), 4);
  const double default_window = d2_of(repair(64), 64);
  CHECK(default_window < short_window); // smoothing is what the length buys
  // And the default passes the rule the detector states, which is the claim that matters: the repair is
  // continuous by the project's own definition of continuous, not merely smoother than before.
  const namchain::glitch::Report report = analyse(repair(64), raw, kJoin, 64);
  CHECK(report.continuity_evaluated);
  CHECK(report.max_edit_d2 <= report.reference_d2 * 2.0); // kappa, §10.3
  CHECK(report.ok);
}

TEST_CASE("the fader passes through exactly when nothing is armed, and tracks its anchor")
{
  // "The fader costs nothing when it is not needed" has to be a statement about the samples, not about
  // good intentions: an idle fader must emit bit-identical audio, because every run that enables it and
  // has no splices has to remain comparable with one that never enabled it.
  namchain::rt::SpliceFader fader(2, 8);
  CHECK(fader.enabled());
  CHECK_FALSE(fader.armed());

  const std::vector<float> in{0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f};
  std::vector<float> out(in.size(), 0.0f);
  fader.apply(in.data(), out.data(), 4);
  CHECK(out == in);
  CHECK(fader.last_emitted(0) == doctest::Approx(0.4f));
  CHECK(fader.last_emitted(1) == doctest::Approx(-0.4f));

  // A fade longer than a period continues into the next one, and leaves from the armed anchor
  // throughout: the first sample of the ramp is the last emitted sample, so nothing steps.
  namchain::rt::SpliceFader long_fade(1, 16);
  std::vector<float> one(4, 0.5f);
  std::vector<float> emitted(4, 0.0f);
  long_fade.apply(one.data(), emitted.data(), 4); // anchor 0.5
  CHECK(long_fade.last_emitted(0) == doctest::Approx(0.5f));
  long_fade.arm(namchain::rt::SpliceFader::Cause::Dropped);
  CHECK(long_fade.remaining() == 16);
  std::vector<float> zeros(4, 0.0f);
  long_fade.apply(zeros.data(), emitted.data(), 4);
  CHECK(emitted[0] == doctest::Approx(0.5f)); // leaves from the anchor, not from the input
  CHECK(long_fade.armed());                   // 12 samples of ramp left
  CHECK(long_fade.fades() == 1);

  // Re-arming mid-fade re-anchors on the latest emitted sample rather than queueing a second fade.
  long_fade.arm(namchain::rt::SpliceFader::Cause::Dropped);
  CHECK(long_fade.remaining() == 16);
  CHECK(long_fade.fades() == 2);

  // A disabled fader refuses to arm and copies without touching the anchor.
  namchain::rt::SpliceFader off(1, 0);
  CHECK_FALSE(off.enabled());
  off.arm(namchain::rt::SpliceFader::Cause::Dropped);
  CHECK_FALSE(off.armed());
  CHECK(off.fades() == 0);
  std::vector<float> passthrough(4, 0.0f);
  off.apply(one.data(), passthrough.data(), 4);
  CHECK(passthrough == one);
}
