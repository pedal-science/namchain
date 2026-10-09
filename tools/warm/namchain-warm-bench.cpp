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
//   measure      P1, the cost table's own bench (pedal.project
//                docs/managing-processing-power-and-optimising-for-fast-switching.md, "Measuring a
//                Core"). One **unit** -- a NAM model at a submodel size, an IR at a tap count, a
//                node type with a parameter set -- is run as n copies on one core, paced, and
//                bisected to the largest n whose p99 is under the deadline with zero overruns.
//                Prints the curve around that n (n-2 .. n+4) and the cost table's JSON fragment:
//                the row's `core_percent` (1/n), its `sources` (n, p99 at n/n+1/n+2) and the curve.
//                The copies run one after another on the measured core, which is the shape the
//                table's n means; --live adds E2's live rig on --render-cpu, and without it the
//                measured thread paces itself, which is "no other work on that core".
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
//   namchain-warm-bench switch     --input in.wav --live A.nam candB.nam [--ir cab.wav]
//                                  [--switch-every S] [--fade-ms 5] [--seconds 30] [--quantum 64]
//                                  [--render-cpu 3] [--warm-cpu 1] [--priority 80]
//   namchain-warm-bench measure    --input in.wav --unit nam|ir|node
//                                  [--model M.nam] [--slim 0.0] [--with-ir cab.wav]   (nam)
//                                  [--ir cab.wav] [--taps N]                          (ir)
//                                  [--node nc.gain] [--node-param k=v]                (node)
//                                  [--levels | --no-levels | --levels-first-frame | --levels-publish-period]
//                                  [--seconds 30] [--quantum 64] [--cpu 2] [--priority 70]
//                                  [--live L.nam] [--live-ir cab.wav] [--render-cpu 3]
//                                  [--max N] [--start N] [--no-curve] [--bar p99|strict]
//                                  [--label NAME] [--json PATH]
//   namchain-warm-bench rig        --input in.wav --document rig.json [--table cost.json]
//                                  [--reps N] [--seconds 30] [--quantum 64] [--cpu 2] [--priority 80]
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

#include "namchain/daemon/cost_table.h"
#include "namchain/daemon/doc_runtime.h"
#include "namchain/glitch.h"
#include "namchain/rt/crossfade.h"
#include "namchain/graph/graph.h"
#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"
#include "namchain/rt/scheduling.h"
#include "namchain/version.h"
#include "namchain/wav.h"
#include "namchain/working_set.h"

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

/// Short fixed forms for the fragment below. Hand-built JSON, in the C locale (nothing here calls
/// setlocale), so the decimal point is a point whatever the host's locale is.
std::string format_g(double v)
{
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3g", v);
  return buf;
}

