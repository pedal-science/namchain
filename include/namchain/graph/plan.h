#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "namchain/graph/graph.h"
#include "namchain/level.h"
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
  /// \brief Whether this plan measures its nodes' levels (`level.h`).
  ///
  /// False leaves `levels_` empty, which is what makes a plan with the feature off cost *nothing* on
  /// the render path rather than a branch per node per quantum: `execute` already passes a null probe
  /// for a node it has no probe for, and `measure_node` returns on a null probe. Decided at compile
  /// time for the same reason the daemon reads Config::levels once: a local transition compiles a plan
  /// on the render path, and probes that appeared or vanished with a flag read mid-flight would be
  /// probes that some plans write and others do not.
  bool levels = true;
  /// \brief Whether a node's probe folds one frame per period rather than every frame.
  ///
  /// The experiment switch behind Config::levels_first_frame: it cuts the per-sample probe work by
  /// the period length and makes a meter miss most transients, which is a trade to be measured rather
  /// than a default. Only meaningful when \ref levels is true.
  bool levels_first_frame = false;
  /// \brief Whether a probe folds into private memory and publishes once per period.
  ///
  /// See Config::levels_publish_period: the sample-per-sample path writes atomics the control thread
  /// reads, which is where a meter's cost actually goes.
  bool levels_publish_period = false;
};

/// The kind of value a parameter holds, which is what a client shows a control for
/// (docs/ui-integration-plan.md §1.4).
enum class ParamKind
{
  Number, ///< A continuous or stepped quantity. A count (`nc.split`'s `outputs`) is a Number with a
          ///< step of 1 and `snap`, not a Flag.
  Option, ///< One of a set of named values. Carried in the spec's string params.
  Flag,   ///< A boolean, drawn as a switch. Carried as 0 or 1 in the numeric params, with no bounds or
          ///< step of its own.
};

/// How a client should lay a numeric parameter out. Presentation, but presentation the *engine* knows:
/// a frequency slider is logarithmic, and every client re-deriving that is how two clients disagree.
enum class ParamScale
{
  Linear,
  Log,
};

/// One accepted value of an option parameter: the wire value and the label a client shows, which are
/// not assumed to be the same string.
struct OptionDesc
{
  const char* value = nullptr;
  const char* label = nullptr;
};

/// \brief One declared parameter of a node type.
///
/// The schema is the catalogue the app builds a palette and an inspector from, and the thing the
/// compiler validates against: without it an unknown parameter name is accepted silently, which from
/// a UI is a knob that does nothing (docs/ljoss-comparison.md §3.5).
///
/// A parameter's **id** is its position in the node type's array plus one, which is the integer a
/// ramp dispatch carries on the render path. Nothing on the audio thread compares a string.
struct ParamDesc
{
  const char* id = nullptr;
  const char* name = nullptr;
  ParamKind kind = ParamKind::Number;
  /// `"Hz"`, `"dB"`, `"ms"`, `"%"`, ... or null. A parameter with no unit is a plain quantity.
  const char* unit = nullptr;
  /// \brief The effective value when the parameter is absent.
  ///
  /// Published so there is exactly one default in the system: the app must never substitute its own,
  /// and a node built with no parameters must render identically to one built with every declared
  /// default. `tests/test_schema.cpp` asserts that rather than trusting it.
  double default_value = 0.0;
  /// Bounds. `has_*` false means unbounded *by the schema*; the engine's own limits (Nyquist, the
  /// delay buffer) are separate and the rejection says which one was hit.
  bool has_min = false;
  double min = 0.0;
  bool min_inclusive = true;
  bool has_max = false;
  double max = 0.0;
  bool max_inclusive = true;
  /// Smallest increment, or 0 for continuous. `snap` decides what an off-grid value means: rounded to
  /// it, or refused. "Granularity" is ambiguous between the increment and the display precision; this
  /// is the increment.
  double step = 0.0;
  bool snap = false;
  ParamScale scale = ParamScale::Linear;
  /// \brief Whether a change can be ramped inside the node with no recompile (§5.3's smooth class).
  ///
  /// This is the flag an inspector streams a slider for; a parameter without it is a structural edit,
  /// dual-rendered and crossfaded, and belongs on a commit-on-release control.
  bool rampable = false;
  /// Whether `rig.patch` may name it at all: false is a build-only parameter whose change is a
  /// rebuild, which is still a patch, so this is the narrower "not addressable" case.
  bool patchable = true;
  /// Whether a generic inspector should leave it out (an alias, or an internal knob).
  bool hidden = false;
  /// The node cannot be built without it.
  bool required = false;
  /// \brief A group name for mutually exclusive alternatives.
  ///
  /// `nc.delay` takes `samples` **or** `ms`, which a flat parameter list cannot express. Parameters
  /// sharing a group name are alternatives: giving two of them is a contradiction rather than a
  /// precedence rule, and is refused. Whether one of them is *required* is \ref one_of_required,
  /// because the same mechanism covers a pair that is merely optional -- `nc.delay`'s line length can
  /// be declared in samples or in milliseconds or not at all.
  const char* one_of = nullptr;
  /// Whether at least one member of the \ref one_of group must be present.
  bool one_of_required = false;
  /// For \ref ParamKind::Option: the accepted values. The wire value is what a client sends; `label`
  /// is what it shows, and they are not assumed to be the same string.
  const OptionDesc* options = nullptr;
  int option_count = 0;
  /// \brief Spellings that are accepted but not advertised.
  ///
  /// `nc.eq.biquad` has always accepted `"low_shelf"` for `"lowshelf"`. A schema that advertised every
  /// spelling would offer a picker with three names for one mode; a schema that rejected them would
  /// break rigs that work today. Aliases are validated and then ignored, which is the third option.
  const char* const* aliases = nullptr;
  int alias_count = 0;
  /// For an option whose values are not known until runtime: `"models"` or `"irs"`, resolved through
  /// the device's asset registry (slice D).
  const char* options_from = nullptr;
};

