// Structural patch ops (plan.md §8.4): the graph mutation, without the plan, the thread or the
// transition. What is tested here is the IR effect of each op, the errors a caller can hit, and the
// all-or-nothing contract the daemon relies on when it applies a patch to a copy.
#include <doctest.h>

#include <string>

#include "namchain/graph/patch.h"
#include "namchain/graph/plan.h"

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::graph::PatchContext;

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

/// in -> g -> out, with `g` a gain, so the ops have something to add to and repoint.
Graph base()
{
  Graph graph;
  NodeSpec gn = node("g", "nc.gain");
  gn.params["gain"] = 1.0;
  graph.nodes = {node("in", "nc.input"), gn, node("out", "nc.output")};
  graph.connections = {{"in", 0, "g", 0}, {"g", 0, "out", 0}};
  return graph;
}

bool has_connection(const Graph& graph, const std::string& from, const std::string& to)
{
  for (const Connection& c : graph.connections)
    if (c.from == from && c.to == to)
      return true;
  return false;
}

} // namespace

TEST_CASE("add_node, connect and rewire grow a rig and repoint an edge")
{
  Graph graph = base();
  PatchContext context;
  std::string error;
  const std::string ops = R"([
    {"op":"add_node","id":"g2","type":"nc.gain","gain":0.5},
    {"op":"connect","from":"g","to":"g2"},
    {"op":"rewire","from":"g2","to":"out"}
  ])";
  REQUIRE_MESSAGE(namchain::graph::apply_patch(graph, ops, context, error), error);

  CHECK(namchain::graph::validate(graph).ok);
  CHECK(graph.nodes.size() == 4u);
  CHECK(has_connection(graph, "g", "g2"));
  CHECK(has_connection(graph, "g2", "out"));
  // rewire replaced the edge into `out` rather than adding a second one.
  CHECK_FALSE(has_connection(graph, "g", "out"));
  CHECK(graph.connections.size() == 3u);
}

TEST_CASE("remove_node drops the node and every edge that touched it")
{
  Graph graph = base();
  PatchContext context;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::apply_patch(graph, R"([{"op":"remove_node","node":"g"}])", context, error),
                  error);
  CHECK(graph.nodes.size() == 2u);
  CHECK(graph.connections.empty());

  // Removing it twice is an error, not a no-op: a client that believes it removed something must not
  // get "ok" for free.
  CHECK_FALSE(namchain::graph::apply_patch(graph, R"([{"op":"remove_node","node":"g"}])", context, error));
  CHECK(error.find("unknown node") != std::string::npos);
}

TEST_CASE("disconnect removes one edge and says so when there is nothing to remove")
{
  SUBCASE("by destination port")
  {
    Graph graph = base();
    PatchContext context;
    std::string error;
    REQUIRE_MESSAGE(
      namchain::graph::apply_patch(graph, R"([{"op":"disconnect","to":"out","to_port":0}])", context, error),
      error);
    CHECK(graph.connections.size() == 1u);
    CHECK_FALSE(namchain::graph::apply_patch(graph, R"([{"op":"disconnect","to":"out"}])", context, error));
    CHECK(error.find("nothing drives") != std::string::npos);
  }

  SUBCASE("by both ends, and a port typo does not match")
  {
    Graph graph = base();
    PatchContext context;
    std::string error;
    CHECK_FALSE(namchain::graph::apply_patch(
      graph, R"([{"op":"disconnect","from":"in","from_port":0,"to":"g","to_port":1}])", context, error));
    REQUIRE_MESSAGE(namchain::graph::apply_patch(
                      graph, R"([{"op":"disconnect","from":"in","to":"g"}])", context, error),
                    error);
    CHECK(graph.connections.size() == 1u);
  }
}

