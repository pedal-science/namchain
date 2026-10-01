// Local transitions (docs/transitions.md, "Local transitions"): what an edit keeps, what it builds,
// where it crossfades, and -- sample for sample -- what the chain plays while it does.
//
// The render thread's part is reproduced exactly: the running plan, then the transitional plan for the
// fade (the carried buffers copied in first), then the final plan. The checks are of two kinds. Exact
// ones compare against the output the design says it must be: the old chain before the edit, an
// equal-power crossfade where the edit is, the new chain after, and a kept node's own history
// continuing (a delay still playing what it heard before the edit). Then the glitch detector judges
// the same transitions the way it judges the whole-chain one.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "namchain/glitch.h"
#include "namchain/graph/carry.h"
#include "namchain/graph/macro.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"
#include "namchain/rt/crossfade.h"

namespace
{

using namchain::graph::CarriedPlans;
using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::LocalPlan;
using namchain::graph::NodeSpec;
using namchain::graph::RenderPlan;

constexpr int kRate = 48000;
constexpr int kQuantum = 64;
constexpr int kFade = 240; // 5 ms at 48 kHz
constexpr std::size_t kFrames = 8192;
constexpr std::size_t kEditAt = 2048; // a block boundary, which is where the render thread switches

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

NodeSpec gain(const std::string& id, double value)
{
  NodeSpec spec = node(id, "nc.gain");
  spec.params["gain"] = value;
  return spec;
}

NodeSpec delay(const std::string& id, double samples)
{
  NodeSpec spec = node(id, "nc.delay");
  spec.params["samples"] = samples;
  return spec;
}

NodeSpec biquad(const std::string& id, const char* mode, double freq, double gain_db)
{
  NodeSpec spec = node(id, "nc.eq.biquad");
  spec.strings["mode"] = mode;
  spec.params["freq"] = freq;
  spec.params["q"] = 0.707;
  spec.params["gain_db"] = gain_db;
  return spec;
}

/// `in -> nodes... -> out`, in series.
Graph series(const std::vector<NodeSpec>& middle)
{
  Graph graph;
  graph.nodes.push_back(node("in", "nc.input"));
  for (const NodeSpec& spec : middle)
    graph.nodes.push_back(spec);
  graph.nodes.push_back(node("out", "nc.output"));
  std::string previous = "in";
  for (const NodeSpec& spec : middle)
  {
    graph.connections.push_back({previous, 0, spec.id, 0});
    previous = spec.id;
  }
  graph.connections.push_back({previous, 0, "out", 0});
  return graph;
}

Graph lowered(const Graph& graph)
{
  Graph out;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::lower_graph(graph, out, error), error);
  return out;
}

LocalPlan plan_for(const Graph& before, const Graph& after, const std::set<std::string>& unusable = {})
{
  LocalPlan plan;
  std::string reason;
  REQUIRE_MESSAGE(namchain::graph::plan_local_transition(lowered(before), lowered(after), unusable, kFade, plan, reason),
                  reason);
  return plan;
}

bool has_node(const Graph& graph, const std::string& id)
{
  return std::any_of(graph.nodes.begin(), graph.nodes.end(), [&id](const NodeSpec& n) { return n.id == id; });
}

const NodeSpec* find(const Graph& graph, const std::string& id)
{
  for (const NodeSpec& n : graph.nodes)
    if (n.id == id)
      return &n;
  return nullptr;
}

/// The node driving one input port, or empty.
std::string driver_of(const Graph& graph, const std::string& id, int port)
{
  for (const Connection& c : graph.connections)
    if (c.to == id && c.to_port == port)
      return c.from;
  return {};
}

int count_type(const Graph& graph, const std::string& type)
{
  return static_cast<int>(std::count_if(graph.nodes.begin(), graph.nodes.end(),
                                        [&type](const NodeSpec& n) { return n.type == type; }));
}

std::vector<float> tone(std::size_t frames, float hz = 1000.0f)
{
  std::vector<float> samples(frames);
  for (std::size_t i = 0; i < frames; ++i)
    samples[i] = 0.4f * std::sin(2.0f * 3.14159265358979f * hz * static_cast<float>(i) / static_cast<float>(kRate));
  return samples;
}