/// A node type's vtable. `create`/`destroy` run on the control thread while a plan is built;
/// `process` is the render path and must not allocate, lock or throw (§3.3).
struct NodeType
{
  std::string id;
  /// What a palette calls it, and what it groups it under. The engine owns these because a node type
  /// the app cannot name is a node type the app cannot offer -- the alternative is every client
  /// keeping its own table, which is a table that goes stale silently.
  const char* name = nullptr;
  const char* category = nullptr;
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
  /// \brief The declared parameters, in id order: `params[i]` has id `i + 1`.
  ///
  /// One table, not two: this is both the schema a client builds a palette and an inspector from and
  /// the id space the render path dispatches on, because a second list of "the settable ones" would be
  /// the same fact written twice. Null, with a count of 0, is a node with no parameters.
  const ParamDesc* params = nullptr;
  int param_count = 0;
  /// \brief Parameters that exist once per port rather than once per node.
  ///
  /// `nc.mix` has a level, a mute and a polarity per input: a lane's settings, not the mixer's. They
  /// are declared exactly like node parameters -- same kinds, same bounds -- and addressed with a
  /// port, because a mixer with N inputs and no per-input control is a summer rather than a mixer
  /// (docs/ui-integration-plan.md §1.4).
  const ParamDesc* per_port_params = nullptr;
  int per_port_param_count = 0;
  /// \brief Channels per port. 1 is mono, which is all the engine has.
  ///
  /// Declared and *refused* rather than absent (docs/ui-integration-plan.md §1.4): the compiler
  /// rejects a type whose ports are wider than the audio path can carry, so the day stereo lands the
  /// error moves and the schema never had to change shape. Published per port in `engine.schema`.
  int channels = 1;
  /// \brief Render path: ramp \p value into the node over the node's own smoothing time.
  ///
  /// The value is range-checked by the caller. Returns false for an unknown id, and the node leaves
  /// its state untouched rather than jumping: an unramped step is the click this exists to avoid
  /// (§5.3, the `smooth` op class). Nothing here may allocate, lock or throw.
  bool (*set_param)(void* state, uint32_t param_id, float value) noexcept = nullptr;
  /// \brief An engine-internal type: compiled like any other, but never offered to a client.
  ///
  /// `engine.schema` leaves it out and a document may not name it; only the engine places one, as
  /// the crossfade inside a local transition (docs/transitions.md, "Local transitions").
  bool internal = false;
  /// \brief Leave this node out of \ref graph::RenderPlan::prewarm_fresh.
  ///
  /// For a node whose first `process` call is the start of something timed -- a local transition's
  /// crossfade -- rather than a cold start to get out of the way.
  bool skip_prewarm = false;
};

/// \brief How a per-port parameter is encoded in the one integer a ramp dispatch carries.
///
/// A port's parameter takes an id no node-level parameter can: node ids are positions in the schema
/// array plus one, so they are small, and a port id sets a bit well above them. Ports are 0-based in
/// the document and 1-based here, so port 0's first parameter is not mistaken for "no port".
inline constexpr uint32_t kPortParamShift = 16;

