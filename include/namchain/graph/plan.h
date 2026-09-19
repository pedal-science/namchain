#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "namchain/graph/graph.h"
#include "namchain/rt/arena.h"

namespace namchain
{
namespace render
{

/// Everything a node needs to know at construction time that does not come from its own params.
///
/// This is the host-side half of the node contract (§6.1): a node converts times and frequencies to
/// samples once, at build, rather than dividing by the rate on the audio thread. `max_quantum` is
/// also the block size a node may assume, so a delay node can size its ring exactly.
struct CompileOptions
{
  int max_quantum = 64;
  /// The engine's rate. 48 kHz is the M1 target; models are loaded at whatever this says (§7.1).
  double sample_rate = 48000.0;
};

/// A node type's vtable. `create`/`destroy` run on the control thread while a plan is built;
/// `process` is the render path and must not allocate, lock or throw (§3.3).
struct NodeType
{
  std::string id;
  int inputs = 0;
  int outputs = 0;
  /// \param error set when the node cannot be constructed (bad params, missing model, ...).
  void* (*create)(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error) = nullptr;
  void (*destroy)(void* state) = nullptr;
  /// Buffers are mono for now; per-port channel counts arrive with the multichannel work.
  void (*process)(void* state, const float* const* in, float* const* out, int frames) noexcept = nullptr;
  /// Control thread only: the concrete implementation behind a state, for measurement reports.
  /// Which A2 kernel set is live changes the cost by ~2x, so a timing number without it is not
  /// evidence (§11). Null for types whose implementation is not worth naming.
  std::string (*describe)(void* state) = nullptr;
  /// Render path, called once per block before any node runs: hands the graph's input block to the
  /// node that samples it. Only `nc.input` uses it. This exists so that `nc.input` samples the
  /// incoming audio at *its* point in the schedule, which is what lets a block-delay node read the
  /// previous block's input rather than the current one.
  void (*set_block_input)(void* state, const float* source) noexcept = nullptr;
  /// \brief True for a node that reads its input as of the *previous* block, and so breaks a cycle.
  ///
  /// This is how feedback is expressed (§5.1). The compiler drops the edges *into* such a node when
  /// it works out the schedule -- the node reads whatever its input buffer already contains, which
  /// is last block's value -- and schedules it before every other node, so that "already contains"
  /// is the previous block and not the current one. Its consumers then run after it and read a
  /// value that is exactly one block old.
  ///
  /// The consequence to be aware of: the value such a node reads is pinned to the end of the block
  /// by the memory plan, because its buffer has to survive until the next block's early read.
  bool block_delay = false;
  /// \brief The spec param that overrides \ref inputs or \ref outputs, when arity is per-spec.
  ///
  /// `nc.split` names `outputs` and `nc.mix` names `inputs`; every other type leaves this null and
  /// keeps the fixed counts above. Resolving the count from the spec rather than hard-coding 1->2 /
  /// 2->1 is what lets one rig fan out to N parallel chains, and it is the first piece of per-spec
  /// arity in the node ABI (plan.md §6.1). The named count defaults to the vtable's own value when
  /// the param is absent, so a rig written before N-way arity still means what it did.
  const char* arity_param = nullptr;
  /// Bounds for the resolved count, inclusive. A spec outside them is a compile error rather than a
  /// silently clamped graph, and the upper bound is what stops a typo from asking the memory plan
  /// for a thousand ports.
  int min_ports = 1;
  int max_ports = 64;
  /// \brief The settable parameter names, in id order: name `i` takes id `i + 1`.
  ///
  /// Control thread, at compile time: the compiler resolves a rig's param name to the integer id
  /// that travels on the render path, so nothing there compares a string (§6.1). Null, with a count
  /// of 0, is a node with nothing to set. A table rather than a name→id function because a tool also
  /// needs to *enumerate* what can be patched without knowing each node type -- which is how the
  /// timeline generator and a future `rig.get` learn the settable set.
  const char* const* settable_params = nullptr;
  int settable_param_count = 0;
  /// \brief Render path: ramp \p value into the node over the node's own smoothing time.
  ///
  /// The value is range-checked by the caller. Returns false for an unknown id, and the node leaves
  /// its state untouched rather than jumping: an unramped step is the click this exists to avoid
  /// (§5.3, the `smooth` op class). Nothing here may allocate, lock or throw.
  bool (*set_param)(void* state, uint32_t param_id, float value) noexcept = nullptr;
};

/// \brief A node spec's concrete port counts, after per-spec arity is resolved.
struct PortCounts
{
  int inputs = 0;
  int outputs = 0;
};

/// \brief Resolve the port counts a node spec actually gets.
///
/// For most types this is the vtable's fixed counts. `nc.split`/`nc.mix` read `outputs`/`inputs`
/// from the spec, so one rig can fan out to N chains. Both `validate` and `compile` call this rather
/// than reading `NodeType::inputs`/`outputs` directly, so the two can never disagree about what
/// port 3 means.
///
/// \return false and set \p error when the parameter is present but is not a whole number in range.
bool resolve_ports(const NodeType& type, const graph::NodeSpec& spec, PortCounts& ports, std::string& error);

/// \return the built-in type, or nullptr if the id is unknown.
const NodeType* find_node_type(const std::string& id);

} // namespace render

namespace graph
{

/// One node placed into a plan, with its buffers already bound.
///
/// Pointer arrays are built at compile time, not per block: constructing them in `execute` would
/// allocate or at least introduce avoidable work on the render path.
struct PlanNode
{
  const render::NodeType* type = nullptr;
  void* state = nullptr;
  std::vector<const float*> inputs;
  std::vector<float*> outputs;
};

/// \brief An immutable, fully allocated render plan.
///
/// Published to the audio thread by swapping an index (§3.2); the audio thread only ever
/// `execute`s it. Everything it needs -- buffers, bound pointers -- exists before publication, so
/// the render path performs no allocation and takes no locks.
class RenderPlan
{
public:
  /// \param arena supplies the intermediate buffers; the plan does not own storage, so one arena
  ///        per plan is the intended usage and is what makes reclamation a pointer swap.
  static std::unique_ptr<RenderPlan> compile(const Graph& graph, const render::CompileOptions& options,
                                             rt::Arena& arena, std::string& error);

