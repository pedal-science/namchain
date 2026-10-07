// namchain-warm-bench — the pedal's experiments E1 and E2 on holding a rig ready to switch to.
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
// E1 (run 2026-09-26) answered B, C and D for one model plus an IR, and the contention cost of D.
// The tier design that came out of it (pedal.project docs/rig-switch-strategies.md) rests on four
// things E1 did not measure, which are E2 and the last four modes here:
//
//   slim         item 1. Lite against Full, for every model: the null between them, and whether
//                handing over from one to the other is audible. Reports the null, and judges a
//                5 ms equal-power handover at every switch point with the glitch detector in both
//                directions; --audition writes the same handovers as WAVs to listen to.
//
//   chain        item 2. Catch-up convergence for chains of models in series, against replay
//                length per model. Bringing stage k up to date needs the true input from
//                (k-1) x replay before the switch, and each stage then costs (d-k+1) x replay
//                samples, so the work is d(d+1)/2 single-model replays — the plan's estimate,
//                which this mode measures rather than assumes.
//
//   ir           item 4. An IR kept warm as a record of its input rather than run: at the switch
//                the convolver is reset and filled from the last `taps` samples of that record.
//                nam::Linear's state is exactly its last receptive_field inputs, so this should
//                be exact; the mode says by how much it is not.
//
//   contention   item 3, and E1's arm D. The live rig renders on --render-cpu at SCHED_FIFO, paced
//                to the wall clock one quantum at a time, while a second thread on --warm-cpu runs
//                N candidate rigs on the same input. Reports the render thread's block times and
//                overruns -- what warm work costs the audio -- and whether the warm thread keeps
//                up, plus memory per candidate held. --slim and --no-ir set what is being kept
//                warm (a Lite or Full submodel, with or without a cab); --spinner loads the warm
//                core as well, for "keeps up on a housekeeping core under load".
//
// A rig here is a NAM model followed by a cab IR (48 kHz WAV, truncated at 8192 taps like
// nc.cab.ir), each a vendored-core nam::DSP, so the work is the same as namchaind's for such a rig.
// --no-ir drops the cab, which is what the density numbers are about.
//
// Usage:
//   namchain-warm-bench state      --input in.wav --ir cab.wav --live live.nam cand1.nam [cand2.nam ...]
//                                  [--switch-every S] [--fade-ms 5] [--quantum 64]
//                                  [--replay-ms MS]   (C's replay; default the rig's own memory)
//   namchain-warm-bench slim       --input in.wav cand1.nam [...] [--ir cab.wav]
//                                  [--switch-every S] [--fade-ms 5] [--slim 0.0] [--audition DIR]
//   namchain-warm-bench chain      --input in.wav --live m1.nam [m2.nam ...] [--depth 4]
//                                  [--replays 85,170,340,680] [--switch-every S]
//   namchain-warm-bench ir         --input in.wav --ir cab.wav --live model.nam [--switch-every S]
//   namchain-warm-bench contention --input in.wav --ir cab.wav --live live.nam cand1.nam [...]
//                                  [--count N] [--seconds S] [--quantum 64] [--slim X] [--no-ir]
//                                  [--spinner] [--render-cpu 3] [--warm-cpu 2] [--priority 80]
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <thread>
#include <vector>

#include <time.h>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/linear.h"
#include "NAM/slimmable.h"

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

  /// `ir` empty means no cab at all (--no-ir): the density question is about models, and E1 showed
  /// the cab is about half the cost of a rig on the render core.
  /// `slim` < 0 leaves the model's own default, which for a SlimmableContainer is its largest
  /// submodel (pedal.project's Full); 0.0 selects the first submodel (Lite).
  Rig(const std::string& model, const std::vector<float>& ir, int max_quantum, double slim = -1.0)
  {
    amp = nam::get_dsp(std::filesystem::path(model));
    if (!amp)
      die("cannot load " + model);
    if (slim >= 0.0)
    {
      auto* slimmable = dynamic_cast<nam::SlimmableModel*>(amp.get());
      if (slimmable == nullptr)
        die(model + " is not a SlimmableModel, so --slim cannot select a submodel");
      slimmable->SetSlimmableSize(slim);
    }
    if (!ir.empty())
    {
      cab = std::make_unique<nam::Linear>(1, 1, static_cast<int>(ir.size()), false, ir, kRate);
      cab->Reset(kRate, max_quantum);
      ir_taps = ir.size();
    }
    amp->Reset(kRate, max_quantum); // Reset prewarms: this is arm B's state
    prewarm = amp->GetPrewarmSamples();
    max_q = max_quantum;
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
    // The amp writes into b; the cab reads b and writes a. With no cab the output is b, not a —
    // which is where the input still is.
    if (cab)
    {
      cab->process(bi, ai, frames);
      for (int i = 0; i < frames; ++i)
        out[i] = static_cast<float>(a[static_cast<std::size_t>(i)]);
    }
    else
    {
      for (int i = 0; i < frames; ++i)
        out[i] = static_cast<float>(b[static_cast<std::size_t>(i)]);
    }
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

  /// The cab's own output for a span, with the model's output `record` as its input: what an IR
  /// kept as a record of its input has to be filled from. Chunked at the quantum the cab was reset
  /// with, which is the buffer size it was told to expect.
  void cab_of(const std::vector<float>& record, std::size_t from, std::size_t frames, std::vector<float>& out)
  {
    if (!cab)
      die("this rig has no cab");
    out.resize(frames);
    std::vector<NAM_SAMPLE> in(static_cast<std::size_t>(max_q)), sink(static_cast<std::size_t>(max_q));
    for (std::size_t pos = 0; pos < frames; pos += static_cast<std::size_t>(max_q))
    {
      const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(max_q), frames - pos));
      for (int i = 0; i < n; ++i)
        in[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(record[from + pos + static_cast<std::size_t>(i)]);
      NAM_SAMPLE* pi[] = {in.data()};
      NAM_SAMPLE* po[] = {sink.data()};
      cab->process(pi, po, n);
      for (int i = 0; i < n; ++i)
        out[pos + static_cast<std::size_t>(i)] = static_cast<float>(sink[static_cast<std::size_t>(i)]);
    }
  }

  /// Back to the state a freshly loaded model has, which is what a candidate is caught up from.
  void reset(int max_quantum)
  {
    amp->Reset(kRate, max_quantum);
    if (cab)
      cab->Reset(kRate, max_quantum);
  }

  /// Just the cab: item 4 keeps the model running and fills the convolver from its record.
  void reset_cab() { cab->Reset(kRate, max_q); }

  std::size_t cab_taps() const { return ir_taps; }

  int max_q = 64;
};

