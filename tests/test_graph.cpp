// Tests for the graph IR, compiler, memory plan and render execution.
//
// The compiler is where a rig becomes something the audio thread can run without allocating, so the
// tests are about two things: that bad graphs are rejected with a reason, and that a good graph
// compiles into something that produces the right samples without touching the allocator.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"
#include "namchain/rt_check.h"

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

Graph gain_chain(double first, double second)
{
  Graph graph;
  NodeSpec input = node("in", "nc.input");
  NodeSpec g1 = node("g1", "nc.gain");
  g1.params["gain"] = first;
  NodeSpec g2 = node("g2", "nc.gain");
  g2.params["gain"] = second;
  NodeSpec output = node("out", "nc.output");
  graph.nodes = {input, g1, g2, output};
  graph.connections = {{"in", 0, "g1", 0}, {"g1", 0, "g2", 0}, {"g2", 0, "out", 0}};
  return graph;
}

std::unique_ptr<namchain::graph::RenderPlan> compile(const Graph& graph, namchain::rt::Arena& arena,
                                                     int quantum = 64)
{
  std::string error;
  auto plan = namchain::graph::RenderPlan::compile(graph, quantum, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

} // namespace

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST_CASE("validation rejects malformed graphs with a reason")
{
  namchain::rt::Arena arena(1 << 16);

  SUBCASE("empty")
  {
    Graph empty;
    CHECK_FALSE(namchain::graph::validate(empty).ok);
  }

  SUBCASE("unknown node type")
  {
    Graph g;
    g.nodes = {node("in", "nc.input"), node("x", "nc.nope"), node("out", "nc.output")};
    const auto result = namchain::graph::validate(g);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("unknown node type") != std::string::npos);
  }

  SUBCASE("duplicate id")
  {
    Graph g;
    g.nodes = {node("in", "nc.input"), node("in", "nc.output")};
    const auto result = namchain::graph::validate(g);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("duplicate") != std::string::npos);
  }

  SUBCASE("missing output node")
  {
    Graph g;
    g.nodes = {node("in", "nc.input")};
    CHECK_FALSE(namchain::graph::validate(g).ok);
  }

  SUBCASE("connection to an unknown node")
  {
    Graph g = gain_chain(1.0, 1.0);
    g.connections.push_back({"g2", 0, "ghost", 0});
    CHECK_FALSE(namchain::graph::validate(g).ok);
  }

  SUBCASE("port out of range")
  {
    Graph g;
    g.nodes = {node("in", "nc.input"), node("out", "nc.output")};
    g.connections = {{"in", 3, "out", 0}}; // nc.input has a single output port
    const auto result = namchain::graph::validate(g);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("output port") != std::string::npos);
  }

  SUBCASE("cycle")
  {
    // Feedback must be expressed with an explicit delay node, so a plain cycle is a compile error.
    // Two mixers, so that the cycle does not also put two connections into one input port -- that
    // is a separate error, checked separately, and would otherwise mask this one.
    Graph g;
    g.nodes = {node("in", "nc.input"), node("a", "nc.mix"), node("b", "nc.mix"), node("out", "nc.output")};
    g.connections = {{"a", 0, "b", 0}, {"b", 0, "a", 0}, {"a", 0, "out", 0}};
    const auto result = namchain::graph::validate(g);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("cycle") != std::string::npos);
  }

  SUBCASE("two connections into one input port")
  {
    Graph g;
    g.nodes = {node("in", "nc.input"), node("a", "nc.gain"), node("b", "nc.gain"), node("out", "nc.output")};
    g.connections = {{"in", 0, "a", 0}, {"b", 0, "a", 0}, {"in", 0, "b", 0}, {"a", 0, "out", 0}};
    const auto result = namchain::graph::validate(g);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("already driven") != std::string::npos);
  }

  SUBCASE("valid graph passes")
  {
    CHECK(namchain::graph::validate(gain_chain(1.0, 1.0)).ok);
  }
}

// ---------------------------------------------------------------------------
// Compilation and the memory plan
// ---------------------------------------------------------------------------

