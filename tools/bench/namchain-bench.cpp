// namchain-bench — capability probe + real-time-factor harness for NAM models.
//
// Deliberately independent of the vendored core's own bench tools, which hardcode
// `double` buffers and a 64-sample block (tools/benchmodel.cpp) and therefore cannot
// measure the float path or sweep quantum sizes.
//
// Usage: namchain-bench <model.nam> [--quantum 32,64,128,256] [--seconds 2]
//                                  [--iters 10] [--csv]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <typeinfo>
#include <vector>

#if defined(__GNUG__)
  #include <cxxabi.h>
#endif

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "namchain/lookback.h"
#include "namchain/version.h"

namespace
{

using Clock = std::chrono::steady_clock;

std::string demangle(const char* name)
{
#if defined(__GNUG__)
  int status = 0;
  std::unique_ptr<char, void (*)(void*)> res{abi::__cxa_demangle(name, nullptr, nullptr, &status), std::free};
  if (status == 0 && res)
    return res.get();
#endif
  return name;
}

struct BlockStats
{
  double p50_us = 0.0;
  double p99_us = 0.0;
  double max_us = 0.0;
  double rtf = 0.0;          ///< process time / audio time (lower is better)
  double pct_deadline = 0.0; ///< p99 block time as a percentage of the block deadline
};

BlockStats run_quantum(nam::DSP& model, int quantum, double seconds, int iters)
{
  const int in_ch = model.NumInputChannels();
  const int out_ch = model.NumOutputChannels();
  const double rate = model.GetExpectedSampleRate() > 0 ? model.GetExpectedSampleRate() : 48000.0;

  std::vector<std::vector<NAM_SAMPLE>> in(static_cast<size_t>(in_ch));
  std::vector<std::vector<NAM_SAMPLE>> out(static_cast<size_t>(out_ch));
  std::vector<NAM_SAMPLE*> in_ptr(static_cast<size_t>(in_ch));
  std::vector<NAM_SAMPLE*> out_ptr(static_cast<size_t>(out_ch));
  for (int c = 0; c < in_ch; ++c)
  {
    in[static_cast<size_t>(c)].assign(static_cast<size_t>(quantum), static_cast<NAM_SAMPLE>(0));
    in_ptr[static_cast<size_t>(c)] = in[static_cast<size_t>(c)].data();
  }
  for (int c = 0; c < out_ch; ++c)
  {
    out[static_cast<size_t>(c)].assign(static_cast<size_t>(quantum), static_cast<NAM_SAMPLE>(0));
    out_ptr[static_cast<size_t>(c)] = out[static_cast<size_t>(c)].data();
  }

  // Deterministic broadband-ish excitation: a sine plus a small square component keeps the
  // model out of any all-zero shortcuts without depending on RNG.
  double phase = 0.0;
  const double inc = 2.0 * 3.14159265358979 * 440.0 / rate;
  auto fill_input = [&]() {
    for (int i = 0; i < quantum; ++i)
    {
      const double s = 0.5 * std::sin(phase) + 0.1 * (std::sin(3.0 * phase) > 0 ? 1.0 : -1.0);
      phase += inc;
      in[0][static_cast<size_t>(i)] = static_cast<NAM_SAMPLE>(s);
    }
    for (int c = 1; c < in_ch; ++c)
      std::memcpy(in[static_cast<size_t>(c)].data(), in[0].data(), sizeof(NAM_SAMPLE) * static_cast<size_t>(quantum));
  };

  model.Reset(rate, quantum);

  const long long audio_samples = static_cast<long long>(seconds * rate);
  const long long blocks = audio_samples / quantum;

  // Warm-up (also settles caches and any lazily-built state).
  for (long long b = 0; b < blocks; ++b)
  {
    fill_input();
    model.process(in_ptr.data(), out_ptr.data(), quantum);
  }

  std::vector<double> block_us;
  block_us.reserve(static_cast<size_t>(blocks));
  for (int it = 0; it < iters; ++it)
  {
    for (long long b = 0; b < blocks; ++b)
    {
      fill_input();
      const auto t0 = Clock::now();
      model.process(in_ptr.data(), out_ptr.data(), quantum);
      const auto t1 = Clock::now();
      if (it == iters - 1)
      {
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        block_us.push_back(us);
      }
    }
  }

  BlockStats st;
  if (!block_us.empty())
  {
    std::sort(block_us.begin(), block_us.end());
    const size_t n = block_us.size();
    double total = 0.0;
    for (double v : block_us)
      total += v;
    st.p50_us = block_us[n / 2];
    st.p99_us = block_us[std::min(n - 1, static_cast<size_t>(0.99 * static_cast<double>(n)))];
    st.max_us = block_us[n - 1];
    const double audio_us = static_cast<double>(audio_samples) / rate * 1e6;
    st.rtf = total / audio_us;
    const double deadline_us = static_cast<double>(quantum) / rate * 1e6;
    st.pct_deadline = 100.0 * st.p99_us / deadline_us;
  }
  return st;
}

} // namespace

