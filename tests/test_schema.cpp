// Tests for the parameter schema (docs/ui-integration-plan.md §1.4).
//
// Three claims are being checked, and each of them is a bug class rather than a detail:
//
//   * every registered type describes itself, so a palette and an inspector can be built from the
//     engine's answer rather than from a table in the client that goes stale silently;
//   * the declared default of a parameter *is* the engine's own fallback, so there are not two
//     default tables that can drift apart and make a rig sound different in the app than in the
//     engine;
//   * every declared parameter does something. A parameter that is declared and ignored is a knob
//     that does nothing, which is the worst failure a control surface can have -- so each one either
//     changes a render or sits in the exception table below with a reason.
#include <doctest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::graph::RenderPlan;
using namchain::render::CompileOptions;
using namchain::render::NodeType;
using namchain::render::ParamDesc;

constexpr int kQuantum = 64;
constexpr double kRate = 48000.0;

std::string test_data(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

/// The parameters a type cannot be built without, as a JSON object a spec can be parsed from: a model
/// path, a delay time. Everything else has a declared default.
std::string required_params(const std::string& type)
{
  if (type == "nc.nam")
    return nlohmann::json{{"model", test_data("A2-Lite.nam")}}.dump();
  if (type == "nc.cab.ir")
    return nlohmann::json{{"ir", test_data("CabIR.nam")}}.dump();
  if (type == "nc.delay")
    return nlohmann::json{{"ms", 5.0}}.dump();
  return "{}";
}

/// What the node needs, plus whatever makes its remaining parameters *observable*. A peaking EQ at
/// its default 0 dB of gain is exactly the identity filter, so `freq` and `q` cannot change a render
/// until the gain is not zero -- which is a fact about the filter rather than a gap in the schema.
std::string baseline_params(const std::string& type)
{
  if (type == "nc.eq.biquad")
    return nlohmann::json{{"gain_db", 6.0}}.dump();
  return required_params(type);
}

/// `in -> the node under test -> out`, with only port 0 connected: every other port is left
/// unconnected on purpose, because that is legal and is bound to silence.
Graph one_node(const std::string& type, const nlohmann::json& params)
{
  nlohmann::json node;
  node["id"] = "n";
  node["type"] = type;
  for (const auto& [key, value] : params.items())
    node[key] = value;

  nlohmann::json document;
  document["chains"] = nlohmann::json::array({{
    {"id", "c"},
    {"nodes", nlohmann::json::array({nlohmann::json{{"id", "in"}, {"type", "nc.input"}}, node,
                                     nlohmann::json{{"id", "out"}, {"type", "nc.output"}}})},
    {"connections", nlohmann::json::array({nlohmann::json{{"from", "in"}, {"to", "n"}},
                                           nlohmann::json{{"from", "n"}, {"to", "out"}}})},
  }});

  namchain::graph::Document parsed;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::parse_document_json(document.dump(), parsed, error), error);
  return parsed.chains.front().graph;
}

std::vector<float> render(const Graph& graph)
{
  namchain::rt::Arena arena(1u << 22);
  CompileOptions options;
  options.max_quantum = kQuantum;
  options.sample_rate = kRate;
  std::string error;
  auto plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);

  std::vector<float> input(kQuantum * 8);
  for (std::size_t i = 0; i < input.size(); ++i)
    input[i] = 0.5f * std::sin(static_cast<float>(i) * 0.05f);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += kQuantum)
    plan->execute(input.data() + offset, output.data() + offset, kQuantum);
  return output;
}

bool differs(const std::vector<float>& a, const std::vector<float>& b)
{
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    if (std::abs(a[i] - b[i]) > 1e-6f)
      return true;
  }
  return false;
}

/// A value for \p param that is not its declared default, used to check that setting it does
/// something. Chosen per parameter rather than per type, because what "different" means is a property
/// of the parameter: a frequency and a Q move in different directions.
nlohmann::json other_value(const std::string& type, const ParamDesc& param)
{
  const std::string id = param.id;
  if (id == "model")
    return test_data("A2-Lite.nam"); // the only fixture model, so `nc.nam`'s own case is elsewhere
  if (id == "ir")
    return test_data("CabIR.nam");
  if (id == "mode")
    return "lowpass";
  if (id == "freq")
    return 2500.0;
  if (id == "q")
    return 4.0;
  if (id == "gain_db")
    return -9.0;
  if (id == "gain")
    return 2.0;
  if (id == "ms")
    return 17.0;
  if (id == "samples")
    return 300.0;
  if (id == "inputs" || id == "outputs")
    return 3.0; // a third port, left unconnected: see the exception table for why this proves nothing
  (void)type;
  return param.default_value + 1.0;
}

} // namespace