std::string format_f(double v)
{
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
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
  /// measure: the core the unit copies run on. `--cpu` and `--warm-cpu` are the same flag under two
  /// names -- contention calls it the warm core, measure calls it the measured core -- so the last
  /// one on the command line wins either way.
  int cpu = 2;
  double slim = -1.0;                 ///< submodel for every rig; <0 is the model's own default
  bool no_ir = false;                 ///< models alone, no cab: what the density numbers are about
  std::string warm_ir;                ///< contention: the candidates' own cab, default --ir
  bool warm_no_ir = false;            ///< contention: candidates without a cab, live rig keeps its own
  bool spinner = false;               ///< contention: load the warm core as well
  int depth = 4;                      ///< chain: models in series
  std::vector<double> replays{85, 170, 340, 680}; ///< chain: replay lengths per model, ms
  double audition_start = 10.0, audition_seconds = 20.0; ///< slim --audition excerpt
  /// measure: the unit and what it is built from. `unit` is "nam", "ir" or "node"; see run_measure.
  std::string unit = "nam", model, node, label, json, with_ir, live_ir;
  std::map<std::string, double> node_params;
  int taps = 0;        ///< ir: the row's tap count; 0 is the file's own length
  int max_n = 0;       ///< measure: stop doubling here; 0 is 128
  int start_n = 1;     ///< measure: the first n tried
  bool no_curve = false; ///< measure: skip n-2, n-1, n+3, n+4 (n+1 and n+2 are always measured)
  /// measure: D7's bar ("every block within the deadline") rather than p99 under it. Off, because a
  /// single late block in 22,500 is the environment's, not the unit's -- see the note in the mode's
  /// comment -- and because that is the bar E2's 8 and 30 were measured with.
  bool strict_bar = false;
  /// rig: the document to cost and measure, the table to cost it with, and how many copies of it to
  /// run at once (1 is the rig as published; 2 is the transition window, T4's "while switching").
  std::string document, table = "/etc/namchain/cost.json";
  int reps = 1;
  /// measure, node units: the level probes, which is P9's question. On is the engine's own default
  /// (Config::levels); `--levels-publish-period` is what vip's namchaind.json ships.
  bool levels = true;
  bool levels_first_frame = false;
  bool levels_publish_period = false;
  /// slim: equal-power | equal-gain | compensated. The default is what the engine does now
  /// (rt::Crossfade normalises for the correlation it measures); equal-power is kept because the
  /// recorded E2 tables were run with it, and the difference is the point of the comparison.
  std::string fade_shape = "compensated";
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

// --- measure: P1, the cost table's measurements ---------------------------------
//
// pedal.project docs/managing-processing-power-and-optimising-for-fast-switching.md, "Measuring a
// Core" and P1. For one **unit** -- a NAM model at a submodel size, an IR at a tap count, a node type
// with a parameter set -- find the largest number `n` of copies that still renders a block within the
// deadline on one core, print the curve around it, and emit the table's JSON fragment. The row's cost
// is 1/n (D2), which is why the copies are what is measured and not the unit on its own.
//
// The copies run one after another on the measured core, each on the block just consumed, which is
// the shape the table's `n` means: E2 measured "8 A2-Full or 30 A2-Lite per core" by running eight or
// thirty whole models on one core, and a rig's cost is the sum of its units' (D2). With `--live` a
// rig renders on another core the way it did in E2, and the measured thread follows it block by
// block; without it the measured thread paces itself, which is "no other work on that core".
//
// What is timed is the whole n-unit loop for one block, so `p99_at_n` is the fraction of a quantum
// the table records as `n` units' p99 -- 89.1% for eight A2-Fulls, in E2's numbers. `behind` is the
// worst lag, in blocks, which is what says a run that is over the cliff never recovers.
//
// **The bar is p99 under the deadline, and the overrun count is recorded rather than required.**
// D2's step 4 says "p99 < deadline and zero overruns" and D7 says the same, but measured, the
// overrun count is not monotone in n: on vip's core 2, one Lite model at n = 22 overran twice, at
// n = 23 never, at n = 24 three times and at n = 25 never again, while p99 climbed smoothly through
// 64.5%, 67.5%, 72.7% and 76.4% over the same four points. A bar that a single late block in 22,500
// decides is a bar the environment decides, and the number it produced moved by three when nothing
// about the unit changed. So n is the largest count whose p99 is under the deadline -- the smooth,
// monotone signal, and the bar E2's own 8 and 30 were measured with -- and `overruns` and `max` are
// part of the curve's shape, where D7's "zero overruns" is what the live soak (T10) confirms. The row
// this produces is the *cheaper* of the two: D3's 0.8 headroom is what pays for the tail, which is
// what the headroom is for. `--bar strict` asks for D7's bar instead, and the fit point it finds is
// reported beside the p99 one.
//
// The header, the no-args usage, the dispatch and the last die all list the modes: keep them in step.

/// One unit under measurement, and how to build a copy of it.
struct Unit
{
  std::string kind;  ///< "nam", "ir" or "node"
  std::string label; ///< the cost table's row, e.g. NAM.WaveNet.full
  std::string what;  ///< the file or type, for the banner

  std::string model;      ///< nam: the model file
  double slim = -1.0;     ///< nam: submodel selector; < 0 is the model's own default (Full), 0.0 Lite
  std::vector<float> cab; ///< nam: an optional cab, which makes the unit a whole rig

  std::vector<float> taps; ///< ir: the impulse response itself

  std::string node_type;                     ///< node: the node type's name
  std::map<std::string, double> node_params; ///< node: its parameter set
  bool levels = true;                        ///< node: compile with level probes (see P9)
  bool levels_first_frame = false;
  bool levels_publish_period = false;
};

/// One copy of a unit.
struct Instance
{
  std::unique_ptr<Rig> rig;                          ///< nam
  std::unique_ptr<nam::Linear> convolver;            ///< ir
  std::vector<NAM_SAMPLE> in, out;                   ///< ir: this copy's own buffers
  std::unique_ptr<namchain::rt::Arena> arena;        ///< node
  std::unique_ptr<namchain::graph::RenderPlan> plan; ///< node
};

std::unique_ptr<Instance> make_instance(const Unit& u, int q)
{
  auto inst = std::make_unique<Instance>();
  if (u.kind == "nam")
  {
    inst->rig = make_rig(u.model, u.cab, q, u.slim);
  }
  else if (u.kind == "ir")
  {
    inst->convolver = std::make_unique<nam::Linear>(1, 1, static_cast<int>(u.taps.size()), false, u.taps, kRate);
    inst->convolver->Reset(kRate, q);
    inst->in.assign(static_cast<std::size_t>(q), 0);
    inst->out.assign(static_cast<std::size_t>(q), 0);
  }
  else
  {
    // A node unit is measured through the engine's own compiler and plan, so what is timed is the
    // node as the render path runs it -- including the plan's own per-block work -- and not a
    // reimplementation of it. One plan per copy, each with its own arena, because that is what makes
    // them independent (the same shape as n rigs on a warm core).
    inst->arena = std::make_unique<namchain::rt::Arena>(1u << 20);
    namchain::graph::NodeSpec middle;
    middle.id = "unit";
    middle.type = u.node_type;
    middle.params = u.node_params;
    // A chain holds exactly one nc.input and one nc.output, so a unit that *is* one of those two is
    // the graph's own: the plan is the unit and the other end, and what is timed is the node's own
    // per-block work rather than a reimplementation of it.
    namchain::graph::Graph graph;
    if (u.node_type == "nc.input")
    {
      graph.nodes = {middle, namchain::graph::NodeSpec{"out", "nc.output"}};
      graph.connections = {{"unit", 0, "out", 0}};
    }
    else if (u.node_type == "nc.output")
    {
      graph.nodes = {namchain::graph::NodeSpec{"in", "nc.input"}, middle};
      graph.connections = {{"in", 0, "unit", 0}};
    }
    else
    {
      graph.nodes = {namchain::graph::NodeSpec{"in", "nc.input"}, middle,
                     namchain::graph::NodeSpec{"out", "nc.output"}};
      graph.connections = {{"in", 0, "unit", 0}, {"unit", 0, "out", 0}};
    }
    namchain::render::CompileOptions options;
    options.max_quantum = q;
    options.levels = u.levels;
    options.levels_first_frame = u.levels_first_frame;
    options.levels_publish_period = u.levels_publish_period;
    std::string error;
    inst->plan = namchain::graph::RenderPlan::compile(graph, options, *inst->arena, error);
    if (!inst->plan)
      die("cannot compile a " + u.node_type + " node: " + error);
  }
  return inst;
}

/// One measured point: `n` copies, one run.
struct Point
{
  int n = 0;
  double p50 = 0, p99 = 0, p999 = 0, max = 0; ///< microseconds for the whole n-unit loop
  std::size_t overruns = 0;                   ///< blocks whose loop went past the deadline
  std::size_t blocks = 0;
  std::size_t behind = 0; ///< worst lag, in blocks: a run over the cliff never catches up
  bool pass = false;      ///< p99 under the deadline: the bar the table's n comes from
  bool strict = false;    ///< and zero overruns too, which is D7's wording (see the note above)
  std::string sched;      ///< what the measured thread actually got, said once per run
};

Point measure_units(const Args& args, const Unit& unit, const std::vector<float>& x, int n)
{
  const int q = args.quantum;
  const double deadline_us = q / kRate * 1e6;
  const std::size_t blocks = static_cast<std::size_t>(args.seconds * kRate) / static_cast<std::size_t>(q);
  const std::size_t loop = x.size() / static_cast<std::size_t>(q) * static_cast<std::size_t>(q);
  const long period_ns = static_cast<long>(q / kRate * 1e9);

  std::vector<std::unique_ptr<Instance>> inst;
  inst.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i)
    inst.push_back(make_instance(unit, q));

  std::vector<float> out(static_cast<std::size_t>(q));
  // The IR unit's input is converted once, outside the loop: in a rig a cab reads the NAM_SAMPLE
  // buffer the model before it wrote, so converting per copy would measure work no rig does.
  std::vector<NAM_SAMPLE> ir_in(unit.kind == "ir" ? static_cast<std::size_t>(q) : 0);
  NAM_SAMPLE* ir_in_ptr[] = {ir_in.data()};
  const bool is_ir = unit.kind == "ir";
  const auto run_all = [&](const float* in, float* sink, int frames) {
    if (is_ir)
    {
      for (int i = 0; i < frames; ++i)
        ir_in[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(in[i]);
      for (auto& it : inst)
      {
        NAM_SAMPLE* po[] = {it->out.data()};
        it->convolver->process(ir_in_ptr, po, frames);
      }
      return;
    }
    for (auto& it : inst)
    {
      if (it->rig)
        it->rig->process(in, sink, frames);
      else
        it->plan->execute(in, sink, frames);
    }
  };

  // Fault every copy's buffers in before the clock starts: a page fault during the first block is
  // not the unit's cost.
  {
    std::vector<float> warm(static_cast<std::size_t>(q));
    run_all(warm.data(), out.data(), q);
  }

  std::vector<double> us;
  us.reserve(blocks);
  Point p;
  p.n = n;
  p.blocks = blocks;
  std::string render_sched;

  if (args.live.empty())
  {
    // Self-paced: the measured core carries the unit copies and nothing else.
    p.sched = namchain::rt::describe_scheduling(namchain::rt::apply_scheduling(args.priority, args.cpu));
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
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
      run_all(in, out.data(), q);
      const double this_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
      us.push_back(this_us);
      if (this_us > deadline_us)
        ++p.overruns;
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const long late = (now.tv_sec - next.tv_sec) * 1000000000L + (now.tv_nsec - next.tv_nsec);
      if (late > 0)
        p.behind = std::max(p.behind, static_cast<std::size_t>(late / period_ns));
      if (p.behind > blocks)
        break; // hopelessly behind: the rest of the run is not a measurement
    }
  }
  else
  {
    // E2's shape: a live rig on --render-cpu, the unit copies following it block by block.
    const std::vector<float> live_ir = args.live_ir.empty() ? std::vector<float>{} : load_ir(args.live_ir);
    auto live = make_rig(args.live, live_ir, q, -1.0);
    std::atomic<std::size_t> published{0};
    std::atomic<bool> done{false};
    std::thread renderer([&] {
      render_sched =
        namchain::rt::describe_scheduling(namchain::rt::apply_scheduling(args.priority, args.render_cpu));
      std::vector<float> lout(static_cast<std::size_t>(q));
      timespec next{};
      clock_gettime(CLOCK_MONOTONIC, &next);
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
        live->process(in, lout.data(), q);
        published.store(b + 1, std::memory_order_release);
      }
      done.store(true, std::memory_order_release);
    });
    const std::string sched =
      namchain::rt::describe_scheduling(namchain::rt::apply_scheduling(args.priority - 10, args.cpu));
    p.sched = "live rig on cpu " + std::to_string(args.render_cpu) + ", measured thread " + sched;
    std::size_t next_block = 0;
    while (!done.load(std::memory_order_acquire) || next_block < published.load(std::memory_order_acquire))
    {
      const std::size_t avail = published.load(std::memory_order_acquire);
      if (next_block >= avail)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      p.behind = std::max(p.behind, avail - next_block);
      const float* in = x.data() + (next_block * static_cast<std::size_t>(q)) % loop;
      const auto t0 = Clock::now();
      run_all(in, out.data(), q);
      const double this_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
      us.push_back(this_us);
      if (this_us > deadline_us)
        ++p.overruns;
      ++next_block;
    }
    renderer.join();
  }

  const Stats s = stats(us);
  p.p50 = s.p50;
  p.p99 = s.p99;
  p.p999 = s.p999;
  p.max = s.max;
  p.pass = p.p99 < deadline_us;
  p.strict = p.pass && p.overruns == 0;
  return p;
}

