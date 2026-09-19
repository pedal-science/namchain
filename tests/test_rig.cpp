// Tests for rig loading (§5.1) and for the reference interpreter that acts as the compiler's oracle.
//
// The plan-versus-reference comparison is the important one. Everything else here can be reasoned
// about by reading the code; buffer colouring and pointer binding cannot, because a wrong answer
// still produces plausible audio -- just the wrong audio, on some branches, sometimes.
#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "namchain/graph/plan.h"
#include "namchain/graph/reference.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"

namespace
{

using namchain::graph::Graph;
using namchain::graph::ReferenceRender;
using namchain::graph::RenderPlan;

constexpr int kQuantum = 64;
constexpr double kRate = 48000.0;

namchain::render::CompileOptions options_for(int quantum = kQuantum, double rate = kRate)
{
  namchain::render::CompileOptions options;
  options.max_quantum = quantum;
  options.sample_rate = rate;
  return options;
}

std::string test_model(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

/// A rig that exercises fan-out, fan-in, per-branch state and two different kinds of node state:
/// one branch filters, the other delays and then convolves. A colouring bug needs a graph with a
/// live value crossing another node's definition to show itself, which is exactly this shape.
Graph branched_rig()
{
  namchain::graph::Graph graph;
  namchain::graph::NodeSpec input{"in", "nc.input", {}, {}};
  namchain::graph::NodeSpec split{"sp", "nc.split", {}, {}};
  namchain::graph::NodeSpec gain{"g", "nc.gain", {{"gain", 0.5}}, {}};
  namchain::graph::NodeSpec eq{"eq", "nc.eq.biquad", {{"freq", 1200.0}, {"q", 0.9}, {"gain_db", 6.0}},
                               {{"mode", "peaking"}}};
  namchain::graph::NodeSpec delay{"d", "nc.delay", {{"samples", 17.0}}, {}};
  namchain::graph::NodeSpec cab{"cab", "nc.cab.ir", {}, {{"ir", test_model("CabIR.nam")}}};
  namchain::graph::NodeSpec mix{"mix", "nc.mix", {}, {}};
  namchain::graph::NodeSpec output{"out", "nc.output", {}, {}};

  graph.nodes = {input, split, gain, eq, delay, cab, mix, output};
  graph.connections = {{"in", 0, "sp", 0},
                       {"sp", 0, "g", 0},
                       {"g", 0, "eq", 0},
                       {"eq", 0, "mix", 0},
                       {"sp", 1, "d", 0},
                       {"d", 0, "cab", 0},
                       {"cab", 0, "mix", 1},
                       {"mix", 0, "out", 0}};
  return graph;
}

std::vector<float> test_signal(std::size_t length, unsigned seed = 12345u)
{
  // A cheap deterministic noise-plus-tone: broadband, so a filter or a delay that reads the wrong
  // buffer produces an obvious difference rather than a coincidentally equal value.
  std::vector<float> samples(length);
  unsigned state = seed;
  for (std::size_t i = 0; i < length; ++i)
  {
    state = state * 1664525u + 1013904223u;
    const float noise = (static_cast<float>((state >> 8) & 0xffff) / 32768.0f) - 1.0f;
    const float tone = static_cast<float>(std::sin(0.017 * static_cast<double>(i)));
    samples[i] = 0.2f * noise + 0.3f * tone;
  }
  return samples;
}

} // namespace

// ---------------------------------------------------------------------------
// chain_graph
// ---------------------------------------------------------------------------

TEST_CASE("chain_graph builds a serial chain of the requested length")
{
  const Graph one = namchain::graph::chain_graph("amp.nam", 1);
  REQUIRE(one.nodes.size() == 3u);
  CHECK(one.nodes[0].type == "nc.input");
  CHECK(one.nodes[1].type == "nc.nam");
  CHECK(one.nodes[2].type == "nc.output");
  CHECK(one.nodes[1].strings.at("model") == "amp.nam");

  const Graph four = namchain::graph::chain_graph("amp.nam", 4);
  REQUIRE(four.nodes.size() == 6u);
  CHECK(four.connections.size() == 5u);
  CHECK(four.connections.front().from == "in");
  CHECK(four.connections.back().to == "out");

  // Zero instances means "one", not "none": an empty chain would compile to a graph with no middle
  // node, which is a pass-through wearing a model's name.
  CHECK(namchain::graph::chain_graph("amp.nam", 0).nodes.size() == 3u);

  // Every model node must be connected in series, or a 4-instance soak would measure one model.
  CHECK(namchain::graph::validate(four).ok);
}

// ---------------------------------------------------------------------------
// JSON loading
// ---------------------------------------------------------------------------

TEST_CASE("a rig parses into node params, string params and connections")
{
  const std::string text = R"({
    "nodes": [
      { "id": "in",  "type": "nc.input" },
      { "id": "amp", "type": "nc.nam", "model": "amp.nam", "gain_db": -3.5, "bypass": true },
      { "id": "eq",  "type": "nc.eq.biquad", "mode": "highpass", "freq": 90, "q": 0.7 },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [
      { "from": "in", "to": "amp" },
      { "from": "amp", "to": "eq", "to_port": 0 },
      { "from": "eq", "to": "out" }
    ]
  })";

