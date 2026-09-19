// rt::Crossfade and the structural transition it serves.
//
// The primitive's own properties are checked directly (equal power, endpoints, a fade that closes
// mid-block). Then the transition is judged the same way the daemon's is: two compiled plans run
// together for the fade, their outputs are crossfaded, and the glitch detector says whether the
// result is continuous and converges to the right chain on either side. The hard-switch case is the
// negative control -- without it, a detector that passes everything would look like success.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "namchain/glitch.h"
#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"
#include "namchain/rt/crossfade.h"
#include "namchain/rt_check.h"

namespace
{

using namchain::graph::Graph;
using namchain::graph::NodeSpec;

constexpr int kRate = 48000;
constexpr int kQuantum = 64;
constexpr std::size_t kFrames = 4096;
constexpr std::size_t kEditAt = 2048;
constexpr int kFade = 240; // 5 ms at 48 kHz

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

/// One gain, or two in series: the transition tests want two plans that differ structurally, not
/// just in a parameter, so the second has a node the first does not.
Graph chain(double gain, bool two_gains)
{
  Graph graph;
  NodeSpec gn = node("g", "nc.gain");
  gn.params["gain"] = gain;
  graph.nodes = {node("in", "nc.input"), gn};
  if (two_gains)
  {
    NodeSpec g2 = node("g2", "nc.gain");
    g2.params["gain"] = 1.0;
    graph.nodes.push_back(g2);
  }
  graph.nodes.push_back(node("out", "nc.output"));

  std::string previous = "g";
  if (two_gains)
  {
    graph.connections.push_back({"g", 0, "g2", 0});
    previous = "g2";
  }
  graph.connections.push_back({"in", 0, "g", 0});
  graph.connections.push_back({previous, 0, "out", 0});
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

std::vector<float> render(const Graph& graph, const std::vector<float>& input)
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(graph, arena);
  plan->prewarm();
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t at = 0; at < input.size(); at += kQuantum)
  {
    const int frames = static_cast<int>(std::min<std::size_t>(kQuantum, input.size() - at));
    plan->execute(input.data() + at, output.data() + at, frames);
  }
  return output;
}

/// Run plan A until \p kEditAt, then either crossfade into plan B over \p kFade frames or switch to
/// it in one sample. This is the daemon's transition, reduced to the two things it does.
std::vector<float> render_transition(const Graph& a_graph, const Graph& b_graph, const std::vector<float>& input,
                                     bool crossfade)
{
  namchain::rt::Arena arena_a(1 << 16);
  namchain::rt::Arena arena_b(1 << 16);
  auto a = compile(a_graph, arena_a);
  auto b = compile(b_graph, arena_b);
  a->prewarm();
  b->prewarm();

  namchain::rt::Crossfade fade(kFade);
  std::vector<float> output(input.size(), 0.0f);
  std::vector<float> block_a(kQuantum, 0.0f);
  std::vector<float> block_b(kQuantum, 0.0f);
  bool switched = false;

  for (std::size_t at = 0; at < input.size(); at += kQuantum)
  {
    const int frames = static_cast<int>(std::min<std::size_t>(kQuantum, input.size() - at));
    a->execute(input.data() + at, block_a.data(), frames);
    b->execute(input.data() + at, block_b.data(), frames);

    if (!switched && at == kEditAt)
    {
      switched = true;
      if (crossfade)
        fade.start(kFade);
    }

    if (!switched)
      std::copy(block_a.begin(), block_a.begin() + frames, output.begin() + static_cast<std::ptrdiff_t>(at));
    else if (crossfade && fade.active())
      fade.mix(block_a.data(), block_b.data(), output.data() + at, frames);
    else
      std::copy(block_b.begin(), block_b.begin() + frames, output.begin() + static_cast<std::ptrdiff_t>(at));
  }
  return output;
}

namchain::glitch::Report analyse(const std::vector<float>& y, const std::vector<float>& before,
                                 const std::vector<float>& after)
{
  namchain::glitch::Options options;
  options.sample_rate = static_cast<double>(kRate);
  const std::vector<namchain::glitch::Edit> edits = {{kEditAt, kEditAt + kFade}};
  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), before.data(), after.data(), y.size(), edits, options, error);
  REQUIRE_MESSAGE(error.empty(), error);
  return report;
}

} // namespace