inline uint32_t port_param_id(int port, int index) noexcept
{
  return ((static_cast<uint32_t>(port) + 1u) << kPortParamShift) | (static_cast<uint32_t>(index) + 1u);
}

/// The port a parameter id names, or -1 for a node-level parameter.
inline int port_of_param_id(uint32_t param_id) noexcept
{
  const uint32_t port = param_id >> kPortParamShift;
  return port == 0 ? -1 : static_cast<int>(port) - 1;
}

/// \brief A node spec's concrete port counts, after per-spec arity is resolved.
struct PortCounts
{
  int inputs = 0;
  int outputs = 0;
  /// Channels per port. See \ref NodeType::channels.
  int channels = 1;
};

/// \brief Resolve the port counts a node spec actually gets.
///
/// For most types this is the vtable's fixed counts. `nc.split`/`nc.mix` read `outputs`/`inputs`
/// from the spec, so one rig can fan out to N chains. Both `validate` and `compile` call this rather
/// than reading `NodeType::inputs`/`outputs` directly, so the two can never disagree about what
/// port 3 means.
///
/// \return false and set \p error when the parameter is present but is not a whole number in range,
/// or when the type's ports are wider than the engine's audio path.
bool resolve_ports(const NodeType& type, const graph::NodeSpec& spec, PortCounts& ports, std::string& error);

/// \return the built-in type, or nullptr if the id is unknown. Internal types are found here, so the
/// compiler can build them; a document cannot name one (\ref graph::validate refuses it).
const NodeType* find_node_type(const std::string& id);

/// Every registered type, in the order the catalogue reports them. What `engine.schema` serves and
/// what a test walks to check that each type's schema matches what the node reads.
std::vector<const NodeType*> all_node_types();

} // namespace render

namespace graph
{

/// \brief A node's state, as its type's `create` made it, shared by every plan that runs it.
///
/// One instance may belong to several plans at once -- the plan that is playing, a local transition's
/// plan and the plan after it -- but the render thread only ever runs one of them, so the state is
/// never processed twice in a block. The type's `destroy` runs when the last plan holding it is
/// freed, which is on the control thread (plans are reclaimed there, never on the render thread).
/// Null for a type with no state.
using NodeInstance = std::shared_ptr<void>;

/// Instances a compile adopts instead of creating, keyed by the (lowered) node id they are for.
using CarriedInstances = std::map<std::string, NodeInstance>;

/// One node placed into a plan, with its buffers already bound.
///
/// Pointer arrays are built at compile time, not per block: constructing them in `execute` would
/// allocate or at least introduce avoidable work on the render path.
struct PlanNode
{
  const render::NodeType* type = nullptr;
  /// The document identity of this node: the client's `element`, and the macro role it was lowered
  /// from. Carried so a patch can resolve `element` + `role` against what is *running* rather than
  /// against a document that may have moved on.
  std::string element;
  std::string role;
  /// Owns the state (shared with any other plan that carried it); \ref state is the same pointer,
  /// kept beside it so the render path reads a plain pointer.
  NodeInstance instance;
  void* state = nullptr;
  /// \brief True when this plan created the state, false when it carried it from another plan.
  ///
  /// Only a fresh node may be prewarmed (\ref RenderPlan::prewarm_fresh): a carried one is still being
  /// run by the plan that is playing.
  bool fresh = true;
  std::vector<const float*> inputs;
  std::vector<float*> outputs;
  /// \brief The node is bypassed: it still runs, but its output is replaced by its dry input.
  ///
  /// Set from `NodeSpec::bypass` at compile time. The node is *not* removed from the plan -- keeping it
  /// there is what keeps its state warm (plan.md G14), and it is what makes un-bypassing a crossfade
  /// back to a node that never stopped running. \ref RenderPlan::set_bypass changes it on the render
  /// path, and the flag is then where the node is going: \ref bypass_fade says it is still on its way.
  bool bypass = false;
  /// The length in frames of a bypass crossfade in progress, 0 when the node is settled.
  int bypass_fade = 0;
  /// How many frames of that crossfade have played.
  int bypass_position = 0;
};

/// \brief Overwrite a node's outputs with its dry input, or with silence when it has none.
///
/// The substitution behind both bypass and the non-finite guard. A node with one input takes that
/// input as its dry path; a node with several cannot name one dry signal, so it is zeroed -- the
/// honest answer for a node whose output cannot be trusted. Nodes with no output ports are untouched.
///
/// Render path: no allocation, no lock.
void substitute_dry(PlanNode& node, int frames) noexcept;

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

