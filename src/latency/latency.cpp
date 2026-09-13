#include "namchain/latency.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "namchain/pcm.h"

#if defined(__linux__)
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

Peak find_peak(const std::vector<double>& scores, long long first_lag)
{
  Peak peak;
  if (scores.empty())
    return peak;

  std::size_t best = 0;
  for (std::size_t i = 1; i < scores.size(); ++i)
    if (std::abs(scores[i]) > std::abs(scores[best]))
      best = i;

  // Sidelobe: the strongest correlation at least one stimulus length away from the peak. Using the
  // stimulus length as the exclusion zone is what makes this comparable between rigs.
  double sidelobe = 0.0;
  for (std::size_t i = 0; i < scores.size(); ++i)
    if (i + 4096 < best || i > best + 4096)
      sidelobe = std::max(sidelobe, std::abs(scores[i]));

  peak.found = true;
  peak.lag = first_lag + static_cast<long long>(best);
  peak.sub = refine_peak(scores, best);
  peak.value = std::abs(scores[best]);
  peak.sidelobe = sidelobe;
  return peak;
}

#if defined(__linux__)

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

  std::string error;
  std::unique_ptr<AlsaRawDuplex> device = AlsaRawDuplex::open(config, error);
  if (!device)
    return fail(error);

  const AlsaRawInfo info = device->info();
  const PcmFormat format = [&] {
    switch (info.sample_format)
    {
    case SNDRV_PCM_FORMAT_S16_LE: return PcmFormat::S16_LE;
    case SNDRV_PCM_FORMAT_S24_3LE: return PcmFormat::S24_3LE;
    case SNDRV_PCM_FORMAT_S32_LE: return PcmFormat::S32_LE;
    default: return PcmFormat::Unknown;
    }
  }();
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

    constexpr long long kRefineWindow = 64;
    for (unsigned repeat = 1; repeat < repeats; ++repeat)
    {
      const long long centre = static_cast<long long>(stimulus_position) +
                               static_cast<long long>(repeat) * repeat_spacing + delay;
      const DelayEstimate refined = estimate_delay(signal, stimulus, centre - kRefineWindow, centre + kRefineWindow);
      if (refined.found)
        channel.per_repeat_frames.push_back(refined.lag - first_lag -
                                           static_cast<long long>(repeat) * repeat_spacing);
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
  const Peak peak = find_peak(scores, first_lag);
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

#if defined(__linux__)
  return run_measurement(options);
#else
  return fail("the round-trip latency measurement needs the ALSA backend, which is Linux-only");
#endif
}

} // namespace namchain