TEST_CASE("every registered node type describes itself")
{
  const std::vector<const NodeType*> types = namchain::render::all_node_types();
  REQUIRE(types.size() > 0u);

  std::set<std::string> ids;
  for (const NodeType* type : types)
  {
    CAPTURE(type->id);
    CHECK(ids.insert(type->id).second); // no type registered twice
    CHECK(type->name != nullptr);
    CHECK(type->category != nullptr);
    CHECK(type->channels == 1); // declared and refused: nothing in the engine is stereo yet

    std::set<std::string> param_ids;
    for (int i = 0; i < type->param_count; ++i)
    {
      const ParamDesc& param = type->params[i];
      CAPTURE(param.id);
      CHECK(param.id != nullptr);
      CHECK(param.name != nullptr);
      CHECK(param_ids.insert(param.id).second);

      // A parameter that cannot be ramped can still be patched structurally; the reverse makes no
      // sense, and the schema is what a client reads to decide.
      if (param.rampable)
        CHECK(param.patchable);
      if (param.kind == namchain::render::ParamKind::Option)
      {
        // An option is either a fixed set of values or one resolved at runtime from the device's
        // registry (`model`, `ir`), and never both: a client that saw neither would have no way to
        // build a picker.
        const bool fixed = param.options != nullptr && param.option_count > 0;
        const bool dynamic = param.options_from != nullptr;
        CHECK(fixed != dynamic);
      }
      if (param.has_min && param.has_max)
        CHECK(param.min < param.max);
    }
  }

  // The types a rig cannot be written without, and the one that exists to break a cycle rather than
  // to be played.
  CHECK(ids.count("nc.input") == 1u);
  CHECK(ids.count("nc.output") == 1u);
  CHECK(namchain::render::find_node_type("nc.nope") == nullptr);
}

TEST_CASE("a parameter's declared default is the engine's own fallback")
{
  // For every type: build it with only what it cannot be built without, and again with those plus
  // every declared default written out. If the two renders differ, a client substituting the
  // published default would change the sound -- the two-default-tables bug, closed by construction.
  for (const NodeType* type : namchain::render::all_node_types())
  {
    // The chain's endpoints are not nodes in the middle of one: a graph with `nc.input` in the middle
    // has two of them, which is a different rule being tested elsewhere.
    if (type->id == "nc.input" || type->id == "nc.output")
      continue;

    CAPTURE(type->id);
    const nlohmann::json required = nlohmann::json::parse(baseline_params(type->id));
    nlohmann::json all = required;
    for (int i = 0; i < type->param_count; ++i)
    {
      const ParamDesc& param = type->params[i];
      // A required parameter and one of a `one_of` group have no default: the schema publishes null
      // for them, and they are already in the baseline.
      if (param.required || param.one_of != nullptr || required.contains(param.id))
        continue;
      if (param.kind == namchain::render::ParamKind::Option)
        all[param.id] = param.options[0].value;
      else
        all[param.id] = param.default_value;
    }

    const std::vector<float> bare = render(one_node(type->id, required));
    const std::vector<float> spelled_out = render(one_node(type->id, all));
    CHECK_FALSE(differs(bare, spelled_out));
  }

  // The one default the loop above cannot reach: `nc.eq.biquad`'s baseline already sets `gain_db` to
  // make `freq` and `q` observable, so its own declared default is checked on its own. A peaking EQ at
  // the declared default of 0 dB is the identity filter, which is exactly what a node with no
  // parameters at all should be.
  const std::vector<float> no_params = render(one_node("nc.eq.biquad", nlohmann::json::object()));
  const std::vector<float> unity_gain =
    render(one_node("nc.eq.biquad", nlohmann::json{{"gain_db", 0.0}, {"freq", 1000.0}, {"q", 0.707},
                                                   {"mode", "peaking"}}));
  CHECK_FALSE(differs(no_params, unity_gain));
}

TEST_CASE("every declared parameter does something")
{
  // Each exception is a parameter whose effect a one-node, eight-block render cannot show, with the
  // reason it cannot. An entry here is a promise that something else covers it.
  struct Exception
  {
    const char* id;
    const char* why;
  };
  const Exception exceptions[] = {
    {"ramp_ms", "it changes how a *later* ramp moves, not a steady-state render"},
    {"outputs", "it changes how many ports exist, not what port 0 carries"},
    {"inputs", "it changes how many ports exist, not what port 0 carries"},
    {"model", "the fixture set has one model, so there is nothing else to swap in"},
    {"ir", "the fixture set has one impulse response"},
  };

  for (const NodeType* type : namchain::render::all_node_types())
  {
    if (type->id == "nc.input" || type->id == "nc.output")
      continue;

    const nlohmann::json baseline_params_json = nlohmann::json::parse(baseline_params(type->id));
    const std::vector<float> baseline = render(one_node(type->id, baseline_params_json));

    for (int i = 0; i < type->param_count; ++i)
    {
      const ParamDesc& param = type->params[i];
      // Required parameters are already in the baseline, and a `one_of` alternative is a different way
      // of saying the same thing rather than a knob: both are covered by the compile tests.
      if (param.hidden || param.required || param.one_of != nullptr)
        continue;

      bool excepted = false;
      for (const Exception& exception : exceptions)
        excepted = excepted || std::string(exception.id) == param.id;
      if (excepted)
        continue;

      INFO("type " << type->id << " param " << std::string(param.id));
      CAPTURE(type->id);
      nlohmann::json changed = baseline_params_json;
      changed[param.id] = other_value(type->id, param);
      CHECK_MESSAGE(differs(baseline, render(one_node(type->id, changed))),
                    type->id << "." << param.id << " is declared and changes nothing");
    }
  }
}