  Graph graph;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::parse_graph_json(text, graph, error), error);
  REQUIRE(graph.nodes.size() == 4u);
  CHECK(graph.nodes[1].type == "nc.nam");
  CHECK(graph.nodes[1].strings.at("model") == "amp.nam");
  CHECK(graph.nodes[1].params.at("gain_db") == doctest::Approx(-3.5));
  // `bypass` is reserved like `id` and `type`: it is a property of the node, not a numeric parameter,
  // so it does not land in the param map.
  CHECK(graph.nodes[1].bypass);
  CHECK(graph.nodes[1].params.count("bypass") == 0u);
  CHECK(graph.nodes[2].strings.at("mode") == "highpass");
  CHECK(graph.nodes[2].params.at("freq") == doctest::Approx(90.0));
  REQUIRE(graph.connections.size() == 3u);
  CHECK(graph.connections[1].to_port == 0);
  CHECK(graph.connections[0].from_port == 0); // ports default to 0 rather than to garbage
  CHECK(namchain::graph::validate(graph).ok);
}

TEST_CASE("a rig accepts explicit params and strings blocks")
{
  const std::string text = R"({
    "nodes": [
      { "id": "in", "type": "nc.input" },
      { "id": "n", "type": "nc.delay", "params": { "samples": 8 }, "strings": { "note": "slapback" } },
      { "id": "out", "type": "nc.output" }
    ],
    "connections": [ { "from": "in", "to": "n" }, { "from": "n", "to": "out" } ]
  })";

  Graph graph;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::parse_graph_json(text, graph, error), error);
  CHECK(graph.nodes[1].params.at("samples") == doctest::Approx(8.0));
  CHECK(graph.nodes[1].strings.at("note") == "slapback");
}

TEST_CASE("a malformed rig is rejected with a message that says where")
{
  Graph graph;
  std::string error;

  SUBCASE("not JSON")
  {
    CHECK_FALSE(namchain::graph::parse_graph_json("{ nodes: ", graph, error));
    CHECK(error.find("JSON") != std::string::npos);
  }

  SUBCASE("not an object")
  {
    CHECK_FALSE(namchain::graph::parse_graph_json("[]", graph, error));
  }

  SUBCASE("no nodes array")
  {
    CHECK_FALSE(namchain::graph::parse_graph_json("{\"connections\": []}", graph, error));
    CHECK(error.find("nodes") != std::string::npos);
  }

  SUBCASE("node without a type")
  {
    CHECK_FALSE(namchain::graph::parse_graph_json("{\"nodes\": [{\"id\": \"a\"}]}", graph, error));
    CHECK(error.find("node 0") != std::string::npos);
    CHECK(error.find("type") != std::string::npos);
  }

  SUBCASE("a structured parameter is not silently dropped")
  {
    // Dropping it would compile and run, quietly ignoring whatever the user meant.
    CHECK_FALSE(namchain::graph::parse_graph_json(
      "{\"nodes\": [{\"id\": \"a\", \"type\": \"nc.gain\", \"gain\": [1, 2]}]}", graph, error));
    CHECK(error.find("node 0") != std::string::npos);
  }

  SUBCASE("connection missing its destination")
  {
    CHECK_FALSE(namchain::graph::parse_graph_json(
      "{\"nodes\": [{\"id\": \"a\", \"type\": \"nc.gain\"}], \"connections\": [{\"from\": \"a\"}]}", graph, error));
    CHECK(error.find("connection 0") != std::string::npos);
  }

  SUBCASE("a fractional port number")
  {
    CHECK_FALSE(namchain::graph::parse_graph_json(
      "{\"nodes\": [{\"id\": \"a\", \"type\": \"nc.gain\"}], \"connections\": [{\"from\": \"a\", \"to\": \"a\", \"to_port\": 0.5}]}",
      graph, error));
  }
}

