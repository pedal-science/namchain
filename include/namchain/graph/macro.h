#pragma once

// Macros: the lowering stage (docs/ui-integration-plan.md §1.5, slice E).
//
// A macro is a document-level node the engine expands into primitives. It exists because a client
// should be able to say "a feedback delay" without knowing that this engine expresses feedback as
// `nc.mix` + `nc.delay` + `nc.gain` + `nc.unit_delay` in a cycle, and because that expansion is DSP
// vocabulary: one implementation in the engine beats every client inventing its own (docs/ljoss-
// comparison.md §3.1).
//
// Three things are declared, not discovered:
//
//   * the **schema**, the same `ParamDesc` array a node type publishes, so a client cannot tell a
//     macro from a primitive except by `kind`;
//   * the **param map**: each macro parameter drives one or more `(role, node param, transform)`
//     targets, which is what makes a macro-level `set_param` possible at all -- the client sends one
//     value and the engine derives the rest;
//   * the **bypass rule**, per macro, because `NodeSpec::bypass` is a 1->1 substitution and a macro
//     is often not 1->1.
//
// Macros are lowered *per compile* and never stored lowered, so `rig.get` returns the document the
// user wrote and a lowering can change without breaking anything a client holds.
#include <string>
#include <vector>

#include "namchain/graph/graph.h"
#include "namchain/graph/plan.h"

namespace namchain
{
namespace graph
{

/// \brief The closed set of transforms a param map may apply.
///
/// Deliberately tiny, and not a language: there is no expression to parse, nothing is evaluated on
/// the audio thread, and a target's value is a pure function of one macro parameter. All five are
/// implemented and unit-tested; the first seven macros use `Direct`, `OneMinus` and `ScaleBy`.
enum class Transform
{
  Direct,     ///< the value as given
  DbToLinear, ///< 10^(value/20)
  LinearToDb, ///< 20*log10(value), clamped to a floor so that silence is a large negative number
  OneMinus,   ///< 1 - value: the other half of a dry/wet pair
  ScaleBy,    ///< value * scale
};

/// \brief Apply \p transform to one macro parameter value. \p scale is used by \ref Transform::ScaleBy.
double apply_transform(Transform transform, double value, double scale);

/// One node parameter a macro parameter drives.
struct MacroTarget
{
  const char* role = nullptr;  ///< which lowered node
  const char* param = nullptr; ///< that node's parameter name
  Transform transform = Transform::Direct;
  double scale = 0.0; ///< \ref Transform::ScaleBy only
};

/// \brief One macro parameter: its schema entry, and what it drives.
///
/// A parameter with no targets is consumed by the lowering itself (a band count, a slope) and has no
/// runtime target at all: editing it is a structural change by definition, which is why the count is
/// allowed to be zero and why the classification rule says so out loud.
struct MacroParamDecl
{
  render::ParamDesc desc;
  const MacroTarget* targets = nullptr;
  int target_count = 0;
};

/// One primitive a macro lowers to. The id is minted by the framework from the macro's element and
/// this role; the lowering never builds an id.
struct MacroNodeDecl
{
  const char* role = nullptr;
  const char* type = nullptr;
};

/// One edge inside a lowering, named by role: the lowering is a table, not a graph builder.
struct MacroEdge
{
  const char* from_role = nullptr;
  int from_port = 0;
  const char* to_role = nullptr;
  int to_port = 0;
};

/// \brief Where an outside connection lands: macro port \p port feeds (or is fed by) \p role's port.
///
/// A list rather than one entry per port, because a macro's input may fan out -- a dry/wet delay
/// feeds both its dry gain and its delay line -- and saying that here is cheaper than an internal
/// split node that exists only to satisfy a data structure.
struct MacroPortLink
{
  int port = 0;
  const char* role = nullptr;
  int role_port = 0;
};

/// How a macro's `bypass` flag is realised (docs/ljoss-comparison.md §3.1, design consequence 1).
enum class MacroBypass
{
  /// Every lowered node takes the flag. Correct for a serial chain of primitives that are transparent
  /// when bypassed, which is all an EQ, a filter, a tone stack or a cab is: each node substitutes its
  /// dry input, so the chain passes the signal and every node keeps running (state stays warm).
  ///
  /// It is an OR, not an assignment: a lowering may bypass a node for its own reason (`slope`,
  /// `bands`), and the macro's flag is added to that rather than replacing it.
  AllRolesCarry,
  /// The lowering is not a serial chain, so bypass is a wet/dry crossfade: the two declared roles are
  /// gains, and bypass sets the dry one to unity and the wet one to silence. The wet path still runs,
  /// so the delay line and the filter states stay warm.
  Crossfade,
};

struct MacroType;

/// \brief The view of its own lowering that a macro's `apply` function is handed.
///
/// Small on purpose: the lowering says what its nodes are, and this says what the document asked for.
/// `number`/`text` fall back to the declared default, so a lowering never repeats a default value and
/// cannot disagree with the schema about one.
struct Lowering
{
  const NodeSpec* spec = nullptr;
  const MacroType* type = nullptr;
  std::vector<NodeSpec>* nodes = nullptr;