void print_point(const Point& p, double deadline_us)
{
  // The verdict column is the p99 bar (`fits`), and `stricts` says D7's stricter one agrees: a point
  // can be under the deadline p99 and still have had a late block (see the note at the top).
  std::printf("  %5d  %10.1f %10.1f %10.1f %10.1f  %8.1f %8.1f  %7zu  %6zu  %s\n", p.n, p.p50, p.p99, p.p999,
              p.max, 100 * p.p99 / deadline_us, 100 * p.max / deadline_us, p.overruns, p.behind,
              p.strict ? "fits" : (p.pass ? "fits*" : "MISSES"));
}

/// The unit the run is about, from the flags. Dies with what is missing rather than guessing.
Unit make_unit(const Args& args)
{
  Unit u;
  u.kind = args.unit;
  u.label = args.label;
  if (u.kind == "nam")
  {
    u.model = !args.model.empty() ? args.model : (args.candidates.empty() ? "" : args.candidates.front());
    if (u.model.empty())
      die("measure --unit nam needs --model (or one positional model)");
    u.slim = args.slim;
    if (!args.with_ir.empty())
      u.cab = load_ir(args.with_ir);
    const std::string family = namchain::model_family_from_file(u.model);
    if (family.empty())
      die("cannot read " + u.model + "'s architecture, so the row it belongs in is unknown");
    // The row's size, from the submodel selector: the model's own default is Full, 0.0 selects the
    // first submodel (Lite), and anything in between is not a row the table has.
    const std::string size = (u.slim < 0.0 || u.slim >= 1.0) ? "full" : (u.slim <= 0.0 ? "lite" : "slim");
    if (u.label.empty())
      u.label = "NAM." + family + "." + size;
    u.what = stem(u.model) + ", " + size +
             (u.cab.empty() ? std::string(", no cab")
                            : ", + " + stem(args.with_ir) + " (" + std::to_string(u.cab.size()) + " taps)");
  }
  else if (u.kind == "ir")
  {
    if (args.ir.empty())
      die("measure --unit ir needs --ir");
    u.taps = load_ir(args.ir);
    std::size_t file_taps = u.taps.size();
    // --taps is the row: the table has a cost per tap count, so the same file is measured at
    // several lengths rather than all of them being one number.
    const std::size_t want = args.taps > 0 ? static_cast<std::size_t>(args.taps) : file_taps;
    if (want > file_taps)
      die(args.ir + " has " + std::to_string(file_taps) + " taps, not " + std::to_string(want));
    u.taps.resize(want);
    if (u.label.empty())
      u.label = "IR." + std::to_string(want);
    u.what = stem(args.ir) + ", " + std::to_string(want) + " taps";
  }
  else if (u.kind == "node")
  {
    if (args.node.empty())
      die("measure --unit node needs --node TYPE (nc.gain, nc.eq.biquad, ...)");
    u.node_type = args.node;
    u.node_params = args.node_params;
    u.levels = args.levels;
    u.levels_first_frame = args.levels_first_frame;
    u.levels_publish_period = args.levels_publish_period;
    if (u.label.empty())
      u.label = "node." + u.node_type;
    u.what = u.node_type + (u.node_params.empty() ? std::string(", default params") : ", params set") +
             (u.levels ? (u.levels_publish_period ? ", levels per period"
                                                  : (u.levels_first_frame ? ", levels first frame" : ", levels on"))
                       : ", levels off");
  }
  else
  {
    die("--unit must be nam, ir or node, not '" + u.kind + "'");
  }
  return u;
}

