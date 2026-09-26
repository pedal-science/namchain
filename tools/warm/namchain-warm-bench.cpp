// namchain-warm-bench — the pedal's experiment E1: can every rig one interaction away be kept warm?
//
// pedal.project plan.md, Phase 3 step 8, compares four ways of holding a rig ready to switch to:
//
//   A  cold          load, parse and prewarm at the switch (measured through namchaind, not here)
//   B  silent warm   loaded and prewarmed with NAM's own prewarm, which runs the net on zeros; the
//                    switch crossfades to a model whose state is silence
//   C  caught up     as B, then the last L samples of real input are replayed into the candidate
//                    just before the switch, so its state matches what is being played
//   D  always warm   every candidate processes the live input all the time; a switch is only a
//                    crossfade between two running rigs
//
// Two modes, because the two questions need different conditions:
//
//   state        offline, deterministic. For each candidate and each switch point in a real input,
//                builds B's, C's and D's output and judges each with namchain's glitch detector
//                against the true "after" (the candidate having run all along), and reports how far
//                B and C are from it over the first 200 ms. Also times C's replay, which is what C
//                adds to a switch.
//
//   contention   real time. The live rig renders on --render-cpu at SCHED_FIFO, paced to the wall
//                clock one quantum at a time, while a second thread on --warm-cpu runs N candidate
//                rigs on the same input (arm D). Reports the render thread's block times and
//                overruns -- what D costs the audio -- and whether the warm thread keeps up, plus
//                memory per candidate held.
//
// A rig here is a NAM model followed by a cab IR (48 kHz WAV, truncated at 8192 taps like
// nc.cab.ir), each a vendored-core nam::DSP, so the work is the same as namchaind's for such a rig.
//
// Usage:
//   namchain-warm-bench state      --input in.wav --ir cab.wav --live live.nam cand1.nam [cand2.nam ...]
//                                  [--switch-every S] [--fade-ms 5] [--quantum 64]
//                                  [--replay-ms MS]   (C's replay; default the rig's own memory)
//   namchain-warm-bench contention --input in.wav --ir cab.wav --live live.nam cand1.nam [...]
//                                  [--count N] [--seconds S] [--quantum 64]
//                                  [--render-cpu 3] [--warm-cpu 2] [--priority 80]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <time.h>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/linear.h"

#include "namchain/glitch.h"
#include "namchain/rt/scheduling.h"
#include "namchain/version.h"
#include "namchain/wav.h"

namespace
{

using Clock = std::chrono::steady_clock;
constexpr double kRate = 48000.0;
constexpr std::size_t kMaxIrTaps = 8192;

[[noreturn]] void die(const std::string& message)
{
  std::fprintf(stderr, "namchain-warm-bench: %s\n", message.c_str());
  std::exit(2);
}

/// A NAM model and a cab, in series, with the float <-> NAM_SAMPLE buffers they need.
struct Rig
{
  std::unique_ptr<nam::DSP> amp;
  std::unique_ptr<nam::DSP> cab;
  std::vector<NAM_SAMPLE> a, b;
  int prewarm = 0; ///< the amp's own prewarm count: its receptive field

  Rig(const std::string& model, const std::vector<float>& ir, int max_quantum)
  {
    amp = nam::get_dsp(std::filesystem::path(model));
    if (!amp)
      die("cannot load " + model);
    cab = std::make_unique<nam::Linear>(1, 1, static_cast<int>(ir.size()), false, ir, kRate);
    amp->Reset(kRate, max_quantum); // Reset prewarms: this is arm B's state
    cab->Reset(kRate, max_quantum);
    prewarm = amp->GetPrewarmSamples();
    a.assign(static_cast<std::size_t>(max_quantum), 0);
    b.assign(static_cast<std::size_t>(max_quantum), 0);
  }

  std::size_t ir_taps = 0;

  /// Longest input this rig remembers, the amp's receptive field or the cab's length: what a
  /// catch-up has to replay.
  std::size_t memory() const { return std::max(static_cast<std::size_t>(prewarm), ir_taps); }

