#include "namchain/graph/rig.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

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

bool parse_graph_json(const std::string& text, Graph& graph, std::string& error)
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
  if (!doc.contains("nodes") || !doc["nodes"].is_array())
  {
    error = "the rig needs a 'nodes' array";
    return false;
  }

  Graph parsed;
  const auto& nodes = doc["nodes"];
  for (std::size_t i = 0; i < nodes.size(); ++i)
  {
    const std::string where = "node " + std::to_string(i);
    const auto& object = nodes[i];
    if (!object.is_object())
    {
      error = at(where, "must be an object");
      return false;
    }
    if (!object.contains("id") || !object["id"].is_string())
    {
      error = at(where, "needs a string 'id'");
      return false;
    }
    if (!object.contains("type") || !object["type"].is_string())
    {
      error = at(where, "needs a string 'type'");
      return false;
    }

    NodeSpec spec;
    spec.id = object["id"].get<std::string>();
    spec.type = object["type"].get<std::string>();

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
      if (key == "id" || key == "type" || key == "params" || key == "strings")
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

    parsed.nodes.push_back(std::move(spec));
  }

  if (doc.contains("connections"))
  {
    if (!doc["connections"].is_array())
    {
      error = "'connections' must be an array";
      return false;
    }
    const auto& connections = doc["connections"];
    for (std::size_t i = 0; i < connections.size(); ++i)
    {
      const std::string where = "connection " + std::to_string(i);
      const auto& object = connections[i];
      if (!object.is_object() || !object.contains("from") || !object.contains("to") ||
          !object["from"].is_string() || !object["to"].is_string())
      {
        error = at(where, "needs string 'from' and 'to'");
        return false;
      }
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

      parsed.connections.push_back(std::move(connection));
    }
  }

  graph = std::move(parsed);
  return true;
}

bool load_graph_json(const std::string& path, Graph& graph, std::string& error)
{
  std::ifstream file(path);
  if (!file)
  {
    error = "cannot open rig '" + path + "'";
    return false;
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  if (!parse_graph_json(contents.str(), graph, error))
  {
    error = "'" + path + "': " + error;
    return false;
  }

  // File-naming string params are resolved against the rig's own directory first. A rig that says
  // "A2-Lite.nam" almost always means the file sitting next to it, and making that work means a
  // rig can be moved, or checked into a directory with its models, without editing paths.
  const std::filesystem::path directory = std::filesystem::path(path).parent_path();
  if (!directory.empty())
  {
    for (NodeSpec& node : graph.nodes)
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
  return true;
}

} // namespace graph
} // namespace namchain