/// Broadband material, for what a tone cannot show (a delay's history is the same tone either way).
std::vector<float> noise(std::size_t frames)
{
  std::vector<float> samples(frames);
  uint32_t state = 12345u;
  for (std::size_t i = 0; i < frames; ++i)
  {
    state = state * 1664525u + 1013904223u;
    samples[i] = (static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 0.5f;
  }
  return samples;
}

namchain::render::CompileOptions options()
{
  namchain::render::CompileOptions options;
  options.max_quantum = kQuantum;
  options.sample_rate = kRate;
  return options;
}

std::unique_ptr<RenderPlan> compile(const Graph& graph, namchain::rt::Arena& arena)
{
  std::string error;
  auto plan = RenderPlan::compile(graph, options(), arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

std::vector<float> render(const Graph& graph, const std::vector<float>& input)
{
  namchain::rt::Arena arena(1 << 20);
  auto plan = compile(graph, arena);
  plan->prewarm();
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t at = 0; at < input.size(); at += kQuantum)
    plan->execute(input.data() + at, output.data() + at, kQuantum);
  return output;
}

void copy_buffers(const std::vector<std::pair<const float*, float*>>& copies)
{
  for (const auto& [from, to] : copies)
    std::copy(from, from + kQuantum, to);
}

struct Run
{
  std::vector<float> output;
  LocalPlan plan;
};

/// The daemon's local transition, reduced to what the render thread does with it: \p before plays
/// until \p kEditAt; then the first plan takes over (the carried buffers copied in), runs until its
/// fades have closed, and the final plan takes over from it. \p copy_carried false leaves the buffer
/// copies out, which is the negative control for a feedback loop.
Run render_local(const Graph& before, const Graph& after, const std::vector<float>& input, bool copy_carried = true)
{
  Run run;
  run.plan = plan_for(before, after);

  namchain::rt::Arena running_arena(1 << 20);
  namchain::rt::Arena first_arena(1 << 20);
  namchain::rt::Arena final_arena(1 << 20);
  std::unique_ptr<RenderPlan> running = compile(lowered(before), running_arena);
  running->prewarm();

  run.output.assign(input.size(), 0.0f);
  std::size_t at = 0;
  for (; at < kEditAt; at += kQuantum)
    running->execute(input.data() + at, run.output.data() + at, kQuantum);

  // Only ramps and bypasses: no new plan, the running one is told, as the render thread does with the
  // daemon's commands.
  if (run.plan.ramps_only)
  {
    for (const namchain::graph::CarriedRamp& ramp : run.plan.ramps)
    {
      std::size_t node = 0;
      uint32_t param = 0;
      REQUIRE(running->resolve_param(ramp.node, ramp.param, -1, node, param));
      REQUIRE(running->set_param(node, param, static_cast<float>(ramp.value)));
    }
    for (const namchain::graph::CarriedBypass& bypass : run.plan.bypasses)
      REQUIRE(running->set_bypass(static_cast<std::size_t>(running->index_of(bypass.node)), bypass.bypass, kFade));
    for (; at < input.size(); at += kQuantum)
      running->execute(input.data() + at, run.output.data() + at, kQuantum);
    return run;
  }

  // Planned and compiled while `running` is still the plan that plays, as on the control thread.
  CarriedPlans plans;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::build_carried_plans(*running, run.plan, options(), first_arena, final_arena,
                                                       plans, error),
                  error);

  RenderPlan* playing = plans.transitional != nullptr ? plans.transitional.get() : plans.final_plan.get();
  if (copy_carried)
    copy_buffers(plans.on_adopt);
  int remaining = plans.transitional != nullptr ? kFade : 0;
  for (; at < input.size(); at += kQuantum)
  {
    playing->execute(input.data() + at, run.output.data() + at, kQuantum);
    if (remaining > 0)
    {
      remaining -= kQuantum;
      if (remaining <= 0)
      {
        if (copy_carried)
          copy_buffers(plans.on_final);
        playing = plans.final_plan.get();
      }
    }
  }
  // The running plan goes first, as the reclaimer frees it first: the instances it kept must survive.
  running.reset();
  return run;
}

/// The equal-power crossfade from \p from to \p to, starting at \p start: what one `nc.xfade` plays.
std::vector<float> expected_fade(const std::vector<float>& from, const std::vector<float>& to, std::size_t start)
{
  std::vector<float> out(from.begin(), from.end());
  namchain::rt::Crossfade fade(kFade);
  fade.start(kFade);
  for (std::size_t at = start; at < out.size(); at += kQuantum)
    fade.mix(from.data() + at, to.data() + at, out.data() + at, kQuantum);
  return out;
}

namchain::glitch::Report judge(const std::vector<float>& y, const std::vector<float>& before,
                               const std::vector<float>& after)
{
  namchain::glitch::Options detector;
  detector.sample_rate = kRate;
  const std::vector<namchain::glitch::Edit> edits = {{kEditAt, kEditAt + kFade}};
  std::string error;
  namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), before.data(), after.data(), y.size(), edits, detector, error);
  REQUIRE_MESSAGE(error.empty(), error);
  for (const namchain::glitch::Finding& finding : report.findings)
    MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  return report;
}

} // namespace

