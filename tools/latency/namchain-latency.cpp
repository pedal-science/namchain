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
    std::printf("  \"output_channel\": %u,\n  \"blocks\": %llu,\n  \"driver_xruns\": %llu,\n",
                result.output_channel, static_cast<unsigned long long>(result.blocks),
                static_cast<unsigned long long>(result.driver_xruns));
    std::printf("  \"channels\": [\n");
    for (std::size_t i = 0; i < result.channels.size(); ++i)
    {
      const namchain::ChannelLatency& channel = result.channels[i];
      std::printf("    {\"capture_channel\": %u, \"found\": %s, \"delay_frames\": %lld, "
                  "\"delay_frames_sub\": %.4f, \"delay_ms\": %.4f, \"captured_rms\": %.6f, "
                  "\"captured_peak\": %.6f, "
                  "\"clipped_frames\": %llu, \"peak_to_sidelobe_db\": %.2f, \"per_repeat_frames\": [",
                  channel.capture_channel, channel.found ? "true" : "false", channel.delay_frames,
                  channel.delay_frames_sub, channel.delay_ms, channel.captured_rms, channel.captured_peak,
                  static_cast<unsigned long long>(channel.clipped_frames), channel.peak_to_sidelobe_db);
      for (std::size_t r = 0; r < channel.per_repeat_frames.size(); ++r)
        std::printf("%s%lld", r == 0 ? "" : ", ", channel.per_repeat_frames[r]);
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
  std::printf("\n");
  std::printf("  capture  delay(frames)      delay(ms)   rms       peak     clip   peak/sidelobe   spread\n");
  for (const namchain::ChannelLatency& channel : result.channels)
  {
    if (!channel.found)
    {
      std::printf("  ch%-6u not found            -           %.5f   %.5f  %-6llu -\n", channel.capture_channel,
                  channel.captured_rms, channel.captured_peak,
                  static_cast<unsigned long long>(channel.clipped_frames));
      continue;
    }
    long long spread = 0;
    if (!channel.per_repeat_frames.empty())
    {
      const auto [low, high] = std::minmax_element(channel.per_repeat_frames.begin(),
                                                   channel.per_repeat_frames.end());
      spread = *high - *low;
    }
    std::printf("  ch%-6u %-6lld (%+.3f)     %8.4f   %.5f   %.5f  %-6llu %6.1f dB       %lld\n",
                channel.capture_channel, channel.delay_frames,
                channel.delay_frames_sub - static_cast<double>(channel.delay_frames), channel.delay_ms,
                channel.captured_rms, channel.captured_peak,
                static_cast<unsigned long long>(channel.clipped_frames), channel.peak_to_sidelobe_db, spread);
  }

  // The sanity check the harness can make for itself: a round trip cannot be shorter than the
  // device's own buffer. If this fires, one of the two numbers is wrong and the delay above should
  // not be quoted.
  bool below_floor = false;
  for (const namchain::ChannelLatency& channel : result.channels)
    if (channel.found && channel.delay_frames < static_cast<long long>(result.period_size))
      below_floor = true;
  if (below_floor)
    std::printf("\nwarning: a delay below one period was measured, which the device's buffering makes "
                "impossible; treat these numbers as suspect\n");

  return 0;
}