// --- switch ---------------------------------------------------------------------
//
// T7's offline half and T8's falsifier (pedal.project
// docs/managing-processing-power-and-optimising-for-fast-switching.md, P6; namchain docs/warm-set.md).
//
// Two rigs, two cores, and the engine's own handover: the device thread renders the rig being heard
// on --render-cpu at SCHED_FIFO, paced to the wall clock; a warm thread on --warm-cpu keeps the other
// rig current from the same input, at ten below that priority, which on the pedal is below the audio
// interrupt too. Every --switch-every seconds the device thread asks the warm thread to let go, takes
// the rig over on the next block, and crossfades its own fresh audio against the outgoing rig's
// published block -- one block of setup and then --fade-ms, exactly what the engine does.
//
// What it answers:
//   * the handover, in blocks and microseconds, from the request to the fade closing;
//   * the device thread's block times while a rig is being kept warm on another core, against the
//     period's deadline, and how many of them went over it;
//   * how far behind the warm thread fell (its own lag distribution, and the worst);
//   * and whether the result is continuous audio, judged by the engine's own glitch detector against
//     both rigs rendered offline over the same span -- which is the thing a listener would call a
//     click, said as four numbers.
struct SwitchReport
{
  std::size_t handover_blocks = 0;
  double handover_us = 0;
  std::size_t switches = 0;
  std::size_t deferred = 0;
};

