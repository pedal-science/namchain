// Tests for the document envelope and for node identity (docs/ui-integration-plan.md §1.1, §1.2).
//
// Two things here are worth more than the rest. The compatibility rule -- a bare graph is a one-chain
// document -- is what lets every rig written before documents existed keep meaning exactly what it
// meant. And the identity rules are what let a client keep its own names for things: an element is
// stored and returned verbatim, and an engine node id is minted by the engine rather than composed by
// the client, because no client addresses one.
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"

namespace
{

using namchain::graph::Chain;
using namchain::graph::Document;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;

const char* kBareGraph = R"({
  "nodes": [
    { "id": "in",  "type": "nc.input" },
    { "id": "g",   "type": "nc.gain", "gain": 0.5 },
    { "id": "out", "type": "nc.output" }
  ],
  "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ]
})";

const char* kTwoChains = R"({
  "name": "Guitar + Vocal",
  "chains": [
    {
      "id": "gtr", "name": "Guitar", "input_channel": 0, "output_channel": 0,
      "nodes": [
        { "id": "in",  "type": "nc.input" },
        { "id": "g",   "type": "nc.gain", "gain": 0.25 },
        { "id": "out", "type": "nc.output" }
      ],
      "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ]
    },
    {
      "id": "voc", "name": "Vocal", "input_channel": 1, "output_channel": 1,
      "nodes": [
        { "id": "in",  "type": "nc.input" },
        { "id": "g",   "type": "nc.gain", "gain": 0.75 },
        { "id": "out", "type": "nc.output" }
      ],
      "connections": [ { "from": "in", "to": "g" }, { "from": "g", "to": "out" } ]
    }
  ]
})";

} // namespace

TEST_CASE("a bare graph is a one-chain document")
{
  Document document;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::parse_document_json(kBareGraph, document, error), error);

  REQUIRE(document.chains.size() == 1u);
  CHECK(document.chains[0].id == "main");
  CHECK(document.chains[0].input_channel == 0);
  CHECK(document.chains[0].output_channel == 0);
  CHECK(document.chains[0].graph.nodes.size() == 3u);

  // A node that does not name a document element is its own, so a hand-written rig says nothing new
  // and still has an element for every node.
  for (const NodeSpec& node : document.chains[0].graph.nodes)
    CHECK(node.element == node.id);

  CHECK(namchain::graph::validate(document).ok);

  // The single-chain entry point accepts it too, which is the compatibility that matters: every
  // caller that never heard of chains keeps working.
  Graph graph;
  REQUIRE_MESSAGE(namchain::graph::parse_graph_json(kBareGraph, graph, error), error);
  CHECK(graph.nodes.size() == 3u);
}

TEST_CASE("a document holds several chains, each with its own graph and channels")
{
  Document document;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::parse_document_json(kTwoChains, document, error), error);

  CHECK(document.name == "Guitar + Vocal");
  REQUIRE(document.chains.size() == 2u);
  CHECK(document.chains[0].id == "gtr");
  CHECK(document.chains[0].name == "Guitar");
  CHECK(document.chains[0].input_channel == 0);
  CHECK(document.chains[0].output_channel == 0);
  CHECK(document.chains[1].id == "voc");
  CHECK(document.chains[1].input_channel == 1);
  CHECK(document.chains[1].output_channel == 1);

  // Each chain's parameters are its own, which is the point of two graphs rather than one.
  CHECK(document.chains[0].graph.nodes[1].params.at("gain") == doctest::Approx(0.25));
  CHECK(document.chains[1].graph.nodes[1].params.at("gain") == doctest::Approx(0.75));

  CHECK(namchain::graph::validate(document).ok);

  // A chain from a document compiles and renders through the same compiler as any other graph.
  namchain::rt::Arena arena(1u << 20);
  auto plan = namchain::graph::RenderPlan::compile(document.chains[1].graph, 64, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  std::vector<float> samples(64, 1.0f);
  plan->execute(samples.data(), samples.data(), 64);
  CHECK(samples[0] == doctest::Approx(0.75f));

  // The single-chain entry point refuses it, and says how many it found rather than choosing one.
  Graph graph;
  CHECK_FALSE(namchain::graph::parse_graph_json(kTwoChains, graph, error));
  CHECK(error.find("exactly one chain") != std::string::npos);
  CHECK(error.find("2") != std::string::npos);
}