// ---------------------------------------------------------------------------
// The planner: what is kept, what is built, where the fades go
// ---------------------------------------------------------------------------

TEST_CASE("adding a block keeps the chain and fades the new block in where it joins")
{
  const LocalPlan plan = plan_for(series({gain("a", 0.8)}), series({gain("a", 0.8), gain("b", 0.5)}));
  CHECK_FALSE(plan.ramps_only);
  CHECK_FALSE(plan.instant);
  CHECK(plan.fades == 1);
  CHECK(plan.created == 1);
  CHECK(plan.retiring == 0);
  CHECK(plan.kept == 3); // in, a, out
  CHECK(plan.carried_from.at("a") == "a");
  CHECK(plan.carried_from.count("b") == 0);

  // The output's input is crossfaded: from `a` (the old signal) to `b` (the new one).
  const std::string fade = driver_of(plan.transitional, "out", 0);
  REQUIRE(find(plan.transitional, fade) != nullptr);
  CHECK(find(plan.transitional, fade)->type == "nc.xfade");
  CHECK(driver_of(plan.transitional, fade, 0) == "a");
  CHECK(driver_of(plan.transitional, fade, 1) == "b");
  CHECK(driver_of(plan.transitional, "b", 0) == "a");
}

TEST_CASE("removing a block keeps it running only until its fade has closed")
{
  const LocalPlan plan = plan_for(series({gain("a", 0.8), gain("b", 0.5)}), series({gain("a", 0.8)}));
  CHECK(plan.fades == 1);
  CHECK(plan.created == 0);
  CHECK(plan.retiring == 1);
  // The retiring node is renamed, and remembers the instance it runs.
  CHECK_FALSE(has_node(plan.transitional, "b"));
  CHECK(plan.carried_from.at("~old:b") == "b");
  const std::string fade = driver_of(plan.transitional, "out", 0);
  CHECK(driver_of(plan.transitional, fade, 0) == "~old:b");
  CHECK(driver_of(plan.transitional, fade, 1) == "a");
  CHECK(driver_of(plan.transitional, "~old:b", 0) == "a");
  CHECK_FALSE(has_node(plan.final_graph, "~old:b"));
}

TEST_CASE("a setting that cannot ramp rebuilds that block alone, beside the old one")
{
  NodeSpec slow = gain("a", 0.8);
  slow.params["ramp_ms"] = 50.0; // not rampable: it is the ramp
  NodeSpec fast = gain("a", 0.8);
  fast.params["ramp_ms"] = 5.0;
  const LocalPlan plan = plan_for(series({slow, delay("d", 100)}), series({fast, delay("d", 100)}));
  CHECK(plan.created == 1);
  CHECK(plan.retiring == 1);
  CHECK(plan.fades == 1);
  // The delay after it is kept, and its input is the crossfade between the two versions of `a`.
  CHECK(plan.carried_from.at("d") == "d");
  const std::string fade = driver_of(plan.transitional, "d", 0);
  CHECK(driver_of(plan.transitional, fade, 0) == "~old:a");
  CHECK(driver_of(plan.transitional, fade, 1) == "a");
}