  void process(const float* in, float* out, int frames)
  {
    for (int i = 0; i < frames; ++i)
      a[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(in[i]);
    NAM_SAMPLE* ai[] = {a.data()};
    NAM_SAMPLE* bi[] = {b.data()};
    amp->process(ai, bi, frames);
    cab->process(bi, ai, frames);
    for (int i = 0; i < frames; ++i)
      out[i] = static_cast<float>(a[static_cast<std::size_t>(i)]);
  }

  /// Run a span through, `quantum` at a time. `out` may be null to discard.
  void run(const float* in, float* out, std::size_t frames, int quantum)
  {
    std::vector<float> scratch;
    if (out == nullptr)
      scratch.resize(static_cast<std::size_t>(quantum));
    for (std::size_t pos = 0; pos < frames; pos += static_cast<std::size_t>(quantum))
    {
      const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(quantum), frames - pos));
      process(in + pos, out != nullptr ? out + pos : scratch.data(), n);
    }
  }
};

std::unique_ptr<Rig> make_rig(const std::string& model, const std::vector<float>& ir, int quantum)
{
  auto rig = std::make_unique<Rig>(model, ir, quantum);
  rig->ir_taps = ir.size();
  return rig;
}

std::vector<float> load_ir(const std::string& path)
{
  std::vector<float> ir;
  double rate = 0;
  std::string error;
  if (!namchain::load_wav_mono(path, ir, rate, error))
    die(error);
  if (std::abs(rate - kRate) > 0.5)
    die(path + " is not 48 kHz; this bench takes 48 kHz IRs only");
  if (ir.size() > kMaxIrTaps)
    ir.resize(kMaxIrTaps);
  return ir;
}

std::vector<float> load_input(const std::string& path)
{
  std::vector<float> x;
  double rate = 0;
  std::string error;
  if (!namchain::load_wav_mono(path, x, rate, error))
    die(error);
  if (std::abs(rate - kRate) > 0.5)
    die(path + " is not 48 kHz");
  return x;
}

double dbfs(double v)
{
  return v > 0 ? 20.0 * std::log10(v) : -1000.0;
}

std::string stem(const std::string& path)
{
  return std::filesystem::path(path).stem().string();
}

long rss_kb()
{
  FILE* f = std::fopen("/proc/self/status", "r");
  if (f == nullptr)
    return -1;
  char line[256];
  long kb = -1;
  while (std::fgets(line, sizeof line, f) != nullptr)
    if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1)
      break;
  std::fclose(f);
  return kb;
}

struct Args
{
  std::string mode, input, ir, live;
  std::vector<std::string> candidates;
  double switch_every = 2.0, fade_ms = 5.0, seconds = 60.0, replay_ms = 0.0;
  int quantum = 64, count = -1, render_cpu = 3, warm_cpu = 2, priority = 80;
};

// --- state ------------------------------------------------------------------

/// Equal-power crossfade from `from` to `to` starting at `t`, over `fade` frames; `to` before `t` is
/// never read.
void crossfade(const std::vector<float>& from, const std::vector<float>& to, std::size_t t, std::size_t fade,
               std::vector<float>& y)
{
  y = from;
  for (std::size_t i = t; i < y.size(); ++i)
  {
    const double k = i - t < fade ? static_cast<double>(i - t) / static_cast<double>(fade) : 1.0;
    const double g_out = std::cos(k * M_PI / 2), g_in = std::sin(k * M_PI / 2);
    y[i] = static_cast<float>(g_out * from[i] + g_in * to[i]);
  }
}

