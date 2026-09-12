#include "namchain/soak.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "namchain/offline.h"
#include "namchain/telemetry.h"
#include "namchain/wav.h"

namespace namchain
{
namespace
{

using Clock = std::chrono::steady_clock;

/// Oracle comparison is quadratic-ish in memory (the whole rendered output is retained), so a
/// multi-hour soak must not ask for it. Timing-only runs are unaffected.
constexpr double kMaxOracleSeconds = 300.0;

SoakResult fail(std::string message)
{
  SoakResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

std::string temp_wav_path(const std::string& tag)
{
  static int counter = 0;
  const auto dir = std::filesystem::temp_directory_path() / "namchain-soak";
  std::filesystem::create_directories(dir);
  return (dir / (tag + "-" + std::to_string(++counter) + ".wav")).string();
}

struct TempFileGuard
{
  std::string path;
  bool keep = false;
  ~TempFileGuard()
  {
    if (!keep && !path.empty())
      std::filesystem::remove(path);
  }
};

} // namespace

SoakResult run_soak(const std::string& model_path, const std::string& input_wav_path, const SoakOptions& options)
{
  if (options.quantum < 1 || options.quantum > 4096)
    return fail("quantum " + std::to_string(options.quantum) + " is out of range [1, 4096]");
  if (options.duration_seconds <= 0.0)
    return fail("duration must be > 0");

  const bool want_oracle = options.self_oracle || !options.oracle_wav.empty();
  if (want_oracle && options.duration_seconds > kMaxOracleSeconds)
    return fail("oracle comparison is intended for bounded runs (<= " + std::to_string(static_cast<int>(kMaxOracleSeconds)) +
                " s); a multi-hour soak should check timing only");

  std::unique_ptr<nam::DSP> model;
  try
  {
    model = nam::get_dsp(std::filesystem::path(model_path));
  }
  catch (const std::exception& e)
  {
    return fail("failed to load model '" + model_path + "': " + e.what());
  }
  if (!model)
    return fail("failed to load model '" + model_path + "'");

  std::vector<float> input;
  double input_rate = 0.0;
  std::string wav_error;
  if (!load_wav_mono(input_wav_path, input, input_rate, wav_error))
    return fail(wav_error);

  const double expected_rate = model->GetExpectedSampleRate();
  if (expected_rate > 0.0 && std::abs(input_rate - expected_rate) > 0.5)
    return fail("input WAV sample rate does not match the model's expected rate");
  const double rate = expected_rate > 0.0 ? expected_rate : input_rate;

  // A self-oracle compare must be a like-for-like render. If the run is longer than the input
  // file the input is looped, and there is no bounded offline render to compare against, so
  // say so rather than produce a comparison that means nothing.
  const double input_seconds = static_cast<double>(input.size()) / input_rate;
  if (options.input_offset_seconds < 0.0)
    return fail("input offset must be >= 0");
  if (options.self_oracle && options.input_offset_seconds + options.duration_seconds > input_seconds + 1e-9)
  {
    return fail("self-oracle requires offset + duration (" + std::to_string(options.input_offset_seconds) + " + " +
                std::to_string(options.duration_seconds) + " s) to stay within the input file (" +
                std::to_string(input_seconds) +
                " s); use --oracle FILE to compare a looped run against a fixed reference");
  }
  const size_t input_start = std::min<size_t>(static_cast<size_t>(options.input_offset_seconds * input_rate),
                                              input.size() - 1);

  const int in_channels = model->NumInputChannels();
  const int out_channels = model->NumOutputChannels();
  if (in_channels != 1)
    return fail("soak harness supports mono input only for now");
  if (want_oracle && out_channels != 1)
    return fail("oracle comparison needs mono output");

  try
  {
    model->Reset(rate, options.quantum);
  }
  catch (const std::exception& e)
  {
    return fail(std::string("Reset failed: ") + e.what());
  }

  std::vector<std::vector<NAM_SAMPLE>> in_buffers(static_cast<size_t>(in_channels));
  std::vector<std::vector<NAM_SAMPLE>> out_buffers(static_cast<size_t>(out_channels));
  std::vector<NAM_SAMPLE*> in_ptrs(static_cast<size_t>(in_channels));
  std::vector<NAM_SAMPLE*> out_ptrs(static_cast<size_t>(out_channels));
  for (int ch = 0; ch < in_channels; ++ch)
  {
    in_buffers[static_cast<size_t>(ch)].assign(static_cast<size_t>(options.quantum), static_cast<NAM_SAMPLE>(0));
    in_ptrs[static_cast<size_t>(ch)] = in_buffers[static_cast<size_t>(ch)].data();
  }
  for (int ch = 0; ch < out_channels; ++ch)
  {
    out_buffers[static_cast<size_t>(ch)].assign(static_cast<size_t>(options.quantum), static_cast<NAM_SAMPLE>(0));
    out_ptrs[static_cast<size_t>(ch)] = out_buffers[static_cast<size_t>(ch)].data();
  }

  const uint64_t deadline_ns = static_cast<uint64_t>(std::llround(1e9 * static_cast<double>(options.quantum) / rate));
  BlockTimer timer(deadline_ns);

  const uint64_t total_frames = static_cast<uint64_t>(options.duration_seconds * rate);
  const uint64_t total_blocks = std::max<uint64_t>(1, total_frames / static_cast<uint64_t>(options.quantum));

  // Keep the audio only when something will consume it: a one-hour run is ~690 MB as float32.
  const bool keep_audio = want_oracle || !options.output_wav.empty();
  std::vector<float> rendered;
  if (keep_audio)
    rendered.reserve(static_cast<size_t>(total_blocks * static_cast<uint64_t>(options.quantum)));

  size_t input_pos = input_start;
  double max_lateness_ns = 0.0;
  const auto start = Clock::now();
  uint64_t expected_elapsed_ns = 0;

  for (uint64_t block = 0; block < total_blocks; ++block)
  {
    const int frames = options.quantum;
    for (int i = 0; i < frames; ++i)
    {
      in_buffers[0][static_cast<size_t>(i)] = static_cast<NAM_SAMPLE>(input[input_pos]);
      // Loop back to the offset, not to the file start: an xrun is a timing failure and content
      // does not matter to it, but CPU load does, and the start of this fixture is silence.
      if (++input_pos >= input.size())
        input_pos = input_start;
    }

    const auto t0 = Clock::now();
    model->process(in_ptrs.data(), out_ptrs.data(), frames);
    const auto t1 = Clock::now();

    const uint64_t duration_ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    timer.record(duration_ns);

    if (keep_audio)
    {
      for (int i = 0; i < frames; ++i)
        rendered.push_back(static_cast<float>(out_buffers[0][static_cast<size_t>(i)]));
    }

    if (options.realtime_paced)
    {
      expected_elapsed_ns += deadline_ns;
      const auto target = start + std::chrono::nanoseconds(expected_elapsed_ns);
      const auto now = Clock::now();
      if (now < target)
      {
        std::this_thread::sleep_until(target);
      }
      else
      {
        const double late_ns = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now - target).count());
        max_lateness_ns = std::max(max_lateness_ns, late_ns);
      }
    }
  }