int run_switch(const Args& args)
{
  if (args.candidates.empty())
    die("switch needs a candidate: --live A.nam candB.nam");
  const std::vector<float> x = load_input(args.input);
  const std::vector<float> ir = load_ir_or_none(args);
  const int q = args.quantum;
  const double deadline_us = q / kRate * 1e6;
  const std::size_t blocks = static_cast<std::size_t>(args.seconds * kRate) / static_cast<std::size_t>(q);
  const std::size_t loop = x.size() / static_cast<std::size_t>(q) * static_cast<std::size_t>(q);
  const int fade_frames = static_cast<int>(args.fade_ms * kRate / 1000.0);
  const std::size_t switch_blocks =
    args.switch_every <= 0 ? blocks
                           : std::max<std::size_t>(2, static_cast<std::size_t>(args.switch_every * kRate) /
                                                        static_cast<std::size_t>(q));

  auto rig_a = make_rig(args.live, ir, q, args.slim);
  auto rig_b = make_rig(args.candidates.front(), ir, q, args.slim);

  // The oracles: each rig rendered alone over the whole span, offline. The detector judges the
  // real-time run against them, so "no click" is a comparison rather than an opinion.
  std::vector<float> oracle_a(blocks * static_cast<std::size_t>(q));
  std::vector<float> oracle_b(blocks * static_cast<std::size_t>(q));
  std::vector<float> oracle_out(static_cast<std::size_t>(q));
  for (std::size_t b = 0; b < blocks; ++b)
  {
    const float* in = x.data() + (b * static_cast<std::size_t>(q)) % loop;
    rig_a->process(in, oracle_out.data(), q);
    std::copy(oracle_out.begin(), oracle_out.end(), oracle_a.begin() + static_cast<long>(b * static_cast<std::size_t>(q)));
    rig_b->process(in, oracle_out.data(), q);
    std::copy(oracle_out.begin(), oracle_out.end(), oracle_b.begin() + static_cast<long>(b * static_cast<std::size_t>(q)));
  }

  namchain::daemon::BlockPool bus_pool;
  bus_pool.configure(static_cast<std::size_t>(q));
  std::atomic<uint64_t> published{0};
  std::atomic<int> holder{0};           ///< which rig the device thread renders: 0 = A, 1 = B
  std::atomic<bool> publish_wanted{false};
  std::atomic<bool> release_request{false};
  std::atomic<bool> released{false};
  std::atomic<bool> done{false};
  std::atomic<uint64_t> warm_lag_max{0};
  std::atomic<uint64_t> warm_blocks{0};

  std::vector<double> render_us;
  render_us.reserve(blocks);
  std::vector<double> warm_us;
  warm_us.reserve(blocks);
  std::vector<float> y(blocks * static_cast<std::size_t>(q), 0.0f);
  std::vector<namchain::glitch::Edit> edits;
  SwitchReport report;
  std::string render_sched, warm_sched;
  std::size_t overruns = 0;

  std::thread warmer([&] {
    warm_sched = namchain::rt::describe_scheduling(
      namchain::rt::apply_scheduling(args.priority - 10, args.warm_cpu));
    std::vector<float> out(static_cast<std::size_t>(q));
    uint64_t next = 0;
    while (!done.load(std::memory_order_acquire) || next < published.load(std::memory_order_acquire))
    {
      const uint64_t avail = published.load(std::memory_order_acquire);
      if (next >= avail)
      {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        continue;
      }
      if (release_request.load(std::memory_order_acquire))
      {
        // Let go: the device thread is about to render this rig, and a rig is rendered by one thread
        // at a time. It does not touch it again until the roles have swapped under it.
        released.store(true, std::memory_order_release);
        while (release_request.load(std::memory_order_acquire) && !done.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        released.store(false, std::memory_order_release);
        // `next` is deliberately *not* advanced past the blocks the handover cost: both rigs must see
        // every input block, or the one this thread was rendering arrives at the swap with a hole in
        // its history and diverges from the oracle it is judged against (2026-10-09: that is exactly
        // what the convergence check was failing on). It catches up over the next few blocks, which
        // is why the device thread drains the pool for the newest block rather than the first.
        continue;
      }
      const bool mine_is_b = holder.load(std::memory_order_acquire) == 0;
      Rig* mine = mine_is_b ? rig_b.get() : rig_a.get();
      const float* in = x.data() + (next * static_cast<std::size_t>(q)) % loop;
      const auto t0 = Clock::now();
      mine->process(in, out.data(), q);
      warm_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      if (publish_wanted.load(std::memory_order_acquire))
      {
        uint32_t index = 0;
        float* block = bus_pool.acquire(next, index);
        if (block != nullptr)
        {
          std::copy(out.begin(), out.end(), block);
          bus_pool.publish(index);
        }
      }
      const uint64_t lag = avail - next - 1;
      if (lag > warm_lag_max.load(std::memory_order_relaxed))
        warm_lag_max.store(lag, std::memory_order_relaxed);
      warm_blocks.fetch_add(1, std::memory_order_relaxed);
      ++next;
    }
  });

  std::thread renderer([&] {
    render_sched = namchain::rt::describe_scheduling(namchain::rt::apply_scheduling(args.priority, args.render_cpu));
    std::vector<float> out(static_cast<std::size_t>(q));
    std::vector<float> outgoing(static_cast<std::size_t>(q), 0.0f);
    std::vector<float> mixed(static_cast<std::size_t>(q), 0.0f);
    namchain::rt::Crossfade fade(namchain::daemon::kMaxTransitionFrames);
    timespec next_time{};
    clock_gettime(CLOCK_MONOTONIC, &next_time);
    const long period_ns = static_cast<long>(q / kRate * 1e9);
    bool fading = false;
    std::size_t fade_started = 0;
    std::size_t next_switch = switch_blocks;

    for (std::size_t b = 0; b < blocks; ++b)
    {
      next_time.tv_nsec += period_ns;
      while (next_time.tv_nsec >= 1000000000L)
      {
        next_time.tv_nsec -= 1000000000L;
        ++next_time.tv_sec;
      }
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, nullptr);

      Rig* mine = holder.load(std::memory_order_relaxed) == 0 ? rig_a.get() : rig_b.get();
      const float* in = x.data() + (b * static_cast<std::size_t>(q)) % loop;
      const auto t0 = Clock::now();
      mine->process(in, out.data(), q);
      const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
      render_us.push_back(us);
      if (us > deadline_us)
        ++overruns;

      if (fading)
      {
        // The outgoing rig's audio for this block: whatever the warm thread has published since the
        // last one, or the block already in hand when it has published nothing. The *newest* block
        // wins -- a ring hands them back in order, so the last one taken is the newest -- because the
        // warm thread may be a block or two behind while it catches up after a handover, and mixing a
        // stale block into the first frames of a fade is a step the detector correctly calls a click.
        uint32_t index = 0;
        uint64_t seq = 0;
        const float* block = nullptr;
        while (bus_pool.take(index, seq, block))
        {
          std::copy(block, block + q, outgoing.begin());
          bus_pool.give_back(index);
        }
        fade.mix(outgoing.data(), out.data(), mixed.data(), q);
        std::copy(mixed.begin(), mixed.end(), y.begin() + static_cast<long>(b * static_cast<std::size_t>(q)));
        if (!fade.active())
        {
          fading = false;
          publish_wanted.store(false, std::memory_order_relaxed);
          // Empty the pool: whatever the warm thread published as the fade closed belongs to the
          // fade that just ended, and mixing it into the *next* one would mix audio from a different
          // moment of the input -- the other half of the same bug.
          while (bus_pool.take(index, seq, block))
            bus_pool.give_back(index);
          report.handover_blocks = b - fade_started + 1;
        }
      }
      else
      {
        std::copy(out.begin(), out.end(), y.begin() + static_cast<long>(b * static_cast<std::size_t>(q)));
      }

      // The outgoing side of the next fade starts as this thread's own last block, which is in hand
      // and cannot be being written by anyone else.
      std::copy(out.begin(), out.end(), outgoing.begin());
      published.store(b + 1, std::memory_order_release);

      if (!fading && b >= next_switch)
      {
        next_switch = b + switch_blocks;
        report.switches++;
        publish_wanted.store(true, std::memory_order_release);
        release_request.store(true, std::memory_order_release);
        // Wait for the warm thread to let go, looking once per block: a switch may be deferred, and
        // it may not make the device thread wait.
        for (int spins = 0; spins < 1000000 && !released.load(std::memory_order_acquire); ++spins)
          std::this_thread::yield();
        if (!released.load(std::memory_order_acquire))
        {
          ++report.deferred;
          publish_wanted.store(false, std::memory_order_release);
          release_request.store(false, std::memory_order_release);
          continue;
        }
        holder.store(1 - holder.load(std::memory_order_relaxed), std::memory_order_release);
        release_request.store(false, std::memory_order_release);
        fade.start(fade_frames);
        fading = true;
        fade_started = b;
        edits.push_back(namchain::glitch::Edit{b * static_cast<std::size_t>(q),
                                               b * static_cast<std::size_t>(q) + static_cast<std::size_t>(fade_frames)});
      }
    }
    done.store(true, std::memory_order_release);
  });
  renderer.join();
  warmer.join();

  const Stats r = stats(render_us), w = stats(warm_us);
  std::printf("namchain-warm-bench %s  switch\n", NAMCHAIN_VERSION_STRING);
  std::printf("rigs:       %s -> %s%s, quantum %d (deadline %.0f us), %.0f s, switch every %.1f s\n",
              stem(args.live).c_str(), stem(args.candidates.front()).c_str(),
              ir.empty() ? " (no ir)" : (" + " + stem(args.ir)).c_str(), q, deadline_us, args.seconds,
              args.switch_every);
  std::printf("render:     %s\n", render_sched.c_str());
  std::printf("warm:       %s\n", warm_sched.c_str());
  std::printf("handover:   %zu switches (%zu deferred), last %zu blocks; blocks here are %.0f us\n",
              report.switches, report.deferred, report.handover_blocks, q / kRate * 1e6);
  std::printf("render:     p50 %.0f us  p99 %.0f us  p99.9 %.0f us  max %.0f us  over deadline %zu\n",
              r.p50, r.p99, r.p999, r.max, overruns);
  std::printf("warm:       p50 %.0f us  p99 %.0f us  max %.0f us  blocks %llu  worst lag %llu blocks\n",
              w.p50, w.p99, w.max, static_cast<unsigned long long>(warm_blocks.load()),
              static_cast<unsigned long long>(warm_lag_max.load()));

  namchain::glitch::Options detector;
  detector.sample_rate = kRate;
  std::string error;
  const namchain::glitch::Report verdict = namchain::glitch::analyse(
    y.data(), oracle_a.data(), oracle_b.data(), y.size(), edits, detector, error);
  if (!error.empty())
    std::printf("glitch:     not judged: %s\n", error.c_str());
  else
  {
    std::printf("glitch:     %s  continuity ref %.3g vs edit %.3g, worst dropout %.2f ms, "
                "hf excess %.2f dB, convergence %.1f dBFS\n",
                verdict.ok ? "PASS" : "FAIL", verdict.reference_d2, verdict.max_edit_d2,
                verdict.max_dropout_ms, verdict.max_hf_excess_db, verdict.worst_convergence_dbfs);
    for (const namchain::glitch::Finding& finding : verdict.findings)
      std::printf("  %-12s frame %zu  measured %.4g  limit %.4g  %s\n", finding.rule.c_str(), finding.frame,
                  finding.measured, finding.limit, finding.detail.c_str());
  }
  return verdict.ok && error.empty() ? 0 : 1;
}