int run_state(const Args& args)
{
  const std::vector<float> x = load_input(args.input);
  const std::vector<float> ir = load_ir(args.ir);
  const int q = args.quantum;
  const std::size_t fade = static_cast<std::size_t>(args.fade_ms * kRate / 1000.0);
  const std::size_t every = static_cast<std::size_t>(args.switch_every * kRate) / static_cast<std::size_t>(q) *
                            static_cast<std::size_t>(q);
  const std::size_t window = static_cast<std::size_t>(0.2 * kRate); // "the first 200 ms"

  std::printf("namchain-warm-bench %s  state\n", NAMCHAIN_VERSION_STRING);
  std::printf("input %s: %.1f s; ir %s: %zu taps; quantum %d; fade %.1f ms; a switch every %.2f s\n\n",
              args.input.c_str(), static_cast<double>(x.size()) / kRate, stem(args.ir).c_str(), ir.size(), q,
              args.fade_ms, static_cast<double>(every) / kRate);

  // The live rig, run all the way through once: "before" for every switch.
  std::vector<float> before(x.size());
  make_rig(args.live, ir, q)->run(x.data(), before.data(), x.size(), q);

  std::printf("%-34s %4s  %-26s %-26s %-26s  %s\n", "candidate", "arm", "clicks (glitch detector)",
              "worst err, 0-200 ms", "worst err, settled", "catch-up");
  int failures = 0;
  for (const std::string& path : args.candidates)
  {
    // D: the candidate has run all along. This is also the truth B and C are judged against.
    std::vector<float> after(x.size());
    auto always = make_rig(path, ir, q);
    always->run(x.data(), after.data(), x.size(), q);
    const std::size_t replay =
      args.replay_ms > 0 ? static_cast<std::size_t>(args.replay_ms * kRate / 1000.0) : always->memory();

    struct Arm
    {
      const char* name = "";
      int ok = 0, n = 0;
      double worst_window = -1000, worst_settled = -1000, max_hf = 0, max_d2_ratio = 0;
      std::string first_finding;
    } arms[3];
    arms[0].name = "B";
    arms[1].name = "C";
    arms[2].name = "D";

    double catchup_ms_max = 0, catchup_ms_sum = 0;
    int catchups = 0;
    for (std::size_t t = std::max(every, replay + fade); t + window + fade < x.size(); t += every)
    {
      for (Arm& arm : arms)
      {
        std::vector<float> incoming(x.size(), 0.0f);
        if (arm.name[0] == 'D')
          incoming = after;
        else
        {
          auto rig = make_rig(path, ir, q); // loaded and prewarmed: B's state
          if (arm.name[0] == 'C')
          {
            const auto t0 = Clock::now();
            rig->run(x.data() + (t - replay), nullptr, replay, q);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            catchup_ms_max = std::max(catchup_ms_max, ms);
            catchup_ms_sum += ms;
            ++catchups;
          }
          rig->run(x.data() + t, incoming.data() + t, x.size() - t, q);
        }
        std::vector<float> y;
        crossfade(before, incoming, t, fade, y);

        double w = 0, s = 0;
        for (std::size_t i = t; i < t + window; ++i)
          w = std::max(w, static_cast<double>(std::abs(incoming[i] - after[i])));
        for (std::size_t i = t + window; i < std::min(x.size(), t + every); ++i)
          s = std::max(s, static_cast<double>(std::abs(incoming[i] - after[i])));
        arm.worst_window = std::max(arm.worst_window, dbfs(w));
        arm.worst_settled = std::max(arm.worst_settled, dbfs(s));

        // Judge a span around the switch: settled "before" ahead of it, the window after it.
        const std::size_t lo = t - std::min<std::size_t>(t, window), hi = std::min(x.size(), t + window + fade);
        namchain::glitch::Options opts;
        std::string error;
        const namchain::glitch::Report r =
          namchain::glitch::analyse(y.data() + lo, before.data() + lo, after.data() + lo, hi - lo,
                                    {{t - lo, t - lo + fade}}, opts, error);
        ++arm.n;
        if (r.ok && error.empty())
          ++arm.ok;
        else if (arm.first_finding.empty())
          arm.first_finding = !error.empty() ? error
                              : !r.findings.empty() ? r.findings.front().rule + ": " + r.findings.front().detail
                                                    : "failed";
        arm.max_hf = std::max(arm.max_hf, r.max_hf_excess_db);
        if (r.continuity_evaluated && r.reference_d2 > 0)
          arm.max_d2_ratio = std::max(arm.max_d2_ratio, r.max_edit_d2 / r.reference_d2);
      }
    }

    for (const Arm& arm : arms)
    {
      char clicks[64], ww[40], ss[40], cu[64] = "";
      std::snprintf(clicks, sizeof clicks, "%d/%d clean, d2 x%.1f", arm.ok, arm.n, arm.max_d2_ratio);
      std::snprintf(ww, sizeof ww, "%s", arm.worst_window <= -999 ? "identical" : "");
      if (arm.worst_window > -999)
        std::snprintf(ww, sizeof ww, "%.1f dBFS", arm.worst_window);
      std::snprintf(ss, sizeof ss, "%s", arm.worst_settled <= -999 ? "identical" : "");
      if (arm.worst_settled > -999)
        std::snprintf(ss, sizeof ss, "%.1f dBFS", arm.worst_settled);
      if (arm.name[0] == 'C' && catchups > 0)
        std::snprintf(cu, sizeof cu, "%zu samples: mean %.2f ms, max %.2f ms", replay, catchup_ms_sum / catchups,
                      catchup_ms_max);
      std::printf("%-34.34s %4s  %-26s %-26s %-26s  %s\n", stem(path).c_str(), arm.name, clicks, ww, ss, cu);
      if (!arm.first_finding.empty())
        std::printf("%-34s       first finding: %s\n", "", arm.first_finding.c_str());
      if (arm.ok != arm.n)
        ++failures;
    }
  }
  std::printf("\nclicks: every switch judged by namchain::glitch (continuity, dropout, HF, convergence) "
              "against the candidate having run all along.\n"
              "err: |output - always-on output| after the switch; -60 dBFS is the glitch detector's "
              "convergence bar.\n");
  return failures == 0 ? 0 : 1;
}

