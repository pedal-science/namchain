// The `smooth` op class of plan.md §5.3: a parameter change that never recompiles and never steps.
//
// The glitch detector (plan.md §10.3) is the judge, and the point of these tests is not that a ramp
// exists but that it is measurably continuous while an *unramped* change is measurably not. The
// negative control is the load-bearing half: a detector that passes everything proves nothing.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "namchain/glitch.h"
#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"
#include "namchain/rt_check.h"

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;

constexpr int kRate = 48000;
constexpr int kQuantum = 64;
constexpr std::size_t kFrames = 4096;
constexpr std::size_t kEditAt = 2048;

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

/// in -> gain -> out, with the two params the ramp tests vary.
Graph gain_graph(double gain, double ramp_ms)
{
  Graph graph;
  NodeSpec input = node("in", "nc.input");
  NodeSpec gn = node("g", "nc.gain");
  gn.params["gain"] = gain;
  gn.params["ramp_ms"] = ramp_ms;
  NodeSpec output = node("out", "nc.output");
  graph.nodes = {input, gn, output};
  graph.connections = {{"in", 0, "g", 0}, {"g", 0, "out", 0}};
  return graph;
}

std::vector<float> tone(std::size_t frames)
{
  std::vector<float> samples(frames);
  for (std::size_t i = 0; i < frames; ++i)
    samples[i] = 0.4f * std::sin(2.0f * 3.14159265358979f * 1000.0f * static_cast<float>(i) /
                                 static_cast<float>(kRate));
  return samples;
}

std::unique_ptr<namchain::graph::RenderPlan> compile(const Graph& graph, namchain::rt::Arena& arena)
{
  std::string error;
  auto plan = namchain::graph::RenderPlan::compile(graph, kQuantum, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

/// Render the whole span. When \p edit is true, one set_param to \p target is applied at \p kEditAt.
std::vector<float> render(const Graph& graph, const std::vector<float>& input, bool edit, float target)
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(graph, arena);
  plan->prewarm();

  std::size_t node_index = 0;
  std::uint32_t param_id = 0;
  REQUIRE(plan->resolve_param("g", "gain", node_index, param_id));

  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t at = 0; at < input.size(); at += kQuantum)
  {
    if (edit && at == kEditAt)
      REQUIRE(plan->set_param(node_index, param_id, target));
    const int frames = static_cast<int>(std::min<std::size_t>(kQuantum, input.size() - at));
    plan->execute(input.data() + at, output.data() + at, frames);
  }
  return output;
}

namchain::glitch::Report analyse(const std::vector<float>& y, const std::vector<float>& before,
                                 const std::vector<float>& after, std::size_t edit_end)
{
  namchain::glitch::Options options;
  options.sample_rate = static_cast<double>(kRate);
  const std::vector<namchain::glitch::Edit> edits = {{kEditAt, edit_end}};
  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), before.data(), after.data(), y.size(), edits, options, error);
  REQUIRE_MESSAGE(error.empty(), error);
  return report;
}

} // namespace

TEST_CASE("a ramped set_param passes the glitch detector")
{
  const std::vector<float> input = tone(kFrames);
  const Graph graph = gain_graph(1.0, 5.0); // 5 ms at 48 kHz = 240 samples

  const std::vector<float> y = render(graph, input, true, 0.5f);
  const std::vector<float> before = render(gain_graph(1.0, 0.0), input, false, 0.0f);
  const std::vector<float> after = render(gain_graph(0.5, 0.0), input, false, 0.0f);

  const namchain::glitch::Report report = analyse(y, before, after, kEditAt + 240);
  if (!report.ok)
    for (const namchain::glitch::Finding& finding : report.findings)
      MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  CHECK(report.ok);
  CHECK(report.continuity_evaluated);
  // The ramp actually changed the signal, or the pass would be vacuous.
  CHECK(report.max_edit_d2 > 0.0);
  CHECK(std::abs(y.back() - after.back()) < 1e-6f);
  CHECK(std::abs(y.front() - before.front()) < 1e-9f);
}

