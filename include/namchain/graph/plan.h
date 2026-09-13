#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "namchain/graph/graph.h"
#include "namchain/rt/arena.h"

namespace namchain
{
namespace render
{

/// A node type's vtable. `create`/`destroy` run on the control thread while a plan is built;
/// `process` is the render path and must not allocate, lock or throw (§3.3).
struct NodeType
{
  std::string id;
  int inputs = 0;
  int outputs = 0;
  /// \param error set when the node cannot be constructed (bad params, missing model, ...).
  void* (*create)(const graph::NodeSpec& spec, int max_quantum, std::string& error) = nullptr;
  void (*destroy)(void* state) = nullptr;
  /// Buffers are mono for now; per-port channel counts arrive with the multichannel work.
  void (*process)(void* state, const float* const* in, float* const* out, int frames) noexcept = nullptr;
};

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
  static std::unique_ptr<RenderPlan> compile(const Graph& graph, int max_quantum, rt::Arena& arena,
                                             std::string& error);

  ~RenderPlan();

  RenderPlan(const RenderPlan&) = delete;
  RenderPlan& operator=(const RenderPlan&) = delete;

  /// Render one block. \p input and \p output are mono and \p frames long. RT-safe.
  void execute(const float* input, float* output, int frames) noexcept;

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
  std::vector<float*> buffers_;    // all from the arena
  int input_buffer_ = -1;          // buffer the graph input is written into
  int output_buffer_ = -1;         // buffer the graph output is read from
  int max_quantum_ = 0;
  std::vector<std::vector<int>> output_buffers_; // per node, per port
};

} // namespace graph
} // namespace namchain