TEST_CASE("values that ramp are ramped, with no new plan")
{
  NodeSpec moved = gain("a", 0.25);
  const LocalPlan plan = plan_for(series({gain("a", 0.8), delay("d", 100)}), series({moved, delay("d", 120)}));
  CHECK(plan.ramps_only);
  REQUIRE(plan.ramps.size() == 2);
  CHECK(plan.ramps[0].node == "a");
  CHECK(plan.ramps[0].param == "gain");
  CHECK(plan.ramps[0].value == doctest::Approx(0.25));
  CHECK(plan.ramps[1].node == "d");
  CHECK(plan.ramps[1].param == "samples");
}

TEST_CASE("a bypass alone is a ramp: no plan, the running one crossfades the block")
{
  NodeSpec bypassed = biquad("eq", "peaking", 1000.0, 6.0);
  bypassed.bypass = true;
  const LocalPlan plan = plan_for(series({biquad("eq", "peaking", 1000.0, 6.0)}), series({bypassed}));
  CHECK(plan.ramps_only);
  CHECK(plan.transitional.nodes.empty());
  REQUIRE(plan.bypasses.size() == 1);
  CHECK(plan.bypasses[0].node == "eq");
  CHECK(plan.bypasses[0].bypass);

  SUBCASE("with a value that ramps, both go to the running plan")
  {
    NodeSpec moved = gain("g", 0.25);
    moved.bypass = true;
    const LocalPlan both = plan_for(series({gain("g", 0.5)}), series({moved}));
    CHECK(both.ramps_only);
    CHECK(both.ramps.size() == 1);
    CHECK(both.bypasses.size() == 1);
  }
}

TEST_CASE("a bypass beside a structural edit keeps the block and crossfades between its output and its input")
{
  NodeSpec bypassed = biquad("eq", "peaking", 1000.0, 6.0);
  bypassed.bypass = true;
  const LocalPlan plan =
    plan_for(series({biquad("eq", "peaking", 1000.0, 6.0)}), series({bypassed, gain("g", 0.5)}));
  CHECK_FALSE(plan.ramps_only);
  CHECK(plan.bypasses.empty());
  CHECK(plan.created == 1);
  CHECK(plan.retiring == 0);
  // It renders wet throughout the transition; its crossfade does the switching.
  CHECK_FALSE(find(plan.transitional, "eq")->bypass);
  CHECK(find(plan.final_graph, "eq")->bypass);
  REQUIRE(find(plan.transitional, "~bypass:eq") != nullptr);
  CHECK(driver_of(plan.transitional, "~bypass:eq", 0) == "eq"); // was wet
  CHECK(driver_of(plan.transitional, "~bypass:eq", 1) == "in"); // becomes dry
}

TEST_CASE("an element renamed and nothing else is an instant switch")
{
  NodeSpec renamed = gain("a", 0.8);
  renamed.element = "block-7";
  const LocalPlan plan = plan_for(series({gain("a", 0.8), delay("d", 10)}), series({renamed, delay("d", 10)}));
  CHECK(plan.instant);
  CHECK(plan.fades == 0);
  CHECK(plan.transitional.nodes.empty());
}

TEST_CASE("two blocks that trade places: one is rebuilt so the transition has no cycle")
{
  const LocalPlan plan =
    plan_for(series({delay("p", 10), delay("q", 20), gain("g", 0.5)}), series({delay("q", 20), delay("p", 10), gain("g", 0.5)}));
  // Kept as they are, each one's old signal would be the other's output. One is rebuilt instead.
  CHECK(plan.created == 1);
  CHECK(plan.retiring == 1);
  CHECK(plan.carried_from.at("g") == "g");
  namchain::rt::Arena arena(1 << 20);
  compile(plan.transitional, arena); // schedulable: it compiles
}

TEST_CASE("a lane added to a parallel section keeps the other lanes")
{
  const auto section = [](int lanes) {
    Graph graph;
    NodeSpec split = node("split", "nc.split");
    split.params["outputs"] = lanes;
    NodeSpec mix = node("mix", "nc.mix");
    mix.params["inputs"] = lanes;
    graph.nodes = {node("in", "nc.input"), split, mix, delay("after", 10), node("out", "nc.output")};
    graph.connections = {{"in", 0, "split", 0}, {"mix", 0, "after", 0}, {"after", 0, "out", 0}};
    for (int lane = 0; lane < lanes; ++lane)
    {
      const std::string id = "lane" + std::to_string(lane);
      graph.nodes.push_back(delay(id, 10 + lane));
      graph.connections.push_back({"split", lane, id, 0});
      graph.connections.push_back({id, 0, "mix", lane});
    }
    return graph;
  };
  const LocalPlan plan = plan_for(section(2), section(3));
  // The split and the mixer change shape, so they are rebuilt; the lanes and what follows are kept.
  CHECK(plan.carried_from.at("lane0") == "lane0");
  CHECK(plan.carried_from.at("lane1") == "lane1");
  CHECK(plan.carried_from.at("after") == "after");
  CHECK(plan.created == 3);  // split, mix, the new lane
  CHECK(plan.retiring == 2); // the old split and mixer
  CHECK(plan.fades == 3);    // each kept lane's input, and the input of what follows the mixer
  namchain::rt::Arena arena(1 << 20);
  compile(plan.transitional, arena);
}