TEST_CASE("the crossfade weights are equal-power and reach both endpoints")
{
  namchain::rt::Crossfade fade(1024);
  CHECK(fade.length() == 0);
  CHECK_FALSE(fade.active());

  const int length = 240;
  std::vector<float> ones(length, 1.0f);
  std::vector<float> zeros(length, 0.0f);
  std::vector<float> out(length, 0.0f);

  fade.start(length);
  REQUIRE(fade.active());
  CHECK(fade.length() == length);
  fade.mix(ones.data(), zeros.data(), out.data(), length);
  const std::vector<float> from = out;

  fade.start(length);
  fade.mix(zeros.data(), ones.data(), out.data(), length);
  const std::vector<float> to = out;

  CHECK(from.front() == doctest::Approx(1.0f));
  CHECK(from.back() == doctest::Approx(0.0f));
  CHECK(to.front() == doctest::Approx(0.0f));
  CHECK(to.back() == doctest::Approx(1.0f));
  for (int i = 0; i < length; ++i)
  {
    // The property that makes it equal-power rather than a 3 dB dip in the middle.
    CHECK(from[static_cast<std::size_t>(i)] * from[static_cast<std::size_t>(i)] +
            to[static_cast<std::size_t>(i)] * to[static_cast<std::size_t>(i)] ==
          doctest::Approx(1.0).epsilon(1e-4));
    CHECK(to[static_cast<std::size_t>(i)] >= 0.0f);
    CHECK(to[static_cast<std::size_t>(i)] <= 1.0f);
  }
  CHECK_FALSE(fade.active());
}

TEST_CASE("a fade that closes mid-block leaves the new signal for the rest of the block")
{
  namchain::rt::Crossfade fade(1024);
  std::vector<float> a(64, 1.0f);
  std::vector<float> b(64, 0.5f);
  std::vector<float> out(64, -1.0f);

  fade.start(10);
  fade.mix(a.data(), b.data(), out.data(), 64);
  CHECK_FALSE(fade.active());
  for (int i = 10; i < 64; ++i)
    CHECK(out[static_cast<std::size_t>(i)] == doctest::Approx(0.5f));
}

TEST_CASE("the crossfade render path allocates nothing")
{
  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped allocation check: interposer not compiled in");
    return;
  }
  namchain::rt::Crossfade fade(4096);
  std::vector<float> a(64, 1.0f);
  std::vector<float> b(64, 0.5f);
  std::vector<float> out(64, 0.0f);

  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int i = 0; i < 100; ++i)
    {
      fade.start(240);
      fade.mix(a.data(), b.data(), out.data(), 64);
    }
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
}

TEST_CASE("a structural change crossfaded between two plans passes the detector")
{
  const std::vector<float> input = tone(kFrames);
  const Graph before_graph = chain(1.0, false);
  const Graph after_graph = chain(0.5, true);

  const std::vector<float> y = render_transition(before_graph, after_graph, input, true);
  const std::vector<float> before = render(before_graph, input);
  const std::vector<float> after = render(after_graph, input);

  const namchain::glitch::Report report = analyse(y, before, after);
  if (!report.ok)
    for (const namchain::glitch::Finding& finding : report.findings)
      MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  CHECK(report.ok);
  CHECK(report.continuity_evaluated);
  // The transition did something: somewhere in the fade window y is neither chain.
  float worst = 0.0f;
  for (std::size_t i = kEditAt; i < kEditAt + kFade; ++i)
    worst = std::max(worst, std::abs(y[i] - before[i]));
  CHECK(worst > 0.01f);
  CHECK(std::abs(y.back() - after.back()) < 1e-6f);
}

TEST_CASE("an uncrossfaded structural switch fails the same detector")
{
  const std::vector<float> input = tone(kFrames);
  const Graph before_graph = chain(1.0, false);
  const Graph after_graph = chain(0.5, true);

  const std::vector<float> y = render_transition(before_graph, after_graph, input, false);
  const std::vector<float> before = render(before_graph, input);
  const std::vector<float> after = render(after_graph, input);

  const namchain::glitch::Report report = analyse(y, before, after);
  CHECK_FALSE(report.ok);
  CHECK(report.max_edit_d2 > report.reference_d2);
}