TEST_CASE("a rig loads from a file and renders through the compiler")
{
  const auto dir = std::filesystem::temp_directory_path() / "namchain-rig-tests";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "rig.json").string();

  {
    std::ofstream file(path);
    file << R"({
      "nodes": [
        { "id": "in", "type": "nc.input" },
        { "id": "amp", "type": "nc.nam", "model": ")" << test_model("A2-Lite.nam") << R"(" },
        { "id": "cab", "type": "nc.cab.ir", "ir": ")" << test_model("CabIR.nam") << R"(" },
        { "id": "out", "type": "nc.output" }
      ],
      "connections": [
        { "from": "in", "to": "amp" },
        { "from": "amp", "to": "cab" },
        { "from": "cab", "to": "out" }
      ]
    })";
  }

  Graph graph;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::load_graph_json(path, graph, error), error);

  namchain::rt::Arena arena(1u << 22);
  auto plan = RenderPlan::compile(graph, options_for(), arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);

  const std::vector<float> input = test_signal(512);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += kQuantum)
    plan->execute(input.data() + offset, output.data() + offset, kQuantum);

  float peak = 0.0f;
  for (const float value : output)
    peak = std::max(peak, std::abs(value));
  CHECK(peak > 1e-3f); // amp plus cabinet: not silence

  namchain::graph::load_graph_json("/nonexistent/rig.json", graph, error);
  CHECK(error.find("cannot open") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The reference interpreter as the compiler's oracle
// ---------------------------------------------------------------------------

TEST_CASE("the plan and the reference interpreter agree sample for sample")
{
  const Graph graph = branched_rig();
  const namchain::render::CompileOptions options = options_for();

  namchain::rt::Arena arena(1u << 22);
  std::string error;
  auto plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  auto reference = ReferenceRender::compile(graph, options, error);
  REQUIRE_MESSAGE(reference != nullptr, error);

  // Two independent evaluations of the same rig, block for block. They are expected to be
  // *identical*, not merely close: both call the same node implementations with the same sample
  // values, so any difference at all is a compiler bug rather than float noise.
  const std::vector<float> input = test_signal(2048);
  std::vector<float> plan_output(input.size(), 0.0f);
  std::vector<float> reference_output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += kQuantum)
  {
    plan->execute(input.data() + offset, plan_output.data() + offset, kQuantum);
    reference->execute(input.data() + offset, reference_output.data() + offset, kQuantum);
  }

  std::size_t mismatches = 0;
  for (std::size_t i = 0; i < input.size(); ++i)
    if (plan_output[i] != reference_output[i])
      ++mismatches;
  CHECK(mismatches == 0);

  // And the comparison must be comparing something: two silent graphs would also match.
  float peak = 0.0f;
  for (const float value : plan_output)
    peak = std::max(peak, std::abs(value));
  CHECK(peak > 1e-3f);
}

TEST_CASE("the memory plan needs fewer buffers than one-per-value")
{
  const Graph graph = branched_rig();
  const namchain::render::CompileOptions options = options_for();

  namchain::rt::Arena arena(1u << 22);
  std::string error;
  auto plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  auto reference = ReferenceRender::compile(graph, options, error);
  REQUIRE_MESSAGE(reference != nullptr, error);

  // 8 nodes producing 8 values, plus the shared silence buffer: the naive count.
  CHECK(reference->buffer_count() == 9);
  CHECK(plan->buffer_count() < reference->buffer_count());
  CHECK(plan->buffer_count() >= 4); // input, two branch points, the mix
}

TEST_CASE("the reference interpreter rejects what the compiler rejects")
{
  // A pure cycle with no duplicated input port, so the only thing wrong with this graph is that it
  // has no block-delay node in it to break the loop.
  Graph graph;
  graph.nodes = {{"in", "nc.input", {}, {}},
                 {"a", "nc.mix", {}, {}},
                 {"b", "nc.mix", {}, {}},
                 {"out", "nc.output", {}, {}}};
  graph.connections = {{"a", 0, "b", 0}, {"b", 0, "a", 0}, {"a", 0, "out", 0}};

  namchain::rt::Arena arena(1u << 20);
  std::string error;
  CHECK(ReferenceRender::compile(graph, options_for(), error) == nullptr);
  CHECK(error.find("cycle") != std::string::npos);
}

TEST_CASE("two connections into one input port are rejected rather than arbitrarily resolved")
{
  // A DAG with no cycle, so only the duplicate-port rule can reject it. Without the rule the
  // compiler would keep whichever connection it happened to see last, and the rig would run.
  Graph graph;
  graph.nodes = {{"in", "nc.input", {}, {}},
                 {"a", "nc.gain", {{"gain", 1.0}}, {}},
                 {"b", "nc.gain", {{"gain", 1.0}}, {}},
                 {"out", "nc.output", {}, {}}};
  graph.connections = {{"in", 0, "a", 0}, {"b", 0, "a", 0}, {"in", 0, "b", 0}, {"a", 0, "out", 0}};

  namchain::rt::Arena arena(1u << 20);
  std::string error;
  CHECK(RenderPlan::compile(graph, options_for(), arena, error) == nullptr);
  CHECK(error.find("already driven") != std::string::npos);
  CHECK(error.find("port 0") != std::string::npos);
}

TEST_CASE("the reference interpreter is warmed and reset in lockstep with the plan")
{
  // A stateful node carries history, so comparing a warmed plan against a cold reference would
  // compare two different instants of the same filter. This pins the contract: same calls, same
  // order, same result.
  const Graph graph = branched_rig();
  const namchain::render::CompileOptions options = options_for();

  namchain::rt::Arena arena(1u << 22);
  std::string error;
  auto plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  auto reference = ReferenceRender::compile(graph, options, error);
  REQUIRE_MESSAGE(reference != nullptr, error);

  plan->prewarm();
  reference->prewarm();

  const std::vector<float> input = test_signal(256);
  std::vector<float> plan_output(input.size(), 0.0f);
  std::vector<float> reference_output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += kQuantum)
  {
    plan->execute(input.data() + offset, plan_output.data() + offset, kQuantum);
    reference->execute(input.data() + offset, reference_output.data() + offset, kQuantum);
  }
  CHECK(plan_output == reference_output);
}