TEST_CASE("a malformed document is rejected with a message that names the chain")
{
  Document document;
  std::string error;

  SUBCASE("no chains at all")
  {
    CHECK_FALSE(namchain::graph::parse_document_json("{\"chains\": []}", document, error));
    CHECK(error.find("at least one chain") != std::string::npos);
  }

  SUBCASE("chains is not an array")
  {
    CHECK_FALSE(namchain::graph::parse_document_json("{\"chains\": {}}", document, error));
    CHECK(error.find("'chains' must be an array") != std::string::npos);
  }

  SUBCASE("a chain without an id")
  {
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"nodes\": [{\"id\": \"a\", \"type\": \"nc.gain\"}]}]}", document, error));
    CHECK(error.find("chain 0") != std::string::npos);
    CHECK(error.find("'id'") != std::string::npos);
  }

  SUBCASE("duplicate chain ids")
  {
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"a\", \"nodes\": []}, {\"id\": \"a\", \"nodes\": []}]}", document, error));
    CHECK(error.find("duplicate chain id") != std::string::npos);
  }

  SUBCASE("a negative channel")
  {
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"a\", \"input_channel\": -1, \"nodes\": []}]}", document, error));
    CHECK(error.find("non-negative") != std::string::npos);
  }

  SUBCASE("a chain with no nodes")
  {
    CHECK_FALSE(namchain::graph::parse_document_json("{\"chains\": [{\"id\": \"a\"}]}", document, error));
    CHECK(error.find("chain 'a'") != std::string::npos);
    CHECK(error.find("'nodes'") != std::string::npos);
  }

  SUBCASE("a node error inside a chain names the chain and the node")
  {
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"voc\", \"nodes\": [{\"id\": \"a\"}]}]}", document, error));
    CHECK(error.find("chain 'voc'") != std::string::npos);
    CHECK(error.find("node 0") != std::string::npos);
  }

  SUBCASE("both a chains array and a top-level nodes array")
  {
    // One or the other. Taking the chains and ignoring the nodes would compile and run while
    // discarding whatever the author meant by the other half of the document.
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"a\", \"nodes\": []}], \"nodes\": []}", document, error));
    CHECK(error.find("both") != std::string::npos);
  }
}

TEST_CASE("a node's element is the client's name for it, and the engine mints the id")
{
  Document document;
  std::string error;

  SUBCASE("element and role are kept verbatim, punctuation and all")
  {
    REQUIRE_MESSAGE(namchain::graph::parse_document_json(R"({
      "chains": [ { "id": "c", "nodes": [
        { "id": "in",  "type": "nc.input" },
        { "element": "3F2504E0-4F89-11D3-9A0C-0305E82C3301", "role": "line",
          "type": "nc.delay", "samples": 8 },
        { "id": "out", "type": "nc.output" }
      ], "connections": [ {"from":"in","to":"3F2504E0-4F89-11D3-9A0C-0305E82C3301/line"},
                          {"from":"3F2504E0-4F89-11D3-9A0C-0305E82C3301/line","to":"out"} ] } ]
    })", document, error), error);

    const NodeSpec& node = document.chains[0].graph.nodes[1];
    CHECK(node.element == "3F2504E0-4F89-11D3-9A0C-0305E82C3301");
    CHECK(node.role == "line");
    CHECK(node.id == "3F2504E0-4F89-11D3-9A0C-0305E82C3301/line");
    CHECK(namchain::graph::validate(document).ok);
  }

  SUBCASE("one element can own several nodes, and its id is untouched by it")
  {
    REQUIRE_MESSAGE(namchain::graph::parse_document_json(R"({
      "chains": [ { "id": "c", "nodes": [
        { "element": "blk7", "role": "line", "type": "nc.delay", "samples": 4 },
        { "element": "blk7", "role": "fb",   "type": "nc.gain", "gain": 0.5 }
      ] } ]
    })", document, error), error);
    CHECK(document.chains[0].graph.nodes[0].element == "blk7");
    CHECK(document.chains[0].graph.nodes[1].element == "blk7");
    CHECK(document.chains[0].graph.nodes[0].id != document.chains[0].graph.nodes[1].id);
  }

  SUBCASE("the same element and role twice are disambiguated, not rejected")
  {
    REQUIRE_MESSAGE(namchain::graph::parse_document_json(R"({
      "chains": [ { "id": "c", "nodes": [
        { "element": "blk7", "role": "line", "type": "nc.delay", "samples": 4 },
        { "element": "blk7", "role": "line", "type": "nc.delay", "samples": 5 }
      ] } ]
    })", document, error), error);
    CHECK(document.chains[0].graph.nodes[0].id == "blk7/line");
    CHECK(document.chains[0].graph.nodes[1].id == "blk7/line~2");
  }

  SUBCASE("an explicit id becomes the element when no element is given")
  {
    REQUIRE_MESSAGE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"c\", \"nodes\": [{\"id\": \"amp\", \"type\": \"nc.gain\"}]}]}", document, error),
      error);
    CHECK(document.chains[0].graph.nodes[0].element == "amp");
    CHECK(document.chains[0].graph.nodes[0].role.empty());
  }

  SUBCASE("a node with neither an id nor an element has nothing to be called")
  {
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"c\", \"nodes\": [{\"type\": \"nc.gain\"}]}]}", document, error));
    CHECK(error.find("node 0") != std::string::npos);
    CHECK(error.find("'element'") != std::string::npos);
  }

  SUBCASE("a duplicate explicit id is refused rather than silently renumbered")
  {
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"c\", \"nodes\": [{\"id\": \"g\", \"type\": \"nc.gain\"},"
      "{\"id\": \"g\", \"type\": \"nc.gain\"}]}]}", document, error));
    CHECK(error.find("duplicate node id") != std::string::npos);
  }

  SUBCASE("element and role must be strings")
  {
    CHECK_FALSE(namchain::graph::parse_document_json(
      "{\"chains\": [{\"id\": \"c\", \"nodes\": [{\"element\": 7, \"type\": \"nc.gain\"}]}]}", document, error));
    CHECK(error.find("'element' must be a string") != std::string::npos);
  }

  SUBCASE("element and role are reserved, not parameters")
  {
    REQUIRE_MESSAGE(namchain::graph::parse_document_json(R"({
      "chains": [ { "id": "c", "nodes": [
        { "id": "g", "element": "blk", "role": "line", "type": "nc.gain", "gain": 0.5 }
      ] } ]
    })", document, error), error);
    const NodeSpec& node = document.chains[0].graph.nodes[0];
    CHECK(node.params.count("element") == 0u);
    CHECK(node.params.count("role") == 0u);
    CHECK(node.params.at("gain") == doctest::Approx(0.5));
  }
}