  const auto end = Clock::now();
  const double wall_seconds = std::chrono::duration<double>(end - start).count();
  const double audio_seconds = static_cast<double>(total_blocks * static_cast<uint64_t>(options.quantum)) / rate;

  SoakResult result;
  result.ok = true;
  result.blocks = timer.blocks();
  result.frames = total_blocks * static_cast<uint64_t>(options.quantum);
  result.deadline_overruns = timer.deadline_overruns();
  result.driver_xruns = 0; // no device backend yet; alsa_raw/pipewire will drive this
  result.deadline_ns = deadline_ns;
  result.p50_ns = timer.p50_ns();
  result.p99_ns = timer.p99_ns();
  result.max_ns = timer.max_ns();
  result.p99_percent_of_deadline = timer.p99_percent_of_deadline();
  result.audio_seconds = audio_seconds;
  result.wall_seconds = wall_seconds;
  result.rtf = audio_seconds > 0.0 ? wall_seconds / audio_seconds : 0.0;
  result.max_lateness_ms = max_lateness_ns / 1e6;

  // ---- oracle: does a paced real-time-shaped run produce the same samples as offline? ----
  if (want_oracle)
  {
    TempFileGuard paced_file;
    paced_file.path = options.output_wav.empty() ? temp_wav_path("paced") : options.output_wav;
    paced_file.keep = !options.output_wav.empty();

    std::string save_error;
    if (!save_wav_float32(paced_file.path, rendered.data(), rendered.size(), out_channels, rate, save_error))
      return fail(save_error);

    std::string reference_path;
    TempFileGuard reference_file;
    if (options.self_oracle)
    {
      reference_file.path = temp_wav_path("offline");
      OfflineRenderOptions offline;
      offline.quantum = options.quantum;
      // Bound the offline render to exactly the frames this run produced, starting where it
      // started. Without this the comparison would fail on length alone and quietly be
      // measuring nothing.
      offline.max_frames = result.frames;
      offline.start_frame = input_start;
      const OfflineRenderResult offline_result =
        render_model_to_wav(model_path, input_wav_path, reference_file.path, offline);
      if (!offline_result.ok)
        return fail("offline oracle render failed: " + offline_result.error);
      reference_path = reference_file.path;
      result.oracle.source = "offline self-render";
    }
    else
    {
      reference_path = options.oracle_wav;
      result.oracle.source = options.oracle_wav;
    }

    WavDiff diff;
    std::string compare_error;
    const WavCompareStatus status =
      compare_wav_mono(reference_path, paced_file.path, options.oracle_tolerance, diff, compare_error);
    result.oracle.checked = true;
    result.oracle.frames = diff.frames;
    result.oracle.max_abs = diff.max_abs;
    result.oracle.rms = diff.rms;
    result.oracle.match = (status == WavCompareStatus::Match);
    if (status == WavCompareStatus::Error)
      return fail(compare_error);
    if (status == WavCompareStatus::Differs)
      result.oracle.detail = compare_error.empty()
                               ? "output differs from the oracle by more than the tolerance"
                               : compare_error;
  }

  return result;
}

} // namespace namchain