TEST_CASE("an unramped set_param fails the same detector")
{
  const std::vector<float> input = tone(kFrames);
  // ramp_ms = 0 is a legal instant change: the negative control, and the click the smooth class
  // exists to remove.
  const Graph graph = gain_graph(1.0, 0.0);

  const std::vector<float> y = render(graph, input, true, 0.5f);
  const std::vector<float> before = render(gain_graph(1.0, 0.0), input, false, 0.0f);
  const std::vector<float> after = render(gain_graph(0.5, 0.0), input, false, 0.0f);

  const namchain::glitch::Report report = analyse(y, before, after, kEditAt + 1);
  CHECK_FALSE(report.ok);
  CHECK(report.max_edit_d2 > report.reference_d2);
}

TEST_CASE("resolve_param names what is settable and refuses what is not")
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(gain_graph(1.0, 5.0), arena);

  std::size_t index = 0;
  std::uint32_t id = 0;
  CHECK(plan->resolve_param("g", "gain", index, id));
  CHECK(id != 0);

  // `gain_db` is the canonical rampable param and `gain` its hidden linear alias: both dispatch, and
  // a name the type does not declare still does not.
  CHECK(plan->resolve_param("g", "gain_db", index, id));
  CHECK_FALSE(plan->resolve_param("g", "ramp_ms", index, id)); // build-only: changing it rebuilds
  CHECK_FALSE(plan->resolve_param("g", "gainz", index, id));
  CHECK_FALSE(plan->resolve_param("nope", "gain", index, id));
  CHECK_FALSE(plan->resolve_param("in", "gain", index, id)); // nc.input has no params

  // The render-path half is a dispatch through the vtable and allocates nothing.
  if (namchain::rt_check_available())
  {
    namchain::rt_reset_counters();
    {
      namchain::RtScope scope;
      for (int i = 0; i < 100; ++i)
        CHECK(plan->set_param(index, id, 1.0f));
    }
    CHECK(namchain::rt_allocation_count() == 0);
    CHECK(namchain::rt_deallocation_count() == 0);
  }
  CHECK_FALSE(plan->set_param(index, 99u, 1.0f));
  CHECK_FALSE(plan->set_param(9999u, id, 1.0f));
}

// ---------------------------------------------------------------------------
// The parameters that cannot be interpolated (docs/ui-integration-plan.md §1.4, slice B3)
// ---------------------------------------------------------------------------
//
// A gain is a value and can be ramped sample by sample. A filter's *coefficients* are not values --
// interpolating them is not guaranteed stable and is not the response anyone asked for at the
// midpoint -- and a delay's read position cannot be interpolated either, because moving it smoothly
// is a varispeed slide rather than a new delay time. Both nodes answer with the house trick: run the
// old and the new for the length of a ramp and crossfade. The judge is the same glitch detector, and
// the negative control is the same shape as the gain's: with `ramp_ms = 0` the change is a splice,
// and the detector has to say so.

namespace
{

/// in -> the node under test -> out.
Graph one_node_graph(NodeSpec middle)
{
  Graph graph;
  NodeSpec input = node("in", "nc.input");
  const std::string id = middle.id;
  NodeSpec output = node("out", "nc.output");
  graph.nodes = {input, std::move(middle), output};
  graph.connections = {{"in", 0, id, 0}, {id, 0, "out", 0}};
  return graph;
}

NodeSpec biquad(double freq, double gain_db, double ramp_ms)
{
  NodeSpec eq = node("n", "nc.eq.biquad");
  eq.strings["mode"] = "peaking";
  eq.params["freq"] = freq;
  eq.params["gain_db"] = gain_db;
  eq.params["ramp_ms"] = ramp_ms;
  return eq;
}

NodeSpec delay(double ms, double ramp_ms)
{
  NodeSpec line = node("n", "nc.delay");
  line.params["ms"] = ms;
  line.params["ramp_ms"] = ramp_ms;
  // The line has to be long enough to be *lengthened* into: the render path may not allocate, so the
  // rig declares the memory rather than the node growing a ring when the knob moves.
  line.params["max_ms"] = 20.0;
  return line;
}

/// Render the whole span. When \p edit is true, one set_param to \p target is applied at \p kEditAt.
std::vector<float> render_node(const Graph& graph, const std::vector<float>& input, const std::string& param,
                               bool edit, float target)
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(graph, arena);
  plan->prewarm();

