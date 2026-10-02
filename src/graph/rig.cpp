#include "namchain/graph/rig.h"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include <json.hpp>

namespace namchain
{
namespace graph
{
namespace
{

std::string at(const std::string& where, const std::string& what)
{
  return where + ": " + what;
}

/// \brief Mint an engine node id for a node that did not bring one.
///
/// `<element>` for a node that is its own element, `<element>/<role>` for a node inside one, and a
/// `~N` suffix if that name is already taken. Ids only have to be unique and legible in a dump; no
/// client addresses them, so nothing depends on the spelling (docs/ui-integration-plan.md §1.2).
std::string mint_node_id(const std::string& element, const std::string& role, const std::set<std::string>& used)
{
  const std::string base = role.empty() ? element : element + "/" + role;
  if (used.count(base) == 0)
    return base;
  for (int n = 2;; ++n)
  {
    std::string candidate = base + "~" + std::to_string(n);
    if (used.count(candidate) == 0)
      return candidate;
  }
}

/// Read an optional string key. \p ok is cleared and \p error set when the key is there but is not a
/// string, which is the difference between "absent, use the default" and "the author meant something".
std::string optional_string(const nlohmann::json& object, const char* key, const std::string& where, bool& ok,
                            std::string& error)
{
  if (!object.contains(key))
    return {};
  if (!object[key].is_string())
  {
    error = at(where, std::string("'") + key + "' must be a string");
    ok = false;
    return {};
  }
  return object[key].get<std::string>();
}

/// \brief Read an optional `meta`: the client's own data, which must be an object, kept as compact JSON.
bool read_meta(const nlohmann::json& object, const std::string& where, std::string& meta, std::string& error)
{
  if (!object.contains("meta"))
    return true;
  if (!object["meta"].is_object())
  {
    error = at(where, "'meta' must be an object");
    return false;
  }
  meta = object["meta"].dump();
  return true;
}

/// \brief Refuse a key this level of the document does not define. A client's own data has a place,
/// `meta`, so an unknown key is a mistake -- `output_chanel` -- rather than something to carry.
bool known_keys(const nlohmann::json& object, std::initializer_list<const char*> known, const std::string& where,
                std::string& error)
{
  for (const auto& item : object.items())
  {
    bool found = false;
    for (const char* key : known)
      found = found || item.key() == key;
    if (!found)
    {
      error = at(where, "unknown key '" + item.key() + "' (a client's own data goes in 'meta')");
      return false;
    }
  }
  return true;
}

bool parse_nodes(const nlohmann::json& nodes, const std::string& prefix, Graph& graph, std::string& error)
{
  std::set<std::string> used;
  for (std::size_t i = 0; i < nodes.size(); ++i)
  {
    const std::string where = prefix + std::to_string(i);
    const auto& object = nodes[i];
    if (!object.is_object())
    {
      error = at(where, "must be an object");
      return false;
    }
    if (!object.contains("type") || !object["type"].is_string())
    {
      error = at(where, "needs a string 'type'");
      return false;
    }

    bool ok = true;
    std::string id = optional_string(object, "id", where, ok, error);
    std::string element = optional_string(object, "element", where, ok, error);
    const std::string role = optional_string(object, "role", where, ok, error);
    if (!ok)
      return false;

    if (id.empty() && element.empty())
    {
      error = at(where, "needs an 'id', or an 'element' to derive one from");
      return false;
    }
    if (id.empty())
      id = mint_node_id(element, role, used);
    if (element.empty())
      element = id; // a node that does not name a document element is its own
    if (!used.insert(id).second)
    {
      error = at(where, "duplicate node id '" + id + "'");
      return false;
    }

    NodeSpec spec;
    spec.id = std::move(id);
    spec.type = object["type"].get<std::string>();
    spec.element = std::move(element);
    spec.role = role;
    if (object.contains("bypass"))
    {
      if (!object["bypass"].is_boolean())
      {
        error = at(where, "'bypass' must be a boolean");
        return false;
      }
      spec.bypass = object["bypass"].get<bool>();
    }
    if (!read_meta(object, where, spec.meta, error))
      return false;

    // Explicit blocks first, so an inline shorthand key can still override one of them.
    for (const char* block : {"params", "strings"})
    {
      if (!object.contains(block))
        continue;
      if (!object[block].is_object())
      {
        error = at(where, std::string("'") + block + "' must be an object");
        return false;
      }
      for (const auto& [key, value] : object[block].items())
      {
        if (std::string(block) == "params")
        {
          if (!value.is_number())
          {
            error = at(where, "params." + key + " must be a number");
            return false;
          }
          spec.params[key] = value.get<double>();
        }
        else
        {
          if (!value.is_string())
          {
            error = at(where, "strings." + key + " must be a string");
            return false;
          }
          spec.strings[key] = value.get<std::string>();
        }
      }
    }

    for (const auto& [key, value] : object.items())
    {
      if (key == "id" || key == "type" || key == "element" || key == "role" || key == "params" ||
          key == "strings" || key == "bypass" || key == "meta")
        continue;
      if (value.is_number())
        spec.params[key] = value.get<double>();
      else if (value.is_boolean())
        spec.params[key] = value.get<bool>() ? 1.0 : 0.0;
      else if (value.is_string())
        spec.strings[key] = value.get<std::string>();
      else
      {
        error = at(where, "'" + key + "' must be a number or a string");
        return false;
      }
    }

    graph.nodes.push_back(std::move(spec));
  }
  return true;
}

bool parse_connections(const nlohmann::json& connections, const std::string& prefix, Graph& graph, std::string& error)
{
  for (std::size_t i = 0; i < connections.size(); ++i)
  {
    const std::string where = prefix + std::to_string(i);
    const auto& object = connections[i];
    if (!object.is_object() || !object.contains("from") || !object.contains("to") ||
        !object["from"].is_string() || !object["to"].is_string())
    {
      error = at(where, "needs string 'from' and 'to'");
      return false;
    }
    if (!known_keys(object, {"from", "from_port", "to", "to_port"}, where, error))
      return false;
    Connection connection;
    connection.from = object["from"].get<std::string>();
    connection.to = object["to"].get<std::string>();

    // Ports are looked up through the same helper so that a bool, a float or a string is rejected
    // here rather than silently truncating to a port number that happens to exist.
    const auto port = [&](const char* key, int& out) {
      if (!object.contains(key))
        return true;
      if (!object[key].is_number_integer())
      {
        error = at(where, std::string("'") + key + "' must be an integer");
        return false;
      }
      out = object[key].get<int>();
      return true;
    };
    if (!port("from_port", connection.from_port) || !port("to_port", connection.to_port))
      return false;

    graph.connections.push_back(std::move(connection));
  }
  return true;
}

/// \brief The `nodes` and `connections` of one chain, from whatever object holds them.
bool parse_graph_body(const nlohmann::json& object, const std::string& where, const std::string& prefix, Graph& graph,
                      std::string& error)
{
  if (!object.contains("nodes") || !object["nodes"].is_array())
  {
    error = at(where, "needs a 'nodes' array");
    return false;
  }
  if (!parse_nodes(object["nodes"], prefix + "node ", graph, error))
    return false;
  if (object.contains("connections"))
  {
    if (!object["connections"].is_array())
    {
      error = at(where, "'connections' must be an array");
      return false;
    }
    if (!parse_connections(object["connections"], prefix + "connection ", graph, error))
      return false;
  }
  return true;
}

bool parse_chain(const nlohmann::json& object, const std::string& where, Chain& chain, std::string& error)
{
  if (!object.is_object())
  {
    error = at(where, "must be an object");
    return false;
  }
  if (!object.contains("id") || !object["id"].is_string() || object["id"].get<std::string>().empty())
  {
    error = at(where, "needs a non-empty string 'id'");
    return false;
  }
  chain.id = object["id"].get<std::string>();
  const std::string named = "chain '" + chain.id + "'";
  if (!known_keys(object, {"id", "name", "input_channel", "output_channel", "nodes", "connections", "meta"}, named,
                  error) ||
      !read_meta(object, named, chain.meta, error))
    return false;

  bool ok = true;
  chain.name = optional_string(object, "name", named, ok, error);
  if (!ok)
    return false;

  const auto channel = [&](const char* key, int& target) {
    if (!object.contains(key))
      return true;
    if (!object[key].is_number_integer() || object[key].get<int>() < 0)
    {
      error = at(named, std::string("'") + key + "' must be a non-negative integer");
      return false;
    }
    target = object[key].get<int>();
    return true;
  };
  if (!channel("input_channel", chain.input_channel) || !channel("output_channel", chain.output_channel))
    return false;

  return parse_graph_body(object, named, named + ": ", chain.graph, error);
}

} // namespace

Graph chain_graph(const std::string& model_path, unsigned instances)
{
  const unsigned count = instances == 0 ? 1u : instances;

  Graph graph;
  graph.nodes.push_back(NodeSpec{"in", "nc.input", {}, {}});
  for (unsigned i = 0; i < count; ++i)
  {
    NodeSpec nam;
    nam.id = "nam" + std::to_string(i);
    nam.type = "nc.nam";
    nam.strings["model"] = model_path;
    graph.nodes.push_back(std::move(nam));
  }
  graph.nodes.push_back(NodeSpec{"out", "nc.output", {}, {}});

  // Serial: in -> nam0 -> nam1 -> ... -> out. The order of `graph.nodes` is irrelevant to the
  // compiler, which is the point of declaring it in this shape.
  std::string previous = "in";
  for (unsigned i = 0; i < count; ++i)
  {
    const std::string id = "nam" + std::to_string(i);
    graph.connections.push_back(Connection{previous, 0, id, 0});
    previous = id;
  }
  graph.connections.push_back(Connection{previous, 0, "out", 0});
  return graph;
}

bool parse_document_json(const std::string& text, Document& document, std::string& error)
{
  nlohmann::json doc;
  try
  {
    doc = nlohmann::json::parse(text);
  }
  catch (const std::exception& e)
  {
    error = std::string("not valid JSON: ") + e.what();
    return false;
  }

  if (!doc.is_object())
  {
    error = "the rig must be a JSON object";
    return false;
  }

  Document parsed;
  bool ok = true;
  parsed.name = optional_string(doc, "name", "the rig", ok, error);
  if (!ok)
    return false;

  if (doc.contains("chains"))
  {
    if (doc.contains("nodes") || doc.contains("connections"))
    {
      error = "the rig has both 'chains' and top-level 'nodes'/'connections'; it is one or the other";
      return false;
    }
    if (!known_keys(doc, {"name", "chains", "meta"}, "the rig", error))
      return false;
    if (!doc["chains"].is_array())
    {
      error = "'chains' must be an array";
      return false;
    }
    if (doc["chains"].empty())
    {
      error = "a document needs at least one chain";
      return false;
    }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < doc["chains"].size(); ++i)
    {
      Chain chain;
      if (!parse_chain(doc["chains"][i], "chain " + std::to_string(i), chain, error))
        return false;
      if (!ids.insert(chain.id).second)
      {
        error = "chain " + std::to_string(i) + ": duplicate chain id '" + chain.id + "'";
        return false;
      }
      parsed.chains.push_back(std::move(chain));
    }
  }
  else
  {
    // The shape everything was in before documents existed: one graph, one chain, channels 0 and 0.
    // It is a compatibility rule rather than a special case, and it keeps every committed rig valid.
    if (!doc.contains("nodes") || !doc["nodes"].is_array())
    {
      error = "the rig needs a 'nodes' array, or a 'chains' array";
      return false;
    }
    if (!known_keys(doc, {"name", "nodes", "connections", "meta"}, "the rig", error))
      return false;
    Chain chain;
    chain.id = "main";
    if (!parse_graph_body(doc, "the rig", "", chain.graph, error))
      return false;
    parsed.chains.push_back(std::move(chain));
  }

