#include "namchain/graph/plan.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace namchain
{
namespace render
{

bool resolve_ports(const NodeType& type, const graph::NodeSpec& spec, PortCounts& ports, std::string& error)
{
  ports.inputs = type.inputs;
  ports.outputs = type.outputs;
  if (type.arity_param == nullptr)
    return true;

  // The param names the count it overrides. Anything else is a vtable bug, not a rig error, so say
  // which type so the message points at the node implementation rather than at the rig author.
  const bool over_inputs = std::strcmp(type.arity_param, "inputs") == 0;
  const bool over_outputs = std::strcmp(type.arity_param, "outputs") == 0;
  if (!over_inputs && !over_outputs)
  {
    error = "node type '" + type.id + "' declares an unknown arity param '" + type.arity_param + "'";
    return false;
  }

  int count = over_inputs ? type.inputs : type.outputs;
  const auto found = spec.params.find(type.arity_param);
  if (found != spec.params.end())
  {
    const double value = found->second;
    // A port count is a count, so 2.5 is a mistake and 2.0 written as a float is not. Rejecting the
    // former rather than truncating it keeps "why is my split only fanning out to two?" from being a
    // bug report about the parser.
    if (!std::isfinite(value) || std::floor(value) != value)
    {
      error = "node '" + spec.id + "': '" + type.arity_param + "' must be a whole number";
      return false;
    }
    count = static_cast<int>(value);
  }

  if (count < type.min_ports || count > type.max_ports)
  {
    error = "node '" + spec.id + "': '" + type.arity_param + "' must be between " +
            std::to_string(type.min_ports) + " and " + std::to_string(type.max_ports) + ", got " +
            std::to_string(count);
    return false;
  }

  if (over_inputs)
    ports.inputs = count;
  else
    ports.outputs = count;
  return true;
}

} // namespace render

namespace graph
{

Validation Validation::failure(std::string reason)
{
  Validation result;
  result.ok = false;
  result.message = std::move(reason);
  return result;
}

namespace
{

/// Every node's resolved type and port counts, keyed by id, plus its position in `graph.nodes`.
///
/// The ports are stored beside the type rather than read from it later because a per-spec arity
/// (`nc.split`/`nc.mix`) means `type->inputs`/`type->outputs` are only a default: the graph's
/// meaning lives in the resolved counts, and every pass -- validation, liveness, colouring, binding
/// -- has to use the same ones.
struct Indexed
{
  std::map<std::string, int> position;
  std::vector<const render::NodeType*> types;
  std::vector<render::PortCounts> ports;
};

Indexed index_graph(const Graph& graph, Validation& validation)
{
  Indexed indexed;
  indexed.types.reserve(graph.nodes.size());
  indexed.ports.reserve(graph.nodes.size());

  for (const NodeSpec& node : graph.nodes)
  {
    if (node.id.empty())
    {
      validation = Validation::failure("a node has an empty id");
      return indexed;
    }
    if (indexed.position.count(node.id) != 0)
    {
      validation = Validation::failure("duplicate node id '" + node.id + "'");
      return indexed;
    }
    const render::NodeType* type = render::find_node_type(node.type);
    if (type == nullptr)
    {
      validation = Validation::failure("unknown node type '" + node.type + "' on node '" + node.id + "'");
      return indexed;
    }
    render::PortCounts ports;
    if (!render::resolve_ports(*type, node, ports, validation.message))
    {
      validation.ok = false;
      return indexed;
    }
    indexed.position[node.id] = static_cast<int>(indexed.types.size());
    indexed.types.push_back(type);
    indexed.ports.push_back(ports);
  }
  return indexed;
}

/// The scheduling dependency graph, with block-delay nodes treated as sources.
///
/// An edge *into* a block-delay node is not a dependency: that node reads whatever its input buffer
/// already holds, which is the previous block's value, so it does not have to wait for the producer
/// to run. Its own outputs are ordinary dependencies. This is what turns a feedback loop into a
/// DAG (§5.1), and dropping the edge on this side is what makes the delay come out as exactly one
/// block rather than two.
void build_dependencies(const Graph& graph, const Indexed& indexed, std::vector<std::vector<int>>& successors,
                        std::vector<int>& indegree)
{
  const std::size_t n = graph.nodes.size();
  successors.assign(n, {});
  indegree.assign(n, 0);

  std::map<std::pair<int, int>, bool> seen;
  for (const Connection& connection : graph.connections)
  {
    const int from = indexed.position.at(connection.from);
    const int to = indexed.position.at(connection.to);
    if (indexed.types[static_cast<std::size_t>(to)]->block_delay)
      continue;
    if (seen.emplace(std::make_pair(from, to), true).second)
    {
      successors[static_cast<std::size_t>(from)].push_back(to);
      ++indegree[static_cast<std::size_t>(to)];
    }
  }
}

/// The next node to place: a block-delay node if one is ready, otherwise the lowest ready index.
///
/// Block-delay nodes have no counted predecessors, so they are ready from the start; taking them
/// first is what guarantees they read the previous block. Without this a graph that is *not* a
/// cycle -- `nc.input -> nc.unit_delay -> nc.output` -- would sample the current block and delay by
/// nothing, because the input node happens to be declared first.
int next_ready(const Indexed& indexed, const std::vector<int>& indegree, const std::vector<bool>& placed)
{
  for (std::size_t i = 0; i < placed.size(); ++i)
    if (!placed[i] && indegree[i] == 0 && indexed.types[i]->block_delay)
      return static_cast<int>(i);
  for (std::size_t i = 0; i < placed.size(); ++i)
    if (!placed[i] && indegree[i] == 0)
      return static_cast<int>(i);
  return -1;
}

} // namespace

Validation validate(const Graph& graph)
{
  if (graph.nodes.empty())
    return Validation::failure("graph has no nodes");

  Validation validation;
  const Indexed indexed = index_graph(graph, validation);
  if (!validation.ok)
    return validation;

  int inputs = 0;
  int outputs = 0;
  for (const NodeSpec& node : graph.nodes)
  {
    const render::NodeType* type = indexed.types[static_cast<std::size_t>(indexed.position.at(node.id))];
    if (type->id == "nc.input")
      ++inputs;
    if (type->id == "nc.output")
      ++outputs;
  }
  if (inputs != 1)
    return Validation::failure("a chain needs exactly one nc.input, found " + std::to_string(inputs));
  if (outputs != 1)
    return Validation::failure("a chain needs exactly one nc.output, found " + std::to_string(outputs));

  // Connections: both ends must exist and the port must be within range.
  std::map<std::pair<int, int>, std::string> driven_by;
  for (const Connection& connection : graph.connections)
  {
    const auto from = indexed.position.find(connection.from);
    if (from == indexed.position.end())
      return Validation::failure("connection from unknown node '" + connection.from + "'");
    const auto to = indexed.position.find(connection.to);
    if (to == indexed.position.end())
      return Validation::failure("connection to unknown node '" + connection.to + "'");

    const render::PortCounts from_ports = indexed.ports[static_cast<std::size_t>(from->second)];
    const render::PortCounts to_ports = indexed.ports[static_cast<std::size_t>(to->second)];
    if (connection.from_port < 0 || connection.from_port >= from_ports.outputs)
      return Validation::failure("node '" + connection.from + "' has no output port " +
                                 std::to_string(connection.from_port));
    if (connection.to_port < 0 || connection.to_port >= to_ports.inputs)
      return Validation::failure("node '" + connection.to + "' has no input port " +
                                 std::to_string(connection.to_port));
    if (from_ports.outputs == 0)
      return Validation::failure("node '" + connection.from + "' produces no output");
    if (to_ports.inputs == 0)
      return Validation::failure("node '" + connection.to + "' accepts no input");

    // Two connections into one port leave the graph meaning "whichever one the compiler happened to
    // keep". Rejecting it turns a silent 50/50 into a message.
    const auto previous = driven_by.emplace(std::make_pair(to->second, connection.to_port), connection.from);
    if (!previous.second)
      return Validation::failure("node '" + connection.to + "' input port " +
                                 std::to_string(connection.to_port) + " is already driven by '" +
                                 previous.first->second + "'");
  }

  // Cycles are a compile error, except through a block-delay node: feedback is expressed with an
  // explicit `nc.unit_delay`, which reads its input as of the previous block (§5.1). Kahn's
  // algorithm both detects a cycle and produces the topological order the compiler needs.
  const std::size_t n = graph.nodes.size();
  std::vector<std::vector<int>> successors;
  std::vector<int> indegree;
  build_dependencies(graph, indexed, successors, indegree);

  std::vector<bool> placed(n, false);
  std::size_t placed_count = 0;
  for (std::size_t step = 0; step < n; ++step)
  {
    const int chosen = next_ready(indexed, indegree, placed);
    if (chosen < 0)
      return Validation::failure("graph contains a cycle; use an explicit nc.unit_delay node for feedback");
    placed[static_cast<std::size_t>(chosen)] = true;
    ++placed_count;
    for (const int next : successors[static_cast<std::size_t>(chosen)])
      --indegree[static_cast<std::size_t>(next)];
  }
  if (placed_count != n)
    return Validation::failure("graph contains a cycle");

  return validation;
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------

namespace
{

/// Topological order, or an empty vector if there is a cycle (validate() has already rejected that).
std::vector<int> topological_order(const Graph& graph, const Indexed& indexed)
{
  const std::size_t n = graph.nodes.size();
  std::vector<std::vector<int>> successors;
  std::vector<int> indegree;
  build_dependencies(graph, indexed, successors, indegree);

  std::vector<int> order;
  order.reserve(n);
  std::vector<bool> placed(n, false);
  for (std::size_t step = 0; step < n; ++step)
  {
    const int chosen = next_ready(indexed, indegree, placed);
    if (chosen < 0)
      return {};
    placed[static_cast<std::size_t>(chosen)] = true;
    order.push_back(chosen);
    for (const int next : successors[static_cast<std::size_t>(chosen)])
      --indegree[static_cast<std::size_t>(next)];
  }
  return order;
}

} // namespace

std::unique_ptr<RenderPlan> RenderPlan::compile(const Graph& graph, const render::CompileOptions& options,
                                                rt::Arena& arena, std::string& error)
{
  const int max_quantum = options.max_quantum;
  if (max_quantum < 1)
  {
    error = "max_quantum must be at least 1";
    return nullptr;
  }
  if (!(options.sample_rate > 0.0))
  {
    error = "sample_rate must be > 0";
    return nullptr;
  }

  const Validation validation = validate(graph);
  if (!validation.ok)
  {
    error = validation.message;
    return nullptr;
  }

  Validation ignored;
  const Indexed indexed = index_graph(graph, ignored);

  const std::vector<int> order = topological_order(graph, indexed);
  if (order.empty())
  {
    error = "graph contains a cycle";
    return nullptr;
  }

  // Position of each node in the execution order, for liveness.
  std::vector<int> position_of(graph.nodes.size(), 0);
  for (std::size_t i = 0; i < order.size(); ++i)
    position_of[static_cast<std::size_t>(order[i])] = static_cast<int>(i);

  // Value = one node output port, identified by a flat index.
  const auto value_of = [&indexed](int node, int port) {
    int base = 0;
    for (int i = 0; i < node; ++i)
      base += indexed.ports[static_cast<std::size_t>(i)].outputs;
    return base + port;
  };
  int value_count = 0;
  for (const render::PortCounts& ports : indexed.ports)
    value_count += ports.outputs;

  std::vector<int> owner(value_count, -1);
  for (std::size_t node = 0; node < indexed.ports.size(); ++node)
    for (int port = 0; port < indexed.ports[node].outputs; ++port)
      owner[static_cast<std::size_t>(value_of(static_cast<int>(node), port))] = static_cast<int>(node);

  // Liveness: a value is live from its definition until the last node that consumes it. An unused
  // value dies at its own node, which is what lets a dangling branch reuse its buffer immediately.
  std::vector<int> last_use(value_count, -1);
  for (int value = 0; value < value_count; ++value)
    if (owner[static_cast<std::size_t>(value)] >= 0)
      last_use[static_cast<std::size_t>(value)] = position_of[static_cast<std::size_t>(owner[static_cast<std::size_t>(value)])];

  for (const Connection& connection : graph.connections)
  {
    const int from = indexed.position.at(connection.from);
    const int to = indexed.position.at(connection.to);
    const int value = value_of(from, connection.from_port);
    last_use[static_cast<std::size_t>(value)] =
      std::max(last_use[static_cast<std::size_t>(value)], position_of[static_cast<std::size_t>(to)]);

    // A value a block-delay node reads has to still be there at the *start of the next block*,
    // because that node reads it before its producer has run. An ordinary last-use would let the
    // colouring recycle the buffer later in the same block, and the next block's read would pick up
    // whatever landed there instead.
    if (indexed.types[static_cast<std::size_t>(to)]->block_delay)
      last_use[static_cast<std::size_t>(value)] = static_cast<int>(order.size());
  }

  // Interval colouring: walk values in definition order and reuse the first buffer whose previous
  // occupant died before this definition. This is the memory plan of §5.2 in miniature.
  std::vector<int> colour(value_count, -1);
  std::vector<int> free_at;
  int next_colour = 0;
  for (const int node : order)
  {
    const int def_position = position_of[static_cast<std::size_t>(node)];
    for (int port = 0; port < indexed.ports[static_cast<std::size_t>(node)].outputs; ++port)
    {
      const int value = value_of(node, port);
      int chosen = -1;
      for (int candidate = 0; candidate < next_colour; ++candidate)
      {
        if (free_at[static_cast<std::size_t>(candidate)] < def_position)
        {
          chosen = candidate;
          break;
        }
      }
      if (chosen < 0)
      {
        chosen = next_colour++;
        free_at.push_back(0);
      }
      colour[static_cast<std::size_t>(value)] = chosen;
      free_at[static_cast<std::size_t>(chosen)] = last_use[static_cast<std::size_t>(value)];
    }
  }

  // One extra buffer, zeroed once, shared by every unconnected input port. Binding silence is
  // cheaper and safer than trusting a node to ignore a null pointer.
  const int total_buffers = next_colour + 1;
  const int silence_buffer = next_colour;

  auto plan = std::unique_ptr<RenderPlan>(new RenderPlan());
  plan->max_quantum_ = max_quantum;
  plan->buffers_.reserve(static_cast<std::size_t>(total_buffers));
  for (int i = 0; i < total_buffers; ++i)
  {
    float* buffer = arena.allocate_array<float>(static_cast<std::size_t>(max_quantum));
    if (buffer == nullptr)
    {
      error = "arena exhausted: the memory plan needs " + std::to_string(total_buffers) + " buffers";
      return nullptr;
    }
    std::fill(buffer, buffer + max_quantum, 0.0f);
    plan->buffers_.push_back(buffer);
  }

  for (const int node : order)
  {
    const NodeSpec& spec = graph.nodes[static_cast<std::size_t>(node)];
    const render::NodeType* type = indexed.types[static_cast<std::size_t>(node)];

    PlanNode plan_node;
    plan_node.type = type;
    // A null `create` means the type carries no state at all (nc.input, nc.split, ...). Using the
    // vtable rather than a list of ids keeps this honest as node types are added: a type that does
    // have a constructor and fails to allocate is an error, and cannot be mistaken for a marker.
    plan_node.state = type->create == nullptr ? nullptr : type->create(spec, options, error);
    if (type->create != nullptr && plan_node.state == nullptr)
      return nullptr; // create() set `error`

    const render::PortCounts ports = indexed.ports[static_cast<std::size_t>(node)];
    plan_node.inputs.assign(static_cast<std::size_t>(ports.inputs), plan->buffers_[static_cast<std::size_t>(silence_buffer)]);
    plan_node.outputs.reserve(static_cast<std::size_t>(ports.outputs) + 1);
    for (int port = 0; port < ports.outputs; ++port)
      plan_node.outputs.push_back(plan->buffers_[static_cast<std::size_t>(colour[static_cast<std::size_t>(value_of(node, port))])]);
    // Terminators on both sides: `process_split` walks outputs until nullptr, `process_mix` walks
    // inputs the same way. Binding below is by port index, so the input terminator goes on last.
    plan_node.outputs.push_back(nullptr);
    plan_node.inputs.push_back(nullptr);

    plan->nodes_.push_back(std::move(plan_node));
  }

  // Bind inputs now that every output buffer is known.
  for (std::size_t i = 0; i < plan->nodes_.size(); ++i)
  {
    for (const Connection& connection : graph.connections)
    {
      if (indexed.position.at(connection.to) != order[i])
        continue;
      const int from = indexed.position.at(connection.from);
      const int value = value_of(from, connection.from_port);
      plan->nodes_[i].inputs[static_cast<std::size_t>(connection.to_port)] =
        plan->buffers_[static_cast<std::size_t>(colour[static_cast<std::size_t>(value)])];
    }

    if (plan->nodes_[i].type->id == "nc.input")
      plan->input_node_ = static_cast<int>(i);
    if (plan->nodes_[i].type->id == "nc.output")
    {
      // The output node's single input buffer holds the chain's result.
      const float* bound = plan->nodes_[i].inputs[0];
      for (int b = 0; b < total_buffers; ++b)
        if (plan->buffers_[static_cast<std::size_t>(b)] == bound)
          plan->output_buffer_ = b;
    }
  }

  for (const int node : order)
  {
    std::vector<int> ports;
    for (int port = 0; port < indexed.ports[static_cast<std::size_t>(node)].outputs; ++port)
      ports.push_back(colour[static_cast<std::size_t>(value_of(node, port))]);
    plan->output_buffers_.push_back(std::move(ports));
  }

  return plan;
}

RenderPlan::~RenderPlan()
{
  for (PlanNode& node : nodes_)
    if (node.type != nullptr && node.type->destroy != nullptr && node.state != nullptr)
      node.type->destroy(node.state);
}

std::size_t RenderPlan::buffer_bytes() const noexcept
{
  return buffers_.size() * static_cast<std::size_t>(max_quantum_) * sizeof(float);
}

const std::vector<int>& RenderPlan::output_buffer_of(std::size_t node_index) const
{
  static const std::vector<int> empty;
  return node_index < output_buffers_.size() ? output_buffers_[node_index] : empty;
}

void RenderPlan::execute(const float* input, float* output, int frames) noexcept
{
  // Contract: frames <= max_quantum(), which is what every node was created for.
  //
  // The graph input is handed to the input node rather than written into its buffer here. That is
  // not a style choice: a block-delay node is scheduled before the input node and reads that
  // buffer, so writing it up front would make the delay come out as zero samples.
  if (input_node_ >= 0)
  {
    PlanNode& node = nodes_[static_cast<std::size_t>(input_node_)];
    if (node.type->set_block_input != nullptr)
      node.type->set_block_input(node.state, input);
  }

  for (PlanNode& node : nodes_)
    node.type->process(node.state, node.inputs.data(), node.outputs.data(), frames);

  if (output_buffer_ >= 0 && output != nullptr)
    std::copy(buffers_[static_cast<std::size_t>(output_buffer_)],
              buffers_[static_cast<std::size_t>(output_buffer_)] + frames, output);
}

void RenderPlan::prewarm() noexcept
{
  if (max_quantum_ > 0)
    execute(nullptr, nullptr, max_quantum_);
}

std::unique_ptr<RenderPlan> RenderPlan::compile(const Graph& graph, int max_quantum, rt::Arena& arena,
                                                std::string& error)
{
  render::CompileOptions options;
  options.max_quantum = max_quantum;
  return compile(graph, options, arena, error);
}

} // namespace graph
} // namespace namchain
