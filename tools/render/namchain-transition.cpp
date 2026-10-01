// namchain-transition — one structural edit, rendered offline both ways the daemon can make it.
//
// Takes two rig documents (the rig before an edit and after it) and an input WAV, and renders the edit
// of one chain at an exact sample:
//
//   * as a local transition (docs/transitions.md, "Local transitions"): what the edit did not touch
//     keeps its instance and state, and only where the signal changed is crossfaded;
//   * as the whole-chain crossfade the daemon falls back to: the new chain built and prewarmed from
//     nothing, and the old and new chains both rendered for the fade.
//
// Each is judged against the before and after oracles with the glitch detector, and every quantum is
// timed, so the question the vip soak asks ("how much more than the rig's own load does the edit
// cost?") can be answered on any machine, with real models, before anything is deployed. The times
// are this machine's; on the Core host they are what the soak measures.
//
// Usage: namchain-transition [options] <before.json> <after.json> <input.wav>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "namchain/assets/registry.h"
#include "namchain/glitch.h"
#include "namchain/graph/carry.h"
#include "namchain/graph/macro.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"
#include "namchain/rt/crossfade.h"
#include "namchain/version.h"
#include "namchain/wav.h"

namespace
{

using namchain::graph::CarriedPlans;
using namchain::graph::Graph;
using namchain::graph::LocalPlan;
using namchain::graph::RenderPlan;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kArenaBytes = 1u << 20; // as the daemon's

void usage()
{
  std::fprintf(stderr,
               "namchain-transition %s\n"
               "usage: namchain-transition [options] <before.json> <after.json> <input.wav>\n"
               "options:\n"
               "  --models DIR    the model library (default ~/Documents/Audio/NAM)\n"
               "  --irs DIR       the IR library (default ~/Documents/Audio/IRs)\n"
               "  --chain ID      the chain to edit (default: the first that differs)\n"
               "  --at S          when the edit lands, in seconds into the input (default 2)\n"
               "  --fade-ms MS    the crossfade length (default 5, as namchaind)\n"
               "  --quantum N     block size in samples (default 64)\n"
               "  --out PREFIX    write PREFIX.local.wav and PREFIX.crossfade.wav\n"
               "\n"
               "Renders the edit as a local transition and as a whole-chain crossfade, judges both with\n"
               "the glitch detector, and times every quantum. Exits non-zero when the local transition\n"
               "fails a glitch rule or could not be planned.\n",
               NAMCHAIN_VERSION_STRING);
}

std::string home_path(const char* rest)
{
  const char* home = std::getenv("HOME");
  return std::string(home != nullptr ? home : "") + rest;
}

bool load_document(const std::string& path, const namchain::assets::Registry& registry,
                   namchain::graph::Document& document, std::string& error)
{
  if (!namchain::graph::load_document_json(path, document, error))
    return false;
  const std::string base_dir = std::filesystem::absolute(path).parent_path().string();
  namchain::graph::resolve_relative_paths(document, base_dir);
  return namchain::assets::resolve_document(document, registry, base_dir, error);
}

/// Per-quantum times, in microseconds.
struct Times
{
  std::vector<double> us;
  void add(Clock::time_point from) { us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - from).count()); }
  double percentile(double p) const
  {
    if (us.empty())
      return 0.0;
    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t at = std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())));
    return sorted[at];
  }
  double max() const { return us.empty() ? 0.0 : *std::max_element(us.begin(), us.end()); }
  double mean() const
  {
    double sum = 0.0;
    for (double v : us)
      sum += v;
    return us.empty() ? 0.0 : sum / static_cast<double>(us.size());
  }
};

std::unique_ptr<RenderPlan> compile(const Graph& graph, const namchain::render::CompileOptions& options,
                                    namchain::rt::Arena& arena, std::string& error)
{
  auto plan = RenderPlan::compile(graph, options, arena, error);
  if (plan != nullptr)
    plan->prewarm();
  return plan;
}

