// The per-node non-finite guard (plan.md §5.3, failure isolation).
//
// A node that returns a non-finite sample is taken out of the signal path for the rest of the plan's
// life and reported. These tests use `nc.gain` with a gain that makes a finite input overflow to
// infinity, which is a real way a node can fail without inventing a test-only node type.
#include <doctest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

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

Graph gain_chain(double gain)
{
  Graph graph;
  NodeSpec gn = node("g", "nc.gain");
  gn.params["gain"] = gain;
  graph.nodes = {node("in", "nc.input"), gn, node("out", "nc.output")};
  graph.connections = {{"in", 0, "g", 0}, {"g", 0, "out", 0}};
  return graph;
}

std::unique_ptr<namchain::graph::RenderPlan> compile(const Graph& graph, namchain::rt::Arena& arena, int quantum)
{
  std::string error;
  auto plan = namchain::graph::RenderPlan::compile(graph, quantum, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

} // namespace

TEST_CASE("a node whose output overflows is failed and replaced by its dry input")
{
  namchain::rt::Arena arena(1 << 16);
  // 16 is the largest gain the schema allows and 1e38 is a finite float; their product is not finite.
  // The overflow is made with the signal rather than with an out-of-range parameter, because the
  // schema now refuses the latter long before the guard could see it.
  auto plan = compile(gain_chain(16.0), arena, 16);
  plan->prewarm();

  const std::vector<float> input(16, 1e38f);
  std::vector<float> output(16, 0.0f);
  plan->execute(input.data(), output.data(), 16);

  CHECK(plan->failed_node_count() == 1u);
  CHECK(plan->guard_trips() == 1u);
  // The dry input stands in for the node's output: the chain does not go silent, and no NaN reaches
  // the device.
  for (const float value : output)
    CHECK(value == 1e38f);

  // It stays failed: the node is not re-run, so the trip is not counted twice.
  std::fill(output.begin(), output.end(), 0.0f);
  plan->execute(input.data(), output.data(), 16);
  CHECK(plan->failed_node_count() == 1u);
  CHECK(plan->guard_trips() == 1u);
  for (const float value : output)
    CHECK(value == 1e38f);
}

TEST_CASE("a non-finite input is caught at the input node and becomes silence")
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(gain_chain(1.0), arena, 16);
  plan->prewarm();

  std::vector<float> input(16, 0.25f);
  input[7] = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> output(16, 0.0f);
  plan->execute(input.data(), output.data(), 16);

  // `nc.input` has no input of its own, so its dry path is silence -- the only honest answer for a
  // node whose output cannot be trusted.
  CHECK(plan->failed_node_count() == 1u);
  CHECK(plan->guard_trips() == 1u);
  for (const float value : output)
    CHECK(value == 0.0f);
}

TEST_CASE("a finite chain trips nothing")
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(gain_chain(0.5), arena, 16);
  plan->prewarm();

  const std::vector<float> input(16, 0.8f);
  std::vector<float> output(16, 0.0f);
  plan->execute(input.data(), output.data(), 16);

  CHECK(plan->failed_node_count() == 0u);
  CHECK(plan->guard_trips() == 0u);
  for (const float value : output)
    CHECK(value == doctest::Approx(0.4f));
}

TEST_CASE("the guard allocates nothing on the render path")
{
  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped allocation check: interposer not compiled in");
    return;
  }
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(gain_chain(0.5), arena, 64);
  plan->prewarm();

  const std::vector<float> input(64, 0.1f);
  std::vector<float> output(64, 0.0f);
  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int block = 0; block < 100; ++block)
      plan->execute(input.data(), output.data(), 64);
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
}