  std::size_t node_index = 0;
  std::uint32_t param_id = 0;
  REQUIRE(plan->resolve_param("n", param, node_index, param_id));

  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t at = 0; at < input.size(); at += kQuantum)
  {
    if (edit && at == kEditAt)
      REQUIRE(plan->set_param(node_index, param_id, target));
    const int frames = static_cast<int>(std::min<std::size_t>(kQuantum, input.size() - at));
    plan->execute(input.data() + at, output.data() + at, frames);
  }
  return output;
}

void check_no_glitch(const std::vector<float>& y, const std::vector<float>& before,
                     const std::vector<float>& after, std::size_t edit_end)
{
  const namchain::glitch::Report report = analyse(y, before, after, edit_end);
  if (!report.ok)
    for (const namchain::glitch::Finding& finding : report.findings)
      MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  CHECK(report.ok);
  CHECK(report.continuity_evaluated);
  CHECK(report.max_edit_d2 > 0.0);
  CHECK(std::abs(y.back() - after.back()) < 1e-5f);
  CHECK(std::abs(y.front() - before.front()) < 1e-9f);
}

} // namespace

TEST_CASE("a ramped filter change passes the glitch detector and lands on the new response")
{
  const std::vector<float> input = tone(kFrames);
  const Graph graph = one_node_graph(biquad(1000.0, 0.0, 5.0));

  const std::vector<float> y = render_node(graph, input, "gain_db", true, -12.0f);
  const std::vector<float> before = render_node(one_node_graph(biquad(1000.0, 0.0, 0.0)), input, "gain_db", false, 0.0f);
  const std::vector<float> after = render_node(one_node_graph(biquad(1000.0, -12.0, 0.0)), input, "gain_db", false, 0.0f);

  // 16 blocks of two different sections is not something to leave to the ear.
  const namchain::glitch::Report report = analyse(y, before, after, kEditAt + 240);
  if (!report.ok)
    for (const namchain::glitch::Finding& finding : report.findings)
      MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  CHECK(report.ok);
  CHECK(report.continuity_evaluated);
  CHECK(report.max_edit_d2 > 0.0);
  // Settled on the new response, and untouched before the edit: the crossfade is a transition, not a
  // new sound of its own.
  CHECK(std::abs(y.back() - after.back()) < 1e-5f);
  CHECK(std::abs(y.front() - before.front()) < 1e-9f);
  // A 1 kHz peaking cut of 12 dB at 1 kHz is not a small change: the tail is quieter than the head.
  CHECK(std::abs(after.back()) < std::abs(before.back()) * 0.5f);
}

TEST_CASE("an unramped filter change is a splice, and the detector says so")
{
  const std::vector<float> input = tone(kFrames);
  const Graph graph = one_node_graph(biquad(1000.0, 0.0, 0.0)); // ramp_ms = 0

  const std::vector<float> y = render_node(graph, input, "gain_db", true, -12.0f);
  const std::vector<float> before = render_node(one_node_graph(biquad(1000.0, 0.0, 0.0)), input, "gain_db", false, 0.0f);
  const std::vector<float> after = render_node(one_node_graph(biquad(1000.0, -12.0, 0.0)), input, "gain_db", false, 0.0f);

  const namchain::glitch::Report report = analyse(y, before, after, kEditAt + 1);
  CHECK_FALSE(report.ok);
  CHECK(report.max_edit_d2 > report.reference_d2);
}

TEST_CASE("a ramped frequency change passes the glitch detector too")
{
  // Frequency is the parameter a player sweeps, so it is the one most likely to be moved often: the
  // crossfade has to survive a big move as well as a small one.
  const std::vector<float> input = tone(kFrames);
  const Graph graph = one_node_graph(biquad(400.0, 12.0, 5.0));

  const std::vector<float> y = render_node(graph, input, "freq", true, 4000.0f);
  const std::vector<float> before = render_node(one_node_graph(biquad(400.0, 12.0, 0.0)), input, "freq", false, 0.0f);
  const std::vector<float> after = render_node(one_node_graph(biquad(4000.0, 12.0, 0.0)), input, "freq", false, 0.0f);

  check_no_glitch(y, before, after, kEditAt + 240);
}