TEST_CASE("compile orders nodes topologically regardless of declaration order")
{
  namchain::rt::Arena arena(1 << 16);

  // Declared output-first and back to front; execution must still be in dependency order.
  Graph g;
  g.nodes = {node("out", "nc.output"), node("g2", "nc.gain"), node("g1", "nc.gain"), node("in", "nc.input")};
  g.connections = {{"in", 0, "g1", 0}, {"g1", 0, "g2", 0}, {"g2", 0, "out", 0}};

  auto plan = compile(g, arena);
  REQUIRE(plan->nodes().size() == 4u);
  CHECK(plan->nodes()[0].type->id == "nc.input");
  CHECK(plan->nodes()[3].type->id == "nc.output");
  // Gains follow the input, in order.
  CHECK(plan->nodes()[1].type->id == "nc.gain");
  CHECK(plan->nodes()[2].type->id == "nc.gain");
}

TEST_CASE("the memory plan reuses buffers instead of giving every node its own")
{
  namchain::rt::Arena arena(1 << 16);

  // A chain of six gains: naive allocation would need one buffer per output. Liveness colouring
  // should collapse it, because each intermediate dies as soon as the next node has read it.
  Graph g;
  g.nodes = {node("in", "nc.input")};
  for (int i = 0; i < 6; ++i)
  {
    NodeSpec gain = node("g" + std::to_string(i), "nc.gain");
    gain.params["gain"] = 1.0;
    g.nodes.push_back(gain);
  }
  g.nodes.push_back(node("out", "nc.output"));
  g.connections.push_back({"in", 0, "g0", 0});
  for (int i = 0; i + 1 < 6; ++i)
    g.connections.push_back({"g" + std::to_string(i), 0, "g" + std::to_string(i + 1), 0});
  g.connections.push_back({"g5", 0, "out", 0});

  auto plan = compile(g, arena);
  REQUIRE(plan->nodes().size() == 8u);
  // 8 nodes produce 7 values if nothing is reused (input + 6 gains); colouring should get well
  // under that, plus the one shared silence buffer.
  CHECK(plan->buffer_count() < 7);
  CHECK(plan->buffer_count() >= 3); // at minimum in, one intermediate, out
}

TEST_CASE("compile is transactional: a bad graph yields no plan and a reason")
{
  namchain::rt::Arena arena(1 << 16);
  Graph g;
  g.nodes = {node("in", "nc.input"), node("x", "nc.gain"), node("out", "nc.output")};
  g.connections = {{"in", 0, "x", 0}, {"x", 0, "out", 0}};
  g.nodes[1].type = "nc.nope";

  std::string error;
  CHECK(namchain::graph::RenderPlan::compile(g, 64, arena, error) == nullptr);
  CHECK_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

TEST_CASE("a compiled chain computes what the parameters say")
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(gain_chain(0.5, 0.5), arena);

  const int frames = 32;
  std::vector<float> input(static_cast<std::size_t>(frames), 0.8f);
  std::vector<float> output(static_cast<std::size_t>(frames), 0.0f);
  plan->execute(input.data(), output.data(), frames);

  for (const float value : output)
    CHECK(value == doctest::Approx(0.8f * 0.25f));
}

TEST_CASE("an unconnected input is silence, not stale memory")
{
  namchain::rt::Arena arena(1 << 16);

  // nc.mix with only its first port fed: the second must read as zero. Leaving it unbound would
  // sum whatever was in the buffer, which is the kind of bug that only shows up as noise on stage.
  Graph g;
  g.nodes = {node("in", "nc.input"), node("mix", "nc.mix"), node("out", "nc.output")};
  g.connections = {{"in", 0, "mix", 0}, {"mix", 0, "out", 0}};

  auto plan = compile(g, arena);
  const int frames = 16;
  std::vector<float> input(static_cast<std::size_t>(frames), 0.5f);
  std::vector<float> output(static_cast<std::size_t>(frames), 0.0f);

  // Run twice: if silence were stale memory, the second run would differ.
  plan->execute(input.data(), output.data(), frames);
  for (const float value : output)
    CHECK(value == doctest::Approx(0.5f));

  plan->execute(input.data(), output.data(), frames);
  for (const float value : output)
    CHECK(value == doctest::Approx(0.5f));
}