  if (!read_meta(doc, "the rig", parsed.meta, error))
    return false;

  document = std::move(parsed);
  return true;
}

bool load_document_json(const std::string& path, Document& document, std::string& error)
{
  std::ifstream file(path);
  if (!file)
  {
    error = "cannot open rig '" + path + "'";
    return false;
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  if (!parse_document_json(contents.str(), document, error))
  {
    error = "'" + path + "': " + error;
    return false;
  }

  // File-naming string params are resolved against the rig's own directory first. A rig that says
  // "A2-Lite.nam" almost always means the file sitting next to it, and making that work means a
  // rig can be moved, or checked into a directory with its models, without editing paths.
  resolve_relative_paths(document, std::filesystem::path(path).parent_path().string());
  return true;
}

void resolve_relative_paths(Document& document, const std::string& base_dir)
{
  if (base_dir.empty())
    return;
  const std::filesystem::path directory(base_dir);
  for (Chain& chain : document.chains)
  {
    for (NodeSpec& node : chain.graph.nodes)
    {
      for (auto& [key, value] : node.strings)
      {
        std::error_code ignored;
        const std::filesystem::path candidate = directory / value;
        if (!std::filesystem::path(value).is_absolute() && std::filesystem::exists(candidate, ignored))
          value = candidate.string();
      }
    }
  }
}

bool parse_graph_json(const std::string& text, Graph& graph, std::string& error)
{
  Document document;
  if (!parse_document_json(text, document, error))
    return false;
  if (document.chains.size() != 1)
  {
    error = "this needs exactly one chain, found " + std::to_string(document.chains.size());
    return false;
  }
  graph = std::move(document.chains.front().graph);
  return true;
}

bool load_graph_json(const std::string& path, Graph& graph, std::string& error)
{
  Document document;
  if (!load_document_json(path, document, error))
    return false;
  if (document.chains.size() != 1)
  {
    error = "'" + path + "': this needs exactly one chain, found " + std::to_string(document.chains.size());
    return false;
  }
  graph = std::move(document.chains.front().graph);
  return true;
}

nlohmann::json graph_json(const Graph& graph)
{
  nlohmann::json nodes = nlohmann::json::array();
  for (const NodeSpec& node : graph.nodes)
  {
    nlohmann::json object;
    object["id"] = node.id;
    object["element"] = node.element;
    if (!node.role.empty())
      object["role"] = node.role;
    object["type"] = node.type;
    if (node.bypass)
      object["bypass"] = true;
    // Parameters keep their own spelling rather than being nested under "params", because that is
    // the form the parser accepts back: a number is a numeric param and a string is a string param.
    for (const auto& [key, value] : node.params)
      object[key] = value;
    for (const auto& [key, value] : node.strings)
      object[key] = value;
    if (!node.meta.empty())
      object["meta"] = nlohmann::json::parse(node.meta);
    nodes.push_back(std::move(object));
  }

  nlohmann::json connections = nlohmann::json::array();
  for (const Connection& connection : graph.connections)
  {
    nlohmann::json object;
    object["from"] = connection.from;
    object["from_port"] = connection.from_port;
    object["to"] = connection.to;
    object["to_port"] = connection.to_port;
    connections.push_back(std::move(object));
  }

  nlohmann::json result;
  result["nodes"] = std::move(nodes);
  result["connections"] = std::move(connections);
  return result;
}

std::string graph_to_json(const Graph& graph)
{
  return graph_json(graph).dump();
}

std::string document_to_json(const Document& document)
{
  nlohmann::json chains = nlohmann::json::array();
  for (const Chain& chain : document.chains)
  {
    const nlohmann::json graph = graph_json(chain.graph);

    nlohmann::json entry;
    entry["id"] = chain.id;
    if (!chain.name.empty())
      entry["name"] = chain.name;
    entry["input_channel"] = chain.input_channel;
    entry["output_channel"] = chain.output_channel;
    entry["nodes"] = graph["nodes"];
    entry["connections"] = graph["connections"];
    if (!chain.meta.empty())
      entry["meta"] = nlohmann::json::parse(chain.meta);
    chains.push_back(std::move(entry));
  }

  nlohmann::json object;
  if (!document.name.empty())
    object["name"] = document.name;
  object["chains"] = std::move(chains);
  if (!document.meta.empty())
    object["meta"] = nlohmann::json::parse(document.meta);
  return object.dump();
}

bool same_graph(const Graph& a, const Graph& b)
{
  if (a.nodes.size() != b.nodes.size() || a.connections.size() != b.connections.size())
    return false;

  // Everything a compile reads, including the identity fields: `element` and `role` are part of what a
  // client authored, and a document that changed only those is a change worth republishing (an op can
  // name an element). `meta` is not: the engine never reads it, so a change to it alone compiles to the
  // same plan and rebuilds nothing. Node order is compared too -- a reordering is semantically free but it is not
  // the *same document*, and treating it as one would silently ignore an edit.
  for (std::size_t i = 0; i < a.nodes.size(); ++i)
  {
    const NodeSpec& x = a.nodes[i];
    const NodeSpec& y = b.nodes[i];
    if (x.id != y.id || x.type != y.type || x.params != y.params || x.strings != y.strings ||
        x.bypass != y.bypass || x.slim != y.slim || x.element != y.element || x.role != y.role)
      return false;
  }
  for (std::size_t i = 0; i < a.connections.size(); ++i)
  {
    const Connection& x = a.connections[i];
    const Connection& y = b.connections[i];
    if (x.from != y.from || x.from_port != y.from_port || x.to != y.to || x.to_port != y.to_port)
      return false;
  }
  return true;
}

} // namespace graph
} // namespace namchain