// --- contention ---------------------------------------------------------------

struct Stats
{
  double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

Stats stats(std::vector<double> v)
{
  Stats s;
  if (v.empty())
    return s;
  std::sort(v.begin(), v.end());
  const auto at = [&](double f) { return v[std::min(v.size() - 1, static_cast<std::size_t>(f * v.size()))]; };
  s.p50 = at(0.5);
  s.p99 = at(0.99);
  s.p999 = at(0.999);
  s.max = v.back();
  return s;
}

int run_contention(const Args& args)
{
  const std::vector<float> x = load_input(args.input);
  const std::vector<float> ir = load_ir(args.ir);
  const int q = args.quantum;
  const int count = args.count >= 0 ? args.count : static_cast<int>(args.candidates.size());
  const double deadline_us = q / kRate * 1e6;

  const long rss0 = rss_kb();
  auto live = make_rig(args.live, ir, q);
  const long rss1 = rss_kb();
  std::vector<std::unique_ptr<Rig>> warm;
  for (int i = 0; i < count; ++i)
    warm.push_back(make_rig(args.candidates[static_cast<std::size_t>(i) % args.candidates.size()], ir, q));
  const long rss2 = rss_kb();

  const std::size_t blocks = static_cast<std::size_t>(args.seconds * kRate) / static_cast<std::size_t>(q);
  const std::size_t loop = x.size() / static_cast<std::size_t>(q) * static_cast<std::size_t>(q);
  std::vector<double> render_us, warm_us;
  render_us.reserve(blocks);
  warm_us.reserve(blocks);
  std::atomic<std::size_t> published{0};
  std::atomic<bool> done{false};
  std::size_t overruns = 0, max_lag = 0;
  std::string render_sched, warm_sched;

  // The warm thread follows the render thread block by block: it runs every candidate on the block
  // the render thread has just consumed, as an engine running D would.
  std::thread warmer([&] {
    warm_sched = namchain::rt::describe_scheduling(namchain::rt::apply_scheduling(args.priority - 10, args.warm_cpu));
    std::vector<float> out(static_cast<std::size_t>(q));
    std::size_t next = 0;
    while (!done.load(std::memory_order_acquire) || next < published.load(std::memory_order_acquire))
    {
      const std::size_t avail = published.load(std::memory_order_acquire);
      if (next >= avail)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      max_lag = std::max(max_lag, avail - next);
      const float* in = x.data() + (next * static_cast<std::size_t>(q)) % loop;
      const auto t0 = Clock::now();
      for (auto& rig : warm)
        rig->process(in, out.data(), q);
      warm_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      ++next;
    }
  });

  std::thread renderer([&] {
    render_sched = namchain::rt::describe_scheduling(namchain::rt::apply_scheduling(args.priority, args.render_cpu));
    std::vector<float> out(static_cast<std::size_t>(q));
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    const long period_ns = static_cast<long>(q / kRate * 1e9);
    for (std::size_t b = 0; b < blocks; ++b)
    {
      next.tv_nsec += period_ns;
      while (next.tv_nsec >= 1000000000L)
      {
        next.tv_nsec -= 1000000000L;
        ++next.tv_sec;
      }
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
      const float* in = x.data() + (b * static_cast<std::size_t>(q)) % loop;
      const auto t0 = Clock::now();
      live->process(in, out.data(), q);
      const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
      render_us.push_back(us);
      if (us > deadline_us)
        ++overruns;
      published.store(b + 1, std::memory_order_release);
    }
    done.store(true, std::memory_order_release);
  });
  renderer.join();
  warmer.join();

  const Stats r = stats(render_us), w = stats(warm_us);
  std::printf("namchain-warm-bench %s  contention\n", NAMCHAIN_VERSION_STRING);
  std::printf("live rig:   %s + %s (%zu taps), quantum %d (deadline %.0f us), %.0f s\n", stem(args.live).c_str(),
              stem(args.ir).c_str(), ir.size(), q, deadline_us, args.seconds);
  std::printf("render:     %s\n", render_sched.c_str());
  std::printf("warm:       %d candidate rig(s), %s\n", count, warm_sched.c_str());
  std::printf("memory:     live rig %ld kB, candidates %ld kB (%.0f kB each), process %ld kB\n", rss1 - rss0,
              rss2 - rss1, count > 0 ? static_cast<double>(rss2 - rss1) / count : 0.0, rss2);
  std::printf("render us:  p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f   p99 %.2f%%  max %.1f%% of deadline  "
              "overruns %zu of %zu\n",
              r.p50, r.p99, r.p999, r.max, 100 * r.p99 / deadline_us, 100 * r.max / deadline_us, overruns,
              render_us.size());
  if (count > 0)
    std::printf("warm us:    p50 %.1f  p99 %.1f  max %.1f   p99 %.1f%% of a quantum; blocks behind at worst: %zu\n",
                w.p50, w.p99, w.max, 100 * w.p99 / deadline_us, max_lag);
  return overruns == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
  Args args;
  if (argc < 2)
    die("usage: namchain-warm-bench state|contention --input in.wav --ir cab.wav --live live.nam cand.nam ...");
  args.mode = argv[1];
  for (int i = 2; i < argc; ++i)
  {
    const std::string a = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc)
        die(a + " needs a value");
      return argv[++i];
    };
    if (a == "--input")
      args.input = next();
    else if (a == "--ir")
      args.ir = next();
    else if (a == "--live")
      args.live = next();
    else if (a == "--switch-every")
      args.switch_every = std::atof(next().c_str());
    else if (a == "--replay-ms")
      args.replay_ms = std::atof(next().c_str());
    else if (a == "--fade-ms")
      args.fade_ms = std::atof(next().c_str());
    else if (a == "--seconds")
      args.seconds = std::atof(next().c_str());
    else if (a == "--quantum")
      args.quantum = std::atoi(next().c_str());
    else if (a == "--count")
      args.count = std::atoi(next().c_str());
    else if (a == "--render-cpu")
      args.render_cpu = std::atoi(next().c_str());
    else if (a == "--warm-cpu")
      args.warm_cpu = std::atoi(next().c_str());
    else if (a == "--priority")
      args.priority = std::atoi(next().c_str());
    else if (a.rfind("--", 0) == 0)
      die("unknown option " + a);
    else
      args.candidates.push_back(a);
  }
  if (args.input.empty() || args.ir.empty() || args.live.empty() || args.candidates.empty())
    die("--input, --ir, --live and at least one candidate model are required");
  if (args.mode == "state")
    return run_state(args);
  if (args.mode == "contention")
    return run_contention(args);
  die("mode must be state or contention");
}