TEST_CASE("a feedback delay is kept across an edit next to it, buffer and all")
{
  NodeSpec echo = node("echo", "fx.delay.feedback");
  echo.element = "echo";
  echo.params = {{"time", 2.0}, {"feedback", 0.5}, {"mix", 1.0}};
  const LocalPlan plan = plan_for(series({echo}), series({echo, gain("g", 0.5)}));
  CHECK(plan.retiring == 0);
  CHECK(plan.created == 1);
  // The loop's block-delay node keeps its state in its plan's buffer, so that buffer is carried.
  bool tap_carried = false;
  for (const auto& [from, to] : plan.buffers_from_running)
    tap_carried = tap_carried || (from == "echo:tap" && to == "echo:tap");
  CHECK(tap_carried);
}

TEST_CASE("a local transition is refused where it cannot keep anything worth keeping")
{
  std::string reason;
  LocalPlan plan;

  SUBCASE("a different input node")
  {
    Graph after = series({gain("a", 0.8)});
    after.nodes[0].id = "in2";
    after.connections[0].from = "in2";
    CHECK_FALSE(namchain::graph::plan_local_transition(series({gain("a", 0.8)}), after, {}, kFade, plan, reason));
    CHECK(reason.find("input") != std::string::npos);
  }
  SUBCASE("everything with state replaced")
  {
    CHECK_FALSE(namchain::graph::plan_local_transition(series({gain("a", 0.8)}), series({gain("b", 0.8)}), {},
                                                       kFade, plan, reason));
  }
  SUBCASE("a failed node is rebuilt rather than kept")
  {
    plan = plan_for(series({gain("a", 0.8), delay("d", 10)}), series({gain("a", 0.8), delay("d", 10), gain("b", 1.0)}),
                    {"a"});
    CHECK(plan.carried_from.count("a") == 0);
    CHECK(plan.carried_from.at("d") == "d");
  }
}

TEST_CASE("an engine-internal type cannot be named by a document")
{
  namchain::graph::Document document;
  namchain::graph::Chain chain;
  chain.id = "main";
  NodeSpec fade = node("f", "nc.xfade");
  fade.params["frames"] = 240.0;
  chain.graph = series({fade});
  document.chains.push_back(chain);
  const namchain::graph::Validation validation = namchain::graph::validate(document);
  CHECK_FALSE(validation.ok);
  CHECK(validation.message.find("only the engine") != std::string::npos);
}

// ---------------------------------------------------------------------------
// What the chain plays: exact
// ---------------------------------------------------------------------------

TEST_CASE("adding a block plays the old chain, then the crossfade, then the new chain, exactly")
{
  const std::vector<float> input = tone(kFrames);
  const Graph before = series({gain("a", 1.0)});
  const Graph after = series({gain("a", 1.0), gain("b", 0.5)});
  const Run run = render_local(before, after, input);

  std::vector<float> half(input.size());
  for (std::size_t i = 0; i < input.size(); ++i)
    half[i] = input[i] * 0.5f;
  const std::vector<float> expected = expected_fade(input, half, kEditAt);
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    if (run.output[i] != expected[i])
    {
      FAIL("sample " << i << ": " << run.output[i] << ", expected " << expected[i]);
      break;
    }
  }
}

