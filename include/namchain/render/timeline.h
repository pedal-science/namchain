#pragma once

// The scripted edit timeline: plan.md §5.1 item 4's offline awareness path, and the deterministic
// driver for the glitch detector (§10.3).
//
// `namchain-render` (or a test) renders a file through a compiled graph while applying a list of
// `{at_sample, op}` edits, then analyses the result against before/after oracles. The point of doing
// it offline with a virtual clock is that an edit lands at an exact sample on every run: no sockets,
// no timing luck, and therefore a failure that can be repeated.
//
// Only the `smooth` op class of §5.3 is implemented so far -- a `set_param` that ramps inside the
// node. The structural classes (`bypass`, load, rewire) need the transition machinery and are
// deliberately absent: naming one here is an error rather than a silent no-op.
#include <cstdint>
#include <string>
#include <vector>

#include "namchain/glitch.h"
#include "namchain/graph/graph.h"

namespace namchain
{
namespace render
{

enum class EditKind
{
  SetParam, ///< ramp a settable parameter; no recompile, no step (§5.3's `smooth` class)
};

struct EditOp
{
  EditKind kind = EditKind::SetParam;
  /// Sample offset from the start of the render. The op is applied *before* this sample is produced.
  uint64_t at_sample = 0;
  std::string node;  ///< node id
  std::string param; ///< settable param name
  double value = 0.0;
};

struct EditTimeline
{
  /// In nondecreasing `at_sample` order. A timeline is a sequence; sorting one silently would hide a
  /// caller's bug, so parse rejects disorder rather than repairing it.
  std::vector<EditOp> ops;
};

/// \brief Parse and validate a timeline against the graph it will run on.
///
/// A timeline that names an unknown op kind, node or parameter is rejected with a reason, before any
/// rendering: the harness must not be able to "pass" by applying nothing.
bool parse_timeline_json(const std::string& text, const graph::Graph& graph, EditTimeline& timeline,
                         std::string& error);

bool load_timeline_json(const std::string& path, const graph::Graph& graph, EditTimeline& timeline,
                        std::string& error);

/// \brief A deterministic randomised set_param timeline: the torture render's input.
///
/// Every op targets a parameter some node in \p graph actually declares settable. Positions are
/// spread across \p frames with a guaranteed minimum gap, because the per-edit oracle model below
/// assumes a parameter is settled before the next edit to it begins. Same \p seed, same timeline.
EditTimeline random_timeline(const graph::Graph& graph, unsigned edits, uint64_t frames, uint64_t seed);

/// The number of (node, param) pairs in \p graph that a timeline could set. 0 means no edit exists.
unsigned settable_param_count(const graph::Graph& graph);

struct TimelineRenderOptions
{
  int quantum = 64;
  /// Judge every edit against before/after oracles with the glitch detector (§10.3).
  bool glitch_checks = true;
  glitch::Options glitch_options{};
};

struct TimelineRenderResult
{
  bool ok = false;          ///< the render was written
  std::string error;
  uint64_t frames = 0;
  unsigned edits = 0;
  unsigned judged = 0;      ///< edits with enough settled signal either side to be judged
  /// False when a judged edit failed a rule. `ok` stays true: the WAV is still written, so a failing
  /// torture render can be listened to.
  bool glitch_ok = true;
  std::vector<glitch::Finding> findings;
};

/// \brief Render \p input_wav through \p graph, applying \p timeline at exact samples.
///
/// Mono, like the render path. The edits go through \ref graph::RenderPlan::set_param, which is the
/// same vtable dispatch the daemon uses, so what is measured here is the engine's mechanism and not
/// a test-only copy of it.
///
/// The before/after oracles are compiled from the same graph and driven to the parameter snapshot
/// that applies at that edit, using the same ramp; each is rendered from frame 0 so its state at the
/// edit is the state the live chain had. That is exact for any node whose state is a function of the
/// input and the parameters, and it is why the generator insists on a gap between edits: a parameter
/// that is still ramping when the next oracle is taken has no settled value to compare against.
TimelineRenderResult render_timeline_to_wav(const graph::Graph& graph, const EditTimeline& timeline,
                                            const std::string& input_wav, const std::string& output_wav,
                                            const TimelineRenderOptions& options = {});

} // namespace render
} // namespace namchain
