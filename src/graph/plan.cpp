#include "namchain/graph/plan.h"

#include <algorithm>
#include <deque>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "namchain/graph/macro.h"
#include "namchain/graph/rig.h"

namespace namchain
{
namespace render
{

bool resolve_ports(const NodeType& type, const graph::NodeSpec& spec, PortCounts& ports, std::string& error)
{
  ports.inputs = type.inputs;
  ports.outputs = type.outputs;
  ports.channels = type.channels;

  // Declared and refused (docs/ui-integration-plan.md §1.4). Every built-in is mono today, so this is
  // unreachable -- which is the point of writing it now rather than on the day the first stereo node
  // appears: the error already exists, it says what the audio path cannot carry, and the schema
  // already publishes the number a client sees.
  if (type.channels != 1)
  {
    error = "node type '" + type.id + "' declares " + std::to_string(type.channels) +
            " channels per port; the engine's audio path is mono, and multichannel ports arrive with "
            "the multichannel work";
    return false;
  }

  if (type.arity_param == nullptr)
    return true;

  // The param names the count it overrides. Anything else is a vtable bug, not a rig error, so say
  // which type so the message points at the node implementation rather than at the rig author.
  const bool over_inputs = std::strcmp(type.arity_param, "inputs") == 0;
  const bool over_outputs = std::strcmp(type.arity_param, "outputs") == 0;
  if (!over_inputs && !over_outputs)
  {
    error = "node type '" + type.id + "' declares an unknown arity param '" + type.arity_param + "'";
    return false;
  }

  int count = over_inputs ? type.inputs : type.outputs;
  const auto found = spec.params.find(type.arity_param);
  if (found != spec.params.end())
  {
    const double value = found->second;
    // A port count is a count, so 2.5 is a mistake and 2.0 written as a float is not. Rejecting the
    // former rather than truncating it keeps "why is my split only fanning out to two?" from being a
    // bug report about the parser.
    if (!std::isfinite(value) || std::floor(value) != value)
    {
      error = "node '" + spec.id + "': '" + type.arity_param + "' must be a whole number";
      return false;
    }
    count = static_cast<int>(value);
  }

  if (count < type.min_ports || count > type.max_ports)
  {
    error = "node '" + spec.id + "': '" + type.arity_param + "' must be between " +
            std::to_string(type.min_ports) + " and " + std::to_string(type.max_ports) + ", got " +
            std::to_string(count);
    return false;
  }

  if (over_inputs)
    ports.inputs = count;
  else
    ports.outputs = count;
  return true;
}

} // namespace render

namespace graph
{

Validation Validation::failure(std::string reason)
{
  Validation result;
  result.ok = false;
  result.message = std::move(reason);
  return result;
}

void substitute_dry(PlanNode& node, int frames) noexcept
{
  // The input terminator means "at least one real input" is `size() > 1`, and inputs[0] is bound to
  // the silence buffer rather than left null for an unconnected port -- so a dry copy is always
  // reading valid memory.
  const bool has_input = node.inputs.size() > 1 && node.inputs[0] != nullptr;
  for (std::size_t port = 0; port + 1 < node.outputs.size(); ++port)
  {
    if (has_input)
      std::copy(node.inputs[0], node.inputs[0] + frames, node.outputs[port]);
    else
      std::fill(node.outputs[port], node.outputs[port] + frames, 0.0f);
  }
}

namespace
{

constexpr float kHalfPi = 1.57079632679489661923f;

/// A bypass crossfade's next \p frames: the node's output (already written) and its input, with the
/// same equal-power law as rt::Crossfade and the same exact landing on the last frame. Once the fade
/// closes, a bypassed node's remaining frames are its input.
void fade_bypass(PlanNode& node, int frames) noexcept
{
  // A node with no output port has no signal to fade: it is a measurement sink (`nc.meter`, were it
  // built) or a node the graph validated away. Nothing to do, and `node.outputs[0]` would be the
  // terminator.
  if (node.outputs.size() <= 1)
    return;
  const float* dry = node.inputs.size() > 1 ? node.inputs[0] : nullptr;
  float* out = node.outputs[0];
  const int length = node.bypass_fade;
  int i = 0;
  for (; i < frames && node.bypass_position < length; ++i, ++node.bypass_position)
  {
    const float t = length > 1 ? static_cast<float>(node.bypass_position) / static_cast<float>(length - 1) : 1.0f;
    const float from = std::cos(kHalfPi * t);
    const float to = std::sin(kHalfPi * t);
    const float wet = out[i];
    const float input = dry != nullptr ? dry[i] : 0.0f;
    out[i] = node.bypass ? wet * from + input * to : input * from + wet * to;
  }
  if (node.bypass_position < length)
    return;
  node.bypass_fade = 0;
  node.bypass_position = 0;
  if (node.bypass)
  {
    for (; i < frames; ++i)
      out[i] = dry != nullptr ? dry[i] : 0.0f;
  }
}

/// All of a node's outputs finite? The non-finite guard's scan: one compare per sample, on buffers
/// the node has just written and which are therefore hot in cache.
bool outputs_finite(const PlanNode& node, int frames) noexcept
{
  for (std::size_t port = 0; port + 1 < node.outputs.size(); ++port)
  {
    const float* out = node.outputs[port];
    for (int i = 0; i < frames; ++i)
      if (!std::isfinite(out[i]))
        return false;
  }
  return true;
}

/// The level probe's scan, for a node whose output is the one downstream nodes will read.
///
/// One probe per node, over the node's first output port -- the ports are separate signals, but a
/// meter answers "how loud is this node", and a node with several outputs (a split) is measured on
/// the one its consumers take first. Reading every port would report a sum that never existed in
/// the signal path.
///
/// **After** a bypass substitution, not before: what a downstream node reads is the dry signal
/// while a node is bypassed, so the level a client sees is the level that is actually there. The
/// scan is a compare and an add per sample on a buffer just written, which is the cheapest a probe
/// can be, and it is the same walk `outputs_finite` already makes -- kept separate rather than
/// fused so the guard stays a guard (it can return early; this cannot).
void measure_node(const PlanNode& node, level::Probe* probe, int frames, bool first_frame_only) noexcept
{
  if (probe == nullptr || node.outputs.size() <= 1)
    return;
  probe->accumulate(node.outputs[0], frames, first_frame_only);
}

/// Every node's resolved type and port counts, keyed by id, plus its position in `graph.nodes`.
///
/// The ports are stored beside the type rather than read from it later because a per-spec arity
/// (`nc.split`/`nc.mix`) means `type->inputs`/`type->outputs` are only a default: the graph's
/// meaning lives in the resolved counts, and every pass -- validation, liveness, colouring, binding
/// -- has to use the same ones.
struct Indexed
{
  std::map<std::string, int> position;
  /// The type of each node, as every later pass wants to read it. A macro node gets a *synthesised*
  /// `NodeType` view (id, ports, category) so the port logic, the dependency walk and the cycle check
  /// do not each need a second branch: a macro is a black box with a declared arity until it is
  /// lowered, and that is exactly what this describes.
  std::vector<const render::NodeType*> types;
  /// The macro behind a synthesised view, or null for a primitive. The parameter lint needs the real
  /// declaration, because its param table carries a param map as well as a description.
  std::vector<const graph::MacroType*> macros;
  std::vector<render::PortCounts> ports;
  /// Storage for the synthesised views; a deque because `types` holds pointers into it.
  std::deque<render::NodeType> macro_views;
};

Indexed index_graph(const Graph& graph, Validation& validation)
{
  Indexed indexed;
  indexed.types.reserve(graph.nodes.size());
  indexed.ports.reserve(graph.nodes.size());

  for (const NodeSpec& node : graph.nodes)
  {
    if (node.id.empty())
    {
      validation = Validation::failure("a node has an empty id");
      return indexed;
    }
    if (indexed.position.count(node.id) != 0)
    {
      validation = Validation::failure("duplicate node id '" + node.id + "'");
      return indexed;
    }
    const graph::MacroType* macro = graph::find_macro_type(node.type);
    const render::NodeType* type = render::find_node_type(node.type);
    if (type == nullptr && macro == nullptr)
    {
      validation = Validation::failure("unknown node type '" + node.type + "' on node '" + node.id + "'");
      return indexed;
    }
    if (macro != nullptr)
    {
      render::NodeType view;
      view.id = macro->id;
      view.name = macro->name;
      view.category = macro->category;
      view.inputs = macro->inputs;
      view.outputs = macro->outputs;
      view.min_ports = macro->inputs;
      view.max_ports = macro->inputs;
      indexed.macro_views.push_back(view);
      type = &indexed.macro_views.back();
    }
    // A bypass replaces the node's output with its input, which is only a signal for a 1->1 node: a
    // fan-out has several outputs and a mixer several inputs, and picking one would be a guess. The
    // check is here because this is the one place every pass gets its types from.
    if (node.bypass && (type->inputs != 1 || type->outputs != 1))
    {
      validation = Validation::failure("node '" + node.id + "' (" + type->id +
                                       ") cannot be bypassed: only 1->1 nodes have a dry path");
      return indexed;
    }
    render::PortCounts ports;
    if (!render::resolve_ports(*type, node, ports, validation.message))
    {
      validation.ok = false;
      return indexed;
    }
    indexed.position[node.id] = static_cast<int>(indexed.types.size());
    indexed.types.push_back(type);
    indexed.macros.push_back(macro);
    indexed.ports.push_back(ports);
  }
  return indexed;
}

/// The scheduling dependency graph, with block-delay nodes treated as sources.
///
/// An edge *into* a block-delay node is not a dependency: that node reads whatever its input buffer
/// already holds, which is the previous block's value, so it does not have to wait for the producer
/// to run. Its own outputs are ordinary dependencies. This is what turns a feedback loop into a
/// DAG (§5.1), and dropping the edge on this side is what makes the delay come out as exactly one
/// block rather than two.
void build_dependencies(const Graph& graph, const Indexed& indexed, std::vector<std::vector<int>>& successors,
                        std::vector<int>& indegree)
{
  const std::size_t n = graph.nodes.size();
  successors.assign(n, {});
  indegree.assign(n, 0);

  std::map<std::pair<int, int>, bool> seen;
  for (const Connection& connection : graph.connections)
  {
    const int from = indexed.position.at(connection.from);
    const int to = indexed.position.at(connection.to);
    if (indexed.types[static_cast<std::size_t>(to)]->block_delay)
      continue;
    if (seen.emplace(std::make_pair(from, to), true).second)
    {
      successors[static_cast<std::size_t>(from)].push_back(to);
      ++indegree[static_cast<std::size_t>(to)];
    }
  }
}

/// The next node to place: a block-delay node if one is ready, otherwise the lowest ready index.
///
/// Block-delay nodes have no counted predecessors, so they are ready from the start; taking them
/// first is what guarantees they read the previous block. Without this a graph that is *not* a
/// cycle -- `nc.input -> nc.unit_delay -> nc.output` -- would sample the current block and delay by
/// nothing, because the input node happens to be declared first.
int next_ready(const Indexed& indexed, const std::vector<int>& indegree, const std::vector<bool>& placed)
{
  for (std::size_t i = 0; i < placed.size(); ++i)
    if (!placed[i] && indegree[i] == 0 && indexed.types[i]->block_delay)
      return static_cast<int>(i);
  for (std::size_t i = 0; i < placed.size(); ++i)
    if (!placed[i] && indegree[i] == 0)
      return static_cast<int>(i);
  return -1;
}

} // namespace

namespace
{



bool option_matches(const render::ParamDesc& param, const std::string& value)
{
  for (int i = 0; i < param.option_count; ++i)
  {
    if (value == param.options[i].value)
      return true;
  }
  for (int i = 0; i < param.alias_count; ++i)
  {
    if (value == param.aliases[i])
      return true;
  }
  return false;
}

/// \brief Check one node's parameters against its type's schema.
///
/// This is the rule that stops a parameter name meaning nothing: an unknown key, a value outside the
/// declared bounds, an option outside the declared set and a missing required parameter are all
/// rejected here, with the accepted set in the message. Before this, a rig could say `"gainz": 3` and
/// be published happily -- which from a UI is a knob that does nothing
/// (docs/ljoss-comparison.md §3.5).
/// One entry of a parameter table: a primitive's is the `ParamDesc` itself, a macro's carries the
/// param map beside it. The lint reads only the description, so it is the same lint for both -- which
/// is what "a client cannot tell a macro from a primitive except by kind" has to mean in practice.
const render::ParamDesc& param_desc(const render::ParamDesc& entry)
{
  return entry;
}

const render::ParamDesc& param_desc(const graph::MacroParamDecl& entry)
{
  return entry.desc;
}

template <typename Entry>
std::string declared_params(const Entry* params, int count)
{
  std::string list;
  for (int i = 0; i < count; ++i)
  {
    if (i != 0)
      list += ", ";
    const std::string name = param_desc(params[i]).id;
    list += "'" + name + "'";
    if (param_desc(params[i]).hidden)
      list += " (alias)";
  }
  return list.empty() ? std::string("no parameters") : list;
}

template <typename Entry>
Validation lint_params(const NodeSpec& spec, const Entry* params, int param_count, const std::string& type_id)
{
  const std::string where = "node '" + spec.id + "' (" + type_id + ")";
  std::vector<bool> seen(static_cast<std::size_t>(param_count), false);

  for (const auto& [key, value] : spec.params)
  {
    int index = -1;
    for (int i = 0; i < param_count; ++i)
    {
      if (key == param_desc(params[i]).id)
        index = i;
    }
    if (index < 0)
      return Validation::failure(where + " has no parameter '" + key + "'; it takes " +
                                 declared_params(params, param_count));

    const render::ParamDesc& param = param_desc(params[index]);
    seen[static_cast<std::size_t>(index)] = true;
    if (param.kind == render::ParamKind::Option)
      return Validation::failure(where + ": '" + key + "' is an option, and takes a string");

    if (param.has_min)
    {
      const bool below = param.min_inclusive ? value < param.min : value <= param.min;
      if (below)
      {
        return Validation::failure(where + ": '" + key + "' is " + std::to_string(value) + ", and the schema's " +
                                   (param.min_inclusive ? "minimum is " : "minimum is more than ") +
                                   std::to_string(param.min));
      }
    }
    if (param.has_max)
    {
      const bool above = param.max_inclusive ? value > param.max : value >= param.max;
      if (above)
      {
        return Validation::failure(where + ": '" + key + "' is " + std::to_string(value) + ", and the schema's " +
                                   (param.max_inclusive ? "maximum is " : "maximum is less than ") +
                                   std::to_string(param.max));
      }
    }
    // A stepped parameter is a count, not a quantity: half a sample is not a shorter delay, it is a
    // number the node would silently round. The grid is measured from the minimum when there is one,
    // which is where a count starts.
    if (param.step > 0.0 && param.snap)
    {
      const double steps = (value - (param.has_min ? param.min : 0.0)) / param.step;
      if (std::abs(steps - std::llround(steps)) > 1e-6)
        return Validation::failure(where + ": '" + key + "' must be a multiple of " + std::to_string(param.step));
    }
  }

  for (const auto& [key, value] : spec.strings)
  {
    int index = -1;
    for (int i = 0; i < param_count; ++i)
    {
      if (key == param_desc(params[i]).id)
        index = i;
    }
    if (index < 0)
      return Validation::failure(where + " has no parameter '" + key + "'; it takes " +
                                 declared_params(params, param_count));

    const render::ParamDesc& param = param_desc(params[index]);
    seen[static_cast<std::size_t>(index)] = true;
    if (param.kind != render::ParamKind::Option)
      return Validation::failure(where + ": '" + key + "' is not an option, and takes a number");
    // An option whose values come from the device's registry (`model`, `ir`) cannot be checked here:
    // the registry is slice D, and until then any non-empty name is accepted and the loader is what
    // says no.
    if (param.options_from == nullptr && !option_matches(param, value))
    {
      std::string list;
      for (int i = 0; i < param.option_count; ++i)
      {
        if (i != 0)
          list += ", ";
        list += std::string("'") + param.options[i].value + "'";
      }
      return Validation::failure(where + ": '" + key + "' is '" + value + "'; it takes " + list);
    }
  }

  for (int i = 0; i < param_count; ++i)
  {
    const render::ParamDesc& param = param_desc(params[i]);
    if (param.required && !seen[static_cast<std::size_t>(i)])
      return Validation::failure(where + " needs '" + param.id + "'");
  }

  // A `one_of` group is a set of alternatives: giving two of them is a contradiction -- there is no
  // precedence rule in the schema, and inventing one would make a rig's meaning depend on an
  // implementation detail -- and whether one of them is required is a property of the group.
  for (int i = 0; i < param_count; ++i)
  {
    const render::ParamDesc& param = param_desc(params[i]);
    if (param.one_of == nullptr)
      continue;

    std::string group;
    std::string given;
    int given_count = 0;
    int member_count = 0;
    for (int j = 0; j < param_count; ++j)
    {
      const render::ParamDesc& other = param_desc(params[j]);
      if (other.one_of == nullptr || std::string(other.one_of) != param.one_of)
        continue;
      ++member_count;
      if (!group.empty())
        group += " or ";
      group += std::string("'") + other.id + "'";
      if (seen[static_cast<std::size_t>(j)])
      {
        ++given_count;
        if (!given.empty())
          given += " and ";
        given += std::string("'") + other.id + "'";
      }
    }
    if (given_count > 1)
      return Validation::failure(where + ": " + given + " are alternatives; give one");
    if (given_count == 0 && param.one_of_required)
      return Validation::failure(where + " needs one of " + group);
    // One pass per group, not per member.
    i += member_count - 1;
  }

  return Validation{};
}

} // namespace

Validation validate(const Graph& graph)
{
  if (graph.nodes.empty())
    return Validation::failure("graph has no nodes");

  Validation validation;
  const Indexed indexed = index_graph(graph, validation);
  if (!validation.ok)
    return validation;

  // Parameters are checked against the type's schema before anything else looks at the graph: a node
  // whose params do not mean anything is a mistake the author can fix, and finding it here is what
  // keeps it out of the render path's silent-ignore behaviour.
  for (const NodeSpec& node : graph.nodes)
  {
    const std::size_t index = static_cast<std::size_t>(indexed.position.at(node.id));
    const render::NodeType* type = indexed.types[index];
    // A macro's parameters are checked against the macro's own schema -- the same lint, the same
    // messages, and a param map beside each description that the lint does not need to know about.
    const Validation lint = indexed.macros[index] != nullptr
                              ? lint_params(node, indexed.macros[index]->params,
                                            indexed.macros[index]->param_count, indexed.macros[index]->id)
                              : lint_params(node, type->params, type->param_count, type->id);
    if (!lint.ok)
      return lint;
  }

  int inputs = 0;
  int outputs = 0;
  for (const NodeSpec& node : graph.nodes)
  {
    const render::NodeType* type = indexed.types[static_cast<std::size_t>(indexed.position.at(node.id))];
    if (type->id == "nc.input")
      ++inputs;
    if (type->id == "nc.output")
      ++outputs;
  }
  if (inputs != 1)
    return Validation::failure("a chain needs exactly one nc.input, found " + std::to_string(inputs));
  if (outputs != 1)
    return Validation::failure("a chain needs exactly one nc.output, found " + std::to_string(outputs));

  // Connections: both ends must exist and the port must be within range.
  std::map<std::pair<int, int>, std::string> driven_by;
  for (const Connection& connection : graph.connections)
  {
    const auto from = indexed.position.find(connection.from);
    if (from == indexed.position.end())
      return Validation::failure("connection from unknown node '" + connection.from + "'");
    const auto to = indexed.position.find(connection.to);
    if (to == indexed.position.end())
      return Validation::failure("connection to unknown node '" + connection.to + "'");

    const render::PortCounts from_ports = indexed.ports[static_cast<std::size_t>(from->second)];
    const render::PortCounts to_ports = indexed.ports[static_cast<std::size_t>(to->second)];
    if (connection.from_port < 0 || connection.from_port >= from_ports.outputs)
      return Validation::failure("node '" + connection.from + "' has no output port " +
                                 std::to_string(connection.from_port));
    if (connection.to_port < 0 || connection.to_port >= to_ports.inputs)
      return Validation::failure("node '" + connection.to + "' has no input port " +
                                 std::to_string(connection.to_port));
    if (from_ports.outputs == 0)
      return Validation::failure("node '" + connection.from + "' produces no output");
    if (to_ports.inputs == 0)
      return Validation::failure("node '" + connection.to + "' accepts no input");

    // Two connections into one port leave the graph meaning "whichever one the compiler happened to
    // keep". Rejecting it turns a silent 50/50 into a message.
    const auto previous = driven_by.emplace(std::make_pair(to->second, connection.to_port), connection.from);
    if (!previous.second)
      return Validation::failure("node '" + connection.to + "' input port " +
                                 std::to_string(connection.to_port) + " is already driven by '" +
                                 previous.first->second + "'");
  }

  // Cycles are a compile error, except through a block-delay node: feedback is expressed with an
  // explicit `nc.unit_delay`, which reads its input as of the previous block (§5.1). Kahn's
  // algorithm both detects a cycle and produces the topological order the compiler needs.
  const std::size_t n = graph.nodes.size();
  std::vector<std::vector<int>> successors;
  std::vector<int> indegree;
  build_dependencies(graph, indexed, successors, indegree);

  std::vector<bool> placed(n, false);
  std::size_t placed_count = 0;
  for (std::size_t step = 0; step < n; ++step)
  {
    const int chosen = next_ready(indexed, indegree, placed);
    if (chosen < 0)
      return Validation::failure("graph contains a cycle; use an explicit nc.unit_delay node for feedback");
    placed[static_cast<std::size_t>(chosen)] = true;
    ++placed_count;
    for (const int next : successors[static_cast<std::size_t>(chosen)])
      --indegree[static_cast<std::size_t>(next)];
  }
  if (placed_count != n)
    return Validation::failure("graph contains a cycle");

  return validation;
}

Validation validate(const Document& document)
{
  if (document.chains.empty())
    return Validation::failure("a document needs at least one chain");

  std::map<std::string, bool> seen;
  for (const Chain& chain : document.chains)
  {
    // The chain is named in every failure below, because "a chain needs exactly one nc.input" is not
    // something a client with two chains can act on (docs/ui-integration-plan.md §1.1).
    if (chain.id.empty())
      return Validation::failure("a chain has no id");
    const std::string where = "chain '" + chain.id + "'";
    if (!seen.emplace(chain.id, true).second)
      return Validation::failure("two chains share the id '" + chain.id + "'");
    if (chain.input_channel < 0)
      return Validation::failure(where + " has a negative input_channel");
    if (chain.output_channel < 0)
      return Validation::failure(where + " has a negative output_channel");

    // An engine-internal type is the engine's to place (a local transition's crossfade); a document
    // that names one is asking for machinery, not for an effect.
    for (const NodeSpec& node : chain.graph.nodes)
    {
      const render::NodeType* type = render::find_node_type(node.type);
      if (type != nullptr && type->internal)
        return Validation::failure(where + ": node '" + node.id + "' names '" + node.type +
                                   "', which only the engine may place");
    }

    const Validation inner = validate(chain.graph);
    if (!inner.ok)
      return Validation::failure(where + ": " + inner.message);
  }
  return Validation{};
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------

namespace
{

/// Topological order, or an empty vector if there is a cycle (validate() has already rejected that).
std::vector<int> topological_order(const Graph& graph, const Indexed& indexed)
{
  const std::size_t n = graph.nodes.size();
  std::vector<std::vector<int>> successors;
  std::vector<int> indegree;
  build_dependencies(graph, indexed, successors, indegree);

  std::vector<int> order;
  order.reserve(n);
  std::vector<bool> placed(n, false);
  for (std::size_t step = 0; step < n; ++step)
  {
    const int chosen = next_ready(indexed, indegree, placed);
    if (chosen < 0)
      return {};
    placed[static_cast<std::size_t>(chosen)] = true;
    order.push_back(chosen);
    for (const int next : successors[static_cast<std::size_t>(chosen)])
      --indegree[static_cast<std::size_t>(next)];
  }
  return order;
}

} // namespace

std::unique_ptr<RenderPlan> RenderPlan::compile(const Graph& graph, const render::CompileOptions& options,
                                                rt::Arena& arena, std::string& error)
{
  return compile(graph, options, arena, error, nullptr);
}

std::unique_ptr<RenderPlan> RenderPlan::compile(const Graph& graph, const render::CompileOptions& options,
                                                rt::Arena& arena, std::string& error,
                                                const CarriedInstances* carried)
{
  const int max_quantum = options.max_quantum;
  if (max_quantum < 1)
  {
    error = "max_quantum must be at least 1";
    return nullptr;
  }
  if (!(options.sample_rate > 0.0))
  {
    error = "sample_rate must be > 0";
    return nullptr;
  }

  // Lower first. A macro is a document-level node -- a feedback delay is one node there and seven
  // here -- and everything below this point is the primitive compiler: it never sees a macro, which is
  // why macros cost nothing on the render path and no node type knows that macros exist.
  //
  // The lowered graph is validated again before it is compiled. A lowering is engine code, so a
  // lowering that produces something the compiler refuses is an engine bug rather than a document
  // error, and the message says so: the alternative is a confusing complaint about a graph the author
  // never wrote.
  Graph lowered_graph;
  if (!lower_graph(graph, lowered_graph, error))
    return nullptr;

  const Validation validation = validate(lowered_graph);
  if (!validation.ok)
  {
    error = validation.message;
    return nullptr;
  }

  Validation ignored;
  const Indexed indexed = index_graph(lowered_graph, ignored);

  const std::vector<int> order = topological_order(lowered_graph, indexed);
  if (order.empty())
  {
    error = "graph contains a cycle";
    return nullptr;
  }

  // Position of each node in the execution order, for liveness.
  std::vector<int> position_of(lowered_graph.nodes.size(), 0);
  for (std::size_t i = 0; i < order.size(); ++i)
    position_of[static_cast<std::size_t>(order[i])] = static_cast<int>(i);

  // Value = one node output port, identified by a flat index.
  const auto value_of = [&indexed](int node, int port) {
    int base = 0;
    for (int i = 0; i < node; ++i)
      base += indexed.ports[static_cast<std::size_t>(i)].outputs;
    return base + port;
  };
  int value_count = 0;
  for (const render::PortCounts& ports : indexed.ports)
    value_count += ports.outputs;

  std::vector<int> owner(value_count, -1);
  for (std::size_t node = 0; node < indexed.ports.size(); ++node)
    for (int port = 0; port < indexed.ports[node].outputs; ++port)
      owner[static_cast<std::size_t>(value_of(static_cast<int>(node), port))] = static_cast<int>(node);

  // Liveness: a value is live from its definition until the last node that consumes it. An unused
  // value dies at its own node, which is what lets a dangling branch reuse its buffer immediately.
  std::vector<int> last_use(value_count, -1);
  for (int value = 0; value < value_count; ++value)
    if (owner[static_cast<std::size_t>(value)] >= 0)
      last_use[static_cast<std::size_t>(value)] = position_of[static_cast<std::size_t>(owner[static_cast<std::size_t>(value)])];

  for (const Connection& connection : lowered_graph.connections)
  {
    const int from = indexed.position.at(connection.from);
    const int to = indexed.position.at(connection.to);
    const int value = value_of(from, connection.from_port);
    last_use[static_cast<std::size_t>(value)] =
      std::max(last_use[static_cast<std::size_t>(value)], position_of[static_cast<std::size_t>(to)]);

    // A value a block-delay node reads has to still be there at the *start of the next block*,
    // because that node reads it before its producer has run. An ordinary last-use would let the
    // colouring recycle the buffer later in the same block, and the next block's read would pick up
    // whatever landed there instead.
    if (indexed.types[static_cast<std::size_t>(to)]->block_delay)
      last_use[static_cast<std::size_t>(value)] = static_cast<int>(order.size());
  }

  // Interval colouring: walk values in definition order and reuse the first buffer whose previous
  // occupant died before this definition. This is the memory plan of §5.2 in miniature.
  std::vector<int> colour(value_count, -1);
  std::vector<int> free_at;
  int next_colour = 0;
  for (const int node : order)
  {
    const int def_position = position_of[static_cast<std::size_t>(node)];
    for (int port = 0; port < indexed.ports[static_cast<std::size_t>(node)].outputs; ++port)
    {
      const int value = value_of(node, port);
      int chosen = -1;
      for (int candidate = 0; candidate < next_colour; ++candidate)
      {
        if (free_at[static_cast<std::size_t>(candidate)] < def_position)
        {
          chosen = candidate;
          break;
        }
      }
      if (chosen < 0)
      {
        chosen = next_colour++;
        free_at.push_back(0);
      }
      colour[static_cast<std::size_t>(value)] = chosen;
      free_at[static_cast<std::size_t>(chosen)] = last_use[static_cast<std::size_t>(value)];
    }
  }

  // One extra buffer, zeroed once, shared by every unconnected input port. Binding silence is
  // cheaper and safer than trusting a node to ignore a null pointer.
  const int total_buffers = next_colour + 1;
  const int silence_buffer = next_colour;

  auto plan = std::unique_ptr<RenderPlan>(new RenderPlan());
  plan->max_quantum_ = max_quantum;
  plan->silence_buffer_ = silence_buffer;
  plan->levels_first_frame_ = options.levels && options.levels_first_frame;
  plan->failed_ = std::vector<std::atomic<bool>>(lowered_graph.nodes.size());
  for (std::atomic<bool>& flag : plan->failed_)
    flag.store(false, std::memory_order_relaxed);
  // Reserved here and filled beside `nodes_`, so the two stay parallel by construction: a probe
  // with no node, or a node with no probe, would be a meter reading somebody else's signal. Left
  // empty when the engine is not measuring levels: `measure_node` returns on a null probe, so the
  // render path then does no per-sample level work at all (CompileOptions::levels).
  if (options.levels)
    plan->levels_.reserve(lowered_graph.nodes.size());
  plan->buffers_.reserve(static_cast<std::size_t>(total_buffers));
  for (int i = 0; i < total_buffers; ++i)
  {
    float* buffer = arena.allocate_array<float>(static_cast<std::size_t>(max_quantum));
    if (buffer == nullptr)
    {
      error = "arena exhausted: the memory plan needs " + std::to_string(total_buffers) + " buffers";
      return nullptr;
    }
    std::fill(buffer, buffer + max_quantum, 0.0f);
    plan->buffers_.push_back(buffer);
  }

  for (const int node : order)
  {
    const NodeSpec& spec = lowered_graph.nodes[static_cast<std::size_t>(node)];
    const render::NodeType* type = indexed.types[static_cast<std::size_t>(node)];

    PlanNode plan_node;
    plan_node.type = type;
    // The document identity travels with the node, because a client addresses `element` + `role` and
    // the plan is what a patch resolves against (docs/ui-integration-plan.md §1.3). Copied rather
    // than looked up in the document: the plan is what is running, and a patch must resolve against
    // the thing the render thread is actually executing.
    plan_node.element = spec.element;
    plan_node.role = spec.role;
    // Bypass runs the node and replaces its output (see execute), so `create` still runs: the node's
    // state must stay warm for the un-bypass.
    plan_node.bypass = spec.bypass;
    // A carried node keeps the instance it already has, state and all: that is the whole of a local
    // transition's saving (docs/transitions.md, "Local transitions"). Everything else is created here.
    const auto carried_instance = carried != nullptr ? carried->find(spec.id) : CarriedInstances::const_iterator{};
    if (carried != nullptr && carried_instance != carried->end() && carried_instance->second != nullptr)
    {
      plan_node.instance = carried_instance->second;
      plan_node.state = plan_node.instance.get();
      plan_node.fresh = false;
    }
    else if (type->create != nullptr)
    {
      // A null `create` means the type carries no state at all (nc.input, nc.split, ...). Using the
      // vtable rather than a list of ids keeps this honest as node types are added: a type that does
      // have a constructor and fails to allocate is an error, and cannot be mistaken for a marker.
      void* state = type->create(spec, options, error);
      if (state == nullptr)
        return nullptr; // create() set `error`
      // The instance owns the state from here: `destroy` runs when the last plan holding it is freed,
      // which may be a later plan this one hands it to rather than this one.
      plan_node.instance = NodeInstance(state, [type](void* raw) {
        if (type->destroy != nullptr && raw != nullptr)
          type->destroy(raw);
      });
      plan_node.state = state;
    }

    const render::PortCounts ports = indexed.ports[static_cast<std::size_t>(node)];
    plan_node.inputs.assign(static_cast<std::size_t>(ports.inputs), plan->buffers_[static_cast<std::size_t>(silence_buffer)]);
    plan_node.outputs.reserve(static_cast<std::size_t>(ports.outputs) + 1);
    for (int port = 0; port < ports.outputs; ++port)
      plan_node.outputs.push_back(plan->buffers_[static_cast<std::size_t>(colour[static_cast<std::size_t>(value_of(node, port))])]);
    // Terminators on both sides: `process_split` walks outputs until nullptr, `process_mix` walks
    // inputs the same way. Binding below is by port index, so the input terminator goes on last.
    plan_node.outputs.push_back(nullptr);
    plan_node.inputs.push_back(nullptr);

    plan->nodes_.push_back(std::move(plan_node));
    plan->node_ids_.push_back(spec.id);
    // One level probe per node, made here so it is parallel to `nodes_` by construction. `emplace`
    // rather than `push_back`: a Probe holds atomics and is not movable. Nothing is made when the
    // engine is not measuring, which is what leaves this vector empty and `measure_node` idle.
    if (options.levels)
      plan->levels_.emplace_back();
  }

  // Bind inputs now that every output buffer is known.
  for (std::size_t i = 0; i < plan->nodes_.size(); ++i)
  {
    for (const Connection& connection : lowered_graph.connections)
    {
      if (indexed.position.at(connection.to) != order[i])
        continue;
      const int from = indexed.position.at(connection.from);
      const int value = value_of(from, connection.from_port);
      plan->nodes_[i].inputs[static_cast<std::size_t>(connection.to_port)] =
        plan->buffers_[static_cast<std::size_t>(colour[static_cast<std::size_t>(value)])];
    }

    if (plan->nodes_[i].type->id == "nc.input")
      plan->input_node_ = static_cast<int>(i);
    if (plan->nodes_[i].type->id == "nc.output")
    {
      // The output node's single input buffer holds the chain's result.
      const float* bound = plan->nodes_[i].inputs[0];
      for (int b = 0; b < total_buffers; ++b)
        if (plan->buffers_[static_cast<std::size_t>(b)] == bound)
          plan->output_buffer_ = b;
    }
  }

  for (const int node : order)
  {
    std::vector<int> ports;
    for (int port = 0; port < indexed.ports[static_cast<std::size_t>(node)].outputs; ++port)
      ports.push_back(colour[static_cast<std::size_t>(value_of(node, port))]);
    plan->output_buffers_.push_back(std::move(ports));
  }

  return plan;
}

// Each node's state is destroyed by its instance's owner count reaching zero: here, unless a plan this
// one handed it to still holds it.
RenderPlan::~RenderPlan() = default;

std::size_t RenderPlan::buffer_bytes() const noexcept
{
  return buffers_.size() * static_cast<std::size_t>(max_quantum_) * sizeof(float);
}

const std::vector<int>& RenderPlan::output_buffer_of(std::size_t node_index) const
{
  static const std::vector<int> empty;
  return node_index < output_buffers_.size() ? output_buffers_[node_index] : empty;
}

bool RenderPlan::node_failed(std::size_t node_index) const noexcept
{
  return node_index < failed_.size() && failed_[node_index].load(std::memory_order_relaxed);
}

std::size_t RenderPlan::failed_node_count() const noexcept
{
  std::size_t count = 0;
  for (const std::atomic<bool>& flag : failed_)
    if (flag.load(std::memory_order_relaxed))
      ++count;
  return count;
}

void RenderPlan::execute(const float* input, float* output, int frames) noexcept
{
  // Contract: frames <= max_quantum(), which is what every node was created for.
  //
  // The graph input is handed to the input node rather than written into its buffer here. That is
  // not a style choice: a block-delay node is scheduled before the input node and reads that
  // buffer, so writing it up front would make the delay come out as zero samples.
  if (input_node_ >= 0)
  {
    PlanNode& node = nodes_[static_cast<std::size_t>(input_node_)];
    if (node.type->set_block_input != nullptr)
      node.type->set_block_input(node.state, input);
  }

  for (std::size_t i = 0; i < nodes_.size(); ++i)
  {
    PlanNode& node = nodes_[i];
    // A node the guard has already failed is not run again: its output is dry for the rest of the
    // plan's life, and re-running it would only re-produce the fault. This is a deliberate exception
    // to G14's "keep it running": a node whose output cannot be trusted is not a node whose state is
    // worth keeping warm.
    if (failed_[i].load(std::memory_order_relaxed))
    {
      substitute_dry(node, frames);
      continue;
    }

    node.type->process(node.state, node.inputs.data(), node.outputs.data(), frames);

    // The guard is on for every node, not only third-party ones: one compare per sample against
    // buffers the node has just written, on top of the work that produced them. A node that returns
    // a non-finite sample is out of the signal path for good and reported; the alternative is
    // NaN propagating into every consumer and out of the device.
    if (!outputs_finite(node, frames))
    {
      failed_[i].store(true, std::memory_order_relaxed);
      guard_trips_.fetch_add(1, std::memory_order_relaxed);
      substitute_dry(node, frames);
      continue;
    }

    if (node.bypass_fade > 0)
      fade_bypass(node, frames);
    else if (node.bypass)
      substitute_dry(node, frames);

    // Last, so the probe measures what the next node in the chain will read rather than what this
    // one computed on the way there.
    measure_node(node, i < levels_.size() ? &levels_[i] : nullptr, frames, levels_first_frame_);
  }

  if (output_buffer_ >= 0 && output != nullptr)
    std::copy(buffers_[static_cast<std::size_t>(output_buffer_)],
              buffers_[static_cast<std::size_t>(output_buffer_)] + frames, output);
}

void RenderPlan::prewarm() noexcept
{
  if (max_quantum_ <= 0)
    return;
  execute(nullptr, nullptr, max_quantum_);
  // The prewarm's block is silence that no client could have read -- the plan is not published until
  // this returns -- so it is dropped rather than left to dilute a node's first window. `take` is the
  // only way to empty a probe, and on the control thread before publication there is no reader to
  // race. (`prewarm_fresh` calls `process` directly and measures nothing, so it has nothing to drop.)
  for (level::Probe& probe : levels_)
    probe.take();
}

void RenderPlan::prewarm_fresh() noexcept
{
  if (max_quantum_ <= 0)
    return;
  for (PlanNode& node : nodes_)
  {
    if (!node.fresh || node.type->skip_prewarm)
      continue;
    // A fresh input node is handed no block, which it plays as silence, the same as prewarm().
    if (node.type->set_block_input != nullptr && node.state != nullptr)
      node.type->set_block_input(node.state, nullptr);
    node.type->process(node.state, node.inputs.data(), node.outputs.data(), max_quantum_);
  }
  // Start from silence. A carried block-delay node's input is copied in when the plan takes over; any
  // other buffer is rewritten by its producer before it is read.
  for (float* buffer : buffers_)
    std::fill(buffer, buffer + max_quantum_, 0.0f);
}

NodeInstance RenderPlan::instance_of(const std::string& node_id) const
{
  const int index = index_of(node_id);
  return index < 0 ? NodeInstance{} : nodes_[static_cast<std::size_t>(index)].instance;
}

int RenderPlan::index_of(const std::string& node_id) const
{
  for (std::size_t i = 0; i < node_ids_.size(); ++i)
  {
    if (node_ids_[i] == node_id)
      return static_cast<int>(i);
  }
  return -1;
}

float* RenderPlan::input_buffer(std::size_t node_index, int port) const noexcept
{
  if (node_index >= nodes_.size() || port < 0)
    return nullptr;
  const PlanNode& node = nodes_[node_index];
  // The input array carries a terminator, so its real ports are all but the last entry.
  if (static_cast<std::size_t>(port) + 1 >= node.inputs.size())
    return nullptr;
  const float* bound = node.inputs[static_cast<std::size_t>(port)];
  if (bound == nullptr ||
      (silence_buffer_ >= 0 && bound == buffers_[static_cast<std::size_t>(silence_buffer_)]))
    return nullptr;
  // Every bound input is one of this plan's own arena buffers, which the plan owns as writable memory.
  for (float* buffer : buffers_)
    if (buffer == bound)
      return buffer;
  return nullptr;
}

bool RenderPlan::resolve_param(const std::string& node_id, const std::string& param, std::size_t& node_index,
                               uint32_t& param_id) const
{
  return resolve_param(node_id, param, -1, node_index, param_id);
}

int RenderPlan::node_index_of_role(const std::string& element, const std::string& role) const
{
  int found = -1;
  for (std::size_t i = 0; i < nodes_.size(); ++i)
  {
    if (nodes_[i].element != element)
      continue;
    // An empty role matches any role of the element: that is the "the element is one node" spelling a
    // hand-written rig uses, and the reason several matches is an answer of its own rather than the
    // first one found.
    if (!role.empty() && nodes_[i].role != role)
      continue;
    if (found >= 0)
      return -2;
    found = static_cast<int>(i);
  }
  return found;
}

const std::string& RenderPlan::element_of(std::size_t node_index) const
{
  static const std::string empty;
  return node_index < nodes_.size() ? nodes_[node_index].element : empty;
}

const std::string& RenderPlan::role_of(std::size_t node_index) const
{
  static const std::string empty;
  return node_index < nodes_.size() ? nodes_[node_index].role : empty;
}

bool RenderPlan::resolve_param(const std::string& node_id, const std::string& param, int port,
                               std::size_t& node_index, uint32_t& param_id) const
{
  for (std::size_t i = 0; i < node_ids_.size(); ++i)
  {
    if (node_ids_[i] != node_id)
      continue;
    const render::NodeType* type = nodes_[i].type;
    if (type->set_param == nullptr)
      return false;

    if (port >= 0)
    {
      if (type->per_port_params == nullptr)
        return false;
      // The compiler terminates a node's input array with a nullptr, so its length minus one is how
      // many ports it has -- the same number the arity parameter resolved to, read from the binding
      // rather than from the spec, so the two cannot disagree.
      const int ports = static_cast<int>(nodes_[i].inputs.size()) - 1;
      if (port >= ports)
        return false;
      for (int p = 0; p < type->per_port_param_count; ++p)
      {
        const render::ParamDesc& desc = type->per_port_params[p];
        if (param != desc.id || !desc.rampable || !desc.patchable)
          continue;
        node_index = i;
        param_id = render::port_param_id(port, p);
        return true;
      }
      return false;
    }

    if (type->params == nullptr)
      return false;
    for (int p = 0; p < type->param_count; ++p)
    {
      // Only a parameter the node declares rampable can be dispatched: everything else is a
      // structural edit, which is the whole distinction §5.3 turns on.
      if (param != type->params[p].id || !type->params[p].rampable || !type->params[p].patchable)
        continue;
      node_index = i;
      param_id = static_cast<uint32_t>(p) + 1;
      return true;
    }
    return false;
  }
  return false;
}

bool RenderPlan::set_param(std::size_t node_index, uint32_t param_id, float value) noexcept
{
  if (node_index >= nodes_.size())
    return false;
  const render::NodeType* type = nodes_[node_index].type;
  if (type->set_param == nullptr || type->params == nullptr)
    return false;
  return type->set_param(nodes_[node_index].state, param_id, value);
}

bool RenderPlan::set_bypass(std::size_t node_index, bool bypass, int frames) noexcept
{
  if (!node_bypassable(node_index))
    return false;
  PlanNode& node = nodes_[node_index];
  if (node.bypass == bypass)
    return true;
  node.bypass = bypass;
  if (node.bypass_fade > 0)
  {
    // Turned around mid-fade: the weights are a quarter turn, so the fade the other way passes through
    // the same pair of weights at the mirrored position, and the signal carries on from where it is.
    node.bypass_position = std::max(0, node.bypass_fade - 1 - node.bypass_position);
    return true;
  }
  node.bypass_fade = std::max(frames, 1);
  node.bypass_position = 0;
  return true;
}

std::unique_ptr<RenderPlan> RenderPlan::compile(const Graph& graph, int max_quantum, rt::Arena& arena,
                                                std::string& error)
{
  render::CompileOptions options;
  options.max_quantum = max_quantum;
  return compile(graph, options, arena, error);
}

} // namespace graph
} // namespace namchain