  /// The node declared with this role. The framework created it, so the role exists.
  NodeSpec& node(const char* role);
  /// Whether the document gave this parameter at all.
  bool has(const char* param) const;
  /// A numeric parameter's value, or its declared default.
  double number(const char* param) const;
  /// A string (option) parameter's value. Empty when the document gave none and it has no default.
  std::string text(const char* param) const;
  /// The macro node's own bypass flag.
  bool bypassed() const { return spec->bypass; }
};

/// \brief One macro: a schema, a shape, a param map and a bypass rule.
struct MacroType
{
  const char* id = nullptr;
  const char* name = nullptr;
  const char* category = nullptr;
  int inputs = 1;
  int outputs = 1;
  /// Where the outside world connects, in port order (possibly several links per port).
  const MacroPortLink* input_links = nullptr;
  int input_link_count = 0;
  const MacroPortLink* output_links = nullptr;
  int output_link_count = 0;
  /// The schema, and the param map -- one array, because a parameter without a map entry would be a
  /// knob that does nothing.
  const MacroParamDecl* params = nullptr;
  int param_count = 0;
  /// The primitives this macro lowers to, and the edges between them.
  const MacroNodeDecl* nodes = nullptr;
  int node_count = 0;
  const MacroEdge* edges = nullptr;
  int edge_count = 0;
  MacroBypass bypass = MacroBypass::AllRolesCarry;
  /// \ref MacroBypass::Crossfade: the two gain roles the flag drives.
  const char* dry_role = nullptr;
  const char* wet_role = nullptr;
  /// \brief Fill in the params and strings the declarative shape cannot express.
  ///
  /// Called after the framework has created the nodes and before it applies the param map, so the map
  /// has the last word about its own targets. \return false with a message for a document this macro
  /// cannot lower (an option it does not know, a required parameter that is missing).
  bool (*apply)(Lowering& lowering, std::string& error) = nullptr;
};

/// Every registered macro, in the order `engine.schema` reports them.
const std::vector<const MacroType*>& all_macro_types();

/// The macro with this type id, or null. A node type id and a macro id never collide: they are
/// different namespaces of the same registry.
const MacroType* find_macro_type(const std::string& id);

/// One macro parameter's declaration, or null.
const MacroParamDecl* find_macro_param(const MacroType& type, const std::string& name);

/// \brief The id the engine mints for a lowered node.
///
/// `<element>:<role>`, which is readable in a graph dump and diagnosable in a bug report, and which
/// the contract calls advisory: a client addresses `element` + `role` and never this
/// (docs/ui-integration-plan.md §1.2). Uniqueness is the engine's job, so a collision with an id the
/// document already used is broken with a `~2`, `~3`, ... suffix.
std::string mint_node_id(const std::string& element, const std::string& role, const std::vector<std::string>& taken);

/// \brief Lower every macro node in \p graph into primitives, writing the result to \p out.
///
/// A graph with no macros comes back as a copy, so a caller can use this unconditionally. The result
/// is validated by the caller (the compiler does it) because a lowering is engine code and a lowering
/// that produces an invalid graph is an engine bug, not a document error.
///
/// \param error names the macro node and what it could not lower.
bool lower_graph(const Graph& graph, Graph& out, std::string& error);

} // namespace graph
} // namespace namchain
