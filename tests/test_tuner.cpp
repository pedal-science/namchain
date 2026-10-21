// Tests for nc.tuner: the Squatch Tuner in the engine, and what it does to a chain.
//
// The claims, in the order they matter:
//
//   * the node **mutes**: its chain's output is exactly zero, whatever came in -- that is the whole
//     of its audio behaviour, and a tuner that leaks a little signal is a tuner nobody can use;
//   * it **reads**: a steady tone comes out as the note it is, to within a couple of cents, which is
//     the vendored core running for real rather than a table of numbers being echoed back;
//   * it is **honest about silence**: no note means no note, not the last one held on screen;
//   * the reading **advances**, so a client can tell a live tuner from a frozen one;
//   * none of it allocates on the render path.
//
// The core is third-party code (third_party/squatch-tuner), so these are not tests *of* the tuner's
// accuracy: they are the tests of the node contract around it. The core's own numbers are in
// squatch-dsp's blocks/tuner/RESULTS.md.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <json.hpp>

#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"
#include "namchain/rt_check.h"

namespace
{

using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::graph::RenderPlan;
using namchain::render::CompileOptions;

constexpr double kRate = 48000.0;
constexpr int kQuantum = 64;
constexpr double kPi = 3.14159265358979323846;

Graph one_node()
{
  Graph graph;
  graph.nodes = {NodeSpec{"in", "nc.input"}, NodeSpec{"n", "nc.tuner"}, NodeSpec{"out", "nc.output"}};
  graph.connections = {{"in", 0, "n", 0}, {"n", 0, "out", 0}};
  return graph;
}

std::unique_ptr<RenderPlan> compile_plan(const Graph& graph, namchain::rt::Arena& arena, int quantum = kQuantum,
                                         double rate = kRate)
{
  CompileOptions options;
  options.max_quantum = quantum;
  options.sample_rate = rate;
  std::string error;
  auto plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

std::vector<float> sine(double frequency, std::size_t length, float amplitude = 0.25f, double rate = kRate)
{
  std::vector<float> samples(length);
  for (std::size_t i = 0; i < length; ++i)
    samples[i] = amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency * static_cast<double>(i) / rate));
  return samples;
}

/// Render `input` through the node, `quantum` frames at a time, and return what the chain output.
std::vector<float> render(const std::vector<float>& input, int quantum, double rate = kRate)
{
  namchain::rt::Arena arena(1u << 22);
  auto plan = compile_plan(one_node(), arena, quantum, rate);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += static_cast<std::size_t>(quantum))
  {
    const int frames =
      static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(quantum), input.size() - offset));
    plan->execute(input.data() + offset, output.data() + offset, frames);
  }
  return output;
}

/// The node's `rig.state` object, after `input` has been rendered through it.
nlohmann::json report_after(const std::vector<float>& input, int quantum = kQuantum, double rate = kRate)
{
  namchain::rt::Arena arena(1u << 22);
  auto plan = compile_plan(one_node(), arena, quantum, rate);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += static_cast<std::size_t>(quantum))
  {
    const int frames =
      static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(quantum), input.size() - offset));
    plan->execute(input.data() + offset, output.data() + offset, frames);
  }
  const int index = plan->index_of("n");
  REQUIRE(index >= 0);
  const namchain::render::NodeType* type = plan->nodes()[static_cast<std::size_t>(index)].type;
  REQUIRE(type != nullptr);
  REQUIRE(type->describe_state != nullptr);
  const std::string text = type->describe_state(plan->nodes()[static_cast<std::size_t>(index)].state);
  REQUIRE(!text.empty());
  return nlohmann::json::parse(text);
}

bool all_zero(const std::vector<float>& samples)
{
  return std::all_of(samples.begin(), samples.end(), [](float value) { return value == 0.0f; });
}

} // namespace

TEST_CASE("nc.tuner mutes whatever it is given")
{
  // The node's audio behaviour in one line: silence, bit-exact, so nothing downstream of it is heard.
  // The zero has to be *exactly* zero -- a denormal or a leaked sample is heard when the rest of the
  // chain has gain.
  const std::vector<float> tone = sine(440.0, static_cast<std::size_t>(kRate), 0.5f);
  SUBCASE("a loud tone")
  {
    CHECK(all_zero(render(tone, kQuantum)));
  }
  SUBCASE("every block size the plan can be built for")
  {
    for (const int quantum : {1, 3, 64, 100, 512})
      CHECK_MESSAGE(all_zero(render(tone, quantum)), "quantum " << quantum);
  }
  SUBCASE("44.1 kHz as well as 48")
  {
    const std::vector<float> other = sine(440.0, 44100, 0.5f, 44100.0);
    CHECK(all_zero(render(other, kQuantum, 44100.0)));
  }
  SUBCASE("silence in")
  {
    CHECK(all_zero(render(std::vector<float>(4096, 0.0f), kQuantum)));
  }
}