int main(int argc, char** argv)
{
  std::string path;
  std::vector<int> quanta{32, 64, 128, 256};
  double seconds = 2.0;
  int iters = 10;
  bool csv = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--quantum" && i + 1 < argc)
    {
      quanta.clear();
      std::string list = argv[++i];
      size_t pos = 0;
      while (pos <= list.size())
      {
        const size_t comma = list.find(',', pos);
        const std::string tok = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!tok.empty())
          quanta.push_back(std::atoi(tok.c_str()));
        if (comma == std::string::npos)
          break;
        pos = comma + 1;
      }
    }
    else if (a == "--seconds" && i + 1 < argc)
      seconds = std::atof(argv[++i]);
    else if (a == "--iters" && i + 1 < argc)
      iters = std::atoi(argv[++i]);
    else if (a == "--csv")
      csv = true;
    else if (a.rfind("--", 0) != 0)
      path = a;
    else
    {
      std::fprintf(stderr, "unknown option: %s\n", a.c_str());
      return 2;
    }
  }

  if (path.empty())
  {
    std::fprintf(stderr,
                 "namchain-bench %s\n"
                 "usage: namchain-bench <model.nam> [--quantum 32,64,128,256] [--seconds 2]\n"
                 "                      [--iters 10] [--csv]\n",
                 NAMCHAIN_VERSION_STRING);
    return 2;
  }

  std::unique_ptr<nam::DSP> model;
  try
  {
    model = nam::get_dsp(std::filesystem::path(path));
  }
  catch (const std::exception& e)
  {
    std::fprintf(stderr, "load failed: %s\n", e.what());
    return 1;
  }
  if (!model)
  {
    std::fprintf(stderr, "load failed: %s\n", path.c_str());
    return 1;
  }

  const namchain::LookbackResult lb = namchain::compute_lookback_from_file(path);

  if (!csv)
  {
    std::printf("model:        %s\n", path.c_str());
    std::printf("cpp_type:     %s\n", demangle(typeid(*model).name()).c_str());
    std::printf("channels:     in=%d out=%d\n", model->NumInputChannels(), model->NumOutputChannels());
    std::printf("expected rate:%.1f Hz\n", model->GetExpectedSampleRate());
    std::printf("core prewarm: %d samples\n", model->GetPrewarmSamples());
    std::printf("lookback:     %d samples (%s) [%s]\n", lb.samples, namchain::to_string(lb.status), lb.detail.c_str());
    std::printf("sample type:  %s\n", sizeof(NAM_SAMPLE) == 4 ? "float" : "double");
    std::printf("\n");
    std::printf("%8s %10s %10s %10s %8s %12s\n", "quantum", "p50_us", "p99_us", "max_us", "RTF", "p99%deadline");
  }
  else
  {
    std::printf("model,quantum,p50_us,p99_us,max_us,rtf,pct_deadline\n");
  }

  for (int q : quanta)
  {
    if (q < 1 || q > 4096)
    {
      std::fprintf(stderr, "quantum %d out of range [1,4096]\n", q);
      return 2;
    }
    // Reset() may prewarm by default; that is intended here (a real host warms before
    // going live) but it is excluded from the timed region by run_quantum()'s warm-up.
    const BlockStats st = run_quantum(*model, q, seconds, iters);
    if (csv)
      std::printf("%s,%d,%.1f,%.1f,%.1f,%.4f,%.2f\n", std::filesystem::path(path).stem().string().c_str(), q, st.p50_us,
                  st.p99_us, st.max_us, st.rtf, st.pct_deadline);
    else
      std::printf("%8d %10.1f %10.1f %10.1f %8.4f %11.2f%%\n", q, st.p50_us, st.p99_us, st.max_us, st.rtf,
                  st.pct_deadline);
  }

  return 0;
}
