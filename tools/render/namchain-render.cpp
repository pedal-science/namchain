// namchain-render — offline WAV -> WAV rendering.
//
// Two modes:
//   * a single `.nam` model (the M0 path that the golden test compares against the core's own
//     `render` tool), and
//   * a compiled graph with a scripted edit timeline: the deterministic driver for the glitch
//     detector, where an edit lands at an exact sample on every run (plan.md §5.1 item 4, §10.3).
//
// Usage: namchain-render [options] <model.nam> <input.wav> <output.wav>
//        namchain-render [options] --graph <rig.json> [--timeline <edits.json>] <input.wav> <output.wav>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "namchain/graph/rig.h"
#include "namchain/offline.h"
#include "namchain/render/timeline.h"
#include "namchain/version.h"
#include "namchain/wav.h"

namespace
{

void usage()
{
  std::fprintf(stderr,
               "namchain-render %s\n"
               "usage: namchain-render [options] <model.nam> <input.wav> <output.wav>\n"
               "       namchain-render [options] --graph <rig.json> [--timeline <edits.json>] <input.wav> <output.wav>\n"
               "options:\n"
               "  --quantum N     block size in samples, 1..4096 (default 64)\n"
               "  --slim X        slimmable size in [0,1]; needs a SlimmableModel (model mode only)\n"
               "  --graph FILE    render this rig instead of a single model\n"
               "  --timeline FILE apply a scripted set_param timeline (requires --graph)\n"
               "  --torture N     generate N randomised set_param edits; an exact, repeatable timeline\n"
               "  --seed S        seed for --torture (default 1)\n"
               "  --no-glitch     do not judge the timeline with the glitch detector\n"
               "  --quiet         do not print a summary\n"
               "\n"
               "A timeline edit is {\"at_sample\":N,\"node\":\"id\",\"param\":\"gain\",\"value\":X}; the\n"
               "only op class implemented so far is the no-recompile parameter ramp. The exit code is\n"
               "non-zero when the render fails or an edit fails a glitch rule.\n",
               NAMCHAIN_VERSION_STRING);
}

int render_graph_mode(const std::string& graph_path, const std::string& timeline_path, unsigned torture,
                      uint64_t seed, bool glitch_checks, int quantum, const std::string& input_path,
                      const std::string& output_path, bool quiet)
{
  namchain::graph::Graph graph;
  std::string error;
  if (!namchain::graph::load_graph_json(graph_path, graph, error))
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  namchain::render::EditTimeline timeline;
  if (!timeline_path.empty())
  {
    if (!namchain::render::load_timeline_json(timeline_path, graph, timeline, error))
    {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
  }
  else if (torture > 0)
  {
    // The generator needs the input length; load the WAV only for that and let the driver load it
    // again for the render, so there is one frame-count source rather than two.
    std::uint64_t frames = 0;
    {
      std::vector<float> interleaved;
      unsigned channels = 0;
      double rate = 0.0;
      if (!namchain::load_wav(input_path, interleaved, channels, rate, error))
      {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
      }
      frames = channels == 0 ? 0 : interleaved.size() / channels;
    }
    timeline = namchain::render::random_timeline(graph, torture, frames, seed);
    if (timeline.ops.empty())
    {
      std::fprintf(stderr, "error: this rig has no settable parameters, so a torture timeline is empty\n");
      return 1;
    }
  }

  namchain::render::TimelineRenderOptions options;
  options.quantum = quantum;
  options.glitch_checks = glitch_checks && !timeline.ops.empty();

  const namchain::render::TimelineRenderResult result =
    namchain::render::render_timeline_to_wav(graph, timeline, input_path, output_path, options);

  if (!quiet)
  {
    std::printf("graph:    %s\n", graph_path.c_str());
    if (!timeline_path.empty())
      std::printf("timeline: %s\n", timeline_path.c_str());
    else if (torture > 0)
      std::printf("timeline: %u generated edits, seed %llu\n", torture,
                  static_cast<unsigned long long>(seed));
    std::printf("input:    %s\n", input_path.c_str());
    std::printf("output:   %s\n", output_path.c_str());
    std::printf("frames:   %llu\n", static_cast<unsigned long long>(result.frames));
    std::printf("edits:    %u (%u judged)\n", result.edits, result.judged);
    std::printf("quantum:  %d\n", quantum);
    std::printf("glitch:   %s\n",
                !options.glitch_checks ? "not checked" : (result.glitch_ok ? "pass" : "FAIL"));
    for (const namchain::glitch::Finding& finding : result.findings)
      std::printf("  %s at frame %zu: %s\n", finding.rule.c_str(), finding.frame, finding.detail.c_str());
  }

  if (!result.ok)
  {
    std::fprintf(stderr, "error: %s\n", result.error.c_str());
    return 1;
  }
  return result.glitch_ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
  namchain::OfflineRenderOptions options;
  bool quiet = false;
  std::string graph_path;
  std::string timeline_path;
  unsigned torture = 0;
  std::uint64_t seed = 1;
  bool glitch_checks = true;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    const auto value = [&]() -> const char* {
      if (i + 1 >= argc)
      {
        std::fprintf(stderr, "error: %s needs a value\n", arg.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--quantum")
      options.quantum = std::atoi(value());
    else if (arg == "--slim")
    {
      char* end = nullptr;
      const double slim = std::strtod(value(), &end);
      if (end == argv[i] || *end != '\0' || slim < 0.0 || slim > 1.0)
      {
        std::fprintf(stderr, "error: --slim needs a number in [0,1]\n");
        return 2;
      }
      options.apply_slim = true;
      options.slim_size = slim;
    }
    else if (arg == "--graph")
      graph_path = value();
    else if (arg == "--timeline")
      timeline_path = value();
    else if (arg == "--torture")
      torture = static_cast<unsigned>(std::strtoul(value(), nullptr, 10));
    else if (arg == "--seed")
      seed = std::strtoull(value(), nullptr, 10);
    else if (arg == "--no-glitch")
      glitch_checks = false;
    else if (arg == "--quiet")
      quiet = true;
    else if (arg == "-h" || arg == "--help")
    {
      usage();
      return 0;
    }
    else if (!arg.empty() && arg[0] == '-')
    {
      std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      usage();
      return 2;
    }
    else
      positional.push_back(arg);
  }

  if (!graph_path.empty())
  {
    if (options.apply_slim)
    {
      std::fprintf(stderr, "error: --slim applies to a single model; it has no meaning for a graph\n");
      return 2;
    }
    if (!timeline_path.empty() && torture > 0)
    {
      std::fprintf(stderr, "error: --timeline and --torture are alternatives, not both\n");
      return 2;
    }
    if (positional.size() != 2)
    {
      usage();
      return 2;
    }
    return render_graph_mode(graph_path, timeline_path, torture, seed, glitch_checks, options.quantum,
                             positional[0], positional[1], quiet);
  }

  if (positional.size() != 3)
  {
    usage();
    return 2;
  }
  const std::string model_path = positional[0];
  const std::string input_path = positional[1];
  const std::string output_path = positional[2];

  const namchain::OfflineRenderResult result =
    namchain::render_model_to_wav(model_path, input_path, output_path, options);
  if (!result.ok)
  {
    std::fprintf(stderr, "error: %s\n", result.error.c_str());
    return 1;
  }

  if (!quiet)
  {
    std::printf("model:   %s\n", model_path.c_str());
    std::printf("input:   %s\n", input_path.c_str());
    std::printf("output:  %s\n", output_path.c_str());
    std::printf("frames:  %d\n", result.stats.frames);
    std::printf("rate:    %.0f Hz\n", result.stats.sample_rate);
    std::printf("quantum: %d\n", result.stats.quantum);
    std::printf("channels: %d in -> %d out\n", result.stats.input_channels, result.stats.output_channels);
  }
  return 0;
}
