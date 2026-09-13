#include "namchain/latency.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "namchain/pcm.h"

#if defined(__linux__)
  #include "namchain/io/alsa_params.h"
  #include "namchain/io/alsa_raw.h"
#endif

namespace namchain
{
namespace
{

LatencyResult fail(std::string message)
{
  LatencyResult result;
  result.ok = false;
  result.error = std::move(message);
  return result;
}

/// A deterministic pseudo-random +-1 sequence.
///
/// Chosen over a click because correlation buys ~10*log10(N) dB of processing gain (36 dB at 4096
/// samples), so the stimulus can be quiet enough to be inaudible while still giving an unambiguous
/// peak. It is deterministic so that a measurement is reproducible, and its autocorrelation
/// sidelobes are low, which is what makes the peak-to-sidelobe figure below meaningful as an error
/// bar rather than decoration.
void build_stimulus(std::vector<float>& out, unsigned length, double level)
{
  out.assign(length, 0.0f);
  uint32_t state = 0x1234567u;
  for (unsigned i = 0; i < length; ++i)
  {
    state = state * 1664525u + 1013904223u;
    const float sign = ((state >> 16) & 1u) != 0u ? 1.0f : -1.0f;
    out[i] = sign * static_cast<float>(level);
  }
}

/// Correlation of the stimulus against the capture over a bounded lag range.
///
/// \param lags the range is inclusive of \p first_lag and exclusive of \p last_lag.
/// \return per-lag normalised correlation, and the index of the strongest.
std::vector<double> correlate(const std::vector<float>& capture, const std::vector<float>& stimulus, long long first_lag,
                              long long last_lag)
{
  const long long count = std::max<long long>(0, last_lag - first_lag);
  std::vector<double> scores(static_cast<size_t>(count), 0.0);
  if (count == 0)
    return scores;

  const std::size_t length = stimulus.size();
  double stimulus_energy = 0.0;
  for (const float value : stimulus)
    stimulus_energy += static_cast<double>(value) * static_cast<double>(value);
  if (stimulus_energy <= 0.0)
    return scores;

  // Running energy of the capture window, so the correlation can be normalised without a second
  // pass over the stimulus for every lag.
  double window_energy = 0.0;
  for (std::size_t i = 0; i < length; ++i)
  {
    const std::size_t index = static_cast<std::size_t>(first_lag) + i;
    if (index >= capture.size())
      break;
    window_energy += static_cast<double>(capture[index]) * static_cast<double>(capture[index]);
  }

  for (long long lag = first_lag; lag < last_lag; ++lag)
  {
    const std::size_t base = static_cast<std::size_t>(lag);
    if (base + length > capture.size())
      break;

    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i)
      sum += static_cast<double>(stimulus[i]) * static_cast<double>(capture[base + i]);

    const double denominator = std::sqrt(stimulus_energy * window_energy);
    scores[static_cast<std::size_t>(lag - first_lag)] = denominator > 0.0 ? sum / denominator : 0.0;

    // Slide the window: subtract the frame leaving, add the one arriving.
    window_energy -= static_cast<double>(capture[base]) * static_cast<double>(capture[base]);
    if (base + length < capture.size())
      window_energy += static_cast<double>(capture[base + length]) * static_cast<double>(capture[base + length]);
  }
  return scores;
}

/// Refine a correlation peak to sub-sample resolution.
///
/// Fitting a parabola through the peak and its two neighbours is the standard refinement, and it
/// matters here: the analogue path's delay is not a whole number of samples, so the integer peak is
/// quantised by up to half a sample (10 us at 48 kHz) -- comparable to the converter latency we are
/// trying to separate from the buffering.
double refine_peak(const std::vector<double>& scores, std::size_t index)
{
  if (index == 0 || index + 1 >= scores.size())
    return 0.0;
  const double left = scores[index - 1];
  const double centre = scores[index];
  const double right = scores[index + 1];
  const double denominator = left - 2.0 * centre + right;
  if (denominator == 0.0)
    return 0.0;
  const double offset = 0.5 * (left - right) / denominator;
  return std::clamp(offset, -1.0, 1.0);
}

struct Peak
{
  bool found = false;
  long long lag = 0;
  double sub = 0.0;
  double value = 0.0;
  double sidelobe = 0.0;
};

Peak find_peak(const std::vector<double>& scores, long long first_lag, std::size_t exclusion)
{
  Peak peak;
  if (scores.empty())
    return peak;

  std::size_t best = 0;
  for (std::size_t i = 1; i < scores.size(); ++i)
    if (std::abs(scores[i]) > std::abs(scores[best]))
      best = i;

  // Sidelobe: the strongest correlation at least one stimulus length away from the peak. Using the
  // stimulus length as the exclusion zone is what makes this comparable between rigs -- and it has
  // to be the *actual* length, not the default: with a hardcoded 4096 a shorter stimulus would let
  // its own autocorrelation sidelobes inside the exclusion zone and report an error bar that is far
  // better than reality.
  double sidelobe = 0.0;
  for (std::size_t i = 0; i < scores.size(); ++i)
    if (i + exclusion < best || i > best + exclusion)
      sidelobe = std::max(sidelobe, std::abs(scores[i]));

  peak.found = true;
  peak.lag = first_lag + static_cast<long long>(best);
  peak.sub = refine_peak(scores, best);
  peak.value = std::abs(scores[best]);
  peak.sidelobe = sidelobe;
  return peak;
}

#if defined(__linux__)

// Why the occupancy correction is exact rather than a fudge
// --------------------------------------------------------
// Let A(t) be the frames the ADC has produced and D(t) the frames the DAC has consumed, both counted
// from their own START. They advance at the same rate, so D(t) - A(t) = phi, a constant -- the fixed
// phase between the device's input and output streams, which is a property of the device.
//
// Let j be the index of a frame in the playback stream we write (so j = stimulus_position for the
// stimulus's first frame), d the offset of that frame inside the period it was written in, and P the
// playback delay read just before that write. If our stream index 0 is played at DAC count Delta,
// then index j is played at DAC count Delta + j, and that instant is D(t_w) + P + d for the write
// time t_w. Meanwhile the capture delay C read at t_w and the N frames read so far give the ADC
// count at t_w as A(t_w) = A_origin + N + C, where A_origin is the ADC count of assembled frame 0.
//
// The stimulus's first frame is therefore produced by the ADC at index
//     A(t_w) + (P + d) + K                     [K = the loop delay we want]
// and the correlation finds it at assembled index
//     L = A(t_w) + P + d + K - A_origin.
// Substituting A(t_w) and using D(t_w) - A(t_w) = phi with D(t_w) = Delta + written - P:
//     L = N + C + P + d + K + phi
// so
//     K + phi = L - (N + C + P + d).
//
// Every term on the right is measured at the write, and the result is independent of the block
// index, of d, and of both rings' occupancy -- which is exactly the variance that made the first
// version of this measurement irreproducible. What it retains is phi, the device's own input/output
// phase, so the figure is a property of the device and its period configuration, not of the run.
//
// The code reports `delay_frames` rather than L, i.e. L minus absolute_start, so what it subtracts
// is `N + C + P + d - absolute_start` (QueueRecord::relative_correction).

LatencyResult run_measurement(const LatencyOptions& options)
{
  AlsaRawConfig config;
  config.capture = options.device;
  config.playback = options.device;
  config.sample_rate = options.sample_rate;
  config.channels = options.channels;
  config.capture_channels = options.capture_channels;
  config.playback_channels = options.playback_channels;
  config.quantum = options.quantum;
  config.periods = options.periods;
  config.require_exact_period = true;
  config.enable_timestamps = true;

  std::string error;
  std::unique_ptr<AlsaRawDuplex> device = AlsaRawDuplex::open(config, error);
  if (!device)
    return fail(error);

  const AlsaRawInfo info = device->info();
  const PcmFormat format = alsa::pcm_format(info.sample_format);
  if (format == PcmFormat::Unknown)
    return fail("the device's sample format is not one this tool can convert");

  std::vector<unsigned> measure_channels = options.measure_channels;
  if (measure_channels.empty())
    measure_channels.push_back(0);
  for (const unsigned channel : measure_channels)
    if (channel >= info.capture_channels)
      return fail("capture channel " + std::to_string(channel) + " does not exist (the device has " +
                  std::to_string(info.capture_channels) + ")");
  if (options.output_channel >= info.playback_channels)
    return fail("playback channel " + std::to_string(options.output_channel) + " does not exist (the device has " +
                std::to_string(info.playback_channels) + ")");

  std::vector<float> stimulus;
  build_stimulus(stimulus, options.stimulus_length == 0 ? 4096u : options.stimulus_length, options.stimulus_level);

  const unsigned max_delay_frames =
    static_cast<unsigned>(std::ceil(options.max_delay_ms * 0.001 * static_cast<double>(info.sample_rate)));
  const unsigned repeats = std::max(1u, options.repeats);
  // Space the repeats so that one burst's response window cannot reach into the next one.
  const unsigned stimulus_position = 8 * info.period_size;
  const unsigned repeat_spacing =
    static_cast<unsigned>(stimulus.size()) + max_delay_frames + 2 * info.period_size;

  const uint64_t total_frames =
    static_cast<uint64_t>(stimulus_position) + static_cast<uint64_t>(repeats - 1) * repeat_spacing +
    stimulus.size() + max_delay_frames + 2 * info.period_size;
  const uint64_t total_blocks = (total_frames + info.period_size - 1) / info.period_size;

  std::vector<unsigned char> capture_bytes(static_cast<size_t>(info.period_size) * info.capture_bytes_per_frame);
  std::vector<unsigned char> playback_bytes(static_cast<size_t>(info.period_size) * info.playback_bytes_per_frame);
  std::vector<std::vector<float>> captured(measure_channels.size());
  for (auto& channel : captured)
    channel.reserve(static_cast<size_t>(total_blocks * info.period_size));

  // Where each stimulus send landed in the rings.
  //
  // This is what makes the result reproducible. `delay_frames` is measured from the start of the
  // captured stream, so it necessarily includes however much audio happened to be queued in the
  // playback ring and buffered in the capture ring at the instant of the write -- and that settles
  // differently from run to run (three identical runs gave 261, 349 and 361 frames against a
  // 128-frame buffer, §9.5). These four terms are that difference, sampled at exactly the right
  // instant: after the read for this block, before the write, so both application pointers are
  // untouched by the transfer being described.
  struct QueueRecord
  {
    long long frames_read = 0;
    long long capture_delay = 0;
    long long playback_delay = 0;
    long long block_offset = 0;
    unsigned long long capture_hw_ptr = 0;
    unsigned long long playback_hw_ptr = 0;
    long long capture_tstamp_ns = 0;
    long long playback_tstamp_ns = 0;
    long long capture_audio_tstamp_ns = 0;
    long long playback_audio_tstamp_ns = 0;
    bool playback_audio_tstamp_valid = false;
    /// The absolute assembled index the stimulus starts at: `stimulus_position + repeat*spacing`.
    long long absolute_start = 0;
    /// What the *relative* delay still carries. `delay_frames` is measured from `absolute_start`
    /// rather than from the start of the stream, so the raw correction has to have that much taken
    /// back off it -- otherwise each successive send would appear to shed a whole repeat spacing.
    long long relative_correction() const
    {
      return frames_read + capture_delay + playback_delay + block_offset - absolute_start;
    }
  };
  std::vector<QueueRecord> queues(repeats);
  std::vector<bool> queued(repeats, false);

  if (!device->start(error))
    return fail(error);

  // One untimed cycle to fill the capture ring, as in the soak harness: the first period after
  // START is an artefact of starting, and including it would offset every delay by a period.
  if (device->wait_capture(2000, error))
  {
    std::string ignored;
    std::fill(playback_bytes.begin(), playback_bytes.end(), 0);
    device->read_capture(capture_bytes.data(), info.period_size, 1000, ignored);
    device->write_playback(playback_bytes.data(), info.period_size, 1000, ignored);
  }

  uint64_t written = 0;
  uint64_t blocks = 0;
  for (uint64_t block = 0; block < total_blocks; ++block)
  {
    if (!device->wait_capture(2000, error))
    {
      device->stop();
      return fail(error);
    }
    const int got = device->read_capture(capture_bytes.data(), info.period_size, 1000, error);
    if (got < 0)
    {
      device->stop();
      return fail(error);
    }

    // The capture timeline and the playback timeline are indexed by the same counter, which is what
    // makes the correlation lag directly interpretable as a delay: read and write advance together,
    // so any constant offset between them IS the round trip.
    const size_t width = info.bytes_per_sample;
    for (size_t i = 0; i < info.period_size; ++i)
      for (size_t m = 0; m < measure_channels.size(); ++m)
        captured[m].push_back(pcm_decode(capture_bytes.data() +
                                           (i * info.capture_channels + measure_channels[m]) * width,
                                         format));

    std::fill(playback_bytes.begin(), playback_bytes.end(), 0);
    for (size_t i = 0; i < info.period_size; ++i)
    {
      const uint64_t position = written + i;
      for (unsigned repeat = 0; repeat < repeats; ++repeat)
      {
        const uint64_t start = static_cast<uint64_t>(stimulus_position) + static_cast<uint64_t>(repeat) * repeat_spacing;
        if (position < start || position >= start + stimulus.size())
          continue;
        pcm_encode(playback_bytes.data() + (i * info.playback_channels + options.output_channel) * width, format,
                   stimulus[static_cast<size_t>(position - start)]);
        if (position == start)
        {
          AlsaRawOccupancy capture_occupancy;
          AlsaRawOccupancy playback_occupancy;
          if (!device->query_occupancy(true, capture_occupancy, error) ||
              !device->query_occupancy(false, playback_occupancy, error))
          {
            device->stop();
            return fail(error);
          }
          QueueRecord record;
          record.frames_read = static_cast<long long>((block + 1) * info.period_size);
          record.capture_delay = capture_occupancy.delay_frames;
          record.playback_delay = playback_occupancy.delay_frames;
          record.block_offset = static_cast<long long>(i);
          record.capture_hw_ptr = capture_occupancy.hw_ptr;
          record.playback_hw_ptr = playback_occupancy.hw_ptr;
          record.capture_tstamp_ns = capture_occupancy.tstamp_ns;
          record.playback_tstamp_ns = playback_occupancy.tstamp_ns;
          record.capture_audio_tstamp_ns = capture_occupancy.audio_tstamp_ns;
          record.playback_audio_tstamp_ns = playback_occupancy.audio_tstamp_ns;
          record.playback_audio_tstamp_valid = playback_occupancy.audio_tstamp_valid;
          record.absolute_start = static_cast<long long>(start);
          queues[repeat] = record;
          queued[repeat] = true;
        }
        break;
      }
    }
    if (device->write_playback(playback_bytes.data(), info.period_size, 1000, error) < 0)
    {
      device->stop();
      return fail(error);
    }
    written += info.period_size;
    ++blocks;
  }
  const uint64_t xruns = device->driver_xruns();
  device->stop();

  for (unsigned repeat = 0; repeat < repeats; ++repeat)
    if (!queued[repeat])
      return fail("internal: stimulus " + std::to_string(repeat) +
                  " was never written, so its queue occupancy is unknown");

  LatencyResult result;
  result.ok = true;
  result.sample_rate = info.sample_rate;
  result.period_size = info.period_size;
  result.buffer_size = info.buffer_size;
  result.capture_channels = info.capture_channels;
  result.playback_channels = info.playback_channels;
  result.buffer_floor_frames = info.buffer_size;
  result.capture_node = info.capture_node;
  result.playback_node = info.playback_node;
  result.driver_xruns = xruns;
  result.blocks = blocks;
  result.timestamps_enabled = info.timestamps_enabled;
  result.captured_frames = captured.empty() ? 0 : captured.front().size();
  result.output_channel = options.output_channel;
  result.stimulus_position = stimulus_position;
  result.stimulus_length = static_cast<unsigned>(stimulus.size());
  result.repeat_spacing = repeat_spacing;

  for (size_t m = 0; m < measure_channels.size(); ++m)
  {
    ChannelLatency channel;
    channel.capture_channel = measure_channels[m];
    const std::vector<float>& signal = captured[m];

    for (const float value : signal)
      if (std::abs(value) >= 0.999f)
        ++channel.clipped_frames;

    // The first burst searches the whole plausible range, because the delay is what is unknown. The
    // rest search a narrow window around it, which is what turns "the delay is stable" into a
    // measurement rather than a hope.
    const long long first_lag = static_cast<long long>(stimulus_position);
    const DelayEstimate peak = estimate_delay(signal, stimulus, first_lag,
                                              first_lag + static_cast<long long>(max_delay_frames));
    if (!peak.found)
    {
      result.channels.push_back(channel);
      continue;
    }

    const long long delay = peak.lag - first_lag;
    channel.found = true;

    // Level of the burst as it came back, not of the recording.
    const std::size_t window_begin = static_cast<std::size_t>(peak.lag);
    const std::size_t window_end = std::min(signal.size(), window_begin + stimulus.size());
    double sum_squares = 0.0;
    for (std::size_t i = window_begin; i < window_end; ++i)
    {
      const double value = static_cast<double>(signal[i]);
      sum_squares += value * value;
      channel.captured_peak = std::max(channel.captured_peak, std::abs(value));
    }
    channel.captured_rms = window_end > window_begin
                             ? std::sqrt(sum_squares / static_cast<double>(window_end - window_begin))
                             : 0.0;
    channel.delay_frames = delay;
    channel.delay_frames_sub = peak.lag_sub - static_cast<double>(first_lag);
    channel.delay_ms = 1000.0 * channel.delay_frames_sub / static_cast<double>(info.sample_rate);
    channel.peak_to_sidelobe_db = peak.peak_to_sidelobe_db;
    channel.per_repeat_frames.push_back(delay);

    // The occupancy correction, and the figure that survives it.
    //
    // `K + phi = L - (N + C + P + d)` with L the absolute assembled index of the peak. Since
    // `delay_frames` is L - absolute_start rather than L, the quantity still riding on it is
    // `N + C + P + d - absolute_start`, which is what comes off here. All four terms are measured at
    // the instant the stimulus entered the playback ring, so the subtraction is exact rather than
    // statistical; what is left is the delay the device itself contributes -- converters, internal
    // buffering, and the fixed phase phi between its input and output streams -- which does not
    // depend on how the run happened to interleave.
    channel.frames_read_before_write = queues[0].frames_read;
    channel.capture_delay_frames = queues[0].capture_delay;
    channel.playback_delay_frames = queues[0].playback_delay;
    channel.block_offset_frames = queues[0].block_offset;
    channel.capture_hw_ptr = queues[0].capture_hw_ptr;
    channel.playback_hw_ptr = queues[0].playback_hw_ptr;
    channel.capture_tstamp_ns = queues[0].capture_tstamp_ns;
    channel.playback_tstamp_ns = queues[0].playback_tstamp_ns;
    channel.capture_audio_tstamp_ns = queues[0].capture_audio_tstamp_ns;
    channel.playback_audio_tstamp_ns = queues[0].playback_audio_tstamp_ns;
    channel.playback_audio_tstamp_valid = queues[0].playback_audio_tstamp_valid;
    channel.queue_correction_frames = queues[0].relative_correction();
    channel.path_frames = delay - channel.queue_correction_frames;
    channel.path_frames_sub = channel.delay_frames_sub - static_cast<double>(channel.queue_correction_frames);
    channel.path_ms = 1000.0 * channel.path_frames_sub / static_cast<double>(info.sample_rate);
    channel.per_repeat_path_frames.push_back(channel.path_frames);

    constexpr long long kRefineWindow = 64;
    for (unsigned repeat = 1; repeat < repeats; ++repeat)
    {
      const long long centre = static_cast<long long>(stimulus_position) +
                               static_cast<long long>(repeat) * repeat_spacing + delay;
      const DelayEstimate refined = estimate_delay(signal, stimulus, centre - kRefineWindow, centre + kRefineWindow);
      if (refined.found)
      {
        const long long repeat_delay =
          refined.lag - first_lag - static_cast<long long>(repeat) * repeat_spacing;
        channel.per_repeat_frames.push_back(repeat_delay);
        channel.per_repeat_path_frames.push_back(repeat_delay - queues[repeat].relative_correction());
      }
    }

    result.channels.push_back(std::move(channel));
  }

  return result;
}

#endif // __linux__

} // namespace

