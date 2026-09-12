// namchain-wavcmp — compare two mono WAV files sample by sample.
//
// Used by the M0 golden test (ours vs the vendored core's `render` output) and available as an
// oracle comparator elsewhere. Reports the maximum absolute difference and the RMS difference;
// exits non-zero when the maximum exceeds the tolerance.
//
// Usage: namchain-wavcmp [--max-abs X] [--quiet] <a.wav> <b.wav>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "namchain/version.h"
#include "namchain/wav.h"

namespace
{

void usage()
{
  std::fprintf(stderr,
               "namchain-wavcmp %s\n"
               "usage: namchain-wavcmp [options] <a.wav> <b.wav>\n"
               "options:\n"
               "  --max-abs X   tolerance on |a-b|; default 0 (bit-exact)\n"
               "  --quiet       only set the exit code\n"
               "exit: 0 within tolerance, 1 over tolerance, 2 usage/IO error\n",
               NAMCHAIN_VERSION_STRING);
}

} // namespace

int main(int argc, char** argv)
{
  double tolerance = 0.0;
  bool quiet = false;
  std::string a_path;
  std::string b_path;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--max-abs" && i + 1 < argc)
    {
      tolerance = std::strtod(argv[++i], nullptr);
      if (tolerance < 0.0)
      {
        std::fprintf(stderr, "error: --max-abs must be >= 0\n");
        return 2;
      }
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
    else if (a_path.empty())
      a_path = arg;
    else if (b_path.empty())
      b_path = arg;
    else
    {
      std::fprintf(stderr, "error: unexpected extra argument '%s'\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (a_path.empty() || b_path.empty())
  {
    usage();
    return 2;
  }

  namchain::WavDiff diff;
  std::string error;
  const namchain::WavCompareStatus status =
    namchain::compare_wav_mono(a_path, b_path, tolerance, diff, error);

  if (status == namchain::WavCompareStatus::Error)
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 2;
  }

  const double rms_dbfs = diff.rms > 0.0 ? 20.0 * std::log10(diff.rms) : -std::numeric_limits<double>::infinity();

  if (!quiet)
  {
    std::printf("samples:      %zu\n", diff.frames);
    std::printf("peak(a):      %.6f\n", diff.peak_a);
    std::printf("max|a-b|:     %.9g (at sample %zu)\n", diff.max_abs, diff.max_abs_index);
    std::printf("rms(a-b):     %.9g (%.2f dBFS)\n", diff.rms, rms_dbfs);
    std::printf("tolerance:    %.9g\n", tolerance);
  }

  if (status == namchain::WavCompareStatus::Differs)
  {
    if (!quiet)
    {
      if (!error.empty())
        std::printf("mismatch:     %s\n", error.c_str());
      std::printf("result:       FAIL\n");
    }
    return 1;
  }

  if (!quiet)
    std::printf("result:       PASS\n");
  return 0;
}