/// A plain render of \p graph from frame 0: an oracle, and the steady load of the rig.
bool render_oracle(const Graph& graph, const namchain::render::CompileOptions& options, const std::vector<float>& input,
                   std::vector<float>& output, Times& times, std::string& error)
{
  namchain::rt::Arena arena(kArenaBytes);
  auto plan = compile(graph, options, arena, error);
  if (plan == nullptr)
    return false;
  output.assign(input.size(), 0.0f);
  const int q = options.max_quantum;
  for (std::size_t at = 0; at + static_cast<std::size_t>(q) <= input.size(); at += static_cast<std::size_t>(q))
  {
    const auto start = Clock::now();
    plan->execute(input.data() + at, output.data() + at, q);
    times.add(start);
  }
  return true;
}

void copy_buffers(const std::vector<std::pair<const float*, float*>>& copies, int frames)
{
  for (const auto& [from, to] : copies)
    std::copy(from, from + frames, to);
}

struct EditRun
{
  std::vector<float> output;
  Times during;        ///< the quanta of the transition itself
  double build_ms = 0; ///< control-thread work before the switch: compile, load, prewarm
};

bool apply_ramps(RenderPlan& plan, const std::vector<namchain::graph::CarriedRamp>& ramps, std::string& error)
{
  for (const namchain::graph::CarriedRamp& ramp : ramps)
  {
    std::size_t node_index = 0;
    uint32_t param_id = 0;
    if (!plan.resolve_param(ramp.node, ramp.param, -1, node_index, param_id) ||
        !plan.set_param(node_index, param_id, static_cast<float>(ramp.value)))
    {
      error = "'" + ramp.node + "." + ramp.param + "' cannot be ramped";
      return false;
    }
  }
  return true;
}

/// A change only to values that ramp: no new plan, only the ramps on the running one.
bool run_ramps(const Graph& before, const LocalPlan& plan, const namchain::render::CompileOptions& options,
               const std::vector<float>& input, std::size_t edit_at, EditRun& run, std::string& error)
{
  const int q = options.max_quantum;
  namchain::rt::Arena arena(kArenaBytes);
  std::unique_ptr<RenderPlan> running = compile(before, options, arena, error);
  if (running == nullptr)
    return false;
  run.output.assign(input.size(), 0.0f);
  for (std::size_t at = 0; at + static_cast<std::size_t>(q) <= input.size(); at += static_cast<std::size_t>(q))
  {
    if (at == edit_at && !apply_ramps(*running, plan.ramps, error))
      return false;
    const auto start = Clock::now();
    running->execute(input.data() + at, run.output.data() + at, q);
    if (at == edit_at)
      run.during.add(start);
  }
  return true;
}

/// What the render thread does with a local transition (as tests/test_carry.cpp's render_local).
bool run_local(const Graph& before, const LocalPlan& plan, const namchain::render::CompileOptions& options,
               const std::vector<float>& input, std::size_t edit_at, int fade, EditRun& run, std::string& error)
{
  const int q = options.max_quantum;
  namchain::rt::Arena running_arena(kArenaBytes);
  namchain::rt::Arena first_arena(kArenaBytes);
  namchain::rt::Arena final_arena(kArenaBytes);
  std::unique_ptr<RenderPlan> running = compile(before, options, running_arena, error);
  if (running == nullptr)
    return false;
  run.output.assign(input.size(), 0.0f);
  std::size_t at = 0;
  for (; at < edit_at; at += static_cast<std::size_t>(q))
    running->execute(input.data() + at, run.output.data() + at, q);

  CarriedPlans plans;
  const auto build_start = Clock::now();
  if (!namchain::graph::build_carried_plans(*running, plan, options, first_arena, final_arena, plans, error))
    return false;
  run.build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();

  // The values that moved on kept nodes ramp, set through the final plan as the daemon routes them:
  // the instance is shared, so the ramp starts at the switch whichever plan is playing.
  if (!apply_ramps(*plans.final_plan, plan.ramps, error))
    return false;

  RenderPlan* playing = plans.transitional != nullptr ? plans.transitional.get() : plans.final_plan.get();
  copy_buffers(plans.on_adopt, q);
  int remaining = plans.transitional != nullptr ? fade : 0;
  for (; at + static_cast<std::size_t>(q) <= input.size(); at += static_cast<std::size_t>(q))
  {
    const bool timed = remaining > 0 || at == edit_at;
    const auto start = Clock::now();
    playing->execute(input.data() + at, run.output.data() + at, q);
    if (timed)
      run.during.add(start);
    if (remaining > 0)
    {
      remaining -= q;
      if (remaining <= 0)
      {
        copy_buffers(plans.on_final, q);
        playing = plans.final_plan.get();
      }
    }
  }
  running.reset();
  return true;
}

