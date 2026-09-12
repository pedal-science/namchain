// Tests for the acceptance instrumentation: the block timer, the soak runner, and the WAV
// comparison the oracle check is built on. These are the parts that decide whether the
// "zero xruns" claim is being measured or merely asserted, so they get tested directly.
#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "namchain/offline.h"
#include "namchain/soak.h"
#include "namchain/telemetry.h"
#include "namchain/wav.h"

namespace
{

std::string temp_path(const std::string& tag)
{
  static int counter = 0;
  const auto dir = std::filesystem::temp_directory_path() / "namchain-soak-tests";
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

} // namespace

TEST_CASE("block timer reports percentiles and counts deadline overruns")
{
  namchain::BlockTimer timer(1000); // 1000 ns deadline
  for (int i = 1; i <= 100; ++i)
    timer.record(static_cast<uint64_t>(i * 10)); // 10..1000 ns, none over the deadline

  CHECK(timer.blocks() == 100);
  CHECK(timer.deadline_overruns() == 0);
  CHECK(timer.p50_ns() == 500);
  CHECK(timer.max_ns() == 1000);
  CHECK(timer.p99_percent_of_deadline() <= 100.0);

  timer.record(1500); // one over the deadline
  timer.record(3000);
  CHECK(timer.deadline_overruns() == 2);
  CHECK(timer.max_ns() == 3000);
  CHECK(timer.blocks() == 102);

  timer.reset();
  CHECK(timer.blocks() == 0);
  CHECK(timer.max_ns() == 0);
  CHECK(timer.deadline_overruns() == 0);
  CHECK(timer.p50_ns() == 0);
}

TEST_CASE("WAV comparison distinguishes match, mismatch and unreadable input")
{
  const std::vector<float> a{0.0f, 0.25f, -0.5f, 1.0f};
  const std::vector<float> b{0.0f, 0.25f, -0.5f, 1.0f};
  const std::vector<float> c{0.0f, 0.25f, -0.5f, 0.9f};

  const std::string pa = temp_path("cmp-a");
  const std::string pb = temp_path("cmp-b");
  const std::string pc = temp_path("cmp-c");
  std::string error;
  REQUIRE(namchain::save_wav_float32(pa, a.data(), a.size(), 1, 48000.0, error));
  REQUIRE(namchain::save_wav_float32(pb, b.data(), b.size(), 1, 48000.0, error));
  REQUIRE(namchain::save_wav_float32(pc, c.data(), c.size(), 1, 48000.0, error));

  namchain::WavDiff diff;
  CHECK(namchain::compare_wav_mono(pa, pb, 0.0, diff, error) == namchain::WavCompareStatus::Match);
  CHECK(diff.max_abs == 0.0);
  CHECK(diff.frames == 4);

  CHECK(namchain::compare_wav_mono(pa, pc, 0.0, diff, error) == namchain::WavCompareStatus::Differs);
  CHECK(diff.max_abs == doctest::Approx(0.1).epsilon(1e-6));
  // A loose tolerance must turn the same pair into a match.
  CHECK(namchain::compare_wav_mono(pa, pc, 0.2, diff, error) == namchain::WavCompareStatus::Match);

  CHECK(namchain::compare_wav_mono(pa, "/nonexistent/x.wav", 0.0, diff, error) ==
        namchain::WavCompareStatus::Error);
  CHECK_FALSE(error.empty());
}

TEST_CASE("a paced soak run keeps up with the wall clock")
{
  namchain::SoakOptions options;
  options.quantum = 64;
  options.duration_seconds = 0.25;
  options.realtime_paced = true;

  const namchain::SoakResult result = namchain::run_soak(model_path("A2-Lite.nam"), input_wav(), options);
  REQUIRE_MESSAGE(result.ok, result.error);

  CHECK(result.blocks > 0);
  CHECK(result.frames == result.blocks * 64);
  CHECK(result.deadline_ns > 0);
  CHECK(result.driver_xruns == 0); // no device backend yet
  // Deliberately NOT asserting "no overruns" here. That is the acceptance claim, and it is
  // judged by namchain-soak against a real device on the target (plan.md §1 criterion 3,
  // §10.5.1). This test also runs under qemu on the aarch64 CI job, where emulated NEON can
  // legitimately miss a 1.33 ms deadline. What is asserted is internal consistency.
  CHECK(result.p50_ns <= result.p99_ns);
  CHECK(result.p99_ns <= result.max_ns);
  CHECK(result.max_lateness_ms >= 0.0);
  CHECK_FALSE(result.oracle.checked);
}

TEST_CASE("a soak run reproduces the offline render sample for sample")
{
  // The strongest statement the harness can make: the block loop the soak drives produces
  // exactly the audio the deterministic offline renderer produces -- no sample dropped,
  // repeated or reordered. Run over the second half of the fixture, which is where the signal
  // is; the first second is digital silence (see SoakOptions::input_offset_seconds), and
  // comparing silence with silence would prove very little.
  namchain::SoakOptions options;
  options.quantum = 64;
  options.duration_seconds = 0.5;
  options.input_offset_seconds = 1.0;
  options.realtime_paced = false; // pacing only costs wall time; the samples cannot depend on it
  options.self_oracle = true;

  const namchain::SoakResult result = namchain::run_soak(model_path("A2-Lite.nam"), input_wav(), options);
  REQUIRE_MESSAGE(result.ok, result.error);

  CHECK(result.frames == 24000);
  REQUIRE(result.oracle.checked);
  CHECK(result.oracle.match);
  CHECK(result.oracle.max_abs == 0.0);
  CHECK(result.oracle.source == "offline self-render");
}

TEST_CASE("the oracle actually detects wrong audio")
{
  // Render a different model offline over the same span, and use it as the oracle: the
  // comparison must fail, or the test above would be proving nothing.
  namchain::OfflineRenderOptions offline_options;
  offline_options.start_frame = 48000; // 1.0 s
  offline_options.max_frames = 24000;  // 0.5 s at 48 kHz

  const std::string wrong = temp_path("wrong-oracle");
  const namchain::OfflineRenderResult offline =
    namchain::render_model_to_wav(model_path("A2-Full.nam"), input_wav(), wrong, offline_options);
  REQUIRE_MESSAGE(offline.ok, offline.error);

  namchain::SoakOptions options;
  options.quantum = 64;
  options.duration_seconds = 0.5;
  options.input_offset_seconds = 1.0;
  options.realtime_paced = false;
  options.oracle_wav = wrong;
  options.oracle_tolerance = 1e-6;

  const namchain::SoakResult result = namchain::run_soak(model_path("A2-Lite.nam"), input_wav(), options);
  REQUIRE_MESSAGE(result.ok, result.error);
  REQUIRE(result.oracle.checked);
  CHECK(result.frames == 24000);
  CHECK_FALSE(result.oracle.match);
  CHECK(result.oracle.max_abs > 1e-2);
  CHECK_FALSE(result.oracle.detail.empty());
}

TEST_CASE("soak reports errors instead of crashing")
{
  namchain::SoakOptions options;
  options.quantum = 64;
  options.duration_seconds = 0.05;

  CHECK_FALSE(namchain::run_soak("/nonexistent/model.nam", input_wav(), options).ok);

  options.quantum = 0;
  CHECK_FALSE(namchain::run_soak(model_path("A2-Lite.nam"), input_wav(), options).ok);

  // An oracle comparison on a multi-hour run is refused rather than silently attempted.
  namchain::SoakOptions long_run;
  long_run.duration_seconds = 3600.0;
  long_run.self_oracle = true;
  const namchain::SoakResult refused = namchain::run_soak(model_path("A2-Lite.nam"), input_wav(), long_run);
  CHECK_FALSE(refused.ok);
  CHECK(refused.error.find("bounded") != std::string::npos);
}
