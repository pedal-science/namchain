// Tests for the delay estimator behind `namchain-latency` (plan.md §10.5.1).
//
// This is the part of the round-trip measurement that hardware cannot validate. A correlation peak
// that is ambiguous, aliased, or picked off the wrong feature still yields a number, and that number
// looks exactly like a measurement. So the estimator is tested against captures whose delay is known
// by construction, including ones designed to fool a naive peak-pick.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "namchain/latency.h"

namespace
{

std::vector<float> pseudo_random(unsigned length, double level, uint32_t seed = 0x1234567u)
{
  std::vector<float> out(length, 0.0f);
  uint32_t state = seed;
  for (unsigned i = 0; i < length; ++i)
  {
    state = state * 1664525u + 1013904223u;
    out[i] = (((state >> 16) & 1u) != 0u ? 1.0f : -1.0f) * static_cast<float>(level);
  }
  return out;
}

/// A capture containing the stimulus at `delay`, scaled, in a bed of deterministic noise.
std::vector<float> capture_with(const std::vector<float>& stimulus, std::size_t delay, double scale,
                                double noise = 0.0, std::size_t length = 0)
{
  const std::size_t total = length != 0 ? length : delay + stimulus.size() + 512;
  std::vector<float> capture(total, 0.0f);
  for (std::size_t i = 0; i < total; ++i)
    capture[i] = static_cast<float>(noise) * (pseudo_random(1, 1.0, static_cast<uint32_t>(i * 2654435761u))[0]);
  for (std::size_t i = 0; i < stimulus.size() && delay + i < total; ++i)
    capture[delay + i] += static_cast<float>(scale) * stimulus[i];
  return capture;
}

} // namespace

TEST_CASE("the delay estimator recovers a known delay exactly")
{
  const std::vector<float> stimulus = pseudo_random(4096, 0.25);
  for (const std::size_t delay : {0u, 1u, 7u, 64u, 462u, 4096u})
  {
    const std::vector<float> capture = capture_with(stimulus, delay, 1.0);
    const namchain::DelayEstimate estimate =
      namchain::estimate_delay(capture, stimulus, 0, static_cast<long long>(capture.size()));
    CAPTURE(delay);
    REQUIRE(estimate.found);
    CHECK(estimate.lag == static_cast<long long>(delay));
    CHECK(estimate.lag_sub == doctest::Approx(static_cast<double>(delay)).epsilon(0.05));
    CHECK(estimate.peak > 0.99); // an exact copy correlates at essentially 1
    CHECK(estimate.peak_to_sidelobe_db > 30.0);
  }
}

TEST_CASE("the delay estimator survives attenuation, noise and clipping")
{
  const std::vector<float> stimulus = pseudo_random(4096, 0.25);

  SUBCASE("attenuated as the analogue path attenuates")
  {
    // Measured on the SSL: -10.5 dB from output 3 to input 1 at minimum gain.
    const double scale = std::pow(10.0, -10.5 / 20.0);
    const std::vector<float> capture = capture_with(stimulus, 300, scale, 0.0001);
    const namchain::DelayEstimate estimate =
      namchain::estimate_delay(capture, stimulus, 0, static_cast<long long>(capture.size()));
    REQUIRE(estimate.found);
    CHECK(estimate.lag == 300);
    CHECK(estimate.peak_to_sidelobe_db > 20.0);
  }

  SUBCASE("buried in noise")
  {
    const std::vector<float> capture = capture_with(stimulus, 250, 0.02, 0.01);
    const namchain::DelayEstimate estimate =
      namchain::estimate_delay(capture, stimulus, 0, static_cast<long long>(capture.size()));
    REQUIRE(estimate.found);
    CHECK(estimate.lag == 250);
  }
}

TEST_CASE("a louder unrelated transient does not capture the peak")
{
  // The failure this guards against: peak-picking on amplitude rather than on correlation. A single
  // loud click elsewhere in the capture is larger than every stimulus sample, so anything that
  // looked for the biggest sample would report the click's position as the latency.
  const std::vector<float> stimulus = pseudo_random(4096, 0.1);
  std::vector<float> capture = capture_with(stimulus, 400, 1.0, 0.0, 8192);
  for (std::size_t i = 0; i < 32; ++i)
    capture[100 + i] = 1.0f; // full-scale click, well before the stimulus returns

  const namchain::DelayEstimate estimate =
    namchain::estimate_delay(capture, stimulus, 0, static_cast<long long>(capture.size()));
  REQUIRE(estimate.found);
  CHECK(estimate.lag == 400);
}

TEST_CASE("a capture with no stimulus in it reports nothing rather than a wrong delay")
{
  // "Not found" has to be reachable, or a silent or mis-patched loop would return a confident
  // number and the harness would report a latency that is really just the largest noise peak.
  const std::vector<float> stimulus = pseudo_random(4096, 0.25);
  const std::vector<float> silence(8192, 0.0f);

  const namchain::DelayEstimate nothing =
    namchain::estimate_delay(silence, stimulus, 0, static_cast<long long>(silence.size()));
  CHECK_FALSE(nothing.found);

  // Noise alone gives a peak, but a poor one -- which is what the reported quality is for.
  const std::vector<float> noise = capture_with(stimulus, 0, 0.0, 0.05, 8192);
  const namchain::DelayEstimate noise_only =
    namchain::estimate_delay(noise, stimulus, 0, static_cast<long long>(noise.size()));
  CHECK(noise_only.peak < 0.2);
}

TEST_CASE("the search range is honoured")
{
  // The first send searches a wide range because the delay is unknown; the later sends search a
  // narrow window around it. A range that is not respected would let a repeat report the first
  // send's delay and make the stability check meaningless.
  const std::vector<float> stimulus = pseudo_random(2048, 0.25);
  std::vector<float> capture = capture_with(stimulus, 500, 1.0, 0.0, 4096);

  const namchain::DelayEstimate narrow = namchain::estimate_delay(capture, stimulus, 480, 520);
  REQUIRE(narrow.found);
  CHECK(narrow.lag == 500);

  const namchain::DelayEstimate elsewhere = namchain::estimate_delay(capture, stimulus, 0, 200);
  if (elsewhere.found)
    CHECK(elsewhere.lag < 200); // never outside what was asked for
}

TEST_CASE("sub-sample refinement recovers a fractional delay")
{
  // Converter latency is not a whole number of samples, so the integer peak is quantised by up to
  // half a sample. Linearly interpolating the capture between samples gives a fractional delay the
  // estimator should find in between the two neighbouring integers.
  const std::vector<float> stimulus = pseudo_random(4096, 0.25);
  const std::size_t integer = 300;
  const double fraction = 0.5;

  std::vector<float> capture(integer + stimulus.size() + 512, 0.0f);
  for (std::size_t i = 0; i < stimulus.size(); ++i)
  {
    const double value = static_cast<double>(stimulus[i]);
    const std::size_t position = integer + i;
    capture[position] += static_cast<float>(value * (1.0 - fraction));
    if (position + 1 < capture.size())
      capture[position + 1] += static_cast<float>(value * fraction);
  }

  const namchain::DelayEstimate estimate =
    namchain::estimate_delay(capture, stimulus, 0, static_cast<long long>(capture.size()));
  REQUIRE(estimate.found);
  CHECK(estimate.lag_sub > static_cast<double>(integer) + 0.2);
  CHECK(estimate.lag_sub < static_cast<double>(integer) + 0.8);
}
