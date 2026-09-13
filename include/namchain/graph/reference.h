#pragma once

#include <memory>
#include <string>
#include <vector>

#include "namchain/graph/graph.h"
#include "namchain/graph/plan.h"

namespace namchain
{
namespace graph
{

/// \brief A deliberately naive evaluator of the same IR: the compiler's oracle.
///
/// A test that compares a plan against a second run of that plan proves determinism and nothing
/// else. The compiler does three things that can be wrong independently -- it orders nodes, it
/// colours buffers so unrelated values share storage, and it binds pointers at build time. This
/// class re-derives all three the obvious way: every value gets its own storage, the order comes
/// from a depth-first walk of the graph rather than from Kahn's algorithm, and nothing is reused.
/// A disagreement therefore points at the compiler rather than at the IR.
///
/// It is not the shipping renderer, and is not written to be fast.
class ReferenceRender
{
public:
  static std::unique_ptr<ReferenceRender> compile(const Graph& graph, const render::CompileOptions& options,
                                                  std::string& error);

  ~ReferenceRender();

  ReferenceRender(const ReferenceRender&) = delete;
  ReferenceRender& operator=(const ReferenceRender&) = delete;

  /// Same contract as \ref RenderPlan::execute: mono, `frames <= max_quantum`.
  void execute(const float* input, float* output, int frames) noexcept;

  /// Same contract as \ref RenderPlan::prewarm -- and it must be called in lockstep with the plan's,
  /// or the two are at different points in every stateful node's history.
  void prewarm() noexcept;

  /// Values allocated: one per node output port, plus the shared silence buffer. Counted the same
  /// way as \ref RenderPlan::buffer_count so the two numbers can be put side by side. By
  /// construction it is never lower than the plan's.
  int buffer_count() const noexcept { return static_cast<int>(values_) + 1; }

private:
  ReferenceRender() = default;

  std::vector<PlanNode> nodes_; // dependency order, from a DFS
  std::vector<float> storage_;          // (values_ + 1) quanta, one per value plus silence
  int values_ = 0;
  int silence_value_ = 0;
  int input_node_ = -1; // index in nodes_
  int output_value_ = -1;
  int max_quantum_ = 0;
};

} // namespace graph
} // namespace namchain