/// The whole-chain crossfade: the new chain built from nothing, both rendered for the fade.
bool run_crossfade(const Graph& before, const Graph& after, const namchain::render::CompileOptions& options,
                   const std::vector<float>& input, std::size_t edit_at, int fade, EditRun& run, std::string& error)
{
  const int q = options.max_quantum;
  namchain::rt::Arena old_arena(kArenaBytes);
  namchain::rt::Arena new_arena(kArenaBytes);
  std::unique_ptr<RenderPlan> old_plan = compile(before, options, old_arena, error);
  if (old_plan == nullptr)
    return false;
  run.output.assign(input.size(), 0.0f);
  std::size_t at = 0;
  for (; at < edit_at; at += static_cast<std::size_t>(q))
    old_plan->execute(input.data() + at, run.output.data() + at, q);

  const auto build_start = Clock::now();
  std::unique_ptr<RenderPlan> new_plan = compile(after, options, new_arena, error);
  if (new_plan == nullptr)
    return false;
  run.build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();

  namchain::rt::Crossfade crossfade(fade);
  crossfade.start(fade);
  std::vector<float> a(static_cast<std::size_t>(q)), b(static_cast<std::size_t>(q));
  for (; at + static_cast<std::size_t>(q) <= input.size(); at += static_cast<std::size_t>(q))
  {
    const auto start = Clock::now();
    if (crossfade.active())
    {
      old_plan->execute(input.data() + at, a.data(), q);
      new_plan->execute(input.data() + at, b.data(), q);
      crossfade.mix(a.data(), b.data(), run.output.data() + at, q);
      run.during.add(start);
    }
    else
      new_plan->execute(input.data() + at, run.output.data() + at, q);
  }
  return true;
}

/// \brief Rules 1-3 of the glitch detector around the edit: continuity, dropouts, added high frequencies.
///
/// Rule 4 (convergence) is left to \ref settle_ms: a rig with a feedback delay or a reverb remembers
/// seconds of what it played, so "settled" against an oracle with a different history is a question of
/// when, not whether.
namchain::glitch::Report judge(const std::vector<float>& y, const std::vector<float>& before,
                               const std::vector<float>& after, std::size_t edit_at, int fade, double rate)
{
  namchain::glitch::Options detector;
  detector.sample_rate = rate;
  detector.convergence_db = 1000.0;
  const std::vector<namchain::glitch::Edit> edits = {{edit_at, edit_at + static_cast<std::size_t>(fade)}};
  std::string error;
  namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), before.data(), after.data(), y.size(), edits, detector, error);
  if (!error.empty())
  {
    report.ok = false;
    report.findings.push_back({});
    report.findings.back().rule = "error";
    report.findings.back().detail = error;
  }
  return report;
}

/// How long after the edit \p y last differs from \p oracle by more than -60 dBFS, in milliseconds;
/// 0 when it never does, and a negative number when it still does at the end of the input.
double settle_ms(const std::vector<float>& y, const std::vector<float>& oracle, std::size_t edit_at, double rate)
{
  const float limit = 0.001f;
  std::size_t last = 0;
  bool differs = false;
  for (std::size_t i = edit_at; i < y.size(); ++i)
    if (std::fabs(y[i] - oracle[i]) > limit)
    {
      last = i;
      differs = true;
    }
  if (!differs)
    return 0.0;
  const double ms = 1000.0 * static_cast<double>(last + 1 - edit_at) / rate;
  return last + rate / 10.0 >= static_cast<double>(y.size()) ? -ms : ms;
}