std::unique_ptr<Rig> make_rig(const std::string& model, const std::vector<float>& ir, int quantum,
                             double slim = -1.0)
{
  return std::make_unique<Rig>(model, ir, quantum, slim);
}

/// One model in a chain: its own NAM_SAMPLE buffers and a float buffer its caller reads.
struct Stage
{
  std::unique_ptr<nam::DSP> model;
  std::vector<NAM_SAMPLE> in, out;
  std::vector<float> fout;
  int prewarm = 0;

  Stage(const std::string& path, int max_quantum, double slim)
  {
    model = nam::get_dsp(std::filesystem::path(path));
    if (!model)
      die("cannot load " + path);
    if (slim >= 0.0)
    {
      auto* slimmable = dynamic_cast<nam::SlimmableModel*>(model.get());
      if (slimmable == nullptr)
        die(path + " is not a SlimmableModel, so --slim cannot select a submodel");
      slimmable->SetSlimmableSize(slim);
    }
    model->Reset(kRate, max_quantum);
    prewarm = model->GetPrewarmSamples();
    in.assign(static_cast<std::size_t>(max_quantum), 0);
    out.assign(static_cast<std::size_t>(max_quantum), 0);
    fout.assign(static_cast<std::size_t>(max_quantum), 0);
  }

  void reset(int max_quantum) { model->Reset(kRate, max_quantum); }

  /// Process `frames` (<= max_quantum) and return the stage's float output.
  const float* process(const float* src, int frames)
  {
    for (int i = 0; i < frames; ++i)
      in[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(src[i]);
    NAM_SAMPLE* pi[] = {in.data()};
    NAM_SAMPLE* po[] = {out.data()};
    model->process(pi, po, frames);
    for (int i = 0; i < frames; ++i)
      fout[static_cast<std::size_t>(i)] = static_cast<float>(out[static_cast<std::size_t>(i)]);
    return fout.data();
  }
};

/// Models in series, each stage fed the one before it.
struct Chain
{
  std::vector<std::unique_ptr<Stage>> stages;
  int quantum = 64;

  Chain(const std::vector<std::string>& models, int max_quantum, double slim) : quantum(max_quantum)
  {
    for (const std::string& m : models)
      stages.push_back(std::make_unique<Stage>(m, max_quantum, slim));
  }

  std::size_t depth() const { return stages.size(); }

  /// What one stage remembers; a chain's is its deepest stage's, since every stage is replayed for
  /// the same length in the mode below.
  std::size_t memory() const
  {
    std::size_t m = 0;
    for (const auto& s : stages)
      m = std::max(m, static_cast<std::size_t>(s->prewarm));
    return m;
  }

  void reset()
  {
    for (auto& s : stages)
      s->reset(quantum);
  }

