// namchain-render — offline WAV -> WAV rendering through a single `.nam` model.
//
// The M0 offline path: one model, mono input, no graph and no transitions. It exists so the
// DSP result is checkable against the vendored core's own `render` tool (tests/golden), and
// it is the ancestor of the engine's `file` backend (M1).
//
// Usage: namchain-render [--quantum N] [--slim X] [--quiet] <model.nam> <input.wav> <output.wav>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "namchain/offline.h"
#include "namchain/version.h"

namespace
{

void usage()
{
  std::fprintf(stderr,
               "namchain-render %s\n"
               "usage: namchain-render [options] <model.nam> <input.wav> <output.wav>\n"
               "options:\n"
               "  --quantum N   block size in samples, 1..4096 (default 64)\n"
               "  --slim X      slimmable size in [0,1]; needs a SlimmableModel\n"
               "  --quiet       do not print a summary\n",
               NAMCHAIN_VERSION_STRING);
}

} // namespace

int main(int argc, char** argv)
{
  namchain::OfflineRenderOptions options;
  bool quiet = false;
  std::string model_path;
  std::string input_path;
  std::string output_path;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--quantum" && i + 1 < argc)
    {
      options.quantum = std::atoi(argv[++i]);
    }
    else if (arg == "--slim" && i + 1 < argc)
    {
      char* end = nullptr;
      const double value = std::strtod(argv[++i], &end);
      if (end == argv[i] || *end != '\0' || value < 0.0 || value > 1.0)
      {
        std::fprintf(stderr, "error: --slim needs a number in [0,1]\n");
        return 2;
      }
      options.apply_slim = true;
      options.slim_size = value;
    }
    else if (arg == "--quiet")
    {
      quiet = true;
    }
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
    else if (model_path.empty())
      model_path = arg;
    else if (input_path.empty())
      input_path = arg;
    else if (output_path.empty())
      output_path = arg;
    else
    {
      std::fprintf(stderr, "error: unexpected extra argument '%s'\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (model_path.empty() || input_path.empty() || output_path.empty())
  {
    usage();
    return 2;
  }

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