int run_measure(const Args& args)
{
  const std::vector<float> x = load_input(args.input);
  const int q = args.quantum;
  const double deadline_us = q / kRate * 1e6;
  const Unit unit = make_unit(args);
  const int max_n = args.max_n > 0 ? args.max_n : 128;

  std::printf("namchain-warm-bench %s  measure\n", NAMCHAIN_VERSION_STRING);
  std::printf("unit:       %s -- %s\n", unit.label.c_str(), unit.what.c_str());
  std::printf("bar:        %s; quantum %d, deadline %.0f us, %.0f s a point, cpu %d\n",
              args.strict_bar ? "p99 under the deadline and zero overruns (D7, --bar strict)"
                              : "p99 under the deadline, overruns recorded (see the note in this mode's comment)",
              q, deadline_us, args.seconds, args.cpu);
  std::printf("\n%8s %11s %11s %11s %11s  %8s %8s  %7s %7s  %s\n", "n", "p50 us", "p99 us", "p99.9 us",
              "max us", "p99 %dl", "max %dl", "overruns", "behind", "verdict");
  (void)0;

  std::map<int, Point> points;
  const auto at = [&](int n) -> const Point& {
    const auto found = points.find(n);
    if (found != points.end())
      return found->second;
    const Point p = measure_units(args, unit, x, n);
    if (points.empty())
      std::printf("scheduling: %s\n", p.sched.c_str());
    print_point(p, deadline_us);
    return points.emplace(n, p).first->second;
  };
  const auto fits = [&](int n) { return args.strict_bar ? at(n).strict : at(n).pass; };

  // D2's bisection: double until it fails, then narrow. `lo` is the last n that fitted.
  int lo = 0, hi = 0;
  bool capped = false;
  int n = std::max(1, args.start_n);
  while (true)
  {
    if (!fits(n))
    {
      hi = n;
      break;
    }
    lo = n;
    if (n >= max_n)
    {
      capped = true;
      break;
    }
    const int doubled = std::min(max_n, n * 2);
    if (doubled == n)
    {
      capped = true;
      break;
    }
    n = doubled;
  }
  if (hi > 0 && lo > 0)
  {
    while (hi - lo > 1)
    {
      const int mid = lo + (hi - lo) / 2;
      if (fits(mid))
        lo = mid;
      else
        hi = mid;
    }
  }
  const int found = lo;

  // The cliff's shape is part of the measurement (D2 step 4): n+1 and n+2 always, and n-2 .. n+4
  // unless --no-curve, which is what T1 reads.
  if (found >= 1)
  {
    for (const int m : {found + 1, found + 2})
      if (m <= max_n)
        at(m);
    if (!args.no_curve)
      for (const int d : {-2, -1, 3, 4})
      {
        const int m = found + d;
        if (m >= 1 && m <= max_n)
          at(m);
      }
  }

  const double fraction = found >= 1 ? 1.0 / found : 1.0;
  std::printf("\n");
  if (found >= 1)
  {
    std::printf("n = %d: %d copies of this unit keep their p99 under the deadline on cpu %d. One unit "
                "costs 1/n = %.3g of a core.\n",
                found, found, args.cpu, fraction);
    if (!args.strict_bar)
    {
      // D7's stricter reading, over the same curve: the largest n that also had no late block at all.
      int strict_n = 0;
      for (const auto& [m, p] : points)
        if (p.strict)
          strict_n = std::max(strict_n, m);
      if (strict_n != found)
        std::printf("    D7's stricter bar (zero overruns too) would say n = %d: 1/n = %.3g. The "
                    "overrun count at the edge is not monotone in n; see the note at the top.\n",
                    strict_n, strict_n >= 1 ? 1.0 / strict_n : 0.0);
    }
  }
  else
    std::printf("n = 0: even one copy misses the deadline on cpu %d. There is no cost this unit could "
                "be admitted with; it is refused (D8's direction).\n",
                args.cpu);
  if (capped)
    std::printf("--max %d was reached without a miss: the cost above is an upper bound, not the cliff.\n", max_n);

  // The table's fragment: the row's value, the source it came from, and the curve it was read off.
  std::string json = "{\"unit\":\"" + unit.label + "\",\"core_percent\":" + format_g(fraction) +
                     ",\"bar\":\"" + (args.strict_bar ? "strict" : "p99") + "\"" +
                     ",\"quantum\":" + std::to_string(q) + ",\"deadline_us\":" + format_f(deadline_us);
  if (found >= 1)
  {
    const auto pct = [&](int m) {
      const auto it = points.find(m);
      return it == points.end() ? std::string("null") : format_f(it->second.p99 / deadline_us);
    };
    const auto beat = [&](int m) {
      const auto it = points.find(m);
      return it == points.end() ? std::string("null") : std::to_string(it->second.behind);
    };
    const auto over = [&](int m) {
      const auto it = points.find(m);
      return it == points.end() ? std::string("null") : std::to_string(it->second.overruns);
    };
    json += ",\"sources\":{\"n\":" + std::to_string(found) + ",\"p99_at_n\":" + pct(found) +
            ",\"overruns_at_n\":" + over(found) + ",\"p99_at_n_plus_1\":" + pct(found + 1) +
            ",\"overruns_at_n_plus_1\":" + over(found + 1) + ",\"p99_at_n_plus_2\":" + pct(found + 2) +
            ",\"blocks_behind_at_n_plus_1\":" + beat(found + 1) +
            ",\"at_most\":" + (capped ? "true" : "false") +
            (capped ? ",\"why\":\"--max " + std::to_string(max_n) + " reached without a miss\"" : "") + "}";
  }
  json += ",\"curve\":[";
  bool first = true;
  for (const auto& [m, p] : points)
  {
    json += std::string(first ? "" : ",") + "{\"n\":" + std::to_string(m) + ",\"p50\":" + format_f(p.p50) +
            ",\"p99\":" + format_f(p.p99) + ",\"p999\":" + format_f(p.p999) + ",\"max\":" + format_f(p.max) +
            ",\"p99_fraction\":" + format_f(p.p99 / deadline_us) + ",\"overruns\":" + std::to_string(p.overruns) +
            ",\"behind\":" + std::to_string(p.behind) + ",\"pass\":" + (p.pass ? "true" : "false") +
            ",\"strict\":" + (p.strict ? "true" : "false") + "}";
    first = false;
  }
  json += "]}";
  std::printf("fragment: %s\n", json.c_str());
  if (!args.json.empty())
  {
    std::ofstream out(args.json);
    if (!out)
      die("cannot write " + args.json);
    out << json << "\n";
    std::printf("wrote %s\n", args.json.c_str());
  }
  return found >= 1 ? 0 : 1;
}