  /// Run `frames` of `in` through every stage, into `out` (may be null to discard).
  void run(const float* in, float* out, std::size_t frames)
  {
    for (std::size_t pos = 0; pos < frames; pos += static_cast<std::size_t>(quantum))
    {
      const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(quantum), frames - pos));
      const float* src = in + pos;
      for (auto& s : stages)
        src = s->process(src, n);
      for (int i = 0; i < n; ++i)
        if (out != nullptr)
          out[pos + static_cast<std::size_t>(i)] = src[i];
    }
  }

  /// Catch the chain up from its reset state, the way the tier design says to: the true input's
  /// last `replay` samples per stage. Stage k is correct only once the stages before it are, so it
  /// is fed the tail of the stage before it, and its own window is (d-k+1) x replay samples long —
  /// which is where the plan's d(d+1)/2 single-model replays come from. Returns the wall time.
  double catch_up(const std::vector<float>& x, std::size_t t, std::size_t replay)
  {
    const auto t0 = Clock::now();
    const std::size_t d = depth();
    std::vector<float> sig(x.begin() + static_cast<std::ptrdiff_t>(t - d * replay),
                           x.begin() + static_cast<std::ptrdiff_t>(t));
    for (std::size_t k = 0; k < d; ++k)
    {
      stages[k]->reset(quantum);
      std::vector<float> out(sig.size());
      run_stage(k, sig, out);
      if (k + 1 < d)
        sig.assign(out.end() - static_cast<std::ptrdiff_t>(sig.size() - replay), out.end());
    }
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  }

  /// One stage over a span, for the catch-up above.
  void run_stage(std::size_t k, const std::vector<float>& in, std::vector<float>& out)
  {
    for (std::size_t pos = 0; pos < in.size(); pos += static_cast<std::size_t>(quantum))
    {
      const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(quantum), in.size() - pos));
      const float* r = stages[k]->process(in.data() + pos, n);
      std::copy(r, r + n, out.begin() + static_cast<std::ptrdiff_t>(pos));
    }
  }
};

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
  std::string mode, input, ir, live, audition;
  std::vector<std::string> candidates;
  double switch_every = 2.0, fade_ms = 5.0, seconds = 60.0, replay_ms = 0.0;
  int quantum = 64, count = -1, render_cpu = 3, warm_cpu = 2, priority = 80;
  double slim = -1.0;                 ///< submodel for every rig; <0 is the model's own default
  bool no_ir = false;                 ///< models alone, no cab: what the density numbers are about
  std::string warm_ir;                ///< contention: the candidates' own cab, default --ir
  bool warm_no_ir = false;            ///< contention: candidates without a cab, live rig keeps its own
  bool spinner = false;               ///< contention: load the warm core as well
  int depth = 4;                      ///< chain: models in series
  std::vector<double> replays{85, 170, 340, 680}; ///< chain: replay lengths per model, ms
  double audition_start = 10.0, audition_seconds = 20.0; ///< slim --audition excerpt
  std::string fade_shape = "equal-power"; ///< slim: equal-power | equal-gain | compensated
};

/// --ir is optional in every mode: with none, a rig is the model alone.
std::vector<float> load_ir_or_none(const Args& args)
{
  if (args.no_ir || args.ir.empty())
    return {};
  return load_ir(args.ir);
}

// --- state ------------------------------------------------------------------

/// The two gains at position `i` of a `fade`, for a shape and the correlation the two signals
/// actually have.
///
/// Equal power (cos/sin) is the right law for signals that are *uncorrelated*: it keeps
/// g1^2 + g2^2 == 1, so the sum's power is constant. It is the wrong law for correlated ones: the
/// power of g1*a + g2*b is g1^2 + g2^2 + 2*rho*g1*g2, which equal power takes to be 1 -- true only
/// at rho = 0, and **+3 dB at the midpoint when rho = 1**. Two submodels of the same rig are
/// rho ~ 0.996, so the fade adds up to 2.6 dB of low-frequency thump at every switch, and no rule in
/// the glitch detector sees it: it is continuous, has no dropout, no HF excess and converges.
///
/// `compensated` divides by the power the sum actually has, given rho; `equal-gain` is the plain
/// (1-k, k) law, which is exact for rho = 1 and 3 dB down at the midpoint for rho = 0.
void fade_gains(const std::string& shape, std::size_t i, std::size_t fade, double rho, double& g1, double& g2)
{
  const double k = fade == 0 ? 1.0 : std::min(1.0, static_cast<double>(i) / static_cast<double>(fade));
  if (shape == "equal-gain")
  {
    g1 = 1.0 - k;
    g2 = k;
    return;
  }
  g1 = std::cos(k * M_PI / 2);
  g2 = std::sin(k * M_PI / 2);
  if (shape == "compensated")
  {
    // **Clamped to [0, 1].** A negative rho would divide by less than one and *amplify* the fade,
    // which is the opposite of the point: an estimate that noisy (20 ms of near-silence is enough
    // to produce one) turned a +2.9 dB thump into +4.6 dB when this was written without the clamp.
    // rho = 0 leaves equal power alone, which is right for signals that really are uncorrelated.
    const double r = std::min(1.0, std::max(0.0, rho));
    const double norm = 1.0 / std::sqrt(1.0 + 2.0 * r * g1 * g2);
    g1 *= norm;
    g2 *= norm;
  }
  else if (shape != "equal-power")
  {
    die("unknown fade shape " + shape + "; equal-power, equal-gain or compensated");
  }
}

