// Tests for macros: lowering, and the claim that a macro is exactly the graph it stands for
// (docs/ui-integration-plan.md slice E, docs/macros.md).
//
// Three kinds of claim are worth testing here, in increasing order of how much they would cost to get
// wrong:
//
//   1. **The declaration is consistent with itself.** Every declared parameter has a param map whose
//      targets name roles that exist and node parameters that exist, and changing a parameter changes
//      exactly those targets. A macro that fails this is one whose knob would move nothing -- or,
//      worse, would move something a `set_param` does not know about, so that a patched value and the
//      next compile would disagree.
//   2. **The lowering is the shape it says it is.** Roles, types, ids and edges asserted rather than
//      assumed, because a client addresses `element` + `role` and a role that moves is a broken
//      address.
//   3. **A macro renders what the hand-built graph renders** -- sample-identical, because the lowering
//      is supposed to be a rewrite and not an approximation. This is the oracle argument the compiler
//      already uses for its interpreter, pointed at the macro layer.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "namchain/graph/macro.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::MacroBypass;
using namchain::graph::MacroParamDecl;
using namchain::graph::MacroType;
using namchain::graph::NodeSpec;
using namchain::graph::RenderPlan;

constexpr double kRate = 48000.0;
constexpr int kQuantum = 64;
constexpr double kPi = 3.14159265358979323846;

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  spec.element = id;
  return spec;
}

/// \brief A macro node as a client writes one: a type, an element, and parameters by name.
///
/// A parameter whose values come from the library (`fx.cab.ir`'s `ir`) is filled in with a real
/// fixture, because it is required: lowering a cab macro with no IR is a document error, and these
/// tests are about everything else.
NodeSpec macro_node(const std::string& type, const std::string& element,
                    const std::map<std::string, double>& params = {},
                    const std::map<std::string, std::string>& strings = {})
{
  NodeSpec spec;
  spec.id = element;
  spec.element = element;
  spec.type = type;
  spec.params = params;
  spec.strings = strings;
  if (const MacroType* macro = namchain::graph::find_macro_type(type))
  {
    for (int i = 0; i < macro->param_count; ++i)
    {
      const namchain::render::ParamDesc& desc = macro->params[i].desc;
      if (desc.options_from != nullptr && spec.strings.count(desc.id) == 0)
        spec.strings[desc.id] = std::string(NAMCHAIN_TEST_DATA_DIR) + "/CabIR.nam";
    }
  }
  return spec;
}

/// input -> the one node under test -> output.
Graph one_node(const NodeSpec& middle)
{
  Graph graph;
  graph.nodes = {node("in", "nc.input"), middle, node("out", "nc.output")};
  graph.connections = {{"in", 0, middle.id, 0}, {middle.id, 0, "out", 0}};
  return graph;
}

std::unique_ptr<RenderPlan> compile_plan(const Graph& graph, namchain::rt::Arena& arena, std::string& error)
{
  namchain::render::CompileOptions options;
  options.max_quantum = kQuantum;
  options.sample_rate = kRate;
  return RenderPlan::compile(graph, options, arena, error);
}

/// Run `input` through a freshly compiled plan in quantum-sized blocks.
std::vector<float> render(const Graph& graph, const std::vector<float>& input)
{
  namchain::rt::Arena arena(1u << 24);
  std::string error;
  auto plan = compile_plan(graph, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += kQuantum)
  {
    const int frames = static_cast<int>(
      std::min<std::size_t>(static_cast<std::size_t>(kQuantum), input.size() - offset));
    plan->execute(input.data() + offset, output.data() + offset, frames);
  }
  return output;
}

/// A signal with something in every band, so a filter's effect is visible rather than incidental.
std::vector<float> test_signal(std::size_t frames)
{
  std::vector<float> signal(frames, 0.0f);
  for (std::size_t i = 0; i < frames; ++i)
  {
    const double t = static_cast<double>(i) / kRate;
    signal[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * 220.0 * t) +
                                   0.25 * std::sin(2.0 * kPi * 3000.0 * t));
  }
  signal[0] += 0.8f; // an impulse, for the delay's sake
  return signal;
}

const NodeSpec* by_role(const Graph& graph, const std::string& role)
{
  for (const NodeSpec& spec : graph.nodes)
  {
    if (spec.role == role)
      return &spec;
  }
  return nullptr;
}

Graph lowered(const Graph& graph)
{
  Graph out;
  std::string error;
  REQUIRE_MESSAGE(namchain::graph::lower_graph(graph, out, error), error);
  return out;
}

