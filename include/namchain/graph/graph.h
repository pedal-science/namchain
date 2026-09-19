#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace namchain
{
namespace graph
{

/// A node as declared by a rig/patch. Parameters are resolved to a fixed layout at compile time, so
/// nothing on the render path ever touches a string (§6.1).
struct NodeSpec
{
  NodeSpec() = default;
  /// The four fields a rig always states, so `NodeSpec{"in", "nc.input"}` and its older four-argument
  /// spelling both read the way a rig does. Everything else has a default and is set by name.
  NodeSpec(std::string node_id, std::string node_type, std::map<std::string, double> node_params = {},
           std::map<std::string, std::string> node_strings = {})
    : id(std::move(node_id)), type(std::move(node_type)), params(std::move(node_params)),
      strings(std::move(node_strings))
  {
  }

  std::string id;
  std::string type;
  std::map<std::string, double> params;
  /// Params that are not numbers: a model path, for instance. Kept separate so the numeric path
  /// stays a plain map lookup rather than a variant.
  std::map<std::string, std::string> strings;
  /// \brief The node is bypassed: it still runs -- so its state stays warm and un-bypassing does not
  /// step on stale state (plan.md G14) -- but its output is replaced by its dry input.
  ///
  /// Only a 1->1 node can be bypassed; the compiler rejects anything else, because "the dry path" of
  /// a fan-out or a mixer is not a single signal. Carried in the IR rather than applied by rewriting
  /// connections, so un-bypass is one flag and the original graph is never lost.
  bool bypass = false;

  /// \brief The client-owned document element this node belongs to: a block, a lane, a parallel
  /// section, or whatever else the author of the document considers one thing (docs/ui-integration-plan.md
  /// §1.2).
  ///
  /// The engine stores this and hands it back; it never interprets it, and it never composes an id
  /// out of it. Several nodes may share one element -- that is the normal case once a macro lowers
  /// into primitives -- so it is deliberately not required to be unique, and it is not an addressing
  /// key on its own. An op that names an element is resolved to the element's declared parameter map
  /// (or to one of its nodes by \ref role), not by matching strings.
  ///
  /// Defaults to \ref id when a document does not say, so a hand-written rig needs no new field. It
  /// may contain anything, including a UUID with punctuation in it: nothing parses it.
  std::string element;

  /// \brief Which node of \ref element this is, from the node type's own declaration (`"line"`,
  /// `"ts.bass"`) -- the second half of the address an op can use, and what makes a lowered document
  /// legible in `rig.get`.
  ///
  /// Empty for a node that is its own element, which is every node in a hand-written rig.
  std::string role;
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

/// \brief True when two graphs would compile to the same plan: same nodes, same parameters, same
/// connections, in the same order.
///
/// Used to decide whether a publish changes anything. A document whose unchanged chains are
/// published again would otherwise recompile them -- reloading every model on the control thread --
/// and crossfade them, which is both expensive and audible for a chain nobody edited
/// (docs/ui-integration-plan.md §1.1).
bool same_graph(const Graph& a, const Graph& b);

} // namespace graph
} // namespace namchain
