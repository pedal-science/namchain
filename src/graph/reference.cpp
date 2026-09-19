#include "namchain/graph/reference.h"

#include <algorithm>
#include <functional>
#include <map>

namespace namchain
{
namespace graph
{

std::unique_ptr<ReferenceRender> ReferenceRender::compile(const Graph& graph, const render::CompileOptions& options,
                                                          std::string& error)
{
  if (options.max_quantum < 1)
  {
    error = "max_quantum must be at least 1";
    return nullptr;
  }

  // Validation is shared with the compiler on purpose: this class is an oracle for the *compiler*,
  // not for the validator, and a rig that validate() rejects has no reference behaviour to compare.
  const Validation validation = validate(graph);
  if (!validation.ok)
  {
    error = validation.message;
    return nullptr;
  }

  std::map<std::string, int> index;
  for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    index[graph.nodes[i].id] = static_cast<int>(i);

  // Resolve each node's ports once, the same way the compiler does. `find_node_type` alone gives
  // only the default for a per-spec arity (`nc.split`/`nc.mix`), so reading `type->inputs` here would
  // make the oracle disagree with the plan about any N-way fan-out and the disagreement would look
  // like a compiler bug. validate() has already rejected an out-of-range count, so this cannot fail
  // for a graph that reached here, but the error path stays for the same reason it exists above.
  std::vector<render::PortCounts> ports(graph.nodes.size());
  for (std::size_t i = 0; i < graph.nodes.size(); ++i)
  {
    if (!render::resolve_ports(*render::find_node_type(graph.nodes[i].type), graph.nodes[i],
                               ports[i], error))
      return nullptr;
  }

  // Flat value numbering, one value per output port -- the same addressing the compiler uses, but
  // that is only an addressing convention; nothing about the layout or the schedule is shared.
  const auto value_of = [&](int node, int port) {
    int base = 0;
    for (int i = 0; i < node; ++i)
      base += ports[static_cast<std::size_t>(i)].outputs;
    return base + port;
  };

  auto reference = std::unique_ptr<ReferenceRender>(new ReferenceRender());
  reference->max_quantum_ = options.max_quantum;

  int values = 0;
  for (const render::PortCounts& counts : ports)
    values += counts.outputs;
  reference->values_ = values;
  reference->silence_value_ = values;

  reference->storage_.assign(static_cast<std::size_t>(values + 1) * static_cast<std::size_t>(options.max_quantum),
                             0.0f);

  // Producers of each node's input ports, so the DFS below can follow the graph backwards from the
  // output. A port with no producer stays bound to the silence value.
  std::vector<std::vector<int>> producer(graph.nodes.size()); // node -> producer per input port
  std::vector<std::vector<int>> producer_port(graph.nodes.size());
  for (std::size_t i = 0; i < graph.nodes.size(); ++i)
  {
    producer[i].assign(static_cast<std::size_t>(ports[i].inputs), -1);
    producer_port[i].assign(static_cast<std::size_t>(ports[i].inputs), 0);
  }
  for (const Connection& connection : graph.connections)
  {
    const int to = index.at(connection.to);
    producer[static_cast<std::size_t>(to)][static_cast<std::size_t>(connection.to_port)] = index.at(connection.from);
    producer_port[static_cast<std::size_t>(to)][static_cast<std::size_t>(connection.to_port)] = connection.from_port;
  }

  // Dependency order by depth-first post-order, starting from every node in declaration order so
  // that nodes the output does not depend on are still evaluated (the plan runs them too). Cycles
  // are impossible here: validate() rejected them above.
  //
  // Block-delay nodes are emitted first and are never recursed into. That is the same scheduling
  // rule the compiler applies -- an edge into such a node is not a dependency, and the node reads
  // its input as of the previous block -- and the two have to agree, or they would disagree about
  // a feedback rig for reasons that have nothing to do with the compiler.
  std::vector<bool> emitted(graph.nodes.size(), false);
  std::vector<bool> active(graph.nodes.size(), false);
  std::vector<int> order;
  order.reserve(graph.nodes.size());
  for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    if (render::find_node_type(graph.nodes[i].type)->block_delay)
    {
      emitted[i] = true;
      order.push_back(static_cast<int>(i));
    }

  std::function<void(int)> visit = [&](int node) {
    if (emitted[static_cast<std::size_t>(node)] || active[static_cast<std::size_t>(node)])
      return;
    active[static_cast<std::size_t>(node)] = true;
    for (const int predecessor : producer[static_cast<std::size_t>(node)])
      if (predecessor >= 0)
        visit(predecessor);
    active[static_cast<std::size_t>(node)] = false;
    emitted[static_cast<std::size_t>(node)] = true;
    order.push_back(node);
  };
  for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    visit(static_cast<int>(i));

  for (const int node : order)
  {
    const NodeSpec& spec = graph.nodes[static_cast<std::size_t>(node)];
    const render::NodeType* type = render::find_node_type(spec.type);

    PlanNode plan_node;
    plan_node.type = type;
    // The oracle mirrors the plan's bypass: the node runs and its output is replaced by its input.
    // It does *not* mirror the non-finite guard, deliberately: the oracle is the ideal graph, and a
    // node that returns NaN is a runtime fault, not a different graph to compare against.
    plan_node.bypass = spec.bypass;
    plan_node.state = type->create == nullptr ? nullptr : type->create(spec, options, error);
    if (type->create != nullptr && plan_node.state == nullptr)
      return nullptr; // create() set `error`

    const render::PortCounts counts = ports[static_cast<std::size_t>(node)];
    plan_node.inputs.assign(static_cast<std::size_t>(counts.inputs),
                            reference->storage_.data() + reference->silence_value_ * options.max_quantum);
    plan_node.outputs.reserve(static_cast<std::size_t>(counts.outputs) + 1);
    for (int port = 0; port < counts.outputs; ++port)
      plan_node.outputs.push_back(reference->storage_.data() +
                                  static_cast<std::size_t>(value_of(node, port)) * options.max_quantum);
    plan_node.outputs.push_back(nullptr);

    for (int port = 0; port < counts.inputs; ++port)
    {
      const int predecessor = producer[static_cast<std::size_t>(node)][static_cast<std::size_t>(port)];
      if (predecessor < 0)
        continue;
      plan_node.inputs[static_cast<std::size_t>(port)] =
        reference->storage_.data() +
        static_cast<std::size_t>(value_of(predecessor, producer_port[static_cast<std::size_t>(node)]
                                                              [static_cast<std::size_t>(port)])) *
          options.max_quantum;
    }
    // Same input terminator the plan binds: `process_mix` walks inputs until nullptr.
    plan_node.inputs.push_back(nullptr);

    if (spec.type == "nc.input")
      reference->input_node_ = static_cast<int>(reference->nodes_.size());
    if (spec.type == "nc.output" && producer[static_cast<std::size_t>(node)][0] >= 0)
      reference->output_value_ = value_of(producer[static_cast<std::size_t>(node)][0], 0);

    reference->nodes_.push_back(std::move(plan_node));
  }

  return reference;
}

ReferenceRender::~ReferenceRender()
{
  for (PlanNode& node : nodes_)
    if (node.type != nullptr && node.type->destroy != nullptr && node.state != nullptr)
      node.type->destroy(node.state);
}

void ReferenceRender::execute(const float* input, float* output, int frames) noexcept
{
  // Same handoff as RenderPlan::execute: the input node samples the block at its own point in the
  // schedule, so that a block-delay node scheduled ahead of it reads the previous block.
  if (input_node_ >= 0)
  {
    PlanNode& node = nodes_[static_cast<std::size_t>(input_node_)];
    if (node.type->set_block_input != nullptr)
      node.type->set_block_input(node.state, input);
  }

  for (PlanNode& node : nodes_)
  {
    node.type->process(node.state, node.inputs.data(), node.outputs.data(), frames);
    if (node.bypass)
      substitute_dry(node, frames);
  }

  if (output_value_ >= 0 && output != nullptr)
  {
    const float* source = storage_.data() + static_cast<std::size_t>(output_value_) * max_quantum_;
    std::copy(source, source + frames, output);
  }
}

void ReferenceRender::prewarm() noexcept
{
  if (max_quantum_ > 0)
    execute(nullptr, nullptr, max_quantum_);
}

} // namespace graph
} // namespace namchain