const namchain::render::ParamDesc* node_param(const namchain::render::NodeType& type, const std::string& name)
{
  for (int i = 0; i < type.param_count; ++i)
  {
    if (name == type.params[i].id)
      return &type.params[i];
  }
  return nullptr;
}

/// A value for \p param that is not its default: mid-range, on the declared grid.
double different_value(const MacroParamDecl& param)
{
  const namchain::render::ParamDesc& desc = param.desc;
  const double low = desc.has_min ? desc.min : 0.0;
  const double high = desc.has_max ? desc.max : 1.0;
  double middle = (low + high) / 2.0;
  if (desc.step > 0.0)
  {
    const double steps = std::round((middle - low) / desc.step);
    middle = low + steps * desc.step;
  }
  if (middle == desc.default_value)
    middle = desc.has_max ? high : high + 1.0;
  return middle;
}

/// One node as it is compared between two lowerings.
struct RoleDifference
{
  std::string role;
  std::vector<std::string> params;
  std::vector<std::string> strings;
  bool bypass = false;
};

std::vector<RoleDifference> differences(const Graph& before, const Graph& after)
{
  std::vector<RoleDifference> result;
  for (const NodeSpec& spec : after.nodes)
  {
    const NodeSpec* was = by_role(before, spec.role);
    REQUIRE(was != nullptr);
    RoleDifference difference;
    difference.role = spec.role;
    difference.bypass = spec.bypass != was->bypass;
    for (const auto& [key, value] : spec.params)
    {
      const auto previous = was->params.find(key);
      if (previous == was->params.end() || std::fabs(previous->second - value) > 1e-9)
        difference.params.push_back(key);
    }
    for (const auto& [key, value] : spec.strings)
    {
      const auto previous = was->strings.find(key);
      if (previous == was->strings.end() || previous->second != value)
        difference.strings.push_back(key);
    }
    if (!difference.params.empty() || !difference.strings.empty() || difference.bypass)
      result.push_back(std::move(difference));
  }
  return result;
}

} // namespace

// ---------------------------------------------------------------------------
// The declaration is consistent with itself
// ---------------------------------------------------------------------------

TEST_CASE("every macro declares roles, ports and targets that exist")
{
  for (const MacroType* macro : namchain::graph::all_macro_types())
  {
    CAPTURE(macro->id);
    REQUIRE(macro->params != nullptr);
    REQUIRE(macro->nodes != nullptr);
    REQUIRE(macro->apply != nullptr);
    CHECK(macro->inputs >= 1);
    CHECK(macro->outputs >= 1);

    // Roles are unique and name real node types, so `element` + `role` addresses one node.
    for (int i = 0; i < macro->node_count; ++i)
    {
      CHECK(namchain::render::find_node_type(macro->nodes[i].type) != nullptr);
      for (int j = i + 1; j < macro->node_count; ++j)
        CHECK(std::string(macro->nodes[i].role) != macro->nodes[j].role);
    }

    // Every edge names roles that exist, and ports those nodes have.
    for (int e = 0; e < macro->edge_count; ++e)
    {
      const namchain::graph::MacroEdge& edge = macro->edges[e];
      const namchain::render::NodeType* from_type = nullptr;
      const namchain::render::NodeType* to_type = nullptr;
      for (int i = 0; i < macro->node_count; ++i)
      {
        if (std::string(macro->nodes[i].role) == edge.from_role)
          from_type = namchain::render::find_node_type(macro->nodes[i].type);
        if (std::string(macro->nodes[i].role) == edge.to_role)
          to_type = namchain::render::find_node_type(macro->nodes[i].type);
      }
      REQUIRE(from_type != nullptr);
      REQUIRE(to_type != nullptr);
      CHECK(edge.from_port < from_type->outputs);
      CHECK(edge.to_port < to_type->inputs);
    }

    // Every port link names a role that exists and a port the macro declares.
    const auto check_links = [macro](const namchain::graph::MacroPortLink* links, int count, int ports) {
      for (int i = 0; i < count; ++i)
      {
        CHECK(links[i].port >= 0);
        CHECK(links[i].port < ports);
        bool known = false;
        for (int n = 0; n < macro->node_count; ++n)
          known = known || std::string(macro->nodes[n].role) == links[i].role;
        CHECK(known);
      }
    };
    check_links(macro->input_links, macro->input_link_count, macro->inputs);
    check_links(macro->output_links, macro->output_link_count, macro->outputs);

    // A crossfade bypass needs two gain roles that really exist.
    if (macro->bypass == MacroBypass::Crossfade)
    {
      REQUIRE(macro->dry_role != nullptr);
      REQUIRE(macro->wet_role != nullptr);
      const Graph primitives = lowered(one_node(macro_node(macro->id, "b1")));
      CHECK(by_role(primitives, macro->dry_role) != nullptr);
      CHECK(by_role(primitives, macro->wet_role) != nullptr);
    }

    // The param map's targets name a declared role and a parameter that node type declares.
    for (int i = 0; i < macro->param_count; ++i)
    {
      const MacroParamDecl& param = macro->params[i];
      CAPTURE(param.desc.id);
      CHECK(param.desc.id != nullptr);
      CHECK(param.desc.name != nullptr);
      for (int t = 0; t < param.target_count; ++t)
      {
        const namchain::graph::MacroTarget& target = param.targets[t];
        const namchain::render::NodeType* target_type = nullptr;
        for (int n = 0; n < macro->node_count; ++n)
        {
          if (std::string(macro->nodes[n].role) == target.role)
            target_type = namchain::render::find_node_type(macro->nodes[n].type);
        }
        REQUIRE_MESSAGE(target_type != nullptr, macro->id << " target role " << target.role);
        CHECK(node_param(*target_type, target.param) != nullptr);
      }
    }
  }
}