  /// \brief Compile, adopting the instances in \p carried instead of creating those nodes.
  ///
  /// A node whose id is in \p carried takes that instance, state and all, and is not fresh; every
  /// other node is created as usual. This is how a local transition keeps the nodes an edit did not
  /// touch (docs/transitions.md, "Local transitions"). The caller vouches that each carried instance
  /// was created for a node of the same type and settings: the compiler cannot check a `void*`.
  static std::unique_ptr<RenderPlan> compile(const Graph& graph, const render::CompileOptions& options,
                                             rt::Arena& arena, std::string& error, const CarriedInstances* carried);

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

  /// \brief The index of the node with this `element` and `role`, or -1 for none and -2 for several.
  ///
  /// An empty \p role matches any role of that element, which is how a client addresses a macro it
  /// does not know the lowering of, and how a hand-written rig's node (whose element defaults to its
  /// id) is addressed. Several matches is -2 rather than the first hit: an ambiguous address is a
  /// question the caller has to answer, not one this can answer for it.
  int node_index_of_role(const std::string& element, const std::string& role) const;

  /// The element and role of a node, for error messages that name what the client named.
  const std::string& element_of(std::size_t node_index) const;
  const std::string& role_of(std::size_t node_index) const;

  /// Whether a node can be bypassed: only a 1->1 node has a dry path. What \ref set_bypass checks,
  /// for the control thread to ask before it queues one.
  bool node_bypassable(std::size_t node_index) const
  {
    return node_index < nodes_.size() && nodes_[node_index].type->inputs == 1 &&
           nodes_[node_index].type->outputs == 1;
  }

  /// \brief Resolve a parameter of one port of \p node_id, or of the node itself when \p port is -1.
  ///
  /// A mixer's per-input settings are addressed by port rather than by a naming convention: the schema
  /// declares them once (\ref NodeType::per_port_params) and this turns "port 2, level" into the
  /// integer the ramp dispatch carries. A port the node does not have is not resolved, so a client
  /// cannot set a lane that does not exist.
  bool resolve_param(const std::string& node_id, const std::string& param, int port, std::size_t& node_index,
                     uint32_t& param_id) const;

  /// \brief Render path: ramp one node's parameter to \p value.
  ///
  /// \return false for an out-of-range index or an id the node does not know. Never allocates or
  /// locks; the node smooths the change over its own ramp, so this is the no-recompile, no-click
  /// path of §5.3's `smooth` class.
  bool set_param(std::size_t node_index, uint32_t param_id, float value) noexcept;

  /// \brief Render path: bypass one node, or bring it back, with an equal-power crossfade of
  /// \p frames between its output and its input.
  ///
  /// The node keeps running either way, so this needs no new plan: it is bypass as a ramp
  /// (docs/transitions.md, "Bypass"). A toggle that arrives mid-fade turns the fade around from where
  /// it is, rather than jumping. \return false for an out-of-range index or a node that is not 1->1.
  bool set_bypass(std::size_t node_index, bool bypass, int frames) noexcept;

  /// Node ids in execution order, parallel to \ref nodes.
  const std::vector<std::string>& node_ids() const noexcept { return node_ids_; }

  /// \brief Run one quantum of silence through every node, discarding the output.
  ///
  /// The first `process()` call on a freshly built model costs milliseconds -- reset and warm-up
  /// work that has nothing to do with steady-state cost -- and a stall that long can drain the
  /// device ring on its own. Call this on the control thread before publishing the plan (§3.3).
  /// It is not part of `execute` because it mutates node state: the oracle has to be warmed too.
  ///
  /// Every node's level probe is emptied on the way out. Those blocks are silence, rendered before
  /// the plan is visible to anything, so nothing can have read them; left in, they would give a node
  /// a first window one quantum longer than the chain's, its mean diluted by audio that never reached
  /// the device.
  void prewarm() noexcept;

  /// \brief Prewarm only the nodes this plan created, leaving carried ones alone.
  ///
  /// A carried node is still being run by the plan that is playing, so touching its state from the
  /// control thread would race the render thread; it is warm anyway. Each fresh node runs one quantum
  /// of silence in schedule order (types marked `skip_prewarm` excepted), and every buffer is zeroed
  /// again afterwards, so the plan starts from silence wherever it does not start from a carried
  /// buffer.
  void prewarm_fresh() noexcept;