TEST_CASE("nc.tuner reads a steady tone as its note")
{
  // Two seconds of A4: long enough for the coarse stage to find the note, the stiffness estimate to
  // settle and the phase fit to be confirmed. The core holds a steady tone from power-on (RESULTS.md,
  // "A steady test tone present from power-on still reads"), which is what makes this a fair test
  // rather than a test of a pluck modelled badly.
  const std::vector<float> tone = sine(440.0, static_cast<std::size_t>(2.0 * kRate), 0.2f);
  const nlohmann::json report = report_after(tone);

  CHECK(report["valid"] == true);
  CHECK(report["a4"] == doctest::Approx(440.0));
  CHECK(report["midi"] == 69);
  CHECK(report["note"] == "A");
  CHECK(report["octave"] == 4);
  CHECK(report["cents"] == doctest::Approx(0.0).epsilon(0.02));
  CHECK(report["hz"] == doctest::Approx(440.0).epsilon(0.0015));
  CHECK(report["steady_hz"] == doctest::Approx(440.0).epsilon(0.0015));
  CHECK(report["clarity"] > 0.8);
  CHECK(report["partials"] >= 1);
  // The level is the signal's RMS at the tuner's own work rate (-17 dBFS for a 0.2 sine), and it
  // reads 0.2 dB under the ideal from the decimation filter's passband.
  CHECK(report["level_db"] == doctest::Approx(-17.2).epsilon(0.01));
  CHECK(report["blocks"] > 0);
}

TEST_CASE("nc.tuner withholds a note it has not heard")
{
  // A stale note is worse than no note: a player looking at "A" while the string is dead would tune
  // nothing. The reading's note keys are absent, not last-known.
  const nlohmann::json report = report_after(std::vector<float>(static_cast<std::size_t>(kRate), 0.0f));

  CHECK(report["valid"] == false);
  CHECK_FALSE(report.contains("midi"));
  CHECK_FALSE(report.contains("note"));
  CHECK_FALSE(report.contains("octave"));
  CHECK_FALSE(report.contains("cents"));
  CHECK_FALSE(report.contains("hz"));
  // What is always there: the reference, how much it has heard, and how loud.
  CHECK(report["a4"] == doctest::Approx(440.0));
  CHECK(report["blocks"] > 0);
  CHECK(report["level_db"] <= -100.0);
}

TEST_CASE("nc.tuner's reading is the newest block's")
{
  // The counter is what lets a client tell a live reading from one the render thread stopped
  // updating. One block, whatever its length, is one count.
  const std::vector<float> tone = sine(440.0, static_cast<std::size_t>(kQuantum) * 10, 0.2f);
  const nlohmann::json ten = report_after(tone);
  CHECK(ten["blocks"] == 10);
  const nlohmann::json twenty = report_after(sine(440.0, static_cast<std::size_t>(kQuantum) * 20, 0.2f));
  CHECK(twenty["blocks"] == 20);
}

TEST_CASE("nc.tuner is a node a document may name, and not one a palette offers")
{
  const namchain::render::NodeType* type = namchain::render::find_node_type("nc.tuner");
  REQUIRE(type != nullptr);
  // Nameable: a client mounts it by naming it in a document, so it is not `internal`.
  CHECK_FALSE(type->internal);
  // Not a block: the schema says a palette should leave it out.
  CHECK_FALSE(type->in_palette);
  CHECK(type->describe_state != nullptr);
  CHECK(type->inputs == 1);
  CHECK(type->outputs == 1);
  // No parameters: A4 and the note range are the engine's, and a tuner's parameters would change a
  // reading rather than the audio (docs/tuner.md).
  CHECK(type->param_count == 0);
  // Every other built-in is a block a palette may offer.
  for (const namchain::render::NodeType* other : namchain::render::all_node_types())
    if (other->id != "nc.tuner")
      CHECK_MESSAGE(other->in_palette, other->id);
}

TEST_CASE("nc.tuner's render path does not touch the heap")
{
  // The interposer is the same one the rest of the suite uses; this node runs a third-party core
  // whose `process` is meant to be allocation-free, and this is where that claim is checked in
  // *this* build, with *this* compiler.
  const std::vector<float> tone = sine(440.0, 8192, 0.2f);
  namchain::rt::Arena arena(1u << 22);
  auto plan = compile_plan(one_node(), arena);
  std::vector<float> output(tone.size(), 0.0f);

  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped allocation check: interposer not compiled in");
    return;
  }
  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (std::size_t offset = 0; offset < tone.size(); offset += kQuantum)
      plan->execute(tone.data() + offset, output.data() + offset, kQuantum);
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
  CHECK(all_zero(output));
}
