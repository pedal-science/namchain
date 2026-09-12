#include "namchain/soak.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "namchain/offline.h"
#include "namchain/pcm.h"
#include "namchain/rt_check.h"
#include "namchain/telemetry.h"
#include "namchain/wav.h"

#if defined(__linux__)
  #include "namchain/io/alsa_raw.h"
#endif

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

#if defined(__linux__)
namespace
{
PcmFormat pcm_format_from_alsa(unsigned format)
{
  switch (format)
  {
  case SNDRV_PCM_FORMAT_S16_LE: return PcmFormat::S16_LE;
  case SNDRV_PCM_FORMAT_S24_3LE: return PcmFormat::S24_3LE;
  case SNDRV_PCM_FORMAT_S32_LE: return PcmFormat::S32_LE;
  default: return PcmFormat::Unknown;
  }
}
} // namespace

/// Device-backed soak: capture a period, (later: run the engine), play it back, and count what the
/// driver reports. This is the passthrough stage -- no DSP yet -- so it establishes the host's
/// real-time baseline before any engine complexity exists to confuse the picture.
SoakResult run_device_soak(const SoakOptions& options)
{
  AlsaRawConfig config;
  config.capture = options.device;
  config.playback = options.device;
  config.sample_rate = options.device_sample_rate;
  config.channels = options.device_channels;
  config.quantum = options.quantum;
  config.periods = options.device_periods;
  config.require_exact_period = true; // §9.5: a silently rounded quantum is not acceptable

  std::string error;
  std::unique_ptr<AlsaRawDuplex> device = AlsaRawDuplex::open(config, error);
  if (!device)
    return fail(error);

  const AlsaRawInfo info = device->info();

  // What the engine does after start (§3.3): lock the address space so a page fault cannot stall
  // a period. Without it a soak measures the page cache as much as the engine.
  const bool memory_locked = ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;

  if (!device->start(error))
    return fail(error);

  const uint64_t deadline_ns =
    static_cast<uint64_t>(std::llround(1e9 * static_cast<double>(info.period_size) / static_cast<double>(info.sample_rate)));
  BlockTimer timer(deadline_ns);
  // Time blocked on the device, kept separate from the work we do. If wait + work exceeds the
  // deadline the run cannot keep real time; which of the two dominates says where the problem is.
  BlockTimer wait_timer(deadline_ns);
  // Phase breakdown. A single "block time" conflates our work with time spent blocked inside the
  // backend's own read/write, and those two call for completely different responses.
  BlockTimer read_timer(deadline_ns);
  BlockTimer chain_timer(deadline_ns);
  BlockTimer write_timer(deadline_ns);

  // Bytes, not samples: period_size frames x bytes_per_frame. See AlsaRawDuplex::read_capture.
  std::vector<unsigned char> buffer(static_cast<size_t>(info.period_size) * info.bytes_per_sample * info.channels);

  // Optional DSP chain. Everything here is allocated before the loop, so the render scope stays
  // allocation-free and the interposer is measuring the thing it is meant to measure.
  std::vector<std::unique_ptr<nam::DSP>> chain;
  const PcmFormat pcm_format = pcm_format_from_alsa(info.sample_format);
  std::vector<NAM_SAMPLE> mono_a;
  std::vector<NAM_SAMPLE> mono_b;
  if (!options.model.empty())
  {
    if (pcm_format == PcmFormat::Unknown)
      return fail("device sample format is not one the chain can convert");
    for (unsigned i = 0; i < std::max(1u, options.instances); ++i)
    {
      std::unique_ptr<nam::DSP> model;
      try
      {
        model = nam::get_dsp(std::filesystem::path(options.model));
      }
      catch (const std::exception& e)
      {
        return fail(std::string("failed to load chain model: ") + e.what());
      }
      if (!model)
        return fail("failed to load chain model '" + options.model + "'");
      if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1)
        return fail("the device chain currently supports mono models only");
      model->Reset(static_cast<double>(info.sample_rate), static_cast<int>(info.period_size));
      chain.push_back(std::move(model));
    }
    mono_a.assign(static_cast<size_t>(info.period_size), static_cast<NAM_SAMPLE>(0));
    mono_b.assign(static_cast<size_t>(info.period_size), static_cast<NAM_SAMPLE>(0));

    // Warm the chain before the stream starts. The first process() call is cold -- measured at
    // ~2.8 ms against an 80 us steady state -- and a stall that long can drain the ring on its
    // own. The engine is expected to pre-touch at plan build for the same reason (§3.3).
    for (auto& model : chain)
    {
      NAM_SAMPLE* in_ptrs[] = {mono_a.data()};
      NAM_SAMPLE* out_ptrs[] = {mono_b.data()};
      model->process(in_ptrs, out_ptrs, static_cast<int>(info.period_size));
      mono_a.swap(mono_b);
    }
  }
  const uint64_t total_blocks =
    std::max<uint64_t>(1, static_cast<uint64_t>(options.duration_seconds * info.sample_rate / info.period_size));

  // Bring-up iteration, deliberately untimed. The first period after START fills an empty capture
  // ring, so its cost is an artefact of starting rather than a real-time property -- every
  // configuration, passthrough included, showed exactly one overrun and it was always block 0.
  // Run one cycle outside the measurement and report xruns seen during bring-up separately from
  // those seen while running, rather than discarding either.
  if (device->wait_capture(2000, error))
  {
    std::string ignored;
    device->read_capture(buffer.data(), info.period_size, 1000, ignored);
    device->write_playback(buffer.data(), info.period_size, 1000, ignored);
  }
  const uint64_t startup_xruns = device->driver_xruns();

  rt_reset_counters();
  uint64_t first_overrun_block = std::numeric_limits<uint64_t>::max();
  const auto start = Clock::now();
  for (uint64_t block = 0; block < total_blocks; ++block)
  {
    // Wait untimed; time only the work. Measuring from the start of the wait would just report the
    // period length, which is not what the deadline is about.
    const auto tw0 = Clock::now();
    if (!device->wait_capture(2000, error))
    {
      device->stop();
      return fail(error);
    }
    const auto tw1 = Clock::now();
    wait_timer.record(
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(tw1 - tw0).count()));

    const auto t0 = Clock::now();
    if (device->read_capture(buffer.data(), info.period_size, 1000, error) < 0)
    {
      device->stop();
      return fail(error);
    }
    const auto t1 = Clock::now();
    read_timer.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    {
      // The engine's render path lives here. The read and write either side of it are the
      // backend's own syscalls and are deliberately outside the scope (namchain/rt_check.h).
      RtScope scope;
      if (!chain.empty())
      {
        const size_t width = info.bytes_per_sample;
        const unsigned in_ch = std::min(options.input_channel, info.channels - 1);
        const unsigned out_ch = std::min(options.output_channel, info.channels - 1);

        for (unsigned i = 0; i < info.period_size; ++i)
          mono_a[i] = static_cast<NAM_SAMPLE>(
            pcm_decode(buffer.data() + (static_cast<size_t>(i) * info.channels + in_ch) * width, pcm_format));

        for (auto& model : chain)
        {
          NAM_SAMPLE* in_ptrs[] = {mono_a.data()};
          NAM_SAMPLE* out_ptrs[] = {mono_b.data()};
          model->process(in_ptrs, out_ptrs, static_cast<int>(info.period_size));
          mono_a.swap(mono_b); // swap, never copy: no allocation inside the scope
        }

        for (unsigned i = 0; i < info.period_size; ++i)
          pcm_encode(buffer.data() + (static_cast<size_t>(i) * info.channels + out_ch) * width, pcm_format,
                     static_cast<float>(mono_a[i]));
      }
    }
    const auto t2 = Clock::now();
    chain_timer.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()));
    if (device->write_playback(buffer.data(), info.period_size, 1000, error) < 0)
    {
      device->stop();
      return fail(error);
    }
    const auto t3 = Clock::now();
    write_timer.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count()));
    const uint64_t block_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t0).count());
    timer.record(block_ns);
    if (block_ns > deadline_ns && first_overrun_block == std::numeric_limits<uint64_t>::max())
      first_overrun_block = block;
  }
  const auto end = Clock::now();
  const uint64_t steady_xruns = device->driver_xruns() - startup_xruns;
  device->stop();

  SoakResult result;
  result.ok = true;
  result.rt_instrumented = rt_check_available();
  result.rt_allocations = rt_allocation_count();
  result.rt_deallocations = rt_deallocation_count();
  result.device_mode = true;
  result.first_overrun_block = first_overrun_block;
  result.memory_locked = memory_locked;
  result.device_period_size = info.period_size;
  result.device_buffer_size = info.buffer_size;
  result.chain_instances = static_cast<unsigned>(chain.size());
  result.chain_model = options.model;
  result.device_sample_format = info.sample_format;
  result.device_bytes_per_sample = info.bytes_per_sample;
  result.device_period_exact = info.period_exact;
  result.device_capture_node = info.capture_node;
  result.device_playback_node = info.playback_node;

  result.blocks = timer.blocks();
  result.frames = timer.blocks() * info.period_size;
  result.deadline_overruns = timer.deadline_overruns();
  result.driver_xruns = steady_xruns;
  result.startup_driver_xruns = startup_xruns;
  result.deadline_ns = deadline_ns;
  result.read_p50_ns = read_timer.p50_ns();
  result.chain_p50_ns = chain_timer.p50_ns();
  result.write_p50_ns = write_timer.p50_ns();
  result.wait_p50_ns = wait_timer.p50_ns();
  result.wait_p99_ns = wait_timer.p99_ns();
  result.wait_max_ns = wait_timer.max_ns();
  result.p50_ns = timer.p50_ns();
  result.p99_ns = timer.p99_ns();
  result.max_ns = timer.max_ns();
  result.p99_percent_of_deadline = timer.p99_percent_of_deadline();
  result.audio_seconds = static_cast<double>(result.frames) / info.sample_rate;
  result.wall_seconds = std::chrono::duration<double>(end - start).count();
  result.rtf = result.audio_seconds > 0.0 ? result.wall_seconds / result.audio_seconds : 0.0;
  return result;
}
#endif // __linux__

} // namespace

SoakResult run_soak(const std::string& model_path, const std::string& input_wav_path, const SoakOptions& options)
{
  if (options.quantum < 1 || options.quantum > 4096)
    return fail("quantum " + std::to_string(options.quantum) + " is out of range [1, 4096]");
  if (options.duration_seconds <= 0.0)
    return fail("duration must be > 0");

  if (!options.device.empty())
  {
#if defined(__linux__)
    return run_device_soak(options);
#else
    return fail("device mode requires Linux; this build has no ALSA backend");
#endif
  }

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
  rt_reset_counters();
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
    {
      // The render-path boundary, expressed in code: everything the engine does happens here and
      // nothing here may allocate (plan.md §3.3, §10.4).
      RtScope scope;
      model->process(in_ptrs.data(), out_ptrs.data(), frames);
    }
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
  result.rt_instrumented = rt_check_available();
  result.rt_allocations = rt_allocation_count();
  result.rt_deallocations = rt_deallocation_count();
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