TEST_CASE("swap_model writes the key the node type actually reads")
{
  Graph graph;
  NodeSpec amp = node("amp", "nc.nam");
  amp.strings["model"] = "a.nam";
  NodeSpec cab = node("cab", "nc.cab.ir");
  cab.strings["ir"] = "a.wav";
  graph.nodes = {node("in", "nc.input"), amp, cab, node("out", "nc.output")};

  PatchContext context;
  std::string error;
  REQUIRE_MESSAGE(
    namchain::graph::apply_patch(graph, R"([{"op":"swap_model","node":"amp","model":"b.nam"}])", context, error),
    error);
  CHECK(graph.nodes[1].strings.at("model") == "b.nam");

  // `nc.cab.ir` reads `ir`; the op still says "model" so a caller does not have to know that.
  REQUIRE_MESSAGE(
    namchain::graph::apply_patch(graph, R"([{"op":"swap_model","node":"cab","model":"b.wav"}])", context, error),
    error);
  CHECK(graph.nodes[2].strings.at("ir") == "b.wav");

  CHECK_FALSE(namchain::graph::apply_patch(
    graph, R"([{"op":"swap_model","node":"in","model":"x.nam"}])", context, error));
  CHECK(error.find("not a model node") != std::string::npos);
}

TEST_CASE("a structural set_param writes the build parameter and drops the alias")
{
  Graph graph = base();
  graph.nodes[1].params["gain_db"] = 3.0;
  PatchContext context;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::apply_patch(
                    graph, R"([{"op":"set_param","node":"g","param":"gain","value":0.25}])", context, error),
                  error);
  CHECK(graph.nodes[1].params.at("gain") == doctest::Approx(0.25));
  // `nc.gain` prefers `gain_db` at build, so leaving it would make this edit a silent no-op.
  CHECK(graph.nodes[1].params.count("gain_db") == 0u);
}

TEST_CASE("a patch names what it cannot do instead of doing nothing")
{
  Graph graph = base();
  PatchContext context;
  std::string error;

  SUBCASE("unknown op")
  {
    CHECK_FALSE(namchain::graph::apply_patch(graph, R"([{"op":"explode","node":"g"}])", context, error));
    CHECK(error.find("unknown op") != std::string::npos);
  }
  SUBCASE("bypass is named, not ignored")
  {
    CHECK_FALSE(namchain::graph::apply_patch(
      graph, R"([{"op":"bypass","node":"g","bypass":true}])", context, error));
    CHECK(error.find("not implemented") != std::string::npos);
  }
  SUBCASE("duplicate id")
  {
    CHECK_FALSE(namchain::graph::apply_patch(
      graph, R"([{"op":"add_node","id":"g","type":"nc.gain"}])", context, error));
    CHECK(error.find("already exists") != std::string::npos);
  }
  SUBCASE("connect to a node that does not exist")
  {
    CHECK_FALSE(namchain::graph::apply_patch(
      graph, R"([{"op":"connect","from":"g","to":"ghost"}])", context, error));
    CHECK(error.find("does not exist") != std::string::npos);
  }
  SUBCASE("ops that is not an array")
  {
    CHECK_FALSE(namchain::graph::apply_patch(graph, R"({"op":"connect"})", context, error));
    CHECK(error.find("array") != std::string::npos);
  }
}

TEST_CASE("a failed op list may half-apply its own copy, which is why the caller copies")
{
  // The contract the daemon depends on: apply a patch to a copy, and publish only if it applies.
  const Graph original = base();
  Graph working = original;
  PatchContext context;
  std::string error;
  const std::string ops = R"([
    {"op":"add_node","id":"g2","type":"nc.gain"},
    {"op":"remove_node","node":"ghost"}
  ])";
  CHECK_FALSE(namchain::graph::apply_patch(working, ops, context, error));
  CHECK(working.nodes.size() == 4u);    // the copy took the first op and then failed
  CHECK(original.nodes.size() == 3u);   // the original is untouched
  CHECK(original.connections.size() == 2u);
}
