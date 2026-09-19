#include "namchain/graph/patch.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include <json.hpp>

#include "namchain/graph/rig.h"

namespace namchain
{
namespace graph
{
namespace
{

NodeSpec* find_node(Graph& graph, const std::string& id)
{
  for (NodeSpec& spec : graph.nodes)
    if (spec.id == id)
      return &spec;
  return nullptr;
}

bool require_string(const nlohmann::json& op, const char* key, const std::string& where, std::string& out,
                    std::string& error)
{
  if (!op.contains(key) || !op[key].is_string() || op[key].get<std::string>().empty())
  {
    error = where + ": needs a non-empty string '" + key + "'";
    return false;
  }
  out = op[key].get<std::string>();
  return true;
}

bool optional_int(const nlohmann::json& op, const char* key, int fallback, int& out, const std::string& where,
                  std::string& error)
{
  if (!op.contains(key))
  {
    out = fallback;
    return true;
  }
  if (!op[key].is_number_integer())
  {
    error = where + ": '" + key + "' must be an integer";
    return false;
  }
  out = op[key].get<int>();
  return true;
}

/// A node reference is `"node"`, or `"id"` for the ops that add one. Both spellings so a caller does
/// not have to remember which op wants which.
bool node_reference(const nlohmann::json& op, const std::string& where, std::string& out, std::string& error)
{
  if (op.contains("node"))
    return require_string(op, "node", where, out, error);
  return require_string(op, "id", where, out, error);
}

/// The same rule `load_graph_json` applies to a rig: a relative string param that names something
/// next to the rig resolves there, so a patch and its rig agree about where models live.
void resolve_strings(NodeSpec& spec, const std::string& base_dir)
{
  if (base_dir.empty())
    return;
  for (auto& entry : spec.strings)
  {
    std::string& value = entry.second;
    std::error_code ignored;
    const std::filesystem::path candidate = std::filesystem::path(base_dir) / value;
    if (!std::filesystem::path(value).is_absolute() && std::filesystem::exists(candidate, ignored))
      value = candidate.string();
  }
}

bool op_add_node(Graph& graph, const nlohmann::json& op, const std::string& where, const PatchContext& context,
                 std::string& error)
{
  // Reuse the rig parser rather than a second node-object reader: a patched node and a declared node
  // must mean exactly the same thing, and two parsers are two places for that to drift.
  nlohmann::json node = op;
  node.erase("op");
  nlohmann::json rig;
  rig["nodes"] = nlohmann::json::array({node});
  Graph parsed;
  std::string reason;
  if (!parse_graph_json(rig.dump(), parsed, reason))
  {
    error = where + ": " + reason;
    return false;
  }
  NodeSpec spec = std::move(parsed.nodes.front());
  if (find_node(graph, spec.id) != nullptr)
  {
    error = where + ": node '" + spec.id + "' already exists";
    return false;
  }
  resolve_strings(spec, context.base_dir);
  graph.nodes.push_back(std::move(spec));
  return true;
}

bool op_remove_node(Graph& graph, const nlohmann::json& op, const std::string& where, std::string& error)
{
  std::string id;
  if (!node_reference(op, where, id, error))
    return false;
  const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                 [&](const NodeSpec& spec) { return spec.id == id; });
  if (node == graph.nodes.end())
  {
    error = where + ": unknown node '" + id + "'";
    return false;
  }
  graph.nodes.erase(node);
  // A connection with a dangling end is a compile error, so removing a node has to remove them too;
  // leaving them would turn "remove this pedal" into "the rig no longer compiles".
  graph.connections.erase(std::remove_if(graph.connections.begin(), graph.connections.end(),
                                         [&](const Connection& c) { return c.from == id || c.to == id; }),
                          graph.connections.end());
  return true;
}

bool op_connect(Graph& graph, const nlohmann::json& op, const std::string& where, std::string& error)
{
  Connection connection;
  if (!require_string(op, "from", where, connection.from, error) ||
      !require_string(op, "to", where, connection.to, error) ||
      !optional_int(op, "from_port", 0, connection.from_port, where, error) ||
      !optional_int(op, "to_port", 0, connection.to_port, where, error))
    return false;
  if (find_node(graph, connection.from) == nullptr || find_node(graph, connection.to) == nullptr)
  {
    error = where + ": connection names a node that does not exist ('" + connection.from + "' -> '" +
            connection.to + "')";
    return false;
  }
  graph.connections.push_back(connection);
  return true;
}

bool op_disconnect(Graph& graph, const nlohmann::json& op, const std::string& where, std::string& error)
{
  const bool has_from = op.contains("from");
  std::string from;
  std::string to;
  int from_port = 0;
  int to_port = 0;
  if (!require_string(op, "to", where, to, error) ||
      !optional_int(op, "to_port", 0, to_port, where, error) ||
      (has_from && (!require_string(op, "from", where, from, error) ||
                    !optional_int(op, "from_port", 0, from_port, where, error))))
    return false;

  const auto matches = [&](const Connection& c) {
    if (has_from && (c.from != from || c.from_port != from_port))
      return false;
    return c.to == to && c.to_port == to_port;
  };
  const auto removed = std::remove_if(graph.connections.begin(), graph.connections.end(), matches);
  if (removed == graph.connections.end())
  {
    error = where + ": nothing drives '" + to + "' input " + std::to_string(to_port);
    return false;
  }
  graph.connections.erase(removed, graph.connections.end());
  return true;
}

bool op_rewire(Graph& graph, const nlohmann::json& op, const std::string& where, std::string& error)
{
  // Repoint one input port: whatever drove it is removed, and the named output drives it instead.
  // This is the single-edge form of a rewire; a caller changing several edges sends several ops.
  Connection connection;
  if (!require_string(op, "from", where, connection.from, error) ||
      !require_string(op, "to", where, connection.to, error) ||
      !optional_int(op, "from_port", 0, connection.from_port, where, error) ||
      !optional_int(op, "to_port", 0, connection.to_port, where, error))
    return false;
  if (find_node(graph, connection.from) == nullptr || find_node(graph, connection.to) == nullptr)
  {
    error = where + ": rewire names a node that does not exist ('" + connection.from + "' -> '" +
            connection.to + "')";
    return false;
  }
  graph.connections.erase(
    std::remove_if(graph.connections.begin(), graph.connections.end(),
                   [&](const Connection& c) {
                     return c.to == connection.to && c.to_port == connection.to_port;
                   }),
    graph.connections.end());
  graph.connections.push_back(connection);
  return true;
}

bool op_swap_model(Graph& graph, const nlohmann::json& op, const std::string& where, const PatchContext& context,
                   std::string& error)
{
  std::string id;
  if (!node_reference(op, where, id, error))
    return false;
  NodeSpec* spec = find_node(graph, id);
  if (spec == nullptr)
  {
    error = where + ": unknown node '" + id + "'";
    return false;
  }

  // The node type owns the string key (`nc.nam` reads `model`, `nc.cab.ir` reads `ir`); the op may
  // spell the value either way rather than making the caller know the node's internals.
  const char* key = nullptr;
  if (spec->type == "nc.nam")
    key = "model";
  else if (spec->type == "nc.cab.ir")
    key = "ir";
  else
  {
    error = where + ": node '" + id + "' (" + spec->type + ") is not a model node";
    return false;
  }

  std::string value;
  if (op.contains("model"))
  {
    if (!require_string(op, "model", where, value, error))
      return false;
  }
  else if (!require_string(op, key, where, value, error))
    return false;

  std::error_code ignored;
  const std::filesystem::path resolved = std::filesystem::path(context.base_dir) / value;
  if (!context.base_dir.empty() && !std::filesystem::path(value).is_absolute() &&
      std::filesystem::exists(resolved, ignored))
    value = resolved.string();

  spec->strings[key] = value;
  return true;
}

bool op_bypass(Graph& graph, const nlohmann::json& op, const std::string& where, std::string& error)
{
  std::string id;
  if (!node_reference(op, where, id, error))
    return false;
  NodeSpec* spec = find_node(graph, id);
  if (spec == nullptr)
  {
    error = where + ": unknown node '" + id + "'";
    return false;
  }
  // `{"op":"bypass","node":"g"}` means enable; an explicit `"bypass": false` un-bypasses. Whether the
  // node *can* be bypassed is the compiler's check (only a 1->1 node has a dry path), so the flag is
  // set here and validation refuses an impossible one with the node's name -- one authority, not two.
  bool bypass = true;
  if (op.contains("bypass"))
  {
    if (!op["bypass"].is_boolean())
    {
      error = where + ": 'bypass' must be a boolean";
      return false;
    }
    bypass = op["bypass"].get<bool>();
  }
  spec->bypass = bypass;
  return true;
}

bool op_set_param(Graph& graph, const nlohmann::json& op, const std::string& where, std::string& error)
{
  std::string id;
  std::string param;
  if (!node_reference(op, where, id, error) || !require_string(op, "param", where, param, error))
    return false;
  if (!op.contains("value") || !op["value"].is_number())
  {
    error = where + ": needs a numeric 'value'";
    return false;
  }
  if (op.contains("port"))
  {
    // A per-port parameter is state in the node, not a property of the graph, so there is nothing
    // here to rebuild: the smooth path is the only path for one (patch.h).
    error = where + ": a per-port parameter is edited with set_param against the live plan, not by "
                     "rebuilding the graph";
    return false;
  }
  NodeSpec* spec = find_node(graph, id);
  if (spec == nullptr)
  {
    error = where + ": unknown node '" + id + "'";
    return false;
  }
  spec->params[param] = op["value"].get<double>();
  // `nc.gain` accepts `gain` or `gain_db` at build and prefers the latter, so a structural change to
  // `gain` has to drop the alias or it would be silently ignored.
  if (spec->type == "nc.gain" && param == "gain")
    spec->params.erase("gain_db");
  return true;
}

} // namespace

bool apply_patch(Graph& graph, const std::string& ops_json, const PatchContext& context, std::string& error)
{
  nlohmann::json ops;
  try
  {
    ops = nlohmann::json::parse(ops_json);
  }
  catch (const std::exception& e)
  {
    error = std::string("not valid JSON: ") + e.what();
    return false;
  }
  if (!ops.is_array())
  {
    error = "ops must be an array";
    return false;
  }

  for (std::size_t i = 0; i < ops.size(); ++i)
  {
    const std::string where = "ops[" + std::to_string(i) + "]";
    const nlohmann::json& op = ops[i];
    if (!op.is_object())
    {
      error = where + " must be an object";
      return false;
    }
    if (!op.contains("op") || !op["op"].is_string())
    {
      error = where + " needs a string 'op'";
      return false;
    }

    const std::string kind = op["op"].get<std::string>();
    std::string reason;
    bool ok = false;
    if (kind == "add_node")
      ok = op_add_node(graph, op, where, context, reason);
    else if (kind == "remove_node")
      ok = op_remove_node(graph, op, where, reason);
    else if (kind == "connect")
      ok = op_connect(graph, op, where, reason);
    else if (kind == "disconnect")
      ok = op_disconnect(graph, op, where, reason);
    else if (kind == "rewire")
      ok = op_rewire(graph, op, where, reason);
    else if (kind == "swap_model")
      ok = op_swap_model(graph, op, where, context, reason);
    else if (kind == "set_param")
      ok = op_set_param(graph, op, where, reason);
    else if (kind == "bypass")
      ok = op_bypass(graph, op, where, reason);
    else
    {
      error = where + ": unknown op '" + kind + "'";
      return false;
    }

    if (!ok)
    {
      error = reason;
      return false;
    }
  }
  return true;
}

} // namespace graph
} // namespace namchain