// --- rig: T2, does the cost model predict? --------------------------------------
//
// The cost table's whole claim is that a rig's cost is the sum of its units' costs (D2), and T2 is the
// test that can falsify it: cost 12-20 heterogeneous rigs from the table alone, measure them on one
// core, and look for a rig predicted to fit that misses the deadline. "If that happens, the additive
// model needs a term the table does not have (the working-set sum against L2 is the obvious
// candidate)".
//
// So this mode takes the rig's *document* -- the thing a client publishes, not a unit -- costs it
// with the engine's own cost model (`daemon/cost_table.h`, the same code the daemon admits with), then
// renders it on the measured core paced, `--reps` copies of it at a time, and prints predicted
// against measured. The fragment carries both, and the exit status is the falsifier: a rig the table
// predicts at or under the headroom whose blocks do not all fit is a failure.
//
// T3's adversaries are the same run with the rigs written out: the two-amp wash, a 16-model rig, N x
// 8192-tap IRs, and an effects-only rig.

/// One document, compiled and ready to run: one plan per chain, the chain buffers, and the bus.
struct DocumentRig
{
  std::vector<std::unique_ptr<namchain::rt::Arena>> arenas;
  std::vector<std::unique_ptr<namchain::graph::RenderPlan>> plans;
  std::vector<std::vector<float>> ins, outs;
  std::vector<float> bus;

  void run(int frames)
  {
    std::fill(bus.begin(), bus.begin() + frames, 0.0f);
    for (std::size_t i = 0; i < plans.size(); ++i)
    {
      plans[i]->execute(ins[i].data(), outs[i].data(), frames);
      for (int f = 0; f < frames; ++f)
        bus[static_cast<std::size_t>(f)] += outs[i][static_cast<std::size_t>(f)];
    }
  }
};

std::unique_ptr<DocumentRig> make_document_rig(const namchain::graph::Document& document, int q, double rate)
{
  auto rig = std::make_unique<DocumentRig>();
  namchain::render::CompileOptions options;
  options.max_quantum = q;
  options.sample_rate = rate;
  for (const namchain::graph::Chain& chain : document.chains)
  {
    auto arena = std::make_unique<namchain::rt::Arena>(1u << 22);
    std::string error;
    auto plan = namchain::graph::RenderPlan::compile(chain.graph, options, *arena, error);
    if (!plan)
      die("cannot compile chain '" + chain.id + "': " + error);
    rig->plans.push_back(std::move(plan));
    rig->arenas.push_back(std::move(arena));
    rig->ins.emplace_back(static_cast<std::size_t>(q), 0.0f);
    rig->outs.emplace_back(static_cast<std::size_t>(q), 0.0f);
  }
  rig->bus.assign(static_cast<std::size_t>(q), 0.0f);
  return rig;
}