TEST_CASE("validate names the chain that is wrong")
{
  Document document;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::parse_document_json(kTwoChains, document, error), error);

  SUBCASE("a chain with no output")
  {
    document.chains[1].graph.nodes.pop_back();
    const auto result = namchain::graph::validate(document);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("chain 'voc'") != std::string::npos);
    CHECK(result.message.find("nc.output") != std::string::npos);
  }

  SUBCASE("two chains with the same id, built by hand")
  {
    document.chains[1].id = document.chains[0].id;
    const auto result = namchain::graph::validate(document);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("share the id") != std::string::npos);
  }

  SUBCASE("a document with no chains")
  {
    document.chains.clear();
    const auto result = namchain::graph::validate(document);
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("at least one chain") != std::string::npos);
  }
}

TEST_CASE("a document loads from a file and resolves every chain's paths against it")
{
  const auto dir = std::filesystem::temp_directory_path() / "namchain-document-tests";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "two-chains.json").string();

  {
    std::ofstream file(path);
    file << R"({
      "chains": [
        { "id": "gtr", "nodes": [
            { "id": "in", "type": "nc.input" },
            { "id": "amp", "type": "nc.nam", "model": "gtr.nam" },
            { "id": "out", "type": "nc.output" } ],
          "connections": [ {"from":"in","to":"amp"}, {"from":"amp","to":"out"} ] },
        { "id": "voc", "nodes": [
            { "id": "in", "type": "nc.input" },
            { "id": "amp", "type": "nc.nam", "model": "voc.nam" },
            { "id": "out", "type": "nc.output" } ],
          "connections": [ {"from":"in","to":"amp"}, {"from":"amp","to":"out"} ] }
      ]
    })";
  }

  // Stand-ins rather than real models: the resolver asks whether the file exists, and nothing here
  // loads one. What is being tested is which directory a bare name resolved against, per chain.
  for (const char* name : {"gtr.nam", "voc.nam"})
  {
    std::ofstream placeholder(dir / name);
    placeholder << "not a model";
  }

  Document document;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::load_document_json(path, document, error), error);
  CHECK(document.chains[0].graph.nodes[1].strings.at("model") == (dir / "gtr.nam").string());
  CHECK(document.chains[1].graph.nodes[1].strings.at("model") == (dir / "voc.nam").string());

  // And the long-standing single-chain loader refuses this file rather than picking a chain.
  Graph graph;
  CHECK_FALSE(namchain::graph::load_graph_json(path, graph, error));
  CHECK(error.find("exactly one chain") != std::string::npos);
}