TEST_CASE("a parameter the schema does not declare is refused by name")
{
  // The bug this exists for: `{"gainz": 3.0}` used to be accepted silently and published, so a UI's
  // typo would have been a knob that does nothing (docs/ljoss-comparison.md §3.5).
  const Graph graph = one_node("nc.gain", nlohmann::json{{"gainz", 3.0}});
  const auto result = namchain::graph::validate(graph);
  CHECK_FALSE(result.ok);
  CHECK(result.message.find("no parameter 'gainz'") != std::string::npos);
  CHECK(result.message.find("'gain_db'") != std::string::npos); // the message lists what it takes
  CHECK(result.message.find("node 'n'") != std::string::npos);
}

TEST_CASE("schema bounds, options, required parameters and one_of groups are enforced")
{
  SUBCASE("a value above the maximum")
  {
    const auto result = namchain::graph::validate(one_node("nc.gain", nlohmann::json{{"gain_db", 40.0}}));
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("maximum") != std::string::npos);
  }
  SUBCASE("a value at an exclusive minimum")
  {
    // A Q of zero is not "a very wide filter", it is a divide by zero, so the bound is exclusive and
    // the schema says so rather than leaving it to the node's own check.
    const auto result = namchain::graph::validate(one_node("nc.eq.biquad", nlohmann::json{{"q", 0.0}}));
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("minimum") != std::string::npos);
  }
  SUBCASE("an option outside the declared set")
  {
    const auto result = namchain::graph::validate(one_node("nc.eq.biquad", nlohmann::json{{"mode", "squelch"}}));
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("'squelch'") != std::string::npos);
    CHECK(result.message.find("'peaking'") != std::string::npos);
  }
  SUBCASE("an option spelling that has always been accepted still is")
  {
    // `low_shelf` is not advertised -- a picker with three names for one mode is worse than one -- but
    // rigs that use it keep working.
    CHECK(namchain::graph::validate(one_node("nc.eq.biquad", nlohmann::json{{"mode", "low_shelf"}})).ok);
  }
  SUBCASE("a number where an option belongs")
  {
    const auto result = namchain::graph::validate(one_node("nc.eq.biquad", nlohmann::json{{"mode", 3.0}}));
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("takes a string") != std::string::npos);
  }
  SUBCASE("a missing required parameter")
  {
    const auto result = namchain::graph::validate(one_node("nc.nam", nlohmann::json::object()));
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("needs 'model'") != std::string::npos);
  }
  SUBCASE("a one_of group with nothing from it")
  {
    const auto result = namchain::graph::validate(one_node("nc.delay", nlohmann::json::object()));
    CHECK_FALSE(result.ok);
    CHECK(result.message.find("one of") != std::string::npos);
    CHECK(result.message.find("'samples'") != std::string::npos);
    CHECK(result.message.find("'ms'") != std::string::npos);
    // The compiler is where a rig is rejected, not the render path: a bad rig never reaches a plan.
    namchain::rt::Arena arena(1u << 16);
    std::string error;
    CHECK(RenderPlan::compile(one_node("nc.delay", nlohmann::json::object()), kQuantum, arena, error) == nullptr);
  }
  SUBCASE("a parameter with no schema maximum is still bounded by the engine")
  {
    // Nyquist belongs to the node, not to the schema: the same 30 kHz is legal at 96 kHz.
    namchain::rt::Arena arena(1u << 16);
    std::string error;
    Graph graph = one_node("nc.eq.biquad", nlohmann::json{{"freq", 30000.0}});
    CHECK(RenderPlan::compile(graph, kQuantum, arena, error) == nullptr);
    CHECK(error.find("freq") != std::string::npos);
  }
}

TEST_CASE("a node type wider than the audio path is refused by name")
{
  // Every built-in is mono, so this is the guard rather than a case in the field: declared and refused
  // is a promise about the day a stereo node appears, and a promise with no test is a comment
  // (docs/ui-integration-plan.md §1.4).
  namchain::render::NodeType stereo;
  stereo.id = "test.stereo";
  stereo.inputs = 1;
  stereo.outputs = 1;
  stereo.channels = 2;

  namchain::render::PortCounts ports;
  std::string error;
  const NodeSpec spec = [] {
    NodeSpec node;
    node.id = "n";
    node.type = "test.stereo";
    return node;
  }();
  CHECK_FALSE(namchain::render::resolve_ports(stereo, spec, ports, error));
  CHECK(error.find("stereo") != std::string::npos);
  CHECK(error.find("mono") != std::string::npos);

  // And the same type at one channel resolves, so the guard is about the width rather than the type.
  stereo.channels = 1;
  CHECK(namchain::render::resolve_ports(stereo, spec, ports, error));
  CHECK(ports.channels == 1);
}