std::string settle_text(double ms)
{
  char text[64];
  if (ms == 0.0)
    std::snprintf(text, sizeof text, "at once");
  else if (ms < 0.0)
    std::snprintf(text, sizeof text, "not by the end (%.0f ms)", -ms);
  else
    std::snprintf(text, sizeof text, "after %.0f ms", ms);
  return text;
}

void print_report(const char* name, const EditRun& run, const namchain::glitch::Report& report, double deadline_us,
                  double settle_instant, double settle_fresh)
{
  std::printf("%-10s build %7.1f ms   during: %3zu quanta, mean %7.1f us, max %7.1f us (%5.1f%% of the deadline)   "
              "glitch: %s\n",
              name, run.build_ms, run.during.us.size(), run.during.mean(), run.during.max(),
              100.0 * run.during.max() / deadline_us, report.ok ? "pass" : "FAIL");
  for (const namchain::glitch::Finding& finding : report.findings)
    std::printf("           %s at %zu: %s\n", finding.rule.c_str(), static_cast<std::size_t>(finding.frame),
                finding.detail.c_str());
  std::printf("           settles to the same edit made instantly %s; to the after rig rendered from the start %s\n",
              settle_text(settle_instant).c_str(), settle_text(settle_fresh).c_str());
}

} // namespace