  /// Convenience for the common case where the engine rate is the 48 kHz default.
  static std::unique_ptr<RenderPlan> compile(const Graph& graph, int max_quantum, rt::Arena& arena,
                                             std::string& error);

  ~RenderPlan();

  RenderPlan(const RenderPlan&) = delete;
  RenderPlan& operator=(const RenderPlan&) = delete;

  /// Render one block. \p input and \p output are mono and \p frames long. RT-safe.
  void execute(const float* input, float* output, int frames) noexcept;

  /// \brief Resolve a node id and a param name to what \ref set_param takes.
  ///
  /// Control thread. The plan's node order is its execution order, not the rig's declaration order,
  /// so a caller that only knows ids has to resolve through here rather than count nodes.
  /// \return false when the node does not exist, its type has no settable params, or the name is
  /// not one of them.
  bool resolve_param(const std::string& node_id, const std::string& param, std::size_t& node_index,
                     uint32_t& param_id) const;

  /// \brief Render path: ramp one node's parameter to \p value.
  ///
  /// \return false for an out-of-range index or an id the node does not know. Never allocates or
  /// locks; the node smooths the change over its own ramp, so this is the no-recompile, no-click
  /// path of §5.3's `smooth` class.
  bool set_param(std::size_t node_index, uint32_t param_id, float value) noexcept;

  /// Node ids in execution order, parallel to \ref nodes.
  const std::vector<std::string>& node_ids() const noexcept { return node_ids_; }

  /// \brief Run one quantum of silence through every node, discarding the output.
  ///
  /// The first `process()` call on a freshly built model costs milliseconds -- reset and warm-up
  /// work that has nothing to do with steady-state cost -- and a stall that long can drain the
  /// device ring on its own. Call this on the control thread before publishing the plan (§3.3).
  /// It is not part of `execute` because it mutates node state: the oracle has to be warmed too.
  void prewarm() noexcept;

  int max_quantum() const noexcept { return max_quantum_; }
  /// Intermediate buffers the memory plan needed. Fewer than the node count means it worked.
  int buffer_count() const noexcept { return static_cast<int>(buffers_.size()); }
  std::size_t buffer_bytes() const noexcept;
  const std::vector<PlanNode>& nodes() const noexcept { return nodes_; }

  /// Debug/inspection: the buffer index each node output was coloured onto.
  const std::vector<int>& output_buffer_of(std::size_t node_index) const;

private:
  RenderPlan() = default;

  std::vector<PlanNode> nodes_;    // topological order
  std::vector<std::string> node_ids_; // parallel to nodes_, for id -> index resolution
  std::vector<float*> buffers_;    // all from the arena
  int input_node_ = -1;            // index in nodes_ whose block input is sampled from the graph
  int output_buffer_ = -1;         // buffer the graph output is read from
  int max_quantum_ = 0;
  std::vector<std::vector<int>> output_buffers_; // per node, per port
};

} // namespace graph
} // namespace namchain
