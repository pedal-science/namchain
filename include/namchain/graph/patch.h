#pragma once

// Structural patch ops: the graph-mutation half of the `rig.patch` protocol (plan.md §8.4).
//
// `rig.patch` has two halves with completely different costs (§5.3). A `set_param` the live plan can
// dispatch is the `smooth` class: a per-sample ramp inside the node, no recompile. Everything else --
// adding, removing, connecting, disconnecting, repointing an edge, swapping a model, or changing a
// parameter no node can ramp -- is structural: it builds a new graph, which the engine compiles,
// dual-renders against the old plan and crossfades into.
//
// This file is only the graph mutation. It knows nothing about plans, threads or transitions, which
// is what makes it testable on its own; the daemon decides smooth versus structural and publishes.
#include <string>

#include "namchain/graph/graph.h"

namespace namchain
{
namespace graph
{

struct PatchContext
{
  /// Directory a relative model/IR path in an added or swapped node resolves against: the current
  /// rig's own directory, so a patch and the rig it patches agree about where models live. Empty
  /// leaves relative paths alone.
  std::string base_dir;
};

/// \brief Apply a JSON array of patch ops to \p graph, in place.
///
/// Ops (all node references accept `"node"`; `add_node` takes `"id"`):
///   {"op":"add_node","id":"g2","type":"nc.gain","gain":0.5}
///   {"op":"remove_node","node":"g2"}
///   {"op":"connect","from":"g","from_port":0,"to":"g2","to_port":0}
///   {"op":"disconnect","to":"g2","to_port":0}          // or with from/from_port too
///   {"op":"rewire","from":"g2","to":"out","to_port":0}  // repoint that input port
///   {"op":"swap_model","node":"amp","model":"other.nam"}
///   {"op":"set_param","node":"g","param":"gain","value":0.5}
///   {"op":"bypass","node":"g","bypass":true}           // omit "bypass" to enable
///
/// The graph is modified in place and **may be left half-applied** if this returns false: callers
/// that need all-or-nothing apply it to a copy (the daemon does), which is why the mutation is kept
/// separate from publication.
///
/// Port ranges, dangling connections, duplicate node ids and cycles are *not* checked here. They are
/// the compiler's validation, and duplicating it would give this layer and `validate()` two chances
/// to disagree; an op list that produces a graph the compiler rejects is a rejected patch.
bool apply_patch(Graph& graph, const std::string& ops_json, const PatchContext& context, std::string& error);

} // namespace graph
} // namespace namchain