int main(int argc, char** argv)
{
  std::string models = home_path("/Documents/Audio/NAM");
  std::string irs = home_path("/Documents/Audio/IRs");
  std::string chain_id;
  std::string out_prefix;
  double at_seconds = 2.0;
  double fade_ms = 5.0;
  int quantum = 64;
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
    if (arg == "--models")
      models = value();
    else if (arg == "--irs")
      irs = value();
    else if (arg == "--chain")
      chain_id = value();
    else if (arg == "--at")
      at_seconds = std::strtod(value(), nullptr);
    else if (arg == "--fade-ms")
      fade_ms = std::strtod(value(), nullptr);
    else if (arg == "--quantum")
      quantum = std::atoi(value());
    else if (arg == "--out")
      out_prefix = value();
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
  if (positional.size() != 3 || quantum < 1 || quantum > 4096 || !(fade_ms > 0.0) || !(at_seconds >= 0.0))
  {
    usage();
    return 2;
  }

  namchain::assets::Registry registry;
  registry.configure(models, irs);
  registry.refresh();

  std::string error;
  namchain::graph::Document before_doc, after_doc;
  if (!load_document(positional[0], registry, before_doc, error) ||
      !load_document(positional[1], registry, after_doc, error))
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  // The chain: the one named, or the first whose graph differs.
  const namchain::graph::Chain* before_chain = nullptr;
  const namchain::graph::Chain* after_chain = nullptr;
  for (const auto& chain : after_doc.chains)
  {
    const auto match = std::find_if(before_doc.chains.begin(), before_doc.chains.end(),
                                     [&](const namchain::graph::Chain& c) { return c.id == chain.id; });
    if (match == before_doc.chains.end())
      continue;
    if (chain_id.empty() ? !namchain::graph::same_graph(match->graph, chain.graph) : chain.id == chain_id)
    {
      before_chain = &*match;
      after_chain = &chain;
      break;
    }
  }
  if (before_chain == nullptr)
  {
    std::fprintf(stderr, "error: %s\n",
                 chain_id.empty() ? "no chain differs between the two documents"
                                  : ("no chain '" + chain_id + "' in both documents").c_str());
    return 1;
  }

  Graph before, after;
  if (!namchain::graph::lower_graph(before_chain->graph, before, error) ||
      !namchain::graph::lower_graph(after_chain->graph, after, error))
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  std::vector<float> input;
  double rate = 0.0;
  if (!namchain::load_wav_mono(positional[2], input, rate, error))
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  const int fade = std::max(1, static_cast<int>(std::lround(fade_ms * rate / 1000.0)));
  const std::size_t edit_at =
    static_cast<std::size_t>(std::llround(at_seconds * rate / quantum)) * static_cast<std::size_t>(quantum);
  if (edit_at + static_cast<std::size_t>(fade) + static_cast<std::size_t>(quantum) * 64 > input.size())
  {
    std::fprintf(stderr, "error: the input is too short for an edit at %.3f s\n", at_seconds);
    return 2;
  }
  input.resize(input.size() / static_cast<std::size_t>(quantum) * static_cast<std::size_t>(quantum));

  namchain::render::CompileOptions options;
  options.max_quantum = quantum;
  options.sample_rate = rate;

  LocalPlan plan;
  std::string reason;
  const bool local = namchain::graph::plan_local_transition(before, after, {}, fade, plan, reason);

  std::printf("chain:     %s, %zu nodes -> %zu nodes (lowered)\n", after_chain->id.c_str(), before.nodes.size(),
              after.nodes.size());
  if (!local)
    std::printf("local:     not possible: %s\n", reason.c_str());
  else if (plan.ramps_only)
    std::printf("local:     ramps only (%zu); no plan is built\n", plan.ramps.size());
  else
    std::printf("local:     %s, %d fade%s, %d kept, %d new, %d retiring\n", plan.instant ? "instant" : "transitional",
                plan.fades, plan.fades == 1 ? "" : "s", plan.kept, plan.created, plan.retiring);

  std::vector<float> before_out, after_out;
  Times before_times, after_times;
  if (!render_oracle(before, options, input, before_out, before_times, error) ||
      !render_oracle(after, options, input, after_out, after_times, error))
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  const double deadline_us = 1e6 * quantum / rate;
  std::printf("deadline:  %.1f us per %d-frame quantum; edit at frame %zu, fade %d frames\n", deadline_us, quantum,
              edit_at, fade);
  std::printf("before     steady: mean %7.1f us, p99 %7.1f us, max %7.1f us\n", before_times.mean(),
              before_times.percentile(0.99), before_times.max());
  std::printf("after      steady: mean %7.1f us, p99 %7.1f us, max %7.1f us\n", after_times.mean(),
              after_times.percentile(0.99), after_times.max());

  // Each way of making the edit is judged against itself made instantly (a one-frame fade): the same
  // instances, the same history, only the fade missing. That is the oracle the fade has to match.
  EditRun crossfade, crossfade_instant;
  if (!run_crossfade(before, after, options, input, edit_at, fade, crossfade, error) ||
      !run_crossfade(before, after, options, input, edit_at, 1, crossfade_instant, error))
  {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  const auto crossfade_report = judge(crossfade.output, before_out, crossfade_instant.output, edit_at, fade, rate);
  print_report("crossfade", crossfade, crossfade_report, deadline_us,
               settle_ms(crossfade.output, crossfade_instant.output, edit_at, rate),
               settle_ms(crossfade.output, after_out, edit_at, rate));

  int status = 0;
  EditRun local_run;
  if (local)
  {
    EditRun local_instant;
    LocalPlan instant_plan;
    bool rendered = false;
    if (plan.ramps_only)
      rendered = run_ramps(before, plan, options, input, edit_at, local_run, error) &&
                 run_ramps(before, plan, options, input, edit_at, local_instant, error);
    else
      rendered = run_local(before, plan, options, input, edit_at, fade, local_run, error) &&
                 namchain::graph::plan_local_transition(before, after, {}, 1, instant_plan, error) &&
                 run_local(before, instant_plan, options, input, edit_at, 1, local_instant, error);
    if (!rendered)
    {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    const auto local_report = judge(local_run.output, before_out, local_instant.output, edit_at, fade, rate);
    print_report(plan.ramps_only ? "ramps" : "local", local_run, local_report, deadline_us,
                 settle_ms(local_run.output, local_instant.output, edit_at, rate),
                 settle_ms(local_run.output, after_out, edit_at, rate));
    status = local_report.ok ? 0 : 1;
  }
  else
    status = 1;

  if (!out_prefix.empty())
  {
    if (!local_run.output.empty() &&
        !namchain::save_wav_float32(out_prefix + ".local.wav", local_run.output.data(), local_run.output.size(), 1, rate,
                                    error))
      std::fprintf(stderr, "error: %s\n", error.c_str());
    if (!namchain::save_wav_float32(out_prefix + ".crossfade.wav", crossfade.output.data(), crossfade.output.size(), 1,
                                    rate, error))
      std::fprintf(stderr, "error: %s\n", error.c_str());
  }
  return status;
}