TEST_CASE("split fans out and mix sums")
{
  namchain::rt::Arena arena(1 << 16);

  Graph g;
  g.nodes = {node("in", "nc.input"), node("sp", "nc.split"), node("mix", "nc.mix"), node("out", "nc.output")};
  g.connections = {{"in", 0, "sp", 0}, {"sp", 0, "mix", 0}, {"sp", 1, "mix", 1}, {"mix", 0, "out", 0}};

  auto plan = compile(g, arena);
  const int frames = 8;
  std::vector<float> input(static_cast<std::size_t>(frames), 0.25f);
  std::vector<float> output(static_cast<std::size_t>(frames), 0.0f);
  plan->execute(input.data(), output.data(), frames);

  // Split duplicates, mix sums the two copies.
  for (const float value : output)
    CHECK(value == doctest::Approx(0.5f));
}

TEST_CASE("execution is deterministic and allocation-free")
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(gain_chain(0.5, 2.0), arena);

  const int frames = 64;
  std::vector<float> input(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i)
    input[static_cast<std::size_t>(i)] = std::sin(0.1f * static_cast<float>(i));

  std::vector<float> first(static_cast<std::size_t>(frames), 0.0f);
  std::vector<float> second(static_cast<std::size_t>(frames), 0.0f);
  plan->execute(input.data(), first.data(), frames);
  plan->execute(input.data(), second.data(), frames);
  CHECK(first == second);

  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped allocation check: interposer not compiled in");
    return;
  }

  // The render path proper: everything is bound before publication, so this must touch no allocator.
  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int block = 0; block < 100; ++block)
      plan->execute(input.data(), second.data(), frames);
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
}

// ---------------------------------------------------------------------------
// nc.nam
// ---------------------------------------------------------------------------

TEST_CASE("nc.nam in a graph produces the same samples as the model run directly")
{
  namchain::rt::Arena arena(1 << 20);

  const std::string model_path = std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Lite.nam";
  Graph g;
  NodeSpec nam = node("nam", "nc.nam");
  nam.strings["model"] = model_path;
  g.nodes = {node("in", "nc.input"), nam, node("out", "nc.output")};
  g.connections = {{"in", 0, "nam", 0}, {"nam", 0, "out", 0}};

  auto plan = compile(g, arena, 64);

  const int frames = 64;
  std::vector<float> input(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i)
    input[static_cast<std::size_t>(i)] = 0.2f * std::sin(0.05f * static_cast<float>(i));
  std::vector<float> graph_output(static_cast<std::size_t>(frames), 0.0f);
  plan->execute(input.data(), graph_output.data(), frames);

  // Drive the same model directly, reset the same way, and compare. Any difference is the node's
  // float <-> NAM_SAMPLE conversion, which should be exact for a float32 model.
  std::unique_ptr<nam::DSP> model = nam::get_dsp(std::filesystem::path(model_path));
  REQUIRE(model != nullptr);
  model->Reset(48000.0, 64);
  std::vector<NAM_SAMPLE> direct_in(static_cast<std::size_t>(frames));
  std::vector<NAM_SAMPLE> direct_out(static_cast<std::size_t>(frames), 0.0);
  for (int i = 0; i < frames; ++i)
    direct_in[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(input[static_cast<std::size_t>(i)]);
  NAM_SAMPLE* in_ptrs[] = {direct_in.data()};
  NAM_SAMPLE* out_ptrs[] = {direct_out.data()};
  model->process(in_ptrs, out_ptrs, frames);

  for (int i = 0; i < frames; ++i)
    CHECK(graph_output[static_cast<std::size_t>(i)] ==
          doctest::Approx(static_cast<float>(direct_out[static_cast<std::size_t>(i)])).epsilon(1e-6));

  // And it must not be silent, or the comparison above would pass vacuously.
  float peak = 0.0f;
  for (const float value : graph_output)
    peak = std::max(peak, std::abs(value));
  CHECK(peak > 1e-4f);
}

TEST_CASE("nc.nam reports a missing model rather than compiling a silent node")
{
  namchain::rt::Arena arena(1 << 16);

  Graph g;
  NodeSpec nam = node("nam", "nc.nam");
  nam.strings["model"] = "/nonexistent/model.nam";
  g.nodes = {node("in", "nc.input"), nam, node("out", "nc.output")};
  g.connections = {{"in", 0, "nam", 0}, {"nam", 0, "out", 0}};

  std::string error;
  CHECK(namchain::graph::RenderPlan::compile(g, 64, arena, error) == nullptr);
  CHECK(error.find("nc.nam") != std::string::npos);
}
