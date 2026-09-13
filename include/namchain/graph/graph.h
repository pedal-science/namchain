#pragma once

#include <map>
#include <string>
#include <vector>

namespace namchain
{
namespace graph
{

/// A node as declared by a rig/patch. Parameters are resolved to a fixed layout at compile time, so
/// nothing on the render path ever touches a string (§6.1).
struct NodeSpec
{
  std::string id;
  std::string type;
  std::map<std::string, double> params;
  /// Params that are not numbers: a model path, for instance. Kept separate so the numeric path
  /// stays a plain map lookup rather than a variant.
  std::map<std::string, std::string> strings;
};

/// `from`'s output port \ref from_port feeds `to`'s input port \ref to_port.
struct Connection
{
  std::string from;
  int from_port = 0;
  std::string to;
  int to_port = 0;
};

struct Graph
{
  std::vector<NodeSpec> nodes;
  std::vector<Connection> connections;
};

/// Compile is transactional (§5.2): a graph either compiles or is rejected with a reason, and the
/// running plan is left untouched. This is the "or is rejected" half.
struct Validation
{
  bool ok = true;
  std::string message;

  static Validation failure(std::string reason);
};

/// Structural checks only -- unknown types, duplicate ids, dangling connections, port ranges,
/// cycles. Cost and param-range checks belong to the compiler.
Validation validate(const Graph& graph);

} // namespace graph
} // namespace namchain
