// namchain-wavcmp — compare two mono WAV files sample by sample.
//
// Written for the M0 golden test (ours vs the vendored core's `render` output) and reused by
// the M2 glitch detector's oracle comparisons. Reports the maximum absolute difference and
// the RMS difference in dBFS; exits non-zero when the maximum exceeds the tolerance.
//
// Usage: namchain-wavcmp [--max-abs X] [--quiet] <a.wav> <b.wav>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

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

  std::vector<float> a;
  std::vector<float> b;
  double a_rate = 0.0;
  double b_rate = 0.0;
  std::string error;
  if (!namchain::load_wav_mono(a_path, a, a_rate, error) || !namchain::load_wav_mono(b_path, b, b_rate, error))
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 2;
  }

  if (a.size() != b.size())
  {
    std::fprintf(stderr, "error: sample count differs: %zu vs %zu\n", a.size(), b.size());
    return 1;
  }
  if (std::abs(a_rate - b_rate) > 0.5)
  {
    std::fprintf(stderr, "error: sample rate differs: %.0f vs %.0f\n", a_rate, b_rate);
    return 1;
  }

  double max_abs = 0.0;
  size_t max_index = 0;
  double sum_sq = 0.0;
  double peak = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
  {
    const double d = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
    if (d > max_abs)
    {
      max_abs = d;
      max_index = i;
    }
    sum_sq += d * d;
    peak = std::max(peak, std::abs(static_cast<double>(a[i])));
  }

  const double rms = a.empty() ? 0.0 : std::sqrt(sum_sq / static_cast<double>(a.size()));
  const double rms_dbfs = rms > 0.0 ? 20.0 * std::log10(rms) : -std::numeric_limits<double>::infinity();

  if (!quiet)
  {
    std::printf("samples:      %zu\n", a.size());
    std::printf("rate:         %.0f Hz\n", a_rate);
    std::printf("peak(a):      %.6f\n", peak);
    std::printf("max|a-b|:     %.9g (at sample %zu)\n", max_abs, max_index);
    std::printf("rms(a-b):     %.9g (%.2f dBFS)\n", rms, rms_dbfs);
    std::printf("tolerance:    %.9g\n", tolerance);
  }

  if (max_abs > tolerance)
  {
    if (!quiet)
      std::printf("result:       FAIL\n");
    return 1;
  }
  if (!quiet)
    std::printf("result:       PASS\n");
  return 0;
}