/// The bypass crossfade RenderPlan::set_bypass plays, from \p from to \p to over kFade frames from
/// \p start: the same equal-power law as rt::Crossfade, computed rather than read from a table.
std::vector<float> expected_bypass(const std::vector<float>& from, const std::vector<float>& to, std::size_t start)
{
  std::vector<float> out(from.begin(), from.end());
  for (std::size_t i = start; i < out.size(); ++i)
  {
    const std::size_t position = i - start;
    if (position >= static_cast<std::size_t>(kFade))
    {
      out[i] = to[i];
      continue;
    }
    const float t = static_cast<float>(position) / static_cast<float>(kFade - 1);
    out[i] = from[i] * std::cos(1.57079632679489661923f * t) + to[i] * std::sin(1.57079632679489661923f * t);
  }
  return out;
}

TEST_CASE("a bypass crossfades from the block's output to its input, and back, exactly")
{
  const std::vector<float> input = tone(kFrames);
  std::vector<float> wet(input.size());
  for (std::size_t i = 0; i < input.size(); ++i)
    wet[i] = input[i] * 0.5f;
  NodeSpec bypassed = gain("a", 0.5);
  bypassed.bypass = true;

  const auto check = [](const std::vector<float>& output, const std::vector<float>& expected) {
    for (std::size_t i = 0; i < output.size(); ++i)
    {
      if (std::fabs(output[i] - expected[i]) > 1e-6f)
      {
        FAIL("sample " << i << ": " << output[i] << ", expected " << expected[i]);
        break;
      }
    }
  };
  SUBCASE("bypassed")
  {
    const Run run = render_local(series({gain("a", 0.5)}), series({bypassed}), input);
    CHECK(run.plan.ramps_only);
    check(run.output, expected_bypass(wet, input, kEditAt));
  }
  SUBCASE("brought back")
  {
    const Run run = render_local(series({bypassed}), series({gain("a", 0.5)}), input);
    CHECK(run.plan.ramps_only);
    check(run.output, expected_bypass(input, wet, kEditAt));
  }
}

TEST_CASE("a bypass turned around mid-fade carries on from where it is")
{
  namchain::rt::Arena arena(1 << 20);
  std::unique_ptr<RenderPlan> plan = compile(lowered(series({gain("a", 0.5)})), arena);
  const std::size_t a = static_cast<std::size_t>(plan->index_of("a"));
  REQUIRE(plan->node_bypassable(a));
  const std::vector<float> ones(kFade * 2, 1.0f);
  std::vector<float> out(ones.size(), 0.0f);

  // A quarter of the way to bypassed, then back: the next sample continues from the same weights.
  REQUIRE(plan->set_bypass(a, true, kFade));
  const int quarter = kFade / 4;
  plan->execute(ones.data(), out.data(), quarter);
  const float last = out[static_cast<std::size_t>(quarter - 1)];
  REQUIRE(plan->set_bypass(a, false, kFade));
  for (int at = quarter; at < kFade * 2; at += kQuantum)
    plan->execute(ones.data() + at, out.data() + at, std::min(kQuantum, kFade * 2 - at));
  CHECK(std::fabs(out[static_cast<std::size_t>(quarter)] - last) < 0.01f);
  // ... and lands back on the block's own output, having taken as long as it had gone.
  CHECK(out[static_cast<std::size_t>(2 * quarter + 1)] == doctest::Approx(0.5f));
  CHECK(out.back() == doctest::Approx(0.5f));
  // A node with no dry path is refused.
  CHECK_FALSE(plan->set_bypass(plan->node_ids().size(), true, kFade));
}

TEST_CASE("a kept delay keeps playing what it heard before the edit")
{
  // A gain inserted in front of a 100-sample delay. Rebuilt, the delay would start empty and play 100
  // samples of silence; kept, it plays the 100 samples it already holds, then the faded input.
  const std::vector<float> input = noise(kFrames);
  const Run run = render_local(series({delay("d", 100)}), series({gain("g", 0.5), delay("d", 100)}), input);
  CHECK(run.plan.carried_from.at("d") == "d");

  for (std::size_t i = kEditAt; i < kEditAt + 100; ++i)
  {
    if (run.output[i] != input[i - 100])
    {
      FAIL("sample " << i << " is not the delay's history: " << run.output[i] << " vs " << input[i - 100]);
      break;
    }
  }
  // And after it, the crossfade from the input to the half-gain input, delayed.
  std::vector<float> half(input.size());
  for (std::size_t i = 0; i < input.size(); ++i)
    half[i] = input[i] * 0.5f;
  const std::vector<float> faded = expected_fade(input, half, kEditAt);
  for (std::size_t i = kEditAt + 100; i < input.size(); ++i)
  {
    if (run.output[i] != faded[i - 100])
    {
      FAIL("sample " << i << ": " << run.output[i] << ", expected " << faded[i - 100]);
      break;
    }
  }
}

