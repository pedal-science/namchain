// M0 offline-render tests: determinism, block-size invariance, frame accounting, and the
// error paths. These are the properties the M2 glitch detector's oracle renders will depend
// on, so they are pinned here first.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "namchain/offline.h"
#include "namchain/wav.h"

namespace
{

std::string temp_path(const std::string& tag)
{
  static int counter = 0;
  const auto dir = std::filesystem::temp_directory_path() / "namchain-tests";
  std::filesystem::create_directories(dir);
  return (dir / (tag + "-" + std::to_string(++counter) + ".wav")).string();
}

std::string model_path(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

std::string input_wav()
{
  return std::string(NAMCHAIN_EXAMPLE_AUDIO_DIR) + "/input.wav";
}

std::vector<float> load_or_fail(const std::string& path, double* rate = nullptr)
{
  std::vector<float> samples;
  double sample_rate = 0.0;
  std::string error;
  REQUIRE_MESSAGE(namchain::load_wav_mono(path, samples, sample_rate, error), error);
  if (rate != nullptr)
    *rate = sample_rate;
  return samples;
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b)
{
  REQUIRE(a.size() == b.size());
  double worst = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
    worst = std::max(worst, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
  return worst;
}

/// Measured on the Pi 500 (see the M0 notes in plan.md §11): the A2 planar kernels are
/// bit-identical across block sizes in practice, but the assertion below is deliberately a
/// tolerance rather than equality, because that is a property of the toolchain's contraction
/// behaviour (§4.2) and not something this engine is entitled to assume on every target.
constexpr double kBlockInvarianceMaxAbsDiff = 1e-6;

} // namespace

TEST_CASE("offline render is deterministic: same input, byte-identical output")
{
  const std::string first = temp_path("det-a");
  const std::string second = temp_path("det-b");

  const auto a = namchain::render_model_to_wav(model_path("A2-Lite.nam"), input_wav(), first);
  REQUIRE_MESSAGE(a.ok, a.error);
  const auto b = namchain::render_model_to_wav(model_path("A2-Lite.nam"), input_wav(), second);
  REQUIRE_MESSAGE(b.ok, b.error);

  CHECK(a.stats.frames == b.stats.frames);
  CHECK(max_abs_diff(load_or_fail(first), load_or_fail(second)) == 0.0);
}

TEST_CASE("offline render emits exactly one output frame per input frame")
{
  std::vector<float> input = load_or_fail(input_wav());
  const std::string out = temp_path("frames");
  const auto result = namchain::render_model_to_wav(model_path("A2-Lite.nam"), input_wav(), out);
  REQUIRE_MESSAGE(result.ok, result.error);

  CHECK(result.stats.frames == static_cast<int>(input.size()));
  // No repeats or insertions -- the frame-accounting half of the §10.3 gap/dropout metric.
  CHECK(load_or_fail(out).size() == input.size());
}

TEST_CASE("offline render is invariant to block size")
{
  const std::string reference_path = temp_path("q64");
  const auto reference = namchain::render_model_to_wav(model_path("A2-Lite.nam"), input_wav(), reference_path,
                                                       namchain::OfflineRenderOptions{.quantum = 64});
  REQUIRE_MESSAGE(reference.ok, reference.error);
  const std::vector<float> expected = load_or_fail(reference_path);

  // 1 and 3 exercise the sub-vector and non-multiple-of-four tails of the planar kernels.
  for (const int quantum : {1, 3, 32, 128, 256})
  {
    const std::string path = temp_path("q" + std::to_string(quantum));
    namchain::OfflineRenderOptions options;
    options.quantum = quantum;
    const auto result = namchain::render_model_to_wav(model_path("A2-Lite.nam"), input_wav(), path, options);
    REQUIRE_MESSAGE(result.ok, result.error);
    CHECK(result.stats.frames == reference.stats.frames);
    const double diff = max_abs_diff(expected, load_or_fail(path));
    INFO("quantum = ", quantum, ", max|diff| = ", diff);
    CHECK(diff <= kBlockInvarianceMaxAbsDiff);
  }
}

TEST_CASE("offline render handles a WaveNet model and an IR model")
{
  for (const char* name : {"A2-Full.nam", "CabIR.nam"})
  {
    const std::string out = temp_path(name);
    const auto result = namchain::render_model_to_wav(model_path(name), input_wav(), out);
    REQUIRE_MESSAGE(result.ok, result.error);

    const std::vector<float> rendered = load_or_fail(out);
    REQUIRE(rendered.size() == static_cast<size_t>(result.stats.frames));

    double peak = 0.0;
    for (const float sample : rendered)
    {
      REQUIRE(std::isfinite(sample));
      peak = std::max(peak, std::abs(static_cast<double>(sample)));
    }
    // A silent render would mean we produced a valid file full of nothing.
    CHECK(peak > 1e-6);
  }
}

TEST_CASE("offline render rejects a sample-rate mismatch instead of resampling")
{
  // The vendored core has no resampler and rejects a rate mismatch (G11), so the offline path
  // must refuse rather than silently render at the wrong rate. Writing this fixture with our
  // own writer also proves the writer round-trips through the core's WAV loader.
  const std::string wav_44k = temp_path("44k");
  std::vector<float> tone(4410, 0.0f);
  for (size_t i = 0; i < tone.size(); ++i)
  {
    const double pi = 3.14159265358979323846;
    tone[i] = static_cast<float>(0.1 * std::sin(2.0 * pi * 220.0 * static_cast<double>(i) / 44100.0));
  }
  std::string error;
  REQUIRE_MESSAGE(namchain::save_wav_float32(wav_44k, tone.data(), tone.size(), 1, 44100.0, error), error);

  const std::string out = temp_path("44k-out");
  const auto result = namchain::render_model_to_wav(model_path("A2-Lite.nam"), wav_44k, out);
  CHECK_FALSE(result.ok);
  // The message must name the mismatch rather than failing obscurely.
  CHECK(result.error.find("Hz") != std::string::npos);
}

TEST_CASE("offline render reports errors instead of crashing")
{
  const std::string out = temp_path("errors");

  const auto missing_model = namchain::render_model_to_wav("/nonexistent/model.nam", input_wav(), out);
  CHECK_FALSE(missing_model.ok);
  CHECK_FALSE(missing_model.error.empty());

  const auto missing_input = namchain::render_model_to_wav(model_path("A2-Lite.nam"), "/nonexistent/in.wav", out);
  CHECK_FALSE(missing_input.ok);
  CHECK_FALSE(missing_input.error.empty());

  const auto bad_quantum = namchain::render_model_to_wav(model_path("A2-Lite.nam"), input_wav(), out,
                                                         namchain::OfflineRenderOptions{.quantum = 0});
  CHECK_FALSE(bad_quantum.ok);

  // Not a WAV at all.
  const std::string junk = temp_path("junk");
  std::ofstream(junk, std::ios::binary) << "this is not a wav file";
  const auto bad_wav = namchain::render_model_to_wav(model_path("A2-Lite.nam"), junk, out);
  CHECK_FALSE(bad_wav.ok);
  CHECK_FALSE(bad_wav.error.empty());
}