TEST_CASE("a ramped delay-time change passes the glitch detector")
{
  // 2 ms -> 5.5 ms on a 1 kHz tone: three and a half periods apart, so the two taps are genuinely
  // different signals rather than a periodic signal that happens to look the same.
  const std::vector<float> input = tone(kFrames);
  const Graph graph = one_node_graph(delay(2.0, 5.0));

  const std::vector<float> y = render_node(graph, input, "ms", true, 5.5f);
  const std::vector<float> before = render_node(one_node_graph(delay(2.0, 0.0)), input, "ms", false, 0.0f);
  const std::vector<float> after = render_node(one_node_graph(delay(5.5, 0.0)), input, "ms", false, 0.0f);

  check_no_glitch(y, before, after, kEditAt + 240);
}

TEST_CASE("an unramped delay-time change is a splice, and the detector says so")
{
  const std::vector<float> input = tone(kFrames);
  const Graph graph = one_node_graph(delay(2.0, 0.0));

  const std::vector<float> y = render_node(graph, input, "ms", true, 5.5f);
  const std::vector<float> before = render_node(one_node_graph(delay(2.0, 0.0)), input, "ms", false, 0.0f);
  const std::vector<float> after = render_node(one_node_graph(delay(5.5, 0.0)), input, "ms", false, 0.0f);

  const namchain::glitch::Report report = analyse(y, before, after, kEditAt + 1);
  CHECK_FALSE(report.ok);
  CHECK(report.max_edit_d2 > report.reference_d2);
}

TEST_CASE("a parameter change the node cannot honour is refused and changes nothing")
{
  // A `set_param` must not be able to build a degenerate filter or a zero-length delay, and a refused
  // edit has to leave the node exactly as it was rather than jumping somewhere else.
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(one_node_graph(biquad(1000.0, 0.0, 5.0)), arena);
  plan->prewarm();

  std::size_t index = 0;
  std::uint32_t id = 0;
  REQUIRE(plan->resolve_param("n", "freq", index, id));
  CHECK_FALSE(plan->set_param(index, id, 30000.0f)); // above Nyquist at 48 kHz
  CHECK_FALSE(plan->set_param(index, id, 0.0f));
  CHECK_FALSE(plan->set_param(index, id, -5.0f));
  CHECK(plan->set_param(index, id, 2000.0f));

  namchain::rt::Arena delay_arena(1 << 16);
  const Graph delay_graph = one_node_graph(delay(5.0, 5.0));
  auto delay_plan = compile(delay_graph, delay_arena);
  delay_plan->prewarm();
  REQUIRE(delay_plan->resolve_param("n", "ms", index, id));
  CHECK_FALSE(delay_plan->set_param(index, id, 0.0f));      // shorter than a sample
  CHECK_FALSE(delay_plan->set_param(index, id, 120000.0f)); // longer than the engine's minute
  CHECK_FALSE(delay_plan->set_param(index, id, 250.0f));    // longer than the line the rig declared
  CHECK(delay_plan->set_param(index, id, 18.0f));           // within it

  // A rig that wants to sweep the time declares the memory for it, and then the longer setting is
  // accepted: this is the difference the `max_ms` parameter exists to make explicit.
  NodeSpec roomy = delay(5.0, 5.0);
  roomy.params["max_ms"] = 500.0;
  namchain::rt::Arena roomy_arena(1 << 16);
  auto roomy_plan = compile(one_node_graph(std::move(roomy)), roomy_arena);
  roomy_plan->prewarm();
  REQUIRE(roomy_plan->resolve_param("n", "ms", index, id));
  CHECK(roomy_plan->set_param(index, id, 250.0f));
  CHECK_FALSE(roomy_plan->set_param(index, id, 600.0f));

  // `ramp_ms` is declared and not rampable: the dispatch must refuse it rather than silently ignore
  // it, because a client that sent it would otherwise believe the smoothing time had changed.
  CHECK_FALSE(delay_plan->resolve_param("n", "ramp_ms", index, id));
}
