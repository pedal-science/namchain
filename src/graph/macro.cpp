#include "namchain/graph/macro.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace namchain
{
namespace graph
{
namespace
{

/// A number as a parameter value spells it: `12`, not `12.000000`. Used for the rare case of an
/// option parameter written numerically in a document, whose string form is then needed by a target.
std::string number_text(double value)
{
  const double rounded = std::round(value);
  if (std::fabs(value - rounded) < 1e-9)
    return std::to_string(static_cast<long long>(rounded));
  return std::to_string(value);
}

/// The parameter of \p node_type called \p name, or null. String targets are the ones this returns a
/// `ParamKind::Option` for, which is what decides whether a target carries a number or a string.
const render::ParamDesc* node_param(const char* node_type, const char* name)
{
  const render::NodeType* type = render::find_node_type(node_type);
  if (type == nullptr || type->params == nullptr)
    return nullptr;
  for (int i = 0; i < type->param_count; ++i)
  {
    if (std::strcmp(type->params[i].id, name) == 0)
      return &type->params[i];
  }
  return nullptr;
}

/// The declared parameter of a macro, by name.
const MacroParamDecl* declared_param(const MacroType& type, const char* name)
{
  for (int i = 0; i < type.param_count; ++i)
  {
    if (std::strcmp(type.params[i].desc.id, name) == 0)
      return &type.params[i];
  }
  return nullptr;
}

/// The value a macro parameter has in \p spec, or its declared default.
double param_value(const NodeSpec& spec, const MacroParamDecl& param)
{
  const auto given = spec.params.find(param.desc.id);
  if (given != spec.params.end())
    return given->second;
  const auto text = spec.strings.find(param.desc.id);
  if (text != spec.strings.end())
  {
    // An option stored as a string that also has a numeric spelling (an option value like "24"): the
    // lowering asks for a number and gets one. A non-numeric option value is a bug in the macro, and
    // `number()` returning the default is the least surprising answer -- the lint has already checked
    // that the value is one of the declared options.
    char* end = nullptr;
    const double parsed = std::strtod(text->second.c_str(), &end);
    if (end != text->second.c_str() && *end == '\0')
      return parsed;
  }
  return param.desc.default_value;
}

std::string string_value(const NodeSpec& spec, const MacroParamDecl& param)
{
  const auto text = spec.strings.find(param.desc.id);
  if (text != spec.strings.end())
    return text->second;
  const auto number = spec.params.find(param.desc.id);
  if (number != spec.params.end())
    return number_text(number->second);
  // An option with a fixed list defaults to the entry its `default_value` indexes, including index 0
  // -- which is why this is not "non-zero means a default".
  if (param.desc.options != nullptr && param.desc.option_count > 0)
  {
    const int index = static_cast<int>(param.desc.default_value);
    if (index >= 0 && index < param.desc.option_count)
      return param.desc.options[index].value;
  }
  return {};
}

} // namespace

double apply_transform(Transform transform, double value, double scale)
{
  switch (transform)
  {
  case Transform::Direct: return value;
  case Transform::DbToLinear: return std::pow(10.0, value / 20.0);
  case Transform::LinearToDb:
    // A linear gain of zero is minus infinity in dB, which is not a number a parameter may hold. The
    // floor is -120 dB: inaudible by any measure, and finite.
    return value <= 1e-6 ? -120.0 : 20.0 * std::log10(value);
  case Transform::OneMinus: return 1.0 - value;
  case Transform::ScaleBy: return value * scale;
  }
  return value;
}

NodeSpec& Lowering::node(const char* role)
{
  for (NodeSpec& candidate : *nodes)
  {
    if (candidate.role == role)
      return candidate;
  }
  // The framework created every declared role before calling the lowering, so this cannot happen
  // unless a lowering asks for a role it did not declare -- an engine bug, and one that fails loudly
  // rather than quietly lowering nothing.
  std::abort();
}

bool Lowering::has(const char* param) const
{
  return spec->params.count(param) != 0 || spec->strings.count(param) != 0;
}

double Lowering::number(const char* param) const
{
  const MacroParamDecl* declared = declared_param(*type, param);
  return declared == nullptr ? 0.0 : param_value(*spec, *declared);
}

std::string Lowering::text(const char* param) const
{
  const MacroParamDecl* declared = declared_param(*type, param);
  return declared == nullptr ? std::string() : string_value(*spec, *declared);
}

std::string mint_node_id(const std::string& element, const std::string& role, const std::vector<std::string>& taken)
{
  const std::string stem = element.empty() ? role : element + ":" + role;
  const auto used = [&taken](const std::string& candidate) {
    return std::find(taken.begin(), taken.end(), candidate) != taken.end();
  };
  if (!used(stem))
    return stem;
  for (int suffix = 2; suffix < 10000; ++suffix)
  {
    const std::string candidate = stem + "~" + std::to_string(suffix);
    if (!used(candidate))
      return candidate;
  }
  return stem;
}

bool lower_graph(const Graph& graph, Graph& out, std::string& error)
{
  out.nodes.clear();
  out.connections.clear();

  // Ids already spoken for: the document's own, plus every id this pass mints, so a lowering cannot
  // collide with a later node of the same graph either.
  std::vector<std::string> taken;
  taken.reserve(graph.nodes.size() * 4);
  for (const NodeSpec& node : graph.nodes)
    taken.push_back(node.id);

  /// Where a macro node's ports went, for rewriting the edges that touch it. The input list keeps the
  /// *macro* port each link came from, because one macro port may fan out to several nodes and each
  /// edge has to find its own.
  struct Expanded
  {
    std::vector<MacroPortLink> input_links;
    std::vector<std::pair<std::string, int>> input_nodes;  // one per link, in the same order
    std::vector<std::pair<std::string, int>> outputs;      // one per macro output port, indexed by it
    bool outputs_by_port = true;
  };
  std::map<std::string, Expanded> expanded;

  for (const NodeSpec& node : graph.nodes)
  {
    const MacroType* macro = find_macro_type(node.type);
    if (macro == nullptr)
    {
      out.nodes.push_back(node);
      taken.push_back(node.id);
      continue;
    }

    // 1. The declared shape, one node per role, with the element and role the contract promises.
    std::vector<NodeSpec> nodes;
    nodes.reserve(static_cast<std::size_t>(macro->node_count));
    for (int i = 0; i < macro->node_count; ++i)
    {
      const MacroNodeDecl& declared = macro->nodes[i];
      NodeSpec lowered;
      lowered.type = declared.type;
      lowered.element = node.element;
      lowered.role = declared.role;
      lowered.id = mint_node_id(node.element, declared.role, taken);
      taken.push_back(lowered.id);
      nodes.push_back(std::move(lowered));
    }

    // 2. The params the declarative shape cannot express: modes, arity, line lengths, and the bypass
    //    a selection parameter implies.
    Lowering lowering;
    lowering.spec = &node;
    lowering.type = macro;
    lowering.nodes = &nodes;
    if (macro->apply != nullptr)
    {
      std::string reason;
      if (!macro->apply(lowering, reason))
      {
        error = "node '" + node.id + "' (" + node.type + "'): " + reason;
        return false;
      }
    }

    // 3. The param map, which is what a macro-level `set_param` will write through later: the same
    //    transform, from the same declaration, so a compiled value and a patched value cannot
    //    disagree.
    for (int i = 0; i < macro->param_count; ++i)
    {
      const MacroParamDecl& param = macro->params[i];
      for (int t = 0; t < param.target_count; ++t)
      {
        const MacroTarget& target = param.targets[t];
        NodeSpec& destination = lowering.node(target.role);
        // Which of the two maps the value lands in is the *target's* declaration, not the macro
        // parameter's: a string target (a model or an IR id) takes the text verbatim and a numeric one
        // goes through the declared transform. Reading it from the node type is what stops a macro
        // from claiming a target is numeric when the node says otherwise.
        const render::ParamDesc* target_desc = node_param(destination.type.c_str(), target.param);
        if (target_desc != nullptr && target_desc->kind == render::ParamKind::Option)
        {
          destination.strings[target.param] = string_value(node, param);
        }
        else
        {
          const double value = param_value(node, param);
          destination.params[target.param] = apply_transform(target.transform, value, target.scale);
        }
      }
    }

    // 4. The bypass rule, last, because for a crossfade it is what decides the two gains the param map
    //    may also have written.
    if (node.bypass)
    {
      if (macro->bypass == MacroBypass::AllRolesCarry)
      {
        // The rule's precondition, checked rather than assumed: "every role carries the flag" means a
        // dry path per role, which only a 1->1 primitive has. A macro that breaks this is an engine
        // bug, and it fails here instead of producing a validation error about a node nobody wrote.
        for (NodeSpec& lowered : nodes)
        {
          const render::NodeType* type = render::find_node_type(lowered.type);
          if (type == nullptr || type->inputs != 1 || type->outputs != 1)
          {
            error = "macro '" + std::string(macro->id) + "' cannot be bypassed: its '" + lowered.role +
                    "' node is not 1->1";
            return false;
          }
          lowered.bypass = true;
        }
      }
      else
      {
        lowering.node(macro->dry_role).params["gain"] = 1.0;
        lowering.node(macro->wet_role).params["gain"] = 0.0;
      }
    }

    Expanded ports;
    for (int i = 0; i < macro->input_link_count; ++i)
    {
      const MacroPortLink& link = macro->input_links[i];
      ports.input_links.push_back(link);
      ports.input_nodes.emplace_back(lowering.node(link.role).id, link.role_port);
    }
    // An output port is produced by exactly one node port -- two connections into one port are already
    // a validation error -- so this one is indexed by the macro port rather than searched.
    ports.outputs.resize(static_cast<std::size_t>(macro->outputs));
    for (int i = 0; i < macro->output_link_count; ++i)
    {
      const MacroPortLink& link = macro->output_links[i];
      if (link.port >= 0 && link.port < macro->outputs)
        ports.outputs[static_cast<std::size_t>(link.port)] = {lowering.node(link.role).id, link.role_port};
    }

    for (int e = 0; e < macro->edge_count; ++e)
    {
      const MacroEdge& edge = macro->edges[e];
      Connection connection;
      connection.from = lowering.node(edge.from_role).id;
      connection.from_port = edge.from_port;
      connection.to = lowering.node(edge.to_role).id;
      connection.to_port = edge.to_port;
      out.connections.push_back(std::move(connection));
    }

    for (NodeSpec& lowered : nodes)
      out.nodes.push_back(std::move(lowered));
    expanded.emplace(node.id, std::move(ports));
  }

  // The edges of the document itself, rewritten where they touch a macro: an input that fans out
  // becomes one connection per link, and an output is whatever node produces it.
  for (const Connection& connection : graph.connections)
  {
    const auto from = expanded.find(connection.from);
    const auto to = expanded.find(connection.to);

    std::vector<std::pair<std::string, int>> sources;
    if (from == expanded.end())
    {
      sources.emplace_back(connection.from, connection.from_port);
    }
    else
    {
      const int port = connection.from_port;
      if (port < 0 || port >= static_cast<int>(from->second.outputs.size()))
      {
        error = "node '" + connection.from + "' has no output port " + std::to_string(port);
        return false;
      }
      sources.push_back(from->second.outputs[static_cast<std::size_t>(port)]);
    }

    std::vector<std::pair<std::string, int>> destinations;
    if (to == expanded.end())
    {
      destinations.emplace_back(connection.to, connection.to_port);
    }
    else
    {
      for (std::size_t i = 0; i < to->second.input_links.size(); ++i)
      {
        if (to->second.input_links[i].port == connection.to_port)
          destinations.push_back(to->second.input_nodes[i]);
      }
      if (destinations.empty())
      {
        error = "node '" + connection.to + "' has no input port " + std::to_string(connection.to_port);
        return false;
      }
    }

    for (const auto& source : sources)
    {
      for (const auto& destination : destinations)
      {
        Connection rewritten;
        rewritten.from = source.first;
        rewritten.from_port = source.second;
        rewritten.to = destination.first;
        rewritten.to_port = destination.second;
        out.connections.push_back(std::move(rewritten));
      }
    }
  }

  return true;
}

} // namespace graph
} // namespace namchain