DelayEstimate estimate_delay(const std::vector<float>& capture, const std::vector<float>& stimulus,
                             long long first_lag, long long last_lag)
{
  DelayEstimate estimate;
  if (stimulus.empty() || capture.empty() || last_lag <= first_lag)
    return estimate;

  const std::vector<double> scores = correlate(capture, stimulus, first_lag, last_lag);
  const Peak peak = find_peak(scores, first_lag, stimulus.size());
  if (!peak.found || peak.value <= 0.0)
    return estimate;

  estimate.found = true;
  estimate.lag = peak.lag;
  estimate.lag_sub = static_cast<double>(peak.lag) + peak.sub;
  estimate.peak = peak.value;
  estimate.peak_to_sidelobe_db = peak.sidelobe > 0.0 ? 20.0 * std::log10(peak.value / peak.sidelobe) : 999.0;
  return estimate;
}

LatencyResult measure_round_trip_latency(const LatencyOptions& options)
{
  if (options.device.empty())
    return fail("a device is required, e.g. hw:4,0");
  if (options.quantum < 1 || options.quantum > 4096)
    return fail("quantum " + std::to_string(options.quantum) + " is out of range [1, 4096]");
  if (options.sample_rate < 8000 || options.sample_rate > 384000)
    return fail("sample rate " + std::to_string(options.sample_rate) + " is out of range");
  if (options.stimulus_level <= 0.0 || options.stimulus_level > 1.0)
    return fail("stimulus level must be in (0, 1]");
  if (options.repeats < 1)
    return fail("repeats must be >= 1");
  if (options.runs < 1)
    return fail("runs must be >= 1");

#if defined(__linux__)
  if (options.runs <= 1)
    return run_measurement(options);

  // Repeat the whole measurement, stream start included. The spread across these runs is the honest
  // error bar on the absolute figures: it is where the time the occupancy correction cannot account
  // for actually shows up, and it is the difference between claiming reproducibility and showing it.
  LatencyResult result;
  std::vector<LatencyRun> runs;
  runs.reserve(options.runs);
  for (unsigned run = 0; run < options.runs; ++run)
  {
    LatencyResult one = run_measurement(options);
    if (!one.ok)
    {
      // A failed run fails the whole thing. Reporting a spread over only the runs that happened to
      // succeed would hide exactly the instability the repetition exists to expose.
      one.error = "run " + std::to_string(run + 1) + " of " + std::to_string(options.runs) +
                  " failed: " + one.error;
      return one;
    }

    LatencyRun summary;
    summary.ok = true;
    summary.driver_xruns = one.driver_xruns;
    for (const ChannelLatency& channel : one.channels)
      summary.channels.push_back(ChannelRun{channel.capture_channel, channel.found, channel.delay_frames,
                                            channel.path_frames, channel.queue_correction_frames});
    runs.push_back(std::move(summary));

    if (run == 0)
      result = std::move(one); // the full per-channel detail comes from the first run
  }
  result.runs = std::move(runs);
  return result;
#else
  return fail("the round-trip latency measurement needs the ALSA backend, which is Linux-only");
#endif
}

} // namespace namchain
