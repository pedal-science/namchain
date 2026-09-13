// namchain-latency — measure true round-trip latency over a physical or internal loop.
//
// This is the measurement plan.md §10.5.1 asks for, done inside the engine rather than with
// `jdelay`, which would need JACK (deferred for the reasons in §4.3). Sending a known stimulus and
// cross-correlating what comes back measures the whole path -- host buffering, USB, converters, and
// the device's own routing -- with no assumption about how any of it is organised.
//
// If the device has a loopback channel, measuring that and an analogue input in the same run is
// worth doing: the difference between them is the converter latency, and each on its own is a
// number whose meaning depends on which one you took.
//
// Usage: namchain-latency --device hw:4,0 --capture-channels 8 --playback-channels 6
//                         --output-channel 2 --measure 0,4
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "namchain/latency.h"
#include "namchain/version.h"

namespace
{

std::vector<unsigned> parse_channels(const std::string& text)
{
  std::vector<unsigned> channels;
  std::size_t start = 0;
  while (start <= text.size())
  {
    const std::size_t comma = text.find(',', start);
    const std::string piece = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!piece.empty())
      channels.push_back(static_cast<unsigned>(std::atoi(piece.c_str())));
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return channels;
}

void usage()
{
  std::fprintf(stderr,
               "namchain-latency %s\n"
               "usage: namchain-latency --device hw:C,D [options]\n"
               "options:\n"
               "  --device SPEC        ALSA device, e.g. hw:4,0 (required)\n"
               "  --quantum N          period size in frames (default 64)\n"
               "  --periods N          buffer length in periods (default 4)\n"
               "  --sample-rate N      Hz (default 48000)\n"
               "  --capture-channels N device capture channels (default 1)\n"
               "  --playback-channels N device playback channels (default 1)\n"
               "  --output-channel N   playback channel carrying the stimulus (default 0)\n"
               "  --measure LIST       comma-separated capture channels to analyse (default 0)\n"
               "  --level X            stimulus peak, linear (default 0.25)\n"
               "  --length N           stimulus length in frames (default 4096)\n"
               "  --repeats N          how many times to send it (default 4)\n"
               "  --max-delay-ms X     upper bound on the delay searched for (default 100)\n"
               "  --json               print JSON instead of text\n"
               "exit: 0 measured, 2 usage/IO error\n",
               NAMCHAIN_VERSION_STRING);
}

} // namespace