/// Crossfade from `from` to `to` starting at `t`, over `fade` frames; `to` before `t` is never read.
void crossfade(const std::vector<float>& from, const std::vector<float>& to, std::size_t t, std::size_t fade,
               std::vector<float>& y, const std::string& shape = "equal-power", double rho = 0.0)
{
  y = from;
  for (std::size_t i = t; i < y.size(); ++i)
  {
    double g_out = 1.0, g_in = 0.0;
    if (i - t < fade)
      fade_gains(shape, i - t, fade, rho, g_out, g_in);
    else
    {
      fade_gains(shape, fade, fade, rho, g_out, g_in);
    }
    y[i] = static_cast<float>(g_out * from[i] + g_in * to[i]);
  }
}

/// What the two signals' correlation is as they meet: over the `n` frames before the switch.
double correlation(const std::vector<float>& a, const std::vector<float>& b, std::size_t t, std::size_t n)
{
  const std::size_t from = t > n ? t - n : 0;
  double ab = 0, aa = 0, bb = 0;
  for (std::size_t i = from; i < t; ++i)
  {
    ab += static_cast<double>(a[i]) * b[i];
    aa += static_cast<double>(a[i]) * a[i];
    bb += static_cast<double>(b[i]) * b[i];
  }
  const double den = std::sqrt(aa * bb);
  return den > 0 ? ab / den : 0.0;
}