TEST_CASE("every declared macro parameter reaches the graph, and only through its targets")
{
  // The consistency the param map exists to guarantee. Compile with the defaults, compile with one
  // parameter changed, and the difference must be exactly that parameter's declared targets -- so a
  // value written by a macro-level `set_param` is the value the next compile produces.
  for (const MacroType* macro : namchain::graph::all_macro_types())
  {
    CAPTURE(macro->id);
    const Graph defaults = lowered(one_node(macro_node(macro->id, "b1")));

    for (int i = 0; i < macro->param_count; ++i)
    {
      const MacroParamDecl& param = macro->params[i];
      CAPTURE(param.desc.id);

      NodeSpec changed = macro_node(macro->id, "b1");
      if (param.desc.kind == namchain::render::ParamKind::Option)
      {
        if (param.desc.options == nullptr || param.desc.option_count < 2)
          continue; // an option whose values come from the library is exercised by the daemon tests
        changed.strings[param.desc.id] = param.desc.options[param.desc.option_count - 1].value;
      }
      else
      {
        changed.params[param.desc.id] = different_value(param);
      }

      const Graph after = lowered(one_node(changed));
      const std::vector<RoleDifference> changed_roles = differences(defaults, after);
      CHECK_MESSAGE(!changed_roles.empty(), macro->id << "." << param.desc.id << " changed nothing");

      // A parameter with no targets is one the *lowering* consumes -- a band count, a slope -- and it
      // is allowed to change anything, because changing it is a relowering rather than a value being
      // written into a node. A mapped parameter is the other case, and there every difference has to
      // be a declared target: that is the property a macro-level `set_param` depends on.
      if (param.target_count == 0)
        continue;

      for (const RoleDifference& difference : changed_roles)
      {
        for (const std::string& key : difference.params)
        {
          bool declared = false;
          for (int t = 0; t < param.target_count; ++t)
            declared = declared || (difference.role == param.targets[t].role &&
                                    key == std::string(param.targets[t].param));
          CHECK_MESSAGE(declared, macro->id << "." << param.desc.id << " moved " << difference.role << "."
                                            << key << ", which it does not declare");
        }
        for (const std::string& key : difference.strings)
        {
          bool declared = false;
          for (int t = 0; t < param.target_count; ++t)
            declared = declared || (difference.role == param.targets[t].role &&
                                    key == std::string(param.targets[t].param));
          CHECK_MESSAGE(declared, macro->id << "." << param.desc.id << " set " << difference.role << "." << key
                                            << ", which it does not declare");
        }
        // A bypass change is how a parameter with no targets does its work: a band count, a slope.
        // It is only allowed for a parameter the lowering consumes itself.
        CHECK_MESSAGE(!difference.bypass, macro->id << "." << param.desc.id << " bypassed "
                                                       << difference.role
                                                       << ", which is not how a mapped parameter works");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The lowering is the shape it says it is
// ---------------------------------------------------------------------------

TEST_CASE("a lowered node is named by its element and role, and its id is readable")
{
  const Graph graph = lowered(one_node(macro_node("fx.amp.tonestack", "b7f3a1")));
  CHECK(graph.nodes.size() == 5); // in, three biquads, out
  const NodeSpec* bass = by_role(graph, "bass");
  REQUIRE(bass != nullptr);
  CHECK(bass->id == "b7f3a1:bass");
  CHECK(bass->element == "b7f3a1");
  CHECK(bass->role == "bass");
  CHECK(bass->type == "nc.eq.biquad");
  CHECK(bass->strings.at("mode") == "lowshelf");
  CHECK(by_role(graph, "mid")->strings.at("mode") == "peaking");
  CHECK(by_role(graph, "treble")->strings.at("mode") == "highshelf");

  // The edges the client wrote are rewritten to the lowering's ends.
  bool in_to_bass = false;
  bool treble_to_out = false;
  for (const Connection& connection : graph.connections)
  {
    in_to_bass = in_to_bass || (connection.from == "in" && connection.to == "b7f3a1:bass");
    treble_to_out = treble_to_out || (connection.from == "b7f3a1:treble" && connection.to == "out");
  }
  CHECK(in_to_bass);
  CHECK(treble_to_out);

  // And the result is a graph the compiler accepts: a lowering that produced something invalid would
  // be an engine bug, and this is where it would show up.
  const auto validation = namchain::graph::validate(graph);
  CHECK_MESSAGE(validation.ok, validation.message);
}

TEST_CASE("two macros with the same element still mint unique ids")
{
  Graph graph;
  const NodeSpec first = macro_node("fx.eq.tilt", "same");
  NodeSpec second = macro_node("fx.eq.tilt", "same");
  second.id = "second";
  second.role = "second";
  graph.nodes = {node("in", "nc.input"), first, second, node("out", "nc.output")};
  graph.connections = {{"in", 0, "same", 0}, {"same", 0, "second", 0}, {"second", 0, "out", 0}};

  const Graph primitives = lowered(graph);
  std::vector<std::string> ids;
  for (const NodeSpec& spec : primitives.nodes)
    ids.push_back(spec.id);
  std::sort(ids.begin(), ids.end());
  CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
  CHECK(namchain::graph::validate(primitives).ok);
}

TEST_CASE("lowering a graph with no macros copies it")
{
  Graph graph;
  graph.nodes = {node("in", "nc.input"), node("g", "nc.gain"), node("out", "nc.output")};
  graph.connections = {{"in", 0, "g", 0}, {"g", 0, "out", 0}};
  graph.nodes[1].params["gain_db"] = -3.0;

  const Graph primitives = lowered(graph);
  REQUIRE(primitives.nodes.size() == 3);
  CHECK(primitives.nodes[1].id == "g");
  CHECK(primitives.nodes[1].params.at("gain_db") == -3.0);
  CHECK(primitives.connections.size() == 2);
}

// ---------------------------------------------------------------------------
// A macro renders what the hand-built graph renders
// ---------------------------------------------------------------------------

TEST_CASE("fx.eq.tilt is the two shelves it stands for")
{
  const std::vector<float> input = test_signal(4096);

  // The macro: one knob that cuts one end and boosts the other by half the tilt each way.
  const Graph macros = one_node(macro_node("fx.eq.tilt", "eq", {{"tilt", 6.0}, {"freq", 800.0}}));

  // The same thing written out by hand, which is the specification the lowering has to meet.
  Graph hand;
  {
    NodeSpec low = node("low", "nc.eq.biquad");
    low.strings["mode"] = "lowshelf";
    low.params["freq"] = 800.0;
    low.params["q"] = 0.707;
    low.params["gain_db"] = -3.0;
    NodeSpec high = node("high", "nc.eq.biquad");
    high.strings["mode"] = "highshelf";
    high.params["freq"] = 800.0;
    high.params["q"] = 0.707;
    high.params["gain_db"] = 3.0;
    hand.nodes = {node("in", "nc.input"), low, high, node("out", "nc.output")};
    hand.connections = {{"in", 0, "low", 0}, {"low", 0, "high", 0}, {"high", 0, "out", 0}};
  }

  const std::vector<float> from_macro = render(macros, input);
  const std::vector<float> from_hand = render(hand, input);
  REQUIRE(from_macro.size() == from_hand.size());
  for (std::size_t i = 0; i < from_macro.size(); ++i)
    REQUIRE(from_macro[i] == from_hand[i]); // sample-identical: a rewrite, not an approximation
}

TEST_CASE("fx.filter.cut is cascaded Butterworth stages")
{
  const std::vector<float> input = test_signal(4096);

  const Graph macros = one_node(macro_node("fx.filter.cut", "cut", {{"low_cut", 120.0}, {"high_cut", 6000.0}},
                                             {{"low_slope", "24"}, {"high_slope", "12"}}));

  // 24 dB/oct is two highpass stages at the Butterworth Qs for a 4th-order response; 12 dB/oct is one
  // lowpass at Q = 0.707. The numbers are the point of the macro, so they are written out here.
  Graph hand;
  {
    NodeSpec low1 = node("low1", "nc.eq.biquad");
    low1.strings["mode"] = "highpass";
    low1.params["freq"] = 120.0;
    low1.params["q"] = 0.5411961001461971;
    NodeSpec low2 = node("low2", "nc.eq.biquad");
    low2.strings["mode"] = "highpass";
    low2.params["freq"] = 120.0;
    low2.params["q"] = 1.3065629648763766;
    NodeSpec high1 = node("high1", "nc.eq.biquad");
    high1.strings["mode"] = "lowpass";
    high1.params["freq"] = 6000.0;
    high1.params["q"] = 0.7071067811865476;
    hand.nodes = {node("in", "nc.input"), low1, low2, high1, node("out", "nc.output")};
    hand.connections = {{"in", 0, "low1", 0},
                        {"low1", 0, "low2", 0},
                        {"low2", 0, "high1", 0},
                        {"high1", 0, "out", 0}};
  }

  const std::vector<float> from_macro = render(macros, input);
  const std::vector<float> from_hand = render(hand, input);
  for (std::size_t i = 0; i < from_macro.size(); ++i)
    REQUIRE(from_macro[i] == from_hand[i]);

  // The stages beyond the slope are bypassed rather than absent, so the shape -- and the addresses a
  // client holds -- do not change when the slope does.
  const Graph primitives = lowered(macros);
  CHECK(by_role(primitives, "low1")->bypass == false);
  CHECK(by_role(primitives, "low2")->bypass == false);
  CHECK(by_role(primitives, "low3")->bypass == true);
  CHECK(by_role(primitives, "low4")->bypass == true);
  CHECK(by_role(primitives, "high1")->bypass == false);
  CHECK(by_role(primitives, "high2")->bypass == true);
}

TEST_CASE("fx.mix.drywet crossfades two lanes and sums back to the input")
{
  const std::vector<float> input = test_signal(2048);

  // Both lanes carry the same signal, so a crossfade has to reproduce it exactly at any mix: that is
  // what makes this macro one knob rather than two.
  Graph macros;
  {
    NodeSpec split = node("split", "nc.split");
    split.params["outputs"] = 2.0;
    macros.nodes = {node("in", "nc.input"), split, macro_node("fx.mix.drywet", "dw", {{"mix", 0.25}}),
                    node("out", "nc.output")};
    macros.connections = {{"in", 0, "split", 0}, {"split", 0, "dw", 0}, {"split", 1, "dw", 1},
                          {"dw", 0, "out", 0}};
  }
  const std::vector<float> output = render(macros, input);
  for (std::size_t i = 0; i < input.size(); ++i)
    REQUIRE(output[i] == doctest::Approx(input[i]).epsilon(1e-6));

  // And the knob really is a crossfade: at mix = 0 the wet lane is silent, at 1 the dry one is.
  const Graph primitives = lowered(macros);
  CHECK(by_role(primitives, "dry")->params.at("gain") == doctest::Approx(0.75));
  CHECK(by_role(primitives, "wet")->params.at("gain") == doctest::Approx(0.25));
}

TEST_CASE("fx.delay.feedback is a delay with feedback and a dry/wet pair")
{
  const std::vector<float> input = test_signal(4096);

  const Graph macros = one_node(
    macro_node("fx.delay.feedback", "dly", {{"time", 2.0}, {"feedback", 0.5}, {"mix", 1.0}}));

  // The hand-built version, from the engine's own feedback idiom (§5.1): sum -> delay -> out, with the
  // delay's output fed back through a gain and a unit delay, which is what breaks the cycle.
  Graph hand;
  {
    NodeSpec sum = node("sum", "nc.mix");
    sum.params["inputs"] = 2.0;
    NodeSpec line = node("line", "nc.delay");
    line.params["ms"] = 2.0;
    line.params["max_ms"] = 1000.0;
    NodeSpec feedback = node("fb", "nc.gain");
    feedback.params["gain"] = 0.5;
    NodeSpec tap = node("tap", "nc.unit_delay");
    hand.nodes = {node("in", "nc.input"), sum, line, feedback, tap, node("out", "nc.output")};
    hand.connections = {{"in", 0, "sum", 0}, {"sum", 0, "line", 0}, {"line", 0, "out", 0},
                        {"line", 0, "fb", 0}, {"fb", 0, "tap", 0}, {"tap", 0, "sum", 1}};
  }

  const std::vector<float> from_macro = render(macros, input);
  const std::vector<float> from_hand = render(hand, input);
  for (std::size_t i = 0; i < from_macro.size(); ++i)
    REQUIRE(from_macro[i] == from_hand[i]);

  // With feedback the repeats come back: the impulse at sample 0 is audible again a delay later.
  const std::size_t repeat = 96; // 2 ms at 48 kHz
  CHECK(std::fabs(from_macro[repeat]) > 0.1f);

  // The delay line is a declared capacity, not a hope: the macro asks for 1000 ms and the node's own
  // check refuses anything longer, so the param map can never ask for a time the line cannot hold.
  CHECK(by_role(lowered(macros), "line")->params.at("max_ms") == 1000.0);
}

TEST_CASE("fx.amp.tonestack and fx.cab.ir are the chains they stand for")
{
  const std::vector<float> input = test_signal(2048);

  const Graph tone = one_node(macro_node("fx.amp.tonestack", "ts", {{"bass", 4.0},
                                                                   {"mid", -3.0},
                                                                   {"treble", 2.0},
                                                                   {"mid_freq", 800.0}}));
  Graph tone_hand;
  {
    NodeSpec bass = node("bass", "nc.eq.biquad");
    bass.strings["mode"] = "lowshelf";
    bass.params["freq"] = 120.0;
    bass.params["q"] = 0.707;
    bass.params["gain_db"] = 4.0;
    NodeSpec mid = node("mid", "nc.eq.biquad");
    mid.strings["mode"] = "peaking";
    mid.params["freq"] = 800.0;
    mid.params["q"] = 0.7;
    mid.params["gain_db"] = -3.0;
    NodeSpec treble = node("treble", "nc.eq.biquad");
    treble.strings["mode"] = "highshelf";
    treble.params["freq"] = 3500.0;
    treble.params["q"] = 0.707;
    treble.params["gain_db"] = 2.0;
    tone_hand.nodes = {node("in", "nc.input"), bass, mid, treble, node("out", "nc.output")};
    tone_hand.connections = {{"in", 0, "bass", 0},
                             {"bass", 0, "mid", 0},
                             {"mid", 0, "treble", 0},
                             {"treble", 0, "out", 0}};
  }
  const std::vector<float> from_macro = render(tone, input);
  const std::vector<float> from_hand = render(tone_hand, input);
  for (std::size_t i = 0; i < from_macro.size(); ++i)
    REQUIRE(from_macro[i] == from_hand[i]);

  // The cabinet: a high-pass, the IR, a low-pass and a level, using a real IR from the generated
  // fixtures -- this is a test about the shape and the cuts, not about the loader.
  const std::string ir = std::string(NAMCHAIN_TEST_DATA_DIR) + "/CabIR.nam";
  const Graph cab = one_node(macro_node("fx.cab.ir", "cab",
                                        {{"low_cut", 80.0}, {"high_cut", 8000.0}, {"level_db", -2.0}},
                                        {{"ir", ir}}));
  Graph cab_hand;
  {
    NodeSpec low = node("low", "nc.eq.biquad");
    low.strings["mode"] = "highpass";
    low.params["freq"] = 80.0;
    low.params["q"] = 0.707;
    NodeSpec convolution = node("ir", "nc.cab.ir");
    convolution.strings["ir"] = ir;
    NodeSpec high = node("high", "nc.eq.biquad");
    high.strings["mode"] = "lowpass";
    high.params["freq"] = 8000.0;
    high.params["q"] = 0.707;
    NodeSpec level = node("level", "nc.gain");
    level.params["gain_db"] = -2.0;
    cab_hand.nodes = {node("in", "nc.input"), low, convolution, high, level, node("out", "nc.output")};
    cab_hand.connections = {{"in", 0, "low", 0},    {"low", 0, "ir", 0},   {"ir", 0, "high", 0},
                            {"high", 0, "level", 0}, {"level", 0, "out", 0}};
  }
  const std::vector<float> cab_macro = render(cab, input);
  const std::vector<float> cab_manual = render(cab_hand, input);
  for (std::size_t i = 0; i < cab_macro.size(); ++i)
    REQUIRE(cab_macro[i] == cab_manual[i]);
}

TEST_CASE("fx.eq.bands is four bands, and `bands` bypasses the ones it does not count")
{
  const std::vector<float> input = test_signal(2048);
  const Graph four = lowered(one_node(macro_node("fx.eq.bands", "eq", {{"bands", 4.0}, {"gain2", 6.0}})));
  for (int i = 1; i <= 4; ++i)
  {
    const NodeSpec* band = by_role(four, "band" + std::to_string(i));
    REQUIRE(band != nullptr);
    CHECK(band->bypass == false);
  }
  CHECK(by_role(four, "band1")->strings.at("mode") == "lowshelf");
  CHECK(by_role(four, "band2")->strings.at("mode") == "peaking");
  CHECK(by_role(four, "band4")->strings.at("mode") == "highshelf");
  CHECK(by_role(four, "band2")->params.at("gain_db") == 6.0);

  const Graph two = lowered(one_node(macro_node("fx.eq.bands", "eq", {{"bands", 2.0}, {"gain2", 6.0}})));
  CHECK(by_role(two, "band1")->bypass == false);
  CHECK(by_role(two, "band2")->bypass == false);
  CHECK(by_role(two, "band3")->bypass == true);
  CHECK(by_role(two, "band4")->bypass == true);

  // A bypassed band is transparent, which is the whole reason a selection bypasses rather than
  // removes: a flat EQ must pass the signal untouched, whatever the band count.
  const Graph flat_bands = lowered(
    one_node(macro_node("fx.eq.bands", "eq", {{"bands", 1.0}, {"gain1", 0.0}})));
  // A 0 dB biquad is unity mathematically and not bit-exactly -- the recursive part rounds -- so the
  // tolerance here is 1e-3, about -60 dBFS of error. Transparency by *substitution* (bypass) is
  // bit-exact and is asserted as such elsewhere; this is the arithmetic case.
  const std::vector<float> flat = render(flat_bands, input);
  for (std::size_t i = 0; i < input.size(); ++i)
    REQUIRE(flat[i] == doctest::Approx(input[i]).epsilon(1e-3));
}

// ---------------------------------------------------------------------------
// Bypass
// ---------------------------------------------------------------------------

TEST_CASE("bypassing a macro follows its declared rule and passes the signal")
{
  const std::vector<float> input = test_signal(2048);

  for (const MacroType* macro : namchain::graph::all_macro_types())
  {
    CAPTURE(macro->id);
    if (macro->inputs != 1 || macro->outputs != 1)
      continue; // a two-input macro has no single dry path: `fx.mix.drywet` is checked separately

    NodeSpec spec = macro_node(macro->id, "b1");
    spec.bypass = true;
    const Graph primitives = lowered(one_node(spec));

    if (macro->bypass == MacroBypass::AllRolesCarry)
    {
      for (const NodeSpec& lowered_spec : primitives.nodes)
      {
        if (lowered_spec.type == "nc.input" || lowered_spec.type == "nc.output")
          continue;
        CHECK_MESSAGE(lowered_spec.bypass, lowered_spec.id << " did not carry the flag");
      }
    }
    else
    {
      const NodeSpec* dry = by_role(primitives, macro->dry_role);
      const NodeSpec* wet = by_role(primitives, macro->wet_role);
      REQUIRE(dry != nullptr);
      REQUIRE(wet != nullptr);
      CHECK(dry->params.at("gain") == 1.0);
      CHECK(wet->params.at("gain") == 0.0);
      CHECK(dry->bypass == false); // the crossfade is in the graph, not a flag on one node
    }

    // The audible consequence either way: a bypassed macro passes its input, sample for sample.
    const std::vector<float> output = render(one_node(spec), input);
    for (std::size_t i = 0; i < input.size(); ++i)
      REQUIRE(output[i] == doctest::Approx(input[i]).epsilon(1e-5));
  }
}

TEST_CASE("a bypassed macro keeps running")
{
  // The rule the engine has had since G14, now for macros: bypass substitutes the output, it does not
  // stop the node. A delay line that was stopped would have silence in its buffer and un-bypassing it
  // would be an audible hole, so the wet path stays fed and the flag only decides what reaches out.
  NodeSpec spec = macro_node("fx.delay.feedback", "dly", {{"time", 2.0}, {"feedback", 0.8}, {"mix", 1.0}});
  spec.bypass = true;

  namchain::rt::Arena arena(1u << 24);
  std::string error;
  auto plan = compile_plan(one_node(spec), arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);

  std::vector<float> input(kQuantum, 0.0f);
  std::vector<float> output(kQuantum, 0.0f);
  input[0] = 1.0f;
  plan->execute(input.data(), output.data(), kQuantum);
  CHECK(output[0] == doctest::Approx(1.0f)); // bypassed: the dry signal comes out

  std::fill(input.begin(), input.end(), 0.0f);
  for (int block = 1; block < 10; ++block)
    plan->execute(input.data(), output.data(), kQuantum);
  CHECK(std::fabs(output[0]) < 1e-6f); // still bypassed: silence out

  // Un-bypass the way a patch does: the crossfade gains, not the flag.
  std::size_t node_index = 0;
  uint32_t param_id = 0;
  REQUIRE(plan->resolve_param("dly:dry", "gain", node_index, param_id));
  CHECK(plan->set_param(node_index, param_id, 0.0f));
  REQUIRE(plan->resolve_param("dly:wet", "gain", node_index, param_id));
  CHECK(plan->set_param(node_index, param_id, 1.0f));

  // The line was fed the whole time, so the decaying repeats are still arriving.
  bool heard = false;
  for (int block = 0; block < 40 && !heard; ++block)
  {
    plan->execute(input.data(), output.data(), kQuantum);
    for (int i = 0; i < kQuantum; ++i)
      heard = heard || std::fabs(output[i]) > 0.02f;
  }
  CHECK_MESSAGE(heard, "the delay line was not running while the macro was bypassed");
}

TEST_CASE("fx.mix.drywet bypassed is all dry")
{
  float dry_gain = 0.0f;
  float wet_gain = 0.0f;
  for (const bool bypassed : {false, true})
  {
    NodeSpec spec = macro_node("fx.mix.drywet", "dw", {{"mix", 0.75}});
    spec.bypass = bypassed;
    const Graph primitives = lowered(one_node(spec));
    if (bypassed)
    {
      dry_gain = static_cast<float>(by_role(primitives, "dry")->params.at("gain"));
      wet_gain = static_cast<float>(by_role(primitives, "wet")->params.at("gain"));
    }
    else
    {
      CHECK(by_role(primitives, "dry")->params.at("gain") == doctest::Approx(0.25));
      CHECK(by_role(primitives, "wet")->params.at("gain") == doctest::Approx(0.75));
    }
  }
  CHECK(dry_gain == 1.0f);
  CHECK(wet_gain == 0.0f);
}

// ---------------------------------------------------------------------------
// Transforms and ids
// ---------------------------------------------------------------------------

TEST_CASE("the transform set does what it says")
{
  using namchain::graph::apply_transform;
  using namchain::graph::Transform;

  CHECK(apply_transform(Transform::Direct, 0.25, 0.0) == 0.25);
  CHECK(apply_transform(Transform::OneMinus, 0.25, 0.0) == 0.75);
  CHECK(apply_transform(Transform::ScaleBy, 6.0, -0.5) == -3.0);

  // dB <-> linear at the points that matter: unity is 0 dB, and half is about -6.
  CHECK(apply_transform(Transform::DbToLinear, 0.0, 0.0) == doctest::Approx(1.0));
  CHECK(apply_transform(Transform::DbToLinear, -6.020599913, 0.0) == doctest::Approx(0.5));
  CHECK(apply_transform(Transform::DbToLinear, 6.020599913, 0.0) == doctest::Approx(2.0));
  CHECK(apply_transform(Transform::LinearToDb, 1.0, 0.0) == doctest::Approx(0.0));
  CHECK(apply_transform(Transform::LinearToDb, 0.5, 0.0) == doctest::Approx(-6.020599913).epsilon(1e-6));
  // Silence is a floor rather than an infinity: a parameter has to hold a finite number.
  CHECK(apply_transform(Transform::LinearToDb, 0.0, 0.0) == -120.0);
  CHECK(std::isfinite(apply_transform(Transform::LinearToDb, 0.0, 0.0)));
}

TEST_CASE("minted ids are readable, deterministic, and unique")
{
  CHECK(namchain::graph::mint_node_id("b1", "line", {}) == "b1:line");
  CHECK(namchain::graph::mint_node_id("b1", "line", {"b1:line"}) == "b1:line~2");
  CHECK(namchain::graph::mint_node_id("b1", "line", {"b1:line", "b1:line~2"}) == "b1:line~3");
  // A node with no element still gets something diagnosable.
  CHECK(namchain::graph::mint_node_id("", "line", {}) == "line");
}