int run_rig(const Args& args)
{
  if (args.document.empty())
    die("rig mode needs --document rig.json");
  const std::vector<float> x = load_input(args.input);
  const int q = args.quantum;
  const double rate = kRate;
  const double deadline_us = q / rate * 1e6;
  const std::size_t blocks = static_cast<std::size_t>(args.seconds * rate) / static_cast<std::size_t>(q);
  const std::size_t loop = x.size() / static_cast<std::size_t>(q) * static_cast<std::size_t>(q);
  const int reps = std::max(1, args.reps);

  namchain::graph::Document document;
  std::string error;
  if (!namchain::graph::load_document_json(args.document, document, error))
    die(error);
  const namchain::graph::Validation validation = namchain::graph::validate(document);
  if (!validation.ok)
    die(validation.message);
  namchain::graph::Document resolved = document;
  namchain::graph::resolve_relative_paths(resolved, std::filesystem::path(args.document).parent_path().string());

  // The prediction: the engine's own model, on the same document, with the same table the device
  // would use. A table that is not there is every unit unmeasured (D8), which is a prediction too.
  const namchain::daemon::CostTable table = namchain::daemon::CostTable::load(args.table);
  namchain::daemon::UnitCache units;
  const namchain::daemon::CostOutcome cost =
    namchain::daemon::apply_cost_table(resolved, table, units, rate, q);

  std::printf("namchain-warm-bench %s  rig\n", NAMCHAIN_VERSION_STRING);
  std::printf("rig:        %s, %zu chain(s), %d copy(ies) at a time\n", args.document.c_str(),
              resolved.chains.size(), reps);
  std::printf("table:      %s (%s), headroom %.2f\n", table.path.c_str(), table.detail.c_str(), table.headroom);
  std::printf("predicted:  %s of a core as written, %s as it would play (%s), %s\n",
              namchain::daemon::format_cost(cost.full_cost).c_str(),
              namchain::daemon::format_cost(cost.cost).c_str(), cost.verdict.c_str(),
              cost.over ? "over the headroom" : "inside the headroom");
  std::printf("units:      %s\n", namchain::daemon::describe_costs(cost, resolved.chains.size() > 1).c_str());
  std::printf("bar:        p99 under the deadline; quantum %d, deadline %.0f us, %.0f s, cpu %d\n\n", q,
              deadline_us, args.seconds, args.cpu);

  std::vector<std::unique_ptr<DocumentRig>> rigs;
  for (int r = 0; r < reps; ++r)
    rigs.push_back(make_document_rig(resolved, q, rate));
  {
    // Fault every buffer in before the clock starts, as the unit measurement does.
    for (auto& rig : rigs)
      rig->run(q / 2);
  }

  const std::string sched =
    namchain::rt::describe_scheduling(namchain::rt::apply_scheduling(args.priority, args.cpu));
  std::printf("scheduling: %s\n", sched.c_str());
  std::printf("%8s %11s %11s %11s %11s  %8s %8s  %7s %7s  %s\n", "reps", "p50 us", "p99 us", "p99.9 us",
              "max us", "p99 %dl", "max %dl", "overruns", "behind", "verdict");

  std::vector<double> us;
  us.reserve(blocks);
  std::size_t overruns = 0, behind = 0;
  timespec next{};
  clock_gettime(CLOCK_MONOTONIC, &next);
  const long period_ns = static_cast<long>(q / rate * 1e9);
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
    for (auto& rig : rigs)
    {
      for (auto& buffer : rig->ins)
        std::copy(in, in + q, buffer.begin());
      rig->run(q);
    }
    const double this_us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    us.push_back(this_us);
    if (this_us > deadline_us)
      ++overruns;
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const long late = (now.tv_sec - next.tv_sec) * 1000000000L + (now.tv_nsec - next.tv_nsec);
    if (late > 0)
      behind = std::max(behind, static_cast<std::size_t>(late / period_ns));
  }

  const Stats s = stats(us);
  const double measured = s.p99 / deadline_us;
  const bool fits = s.p99 < deadline_us;
  std::printf("  %6d  %11.1f %11.1f %11.1f %11.1f  %8.1f %8.1f  %7zu  %6zu  %s\n", reps, s.p50, s.p99, s.p999,
              s.max, 100 * s.p99 / deadline_us, 100 * s.max / deadline_us, overruns, behind,
              fits ? "fits" : "MISSES");

  // T2's falsifier, said in one line: the table said this rig fits, and it did not.
  const bool predicted_fits = !cost.over && cost.cost <= table.headroom;
  const bool false_confidence = predicted_fits && !fits;
  std::printf("\npredicted %s (p99 at most %.1f%% of the deadline for %.2f of a core), measured %s "
              "(p99 %.1f%%, max %.1f%%, %zu overrun(s) of %zu blocks)\n",
              namchain::daemon::format_cost(cost.cost).c_str(), 100 * table.headroom, cost.cost,
              fits ? "fits" : "MISSES", 100 * measured, 100 * s.max / deadline_us, overruns, us.size());
  if (false_confidence)
    std::printf("T2 FALSIFIED: the table admitted this rig and its blocks do not all fit. The additive "
                "model needs a term it does not have (the working-set sum against L2 is the obvious "
                "candidate), or more headroom -- decided by the size of the error.\n");
  else if (!predicted_fits && fits)
    std::printf("(the table refused a rig that measured inside the deadline: the conservative "
                "direction, D8.)\n");

  std::printf("fragment: {\"document\":\"%s\",\"reps\":%d,\"predicted_cost\":%s,\"predicted_over\":%s,"
              "\"predicted_verdict\":\"%s\",\"headroom\":%s,\"measured_p99_fraction\":%s,"
              "\"measured_max_fraction\":%s,\"overruns\":%zu,\"behind\":%zu,\"predicted_fits\":%s,"
              "\"measured_fits\":%s,\"falsified\":%s}\n",
              args.document.c_str(), reps, format_g(cost.cost).c_str(), cost.over ? "true" : "false",
              cost.verdict.c_str(), format_g(table.headroom).c_str(), format_f(measured).c_str(),
              format_f(s.max / deadline_us).c_str(), overruns, behind, predicted_fits ? "true" : "false",
              fits ? "true" : "false", false_confidence ? "true" : "false");
  if (!args.json.empty())
  {
    std::ofstream out(args.json);
    if (!out)
      die("cannot write " + args.json);
    out << "{\"document\":\"" << args.document << "\",\"predicted_cost\":" << format_g(cost.cost)
        << ",\"measured_p99_fraction\":" << format_f(measured) << ",\"falsified\":"
        << (false_confidence ? "true" : "false") << "}\n";
  }
  return false_confidence ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
  Args args;
  if (argc < 2)
    die("usage: namchain-warm-bench state|slim|chain|ir|contention|measure|rig --input in.wav "
        "--live model.nam cand.nam ... [--ir cab.wav]; see the comment at the top of this file");
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
    {
      args.warm_cpu = std::atoi(next().c_str());
      args.cpu = args.warm_cpu;
    }
    else if (a == "--cpu")
    {
      args.cpu = std::atoi(next().c_str());
      args.warm_cpu = args.cpu;
    }
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
    else if (a == "--unit")
      args.unit = next();
    else if (a == "--model")
      args.model = next();
    else if (a == "--node")
      args.node = next();
    else if (a == "--node-param")
    {
      const std::string pair = next();
      const std::size_t eq = pair.find('=');
      if (eq == std::string::npos)
        die("--node-param takes name=value, not '" + pair + "'");
      args.node_params[pair.substr(0, eq)] = std::atof(pair.c_str() + eq + 1);
    }
    else if (a == "--taps")
      args.taps = std::atoi(next().c_str());
    else if (a == "--max")
      args.max_n = std::atoi(next().c_str());
    else if (a == "--start")
      args.start_n = std::atoi(next().c_str());
    else if (a == "--no-curve")
      args.no_curve = true;
    else if (a == "--document")
      args.document = next();
    else if (a == "--table")
      args.table = next();
    else if (a == "--reps")
      args.reps = std::atoi(next().c_str());
    else if (a == "--bar")
    {
      const std::string bar = next();
      if (bar != "p99" && bar != "strict")
        die("--bar takes p99 or strict, not '" + bar + "'");
      args.strict_bar = bar == "strict";
    }
    else if (a == "--label")
      args.label = next();
    else if (a == "--json")
      args.json = next();
    else if (a == "--with-ir")
      args.with_ir = next();
    else if (a == "--live-ir")
      args.live_ir = next();
    else if (a == "--levels")
      args.levels = true;
    else if (a == "--no-levels")
      args.levels = false;
    else if (a == "--levels-first-frame")
    {
      args.levels = true;
      args.levels_first_frame = true;
    }
    else if (a == "--levels-publish-period")
    {
      args.levels = true;
      args.levels_publish_period = true;
    }
    else if (a.rfind("--", 0) == 0)
      die("unknown option " + a);
    else
      args.candidates.push_back(a);
  }
  if (args.input.empty())
    die("--input is required");
  // What each mode needs. --ir is optional everywhere (without one a rig is the model alone);
  // chain mode takes its models from the positional list, so it needs no --live, and ir mode is
  // about one rig's cab, so it needs no candidates. measure mode is about one unit, which its own
  // flags describe: it needs neither a candidate list nor --live.
  if (args.mode == "measure")
    return run_measure(args);
  if (args.mode == "rig")
    return run_rig(args);
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
  if (args.mode == "switch")
    return run_switch(args);
  die("mode must be state, slim, chain, ir, contention, switch, measure or rig");
}