TEST_CASE("a feedback loop carried across an edit continues sample for sample")
{
  // The echo's own output must be exactly what it would have been with no edit at all: the edit is
  // after it. Its loop runs through a block-delay node whose state is a buffer in the plan, which is
  // what the copy at the switch carries.
  const std::vector<float> input = noise(kFrames);
  NodeSpec echo = node("echo", "fx.delay.feedback");
  echo.element = "echo";
  echo.params = {{"time", 3.0}, {"feedback", 0.6}, {"mix", 1.0}};
  const Graph before = series({echo});
  const Graph after = series({echo, gain("g", 0.5)});

  const std::vector<float> untouched = render(lowered(before), input);
  std::vector<float> half(untouched.size());
  for (std::size_t i = 0; i < untouched.size(); ++i)
    half[i] = untouched[i] * 0.5f;
  const std::vector<float> expected = expected_fade(untouched, half, kEditAt);

  const Run run = render_local(before, after, input);
  bool exact = true;
  for (std::size_t i = 0; i < input.size() && exact; ++i)
    exact = run.output[i] == expected[i];
  CHECK_MESSAGE(exact, "the echo did not continue exactly through the transition");

  // The negative control: without the buffer copy the loop loses a block of its feedback, and the
  // echoes that follow are wrong.
  const Run broken = render_local(before, after, input, /*copy_carried=*/false);
  bool differs = false;
  for (std::size_t i = kEditAt; i < input.size() && !differs; ++i)
    differs = broken.output[i] != expected[i];
  CHECK(differs);
}

TEST_CASE("two blocks trading places still lands on the new chain")
{
  const std::vector<float> input = noise(kFrames);
  const Graph before = series({delay("p", 10), delay("q", 20), gain("g", 0.5)});
  const Graph after = series({delay("q", 20), delay("p", 10), gain("g", 0.5)});
  const Run run = render_local(before, after, input);
  const std::vector<float> settled = render(after, input);
  // Well after the edit (both delays flushed), the chain is the new one exactly.
  for (std::size_t i = kEditAt + 1024; i < input.size(); ++i)
  {
    if (run.output[i] != settled[i])
    {
      FAIL("sample " << i << ": " << run.output[i] << ", expected " << settled[i]);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// What the chain plays: judged
// ---------------------------------------------------------------------------

TEST_CASE("local transitions pass the glitch detector")
{
  const std::vector<float> input = tone(kFrames);
  const auto check = [&input](const Graph& before, const Graph& after) {
    const Run run = render_local(before, after, input);
    const namchain::glitch::Report report = judge(run.output, render(before, input), render(after, input));
    CHECK(report.ok);
    CHECK(report.continuity_evaluated);
  };

  SUBCASE("a block added")
  {
    check(series({biquad("lo", "lowshelf", 120.0, 3.0)}),
          series({biquad("lo", "lowshelf", 120.0, 3.0), biquad("hi", "highshelf", 3500.0, -6.0)}));
  }
  SUBCASE("a block removed")
  {
    check(series({biquad("lo", "lowshelf", 120.0, 3.0), biquad("hi", "highshelf", 3500.0, -6.0)}),
          series({biquad("lo", "lowshelf", 120.0, 3.0)}));
  }
  SUBCASE("a block rebuilt in place")
  {
    check(series({biquad("lo", "lowshelf", 120.0, 3.0), biquad("x", "peaking", 800.0, 6.0)}),
          series({biquad("lo", "lowshelf", 120.0, 3.0), biquad("x", "highpass", 200.0, 0.0)}));
  }
  SUBCASE("a block bypassed")
  {
    NodeSpec bypassed = biquad("x", "peaking", 1000.0, 9.0);
    bypassed.bypass = true;
    check(series({biquad("lo", "lowshelf", 120.0, 3.0), biquad("x", "peaking", 1000.0, 9.0)}),
          series({biquad("lo", "lowshelf", 120.0, 3.0), bypassed}));
  }
}
