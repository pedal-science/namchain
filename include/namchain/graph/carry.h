#pragma once

// Local transitions: carry what an edit did not touch from the plan that is playing into the next one,
// and crossfade only where the edit changed the signal (docs/transitions.md, "Local transitions").
//
// A structural edit used to rebuild the whole chain: every node created again (every model and IR read
// from its file and reset to silence, every delay emptied), then the old and new plans both rendered
// for the fade. Adding one amp to a one-amp rig ran three amps for the fade. Here the two graphs are
// compared node by node instead:
//
//   * a node whose type and settings are unchanged keeps its instance, state and all;
//   * a node whose only change is a value that ramps keeps its instance and is ramped;
//   * a node that is new, or changed in a way that cannot ramp, is created;
//   * where a kept node's input now comes from somewhere else, a crossfade (`nc.xfade`) is placed on
//     that input, fed by the signal as it was and the signal as it is becoming. A node that is going
//     away runs only until those fades close.
//
// So a chain runs two plans in turn: a transitional one while the fades are open, then the final one.
// Both share the kept instances with the plan that was playing, and the render thread only ever runs
// one of the three.
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "namchain/graph/graph.h"
#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"

namespace namchain
{
namespace graph
{

/// One parameter of a kept node whose value moved: ramped on the render thread, not rebuilt.
struct CarriedRamp
{
  std::string node;  ///< lowered node id
  std::string param; ///< the node type's parameter id
  double value = 0.0;
};

/// \brief How a chain gets from the graph it is running to the one it was given, keeping what it can.
///
/// Both graphs are *lowered* (every macro expanded), because instances belong to primitive nodes.
struct LocalPlan
{
  /// Nothing changed but values that ramp: no new plan at all, only \ref ramps.
  bool ramps_only = false;
  /// No fade is needed and nothing old has to keep running (an edit that only renamed an element, or
  /// dropped a node that fed nothing): the final plan takes over from the next block.
  bool instant = false;
  /// What the chain runs while the fades are open. Empty when \ref ramps_only or \ref instant.
  Graph transitional;
  /// The new graph, as the final plan runs it.
  Graph final_graph;
  /// Every node of \ref transitional whose instance comes from the running plan, and its id there.
  /// A node kept as it was has the same id in both; a node that is going away is renamed in the
  /// transitional graph, because a new node may now have its id.
  std::map<std::string, std::string> carried_from;
  /// Block-delay nodes whose state is the input buffer the running plan last wrote: (id in the running
  /// plan, id in the plan that takes over), for that buffer to be copied across at the switch.
  std::vector<std::pair<std::string, std::string>> buffers_from_running;
  /// Kept nodes whose ramping parameters moved.
  std::vector<CarriedRamp> ramps;

  /// For reports and tests.
  int fades = 0;           ///< crossfades in the transitional graph
  int kept = 0;            ///< nodes of the final graph whose instance carried over
  int created = 0;         ///< nodes of the final graph created for it
  int retiring = 0;        ///< nodes kept running only until the fades close
};

/// \brief Work out a local transition from \p before (the running chain) to \p after, both lowered.
///
/// \param unusable ids in \p before whose instances must not be kept, because the non-finite guard
///        failed them: a failed node is rebuilt, which is how an edit heals one.
/// \param fade_frames the length of every crossfade, in frames.
/// \return false, with \p reason, when a local transition does not apply and the caller should use a
///         whole-chain crossfade instead: the chain's input or output node is not the same node, or
///         nothing worth keeping would carry over (a different rig in the same chain).
bool plan_local_transition(const Graph& before, const Graph& after, const std::set<std::string>& unusable,
                           int fade_frames, LocalPlan& plan, std::string& reason);

/// The plans a local transition runs, compiled, and what has to be copied between them.
struct CarriedPlans
{
  /// Runs while the fades are open; null for an instant transition.
  std::unique_ptr<RenderPlan> transitional;
  /// Runs once they close (or at once, for an instant transition).
  std::unique_ptr<RenderPlan> final_plan;
  /// Copied when the first of the two takes over from the running plan, and when the final plan takes
  /// over from the transitional one: (source, destination), `max_quantum` floats each.
  std::vector<std::pair<const float*, float*>> on_adopt;
  std::vector<std::pair<const float*, float*>> on_final;
};

/// \brief Compile \p plan against the running plan: the kept instances adopted, the rest created, the
/// fresh ones prewarmed, and the buffer copies resolved to pointers.
///
/// Control thread. Nothing here touches a kept instance's state, which is still being rendered.
bool build_carried_plans(const RenderPlan& running, const LocalPlan& plan, const render::CompileOptions& options,
                         rt::Arena& transitional_arena, rt::Arena& final_arena, CarriedPlans& plans,
                         std::string& error);

} // namespace graph
} // namespace namchain