int main(int argc, char** argv)
{
  namchain::LatencyOptions options;
  bool json = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--device" && i + 1 < argc)
      options.device = argv[++i];
    else if (arg == "--quantum" && i + 1 < argc)
      options.quantum = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--periods" && i + 1 < argc)
      options.periods = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--sample-rate" && i + 1 < argc)
      options.sample_rate = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--capture-channels" && i + 1 < argc)
      options.capture_channels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--playback-channels" && i + 1 < argc)
      options.playback_channels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--channels" && i + 1 < argc)
      options.channels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--output-channel" && i + 1 < argc)
      options.output_channel = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--measure" && i + 1 < argc)
      options.measure_channels = parse_channels(argv[++i]);
    else if (arg == "--level" && i + 1 < argc)
      options.stimulus_level = std::atof(argv[++i]);
    else if (arg == "--length" && i + 1 < argc)
      options.stimulus_length = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--repeats" && i + 1 < argc)
      options.repeats = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--max-delay-ms" && i + 1 < argc)
      options.max_delay_ms = std::atof(argv[++i]);
    else if (arg == "--runs" && i + 1 < argc)
      options.runs = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--json")
      json = true;
    else if (arg == "-h" || arg == "--help")
    {
      usage();
      return 0;
    }
    else
    {
      std::fprintf(stderr, "error: unknown or incomplete option '%s'\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (options.device.empty())
  {
    usage();
    return 2;
  }

  const namchain::LatencyResult result = namchain::measure_round_trip_latency(options);
  if (!result.ok)
  {
    std::fprintf(stderr, "error: %s\n", result.error.c_str());
    return 2;
  }

  const double frame_ms = 1000.0 / static_cast<double>(result.sample_rate);
  if (json)
  {
    std::printf("{\n");
    std::printf("  \"device\": {\"capture\": \"%s\", \"playback\": \"%s\"},\n", result.capture_node.c_str(),
                result.playback_node.c_str());
    std::printf("  \"sample_rate\": %u,\n  \"period_size\": %u,\n  \"buffer_size\": %u,\n", result.sample_rate,
                result.period_size, result.buffer_size);
    std::printf("  \"capture_channels\": %u,\n  \"playback_channels\": %u,\n", result.capture_channels,
                result.playback_channels);
    std::printf("  \"buffer_floor_frames\": %u,\n", result.buffer_floor_frames);
    std::printf("  \"timestamps_enabled\": %s,\n", result.timestamps_enabled ? "true" : "false");
    std::printf("  \"output_channel\": %u,\n  \"blocks\": %llu,\n  \"driver_xruns\": %llu,\n",
                result.output_channel, static_cast<unsigned long long>(result.blocks),
                static_cast<unsigned long long>(result.driver_xruns));
    if (!result.runs.empty())
    {
      std::printf("  \"runs\": [\n");
      for (std::size_t r = 0; r < result.runs.size(); ++r)
      {
        std::printf("    {\"ok\": %s, \"driver_xruns\": %llu, \"channels\": [",
                    result.runs[r].ok ? "true" : "false",
                    static_cast<unsigned long long>(result.runs[r].driver_xruns));
        for (std::size_t c = 0; c < result.runs[r].channels.size(); ++c)
        {
          const namchain::ChannelRun& run = result.runs[r].channels[c];
          std::printf("%s{\"capture_channel\": %u, \"found\": %s, \"delay_frames\": %lld, "
                      "\"path_frames\": %lld, \"queue_correction_frames\": %lld}",
                      c == 0 ? "" : ", ", run.capture_channel, run.found ? "true" : "false",
                      run.delay_frames, run.path_frames, run.queue_correction_frames);
        }
        std::printf("]}%s\n", r + 1 == result.runs.size() ? "" : ",");
      }
      std::printf("  ],\n");
    }
    std::printf("  \"channels\": [\n");
    for (std::size_t i = 0; i < result.channels.size(); ++i)
    {
      const namchain::ChannelLatency& channel = result.channels[i];
      std::printf("    {\"capture_channel\": %u, \"found\": %s, \"delay_frames\": %lld, "
                  "\"delay_frames_sub\": %.4f, \"delay_ms\": %.4f, \"path_frames\": %lld, "
                  "\"path_frames_sub\": %.4f, \"path_ms\": %.4f, "
                  "\"queue\": {\"frames_read\": %lld, \"capture_delay\": %lld, \"playback_delay\": %lld, "
                  "\"block_offset\": %lld, \"correction\": %lld, \"capture_hw_ptr\": %llu, "
                  "\"playback_hw_ptr\": %llu, "
                  "\"capture_tstamp_ns\": %lld, \"playback_tstamp_ns\": %lld, "
                  "\"capture_audio_tstamp_ns\": %lld, \"playback_audio_tstamp_ns\": %lld, "
                  "\"playback_audio_tstamp_valid\": %s}, "
                  "\"captured_rms\": %.6f, "
                  "\"captured_peak\": %.6f, "
                  "\"clipped_frames\": %llu, \"peak_to_sidelobe_db\": %.2f, \"per_repeat_frames\": [",
                  channel.capture_channel, channel.found ? "true" : "false", channel.delay_frames,
                  channel.delay_frames_sub, channel.delay_ms, channel.path_frames, channel.path_frames_sub,
                  channel.path_ms, channel.frames_read_before_write, channel.capture_delay_frames,
                  channel.playback_delay_frames, channel.block_offset_frames, channel.queue_correction_frames,
                  channel.capture_hw_ptr, channel.playback_hw_ptr, channel.capture_tstamp_ns,
                  channel.playback_tstamp_ns, channel.capture_audio_tstamp_ns,
                  channel.playback_audio_tstamp_ns, channel.playback_audio_tstamp_valid ? "true" : "false",
                  channel.captured_rms, channel.captured_peak,
                  static_cast<unsigned long long>(channel.clipped_frames), channel.peak_to_sidelobe_db);
      for (std::size_t r = 0; r < channel.per_repeat_frames.size(); ++r)
        std::printf("%s%lld", r == 0 ? "" : ", ", channel.per_repeat_frames[r]);
      std::printf("], \"per_repeat_path_frames\": [");
      for (std::size_t r = 0; r < channel.per_repeat_path_frames.size(); ++r)
        std::printf("%s%lld", r == 0 ? "" : ", ", channel.per_repeat_path_frames[r]);
      std::printf("]}%s\n", i + 1 == result.channels.size() ? "" : ",");
    }
    std::printf("  ]\n}\n");
    return 0;
  }

  std::printf("device:           %s\n", result.capture_node.c_str());
  std::printf("channels:         %u capture, %u playback\n", result.capture_channels, result.playback_channels);
  std::printf("period/buffer:    %u / %u frames (%.3f ms of device buffering)\n", result.period_size,
              result.buffer_size, static_cast<double>(result.buffer_size) * frame_ms);
  std::printf("stimulus:         channel %u, %u frames, %.0f dBFS peak, one send every %u frames\n",
              result.output_channel, result.stimulus_length, 20.0 * std::log10(options.stimulus_level),
              result.repeat_spacing);
  std::printf("blocks:           %llu  (%.3f s)\n", static_cast<unsigned long long>(result.blocks),
              static_cast<double>(result.captured_frames) / static_cast<double>(result.sample_rate));
  std::printf("driver xruns:     %llu\n", static_cast<unsigned long long>(result.driver_xruns));

  // What was in the rings when the stimulus went in. Printing this is not decoration: it is the one
  // term that differs between two otherwise identical runs, so when a figure looks wrong this is
  // what says whether the run or the device is responsible.
  bool have_queue = false;
  namchain::ChannelLatency first;
  for (const namchain::ChannelLatency& channel : result.channels)
    if (channel.found)
    {
      first = channel;
      have_queue = true;
      break;
    }
  if (have_queue)
  {
    std::printf("queue occupancy:  send 0: %lld read + %lld capture + %lld playback + %lld in-block "
                "- %lld stimulus offset = %lld frames\n",
                first.frames_read_before_write, first.capture_delay_frames, first.playback_delay_frames,
                first.block_offset_frames,
                first.frames_read_before_write + first.capture_delay_frames + first.playback_delay_frames +
                  first.block_offset_frames - first.queue_correction_frames,
                first.queue_correction_frames);
    if (first.per_repeat_frames.size() > 1)
    {
      std::printf("                  correction per send:");
      const std::size_t count = std::min(first.per_repeat_frames.size(), first.per_repeat_path_frames.size());
      for (std::size_t r = 0; r < count; ++r)
        std::printf(" %lld", first.per_repeat_frames[r] - first.per_repeat_path_frames[r]);
      std::printf("\n");
    }
  }
  std::printf("\n");
  std::printf("  'raw' is measured from the start of the captured stream and includes the queue\n");
  std::printf("  occupancy above. 'path' has that occupancy removed, which removes much but not all of\n");
  std::printf("  the run-to-run movement -- see the reproducibility block below. Nothing here is as\n");
  std::printf("  trustworthy as the difference between two channels of the same run.\n\n");
  std::printf("  capture  raw fr  raw ms    path fr  path ms   spread(raw/path)  rms        peak      clip   P/S\n");
  for (const namchain::ChannelLatency& channel : result.channels)
  {
    if (!channel.found)
    {
      std::printf("  ch%-6u not found  -         -        -         -                 %.5f   %.5f  %-6llu -\n",
                  channel.capture_channel, channel.captured_rms, channel.captured_peak,
                  static_cast<unsigned long long>(channel.clipped_frames));
      continue;
    }
    const auto spread_of = [](const std::vector<long long>& values) {
      if (values.size() < 2)
        return 0LL;
      const auto [low, high] = std::minmax_element(values.begin(), values.end());
      return *high - *low;
    };
    const long long spread = spread_of(channel.per_repeat_frames);
    const long long path_spread = spread_of(channel.per_repeat_path_frames);
    std::printf("  ch%-6u %-7lld %-9.4f  %-8lld %-9.4f  %lld / %-11lld %.5f   %.5f  %-6llu %.1f dB\n",
                channel.capture_channel, channel.delay_frames, channel.delay_ms, channel.path_frames,
                channel.path_ms, spread, path_spread, channel.captured_rms, channel.captured_peak,
                static_cast<unsigned long long>(channel.clipped_frames), channel.peak_to_sidelobe_db);
  }

  if (!result.runs.empty())
  {
    std::printf("\nreproducibility: %zu runs of the same configuration\n", result.runs.size());
    std::printf("  capture  raw min..max     spread   path min..max    spread   queue correction\n");
    for (std::size_t c = 0; c < result.channels.size(); ++c)
    {
      std::vector<long long> raw;
      std::vector<long long> path;
      std::vector<long long> correction;
      for (const namchain::LatencyRun& run : result.runs)
        for (const namchain::ChannelRun& channel : run.channels)
          if (channel.capture_channel == result.channels[c].capture_channel && channel.found)
          {
            raw.push_back(channel.delay_frames);
            path.push_back(channel.path_frames);
            correction.push_back(channel.queue_correction_frames);
          }
      if (raw.empty())
        continue;
      const auto extremes = [](const std::vector<long long>& values) {
        const auto [low, high] = std::minmax_element(values.begin(), values.end());
        return std::pair<long long, long long>{*low, *high};
      };
      const std::vector<long long> correction_range = correction;
      const auto [raw_low, raw_high] = extremes(raw);
      const auto [path_low, path_high] = extremes(path);
      const auto [correction_low, correction_high] = extremes(correction_range);
      std::printf("  ch%-6u %lld..%-12lld %-8lld %lld..%-11lld %-8lld %lld..%lld\n",
                  result.channels[c].capture_channel, raw_low, raw_high, raw_high - raw_low, path_low, path_high,
                  path_high - path_low, correction_low, correction_high);
    }
    std::printf("  The occupancy correction removes the queue term; whatever spread is left in 'path' is\n");
    std::printf("  the part of the round trip that no occupancy measurement can see. Compare channels\n");
    std::printf("  within one run, not across runs, for the figures that are exact.\n");
  }

  // The sanity checks the harness can make for itself. Both of these firing means the occupancy
  // model behind `path` does not describe this device, and the figures should not be quoted.
  bool below_floor = false;
  bool model_violated = false;
  for (const namchain::ChannelLatency& channel : result.channels)
  {
    if (!channel.found)
      continue;
    if (channel.delay_frames < static_cast<long long>(result.period_size))
      below_floor = true;
    if (channel.path_frames < 0 || channel.path_frames >= static_cast<long long>(result.buffer_size))
      model_violated = true;
  }
  if (below_floor)
    std::printf("\nwarning: a raw delay below one period was measured, which the device's buffering makes "
                "impossible; treat these numbers as suspect\n");
  if (model_violated)
    std::printf("\nwarning: a path delay outside [0, buffer) was measured, so the ring occupancy did not "
                "account for the whole difference; treat the path figures as suspect\n");

  return 0;
}