  /// The instance behind a node, by id, for a later plan to carry; null when there is none.
  NodeInstance instance_of(const std::string& node_id) const;

  /// The node index of an id in execution order, or -1.
  int index_of(const std::string& node_id) const;

  /// \brief The buffer bound to one of a node's input ports, or null for an unconnected port.
  ///
  /// What a local transition copies across for a block-delay node, whose input buffer holds the
  /// previous block (its state lives in the plan's memory, not in the node).
  float* input_buffer(std::size_t node_index, int port) const noexcept;

  /// Whether the node at \p node_index was created by this plan rather than carried into it.
  bool node_fresh(std::size_t node_index) const noexcept
  {
    return node_index < nodes_.size() && nodes_[node_index].fresh;
  }

  int max_quantum() const noexcept { return max_quantum_; }
  /// Intermediate buffers the memory plan needed. Fewer than the node count means it worked.
  int buffer_count() const noexcept { return static_cast<int>(buffers_.size()); }
  std::size_t buffer_bytes() const noexcept;
  const std::vector<PlanNode>& nodes() const noexcept { return nodes_; }

  /// \brief Nodes the non-finite guard has taken out of the signal path.
  ///
  /// Control thread (or a report); the render thread flips the flags. A failed node's output is its
  /// dry input for the rest of the plan's life.
  std::size_t failed_node_count() const noexcept;
  /// Whether the guard has taken the node at \p node_index out of the signal path. Read for a report
  /// rather than to render a block, and what lets a client be told *which* node failed.
  bool node_failed(std::size_t node_index) const noexcept;
  /// How many times the guard tripped. Counted separately from \ref failed_node_count because a
  /// second trip on an already-failed node is not a second failure.
  uint64_t guard_trips() const noexcept { return guard_trips_.load(std::memory_order_relaxed); }

  /// \brief The level probe for a node's output, or null when there is no such node.
  ///
  /// One probe per node rather than one per place the wire asks about: a node's `element` is what a
  /// client named (its own id, or the macro an element was lowered from), so aggregating a macro's
  /// level is a sum over the nodes that share its element (\ref Daemon::levels_json), and a node
  /// that stands alone needs no special case. Control thread; returns null rather than throwing,
  /// because a status request may name a node from an older plan.
  level::Probe* level_of(std::size_t node_index) noexcept
  {
    return node_index < levels_.size() ? &levels_[node_index] : nullptr;
  }

  /// Debug/inspection: the buffer index each node output was coloured onto.
  const std::vector<int>& output_buffer_of(std::size_t node_index) const;

private:
  RenderPlan() = default;

  std::vector<PlanNode> nodes_;    // topological order
  std::vector<std::string> node_ids_; // parallel to nodes_, for id -> index resolution
  std::vector<float*> buffers_;    // all from the arena
  /// Per node, whether the non-finite guard has failed it. Written by the render thread, read for a
  /// report; an atomic per node rather than a `PlanNode` field because `PlanNode` lives in a vector
  /// and `std::atomic` is not movable.
  std::vector<std::atomic<bool>> failed_;
  /// \brief One level probe per node, parallel to \ref nodes_ (\ref level_of).
  ///
  /// In the plan rather than in the daemon because a plan is executed by whichever pool worker owns
  /// its chain: with more than one render thread a shared probe would be written by several
  /// threads, and only the plan knows which nodes are its own. It also dies with the plan, which is
  /// right -- a level from a plan nobody is running is not a level.
  std::vector<level::Probe> levels_;
  /// Whether a probe folds one frame per period or all of them (CompileOptions::levels_first_frame).
  /// Fixed at compile time like \ref levels_ itself: `execute` reads it per node per quantum, so it
  /// has to be a value the plan owns rather than a flag consulted through anything.
  bool levels_first_frame_ = false;
  /// Whether a node's probe folds into private memory and is published once per period
  /// (CompileOptions::levels_publish_period). \ref execute publishes them at the end of the quantum.
  bool levels_publish_period_ = false;
  std::atomic<uint64_t> guard_trips_{0};
  int input_node_ = -1;            // index in nodes_ whose block input is sampled from the graph
  int output_buffer_ = -1;         // buffer the graph output is read from
  int silence_buffer_ = -1;        // the zeroed buffer every unconnected input is bound to
  int max_quantum_ = 0;
  std::vector<std::vector<int>> output_buffers_; // per node, per port
};

} // namespace graph
} // namespace namchain