int run_state(const Args& args)
{
  const std::vector<float> x = load_input(args.input);
  const std::vector<float> ir = load_ir_or_none(args);
  const int q = args.quantum;
  const std::size_t fade = static_cast<std::size_t>(args.fade_ms * kRate / 1000.0);
  const std::size_t every = static_cast<std::size_t>(args.switch_every * kRate) / static_cast<std::size_t>(q) *
                            static_cast<std::size_t>(q);
  const std::size_t window = static_cast<std::size_t>(0.2 * kRate); // "the first 200 ms"

  std::printf("namchain-warm-bench %s  state\n", NAMCHAIN_VERSION_STRING);
  std::printf("input %s: %.1f s; ir %s; quantum %d; fade %.1f ms; a switch every %.2f s\n\n",
              args.input.c_str(), static_cast<double>(x.size()) / kRate,
              ir.empty() ? "none" : (stem(args.ir) + ": " + std::to_string(ir.size()) + " taps").c_str(), q,
              args.fade_ms, static_cast<double>(every) / kRate);

  // The live rig, run all the way through once: "before" for every switch.
  std::vector<float> before(x.size());
  make_rig(args.live, ir, q, args.slim)->run(x.data(), before.data(), x.size(), q);

  std::printf("%-34s %4s  %-26s %-26s %-26s  %s\n", "candidate", "arm", "clicks (glitch detector)",
              "worst err, 0-200 ms", "worst err, settled", "catch-up");
  int failures = 0;
  for (const std::string& path : args.candidates)
  {
    // D: the candidate has run all along. This is also the truth B and C are judged against.
    std::vector<float> after(x.size());
    auto always = make_rig(path, ir, q, args.slim);
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
          auto rig = make_rig(path, ir, q, args.slim); // loaded and prewarmed: B's state
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

// --- slim: E2 item 1, Lite against Full ---------------------------------------

/// The switch points the slim, chain and ir modes all use: after the longest replay and the fade,
/// one every `every` samples, with room for the 200 ms window after each.
std::vector<std::size_t> switch_points(const std::vector<float>& x, const Args& args, std::size_t lead)
{
  const std::size_t every = static_cast<std::size_t>(args.switch_every * kRate);
  const std::size_t window = static_cast<std::size_t>(0.2 * kRate);
  std::vector<std::size_t> ts;
  for (std::size_t t = std::max(every, lead); t + window < x.size(); t += every)
    ts.push_back(t);
  return ts;
}

int run_slim(const Args& args)
{
  const std::vector<float> x = load_input(args.input);
  const std::vector<float> ir = load_ir_or_none(args);
  const int q = args.quantum;
  const std::size_t fade = static_cast<std::size_t>(args.fade_ms * kRate / 1000.0);
  const std::size_t window = static_cast<std::size_t>(0.2 * kRate);
  const std::vector<std::size_t> ts = switch_points(x, args, fade);
  const bool audition = !args.audition.empty();
  if (audition)
    std::filesystem::create_directories(args.audition);

  std::printf("namchain-warm-bench %s  slim\n", NAMCHAIN_VERSION_STRING);
  std::printf("input %s: %.1f s; ir %s; quantum %d; fade %.1f ms; %zu switch points every %.2f s\n",
              args.input.c_str(), static_cast<double>(x.size()) / kRate,
              ir.empty() ? "none" : (stem(args.ir) + ": " + std::to_string(ir.size()) + " taps").c_str(), q,
              args.fade_ms, ts.size(), args.switch_every);
  std::printf("Lite is --slim 0.0 (the first submodel), Full --slim 1.0 (the last, and the default).\n");
  std::printf("fade %.1f ms, %s.\n\n", args.fade_ms, args.fade_shape.c_str());
  std::printf("%-30s %-10s %8s %8s %9s %9s  %-24s %-12s %-18s %s\n", "candidate", "direction", "lite rms",
              "full rms", "null rms", "null max", "handover (glitch detector)", "wrong-model", "fade excess",
              "first finding");
  std::printf("%-30s %-10s %8s %8s %9s %9s  %-24s %-12s %-18s\n", "", "", "dBFS", "dBFS", "dBFS", "dBFS", "",
              "0-200 ms", "dB, and rho");

  int failures = 0;
  for (const std::string& path : args.candidates)
  {
    auto full = make_rig(path, ir, q, 1.0), lite = make_rig(path, ir, q, 0.0);
    std::vector<float> F(x.size()), L(x.size());
    full->run(x.data(), F.data(), x.size(), q);
    lite->run(x.data(), L.data(), x.size(), q);

    double sq = 0, mx = 0, lsq = 0, fsq = 0;
    bool finite = true;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
      const double d = std::abs(static_cast<double>(L[i]) - F[i]);
      sq += d * d;
      mx = std::max(mx, d);
      lsq += static_cast<double>(L[i]) * L[i];
      fsq += static_cast<double>(F[i]) * F[i];
      if (!std::isfinite(L[i]) || !std::isfinite(F[i]))
        finite = false;
    }
    const double n = static_cast<double>(x.size());
    const double null_rms = dbfs(std::sqrt(sq / n));
    if (!finite)
      std::printf("%-30.30s ** NON-FINITE SAMPLES in this model's output: the numbers below mean nothing\n",
                  stem(path).c_str());

    struct Row
    {
      const char* dir;
      const std::vector<float>* before;
      const std::vector<float>* after;
      int ok = 0, n = 0;
      double worst = -1000, d2 = 0, excess = -1000, rho_mean = 0;
      std::string finding;
    } rows[2] = {{"Lite->Full", &L, &F, 0, 0, -1000, 0, -1000, 0, ""},
                 {"Full->Lite", &F, &L, 0, 0, -1000, 0, -1000, 0, ""}};

    std::vector<float> y;
    for (Row& r : rows)
    {
      crossfade(*r.before, *r.after, ts.empty() ? 0 : ts.front(), fade, y, args.fade_shape); // sized
      for (std::size_t t : ts)
      {
        // The correlation the two signals have as they meet is what the fade's law has to account
        // for; the bench measures it rather than assuming it.
        const double rho = correlation(*r.before, *r.after, t, static_cast<std::size_t>(0.02 * kRate));
        r.rho_mean += rho;
        crossfade(*r.before, *r.after, t, fade, y, args.fade_shape, rho);
        // And the fade's own excess: peak inside the fade against the louder of the two signals.
        // A fade that adds energy is a thump, and no glitch rule sees one.
        double pe = 0, pa = 0, pb = 0;
        for (std::size_t i = t; i < std::min(x.size(), t + fade); ++i)
        {
          pe = std::max(pe, std::abs(static_cast<double>(y[i])));
          pa = std::max(pa, std::abs(static_cast<double>((*r.before)[i])));
          pb = std::max(pb, std::abs(static_cast<double>((*r.after)[i])));
        }
        const double louder = std::max(pa, pb);
        if (louder > 0)
          r.excess = std::max(r.excess, dbfs(pe / louder));
        // What the player hears if the *other* submodel is substituted and never handed over: the
        // steady difference, over the same 200 ms the state mode judges its arms on.
        double w = 0;
        for (std::size_t i = t; i < std::min(x.size(), t + window); ++i)
          w = std::max(w, std::abs(static_cast<double>((*r.before)[i]) - (*r.after)[i]));
        r.worst = std::max(r.worst, dbfs(w));

        const std::size_t lo = t - std::min<std::size_t>(t, window), hi = std::min(x.size(), t + window + fade);
        namchain::glitch::Options opts;
        std::string error;
        const namchain::glitch::Report rep = namchain::glitch::analyse(
          y.data() + lo, r.before->data() + lo, r.after->data() + lo, hi - lo, {{t - lo, t - lo + fade}}, opts, error);
        ++r.n;
        if (rep.ok && error.empty())
          ++r.ok;
        else if (r.finding.empty())
        {
          // The switch time and the level the span is playing at: a "dropout" at -90 dBFS in a
          // silent tail is a different thing from one at -20 dBFS, and the time is what makes the
          // finding reproducible.
          double peak = 0;
          for (std::size_t i = lo; i < hi; ++i)
            peak = std::max(peak, std::max(std::abs(static_cast<double>((*r.before)[i])),
                                           std::abs(static_cast<double>((*r.after)[i]))));
          char at[64];
          std::snprintf(at, sizeof at, "at %.2f s, oracle peak %.1f dBFS: ", static_cast<double>(t) / kRate,
                        dbfs(peak));
          r.finding = at + (!error.empty() ? error
                                           : (!rep.findings.empty() ? rep.findings.front().rule + ": " +
                                                                        rep.findings.front().detail
                                                                    : std::string("failed")));
        }
        r.d2 = std::max(r.d2, rep.reference_d2 > 0 ? rep.max_edit_d2 / rep.reference_d2 : 0.0);
      }
    }

    for (const Row& r : rows)
    {
      char glitch[64], wrong[40], excess[32];
      std::snprintf(glitch, sizeof glitch, "%d/%d clean, d2 x%.1f", r.ok, r.n, r.d2);
      std::snprintf(wrong, sizeof wrong, "%.1f dBFS", r.worst);
      std::snprintf(excess, sizeof excess, "%+.2f (rho %+.3f)", r.excess, r.n > 0 ? r.rho_mean / r.n : 0.0);
      std::printf("%-30.30s %-10s %8.2f %8.2f %9.2f %9.2f  %-24s %-12s %-18s %s\n", stem(path).c_str(), r.dir,
                  dbfs(std::sqrt(lsq / n)), dbfs(std::sqrt(fsq / n)), null_rms, dbfs(mx), glitch, wrong, excess,
                  r.finding.c_str());
      if (r.ok != r.n)
        ++failures;
    }

    if (audition)
    {
      // The handover, alternating, so one file carries both directions and the ear can compare it
      // with the two references either side of it. An excerpt, because the full input is 2.7 M
      // samples per file and nothing in the judgement turns on the other 30 seconds.
      const std::size_t from = static_cast<std::size_t>(args.audition_start * kRate);
      const std::size_t frames =
        std::min(static_cast<std::size_t>(args.audition_seconds * kRate), x.size() - std::min(from, x.size()));
      std::vector<float> alt = L;
      for (std::size_t i = 0; i < ts.size(); ++i)
      {
        const std::vector<float>& other = i % 2 == 0 ? F : L;
        crossfade(alt, other, ts[i], fade, alt, args.fade_shape,
                  correlation(alt, other, ts[i], static_cast<std::size_t>(0.02 * kRate)));
      }

      std::string error;
      const std::string tag = stem(path);
      const std::pair<const char*, const std::vector<float>*> files[3] = {
        {"handover", &alt}, {"lite", &L}, {"full", &F}};
      for (const auto& f : files)
      {
        const std::string out = args.audition + "/" + tag + "-" + f.first + ".wav";
        if (!namchain::save_wav_float32(out, f.second->data() + from, frames, 1, kRate, error))
          die(out + ": " + error);
        std::printf("%-30s   wrote %s (%.0f s from %.0f s)\n", "", out.c_str(), frames / kRate,
                    args.audition_start);
      }
    }
  }

  std::printf("\nnull: |Lite - Full| over the whole input, sample aligned (both submodels are zero-latency).\n"
              "handover: the %zu switches judged by namchain::glitch against the submodel running after it.\n"
              "wrong-model: the worst |Lite - Full| in the 200 ms after a switch -- what a listener hears\n"
              "while a Lite stands in for the Full, which is tier 2's first step.\n"
              "fade excess: the worst peak inside a fade against the louder of the two signals, in dB.\n"
              "  Equal power adds 2*rho*g1*g2 at the midpoint where the signals are correlated, so on\n"
              "  two submodels of one rig (rho ~ 1) it is a +3 dB thump and no glitch rule sees it.\n",
              ts.size());
  return failures == 0 ? 0 : 1;
}

// --- chain: E2 item 2, catch-up for models in series --------------------------

int run_chain(const Args& args)
{
  const std::vector<float> x = load_input(args.input);
  const int q = args.quantum;
  const std::size_t window = static_cast<std::size_t>(0.2 * kRate);
  const std::size_t every = static_cast<std::size_t>(args.switch_every * kRate);

  std::printf("namchain-warm-bench %s  chain\n", NAMCHAIN_VERSION_STRING);
  std::printf("input %s: %.1f s; quantum %d; depths 1..%d; replays", args.input.c_str(),
              static_cast<double>(x.size()) / kRate, q, args.depth);
  for (double r : args.replays)
    std::printf(" %.0f", r);
  std::printf(" ms per model; a switch every %.2f s\n\n", args.switch_every);
  std::printf("catch-up: stage k is reset and fed the tail of the true input (k = 1) or of the stage\n"
              "before it (k > 1), so stage k costs (d-k+1) x replay samples and the whole chain costs\n"
              "d(d+1)/2 of them -- the plan's estimate, measured here rather than assumed.\n\n");
  std::printf("%5s %8s %8s %10s %10s %10s %10s %8s\n", "depth", "replay", "switches", "err 0-200ms", "err settled",
              "replay mean", "replay max", "replays");
  std::printf("%5s %8s %8s %10s %10s %10s %10s %8s\n", "", "ms", "", "dBFS", "dBFS", "ms", "ms", "");

  for (int d = 1; d <= args.depth; ++d)
  {
    std::vector<std::string> models;
    for (int i = 0; i < d; ++i)
      models.push_back(args.candidates[static_cast<std::size_t>(i) % args.candidates.size()]);
    Chain chain(models, q, args.slim);

    std::vector<float> ref(x.size());
    chain.reset();
    chain.run(x.data(), ref.data(), x.size());

    for (double replay_ms : args.replays)
    {
      const std::size_t replay = static_cast<std::size_t>(replay_ms * kRate / 1000.0);
      const std::size_t lead = static_cast<std::size_t>(d) * replay;
      if (lead + window >= x.size())
        continue;
      const std::vector<std::size_t> ts = switch_points(x, args, lead);
      if (ts.empty())
        continue;
      double worst_window = -1000, worst_settled = -1000, sum = 0, mx = 0;
      for (std::size_t t : ts)
      {
        chain.reset();
        const double ms = chain.catch_up(x, t, replay);
        sum += ms;
        mx = std::max(mx, ms);
        std::vector<float> y(window);
        chain.run(x.data() + t, y.data(), window);
        double w = 0, s = 0;
        for (std::size_t i = 0; i < window; ++i)
          w = std::max(w, std::abs(static_cast<double>(y[i]) - ref[t + i]));
        // The settled error is over the rest of the interval, run on from where the window ended:
        // the chain has to keep going for it to mean anything.
        const std::size_t end = std::min(x.size(), t + every);
        const std::size_t rest = end > t + window ? end - (t + window) : 0;
        if (rest > 0)
        {
          std::vector<float> y2(rest);
          chain.run(x.data() + t + window, y2.data(), rest);
          for (std::size_t i = 0; i < rest; ++i)
            s = std::max(s, std::abs(static_cast<double>(y2[i]) - ref[t + window + i]));
        }
        worst_window = std::max(worst_window, dbfs(w));
        worst_settled = std::max(worst_settled, dbfs(s));
      }
      std::printf("%5d %8.0f %8zu %10.1f %10.1f %10.2f %10.2f %8zu\n", d, replay_ms, ts.size(), worst_window,
                  worst_settled, ts.empty() ? 0.0 : sum / static_cast<double>(ts.size()), mx,
                  static_cast<std::size_t>(d) * (static_cast<std::size_t>(d) + 1) / 2);
    }
  }
  std::printf("\nerr 0-200ms: worst |caught-up chain - chain that ran all along| over the 200 ms after the\n"
              "switch. err settled: the same over the rest of the interval. replay: wall time for one\n"
              "catch-up, on this machine -- the budget number has to come from the Core host.\n");
  return 0;
}

// --- ir: E2 item 4, an IR kept as a record of its input -----------------------

int run_ir(const Args& args)
{
  if (args.ir.empty())
    die("ir mode needs --ir: it is about filling a convolver from a record of its input");
  const std::vector<float> x = load_input(args.input);
  const std::vector<float> ir = load_ir(args.ir);
  const int q = args.quantum;
  const std::size_t window = static_cast<std::size_t>(0.2 * kRate);
  const std::vector<std::size_t> ts = switch_points(x, args, ir.size());

  // The cab's input is the model's output, so the record is a model-only run of the same rig.
  auto amp_only = make_rig(args.live, {}, q, args.slim);
  std::vector<float> record(x.size());
  amp_only->run(x.data(), record.data(), x.size(), q);

  auto rig = make_rig(args.live, ir, q, args.slim);
  std::vector<float> ref;
  rig->reset_cab();
  rig->cab_of(record, 0, record.size(), ref);

  std::printf("namchain-warm-bench %s  ir\n", NAMCHAIN_VERSION_STRING);
  std::printf("input %s: %.1f s; ir %s: %zu taps; quantum %d; %zu switch points\n", args.input.c_str(),
              static_cast<double>(x.size()) / kRate, stem(args.ir).c_str(), ir.size(), q, ts.size());
  std::printf("A warm rig does not have to run its IR, only record what goes into it: at the switch the\n"
              "convolver is reset and filled from the last %zu samples of that record. nam::Linear's\n"
              "state is exactly its last receptive_field inputs, so this should be exact.\n\n", ir.size());
  std::printf("%8s %12s %12s %12s\n", "switch", "err 0-200ms", "err settled", "fill");
  std::printf("%8s %12s %12s %12s\n", "", "dBFS", "dBFS", "ms");

  double worst_window = -1000, worst_settled = -1000, worst_fill_ms = 0;
  std::vector<float> fill, y;
  for (std::size_t t : ts)
  {
    rig->reset_cab();
    const auto t0 = Clock::now();
    rig->cab_of(record, t - ir.size(), ir.size(), fill);
    const double fill_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    worst_fill_ms = std::max(worst_fill_ms, fill_ms);

    rig->cab_of(record, t, window, y);
    double w = 0;
    for (std::size_t i = 0; i < window; ++i)
      w = std::max(w, std::abs(static_cast<double>(y[i]) - ref[t + i]));
    const std::size_t rest = std::min(x.size(), t + 2 * window) - (t + window);
    std::vector<float> y2;
    rig->cab_of(record, t + window, rest, y2);
    double s = 0;
    for (std::size_t i = 0; i < rest; ++i)
      s = std::max(s, std::abs(static_cast<double>(y2[i]) - ref[t + window + i]));
    worst_window = std::max(worst_window, dbfs(w));
    worst_settled = std::max(worst_settled, dbfs(s));
    std::printf("%8zu %12.1f %12.1f %12.3f\n", t, dbfs(w), dbfs(s), fill_ms);
  }
  std::printf("\nworst: %.1f dBFS in the first 200 ms, %.1f dBFS settled, fill %.3f ms per switch.\n",
              worst_window, worst_settled, worst_fill_ms);
  return worst_window > -120.0 ? 1 : 0;
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
  const std::vector<float> ir = load_ir_or_none(args);
  // The warm candidates' cab: item 3 keeps the models warm and lets the live rig keep its IR, so
  // the render tail is the one the pedal actually has.
  std::vector<float> warm_ir_v;
  if (args.warm_no_ir)
    warm_ir_v.clear();
  else if (!args.warm_ir.empty())
    warm_ir_v = load_ir(args.warm_ir);
  else
    warm_ir_v = ir;
  const int q = args.quantum;
  const int count = args.count >= 0 ? args.count : static_cast<int>(args.candidates.size());
  const double deadline_us = q / kRate * 1e6;

  const long rss0 = rss_kb();
  auto live = make_rig(args.live, ir, q, args.slim);
  const long rss1 = rss_kb();
  std::vector<std::unique_ptr<Rig>> warm;
  for (int i = 0; i < count; ++i)
    warm.push_back(
      make_rig(args.candidates[static_cast<std::size_t>(i) % args.candidates.size()], warm_ir_v, q, args.slim));
  // Fault the candidates' buffers in before measuring: RSS right after construction counts what was
  // allocated, not what the warm thread will touch, which under-reported a rig by its whole working
  // set and made a Lite look bigger than a Full.
  {
    std::vector<float> warmup(static_cast<std::size_t>(q));
    for (auto& rig : warm)
      rig->process(warmup.data(), warmup.data(), q);
  }
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

  // "Keeps up on a housekeeping core under load": a third thread saturating the warm core, as
  // another job on that core would. Lower priority than the warm thread, so the candidates still
  // get their turn and what is measured is the contention, not a starved warm thread.
  std::thread spinner;
  if (args.spinner)
    spinner = std::thread([&] {
      namchain::rt::apply_scheduling(args.priority - 20, args.warm_cpu);
      volatile double acc = 1.0;
      while (!done.load(std::memory_order_acquire))
        acc = acc * 1.0000001 + 1.0;
      (void)acc;
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
  if (spinner.joinable())
    spinner.join();
  warmer.join();

  const Stats r = stats(render_us), w = stats(warm_us);
  std::printf("namchain-warm-bench %s  contention\n", NAMCHAIN_VERSION_STRING);
  std::printf("live rig:   %s%s, quantum %d (deadline %.0f us), %.0f s\n", stem(args.live).c_str(),
              ir.empty() ? " (no ir)"
                         : (" + " + stem(args.ir) + " (" + std::to_string(ir.size()) + " taps)").c_str(),
              q, deadline_us, args.seconds);
  std::printf("render:     %s\n", render_sched.c_str());
  std::printf("warm:       %d candidate rig(s)%s%s%s, %s\n", count,
              args.slim >= 0.0 ? (" at slim " + std::to_string(args.slim)).c_str() : "",
              warm_ir_v.empty() ? " with no ir" : " with an ir",
              args.spinner ? " and a spinner on the same core" : "", warm_sched.c_str());
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
    die("usage: namchain-warm-bench state|slim|chain|ir|contention --input in.wav --live model.nam "
        "cand.nam ... [--ir cab.wav]; see the comment at the top of this file");
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
    else if (a == "--fade-shape")
      args.fade_shape = next();
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
    else if (a == "--slim")
      args.slim = std::atof(next().c_str());
    else if (a == "--no-ir")
      args.no_ir = true;
    else if (a == "--warm-ir")
      args.warm_ir = next();
    else if (a == "--warm-no-ir")
      args.warm_no_ir = true;
    else if (a == "--spinner")
      args.spinner = true;
    else if (a == "--depth")
      args.depth = std::atoi(next().c_str());
    else if (a == "--replays")
    {
      args.replays.clear();
      std::string list = next(), item;
      std::stringstream ss(list);
      while (std::getline(ss, item, ','))
        if (!item.empty())
          args.replays.push_back(std::atof(item.c_str()));
    }
    else if (a == "--audition")
      args.audition = next();
    else if (a == "--audition-start")
      args.audition_start = std::atof(next().c_str());
    else if (a == "--audition-seconds")
      args.audition_seconds = std::atof(next().c_str());
    else if (a.rfind("--", 0) == 0)
      die("unknown option " + a);
    else
      args.candidates.push_back(a);
  }
  if (args.input.empty())
    die("--input is required");
  // What each mode needs. --ir is optional everywhere (without one a rig is the model alone);
  // chain mode takes its models from the positional list, so it needs no --live, and ir mode is
  // about one rig's cab, so it needs no candidates.
  if (args.mode != "ir" && args.candidates.empty())
    die(args.mode + " mode needs at least one model");
  if (args.mode != "chain" && args.mode != "slim" && args.live.empty())
    die("--live is required for " + args.mode + " mode");
  if (args.mode == "state")
    return run_state(args);
  if (args.mode == "slim")
    return run_slim(args);
  if (args.mode == "chain")
    return run_chain(args);
  if (args.mode == "ir")
    return run_ir(args);
  if (args.mode == "contention")
    return run_contention(args);
  die("mode must be state, slim, chain, ir or contention");
}
