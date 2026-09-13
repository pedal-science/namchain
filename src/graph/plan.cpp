#include "namchain/graph/plan.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace namchain
{
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

/// Every node's resolved type, keyed by id, plus its position in `graph.nodes`.
struct Indexed
{
  std::map<std::string, int> position;
  std::vector<const render::NodeType*> types;
};

Indexed index_graph(const Graph& graph, Validation& validation)
{
  Indexed indexed;
  indexed.types.reserve(graph.nodes.size());

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
    indexed.position[node.id] = static_cast<int>(indexed.types.size());
    indexed.types.push_back(type);
  }
  return indexed;
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
  for (const Connection& connection : graph.connections)
  {
    const auto from = indexed.position.find(connection.from);
    if (from == indexed.position.end())
      return Validation::failure("connection from unknown node '" + connection.from + "'");
    const auto to = indexed.position.find(connection.to);
    if (to == indexed.position.end())
      return Validation::failure("connection to unknown node '" + connection.to + "'");

    const render::NodeType* from_type = indexed.types[static_cast<std::size_t>(from->second)];
    const render::NodeType* to_type = indexed.types[static_cast<std::size_t>(to->second)];
    if (connection.from_port < 0 || connection.from_port >= from_type->outputs)
      return Validation::failure("node '" + connection.from + "' has no output port " +
                                 std::to_string(connection.from_port));
    if (connection.to_port < 0 || connection.to_port >= to_type->inputs)
      return Validation::failure("node '" + connection.to + "' has no input port " +
                                 std::to_string(connection.to_port));
    if (from_type->outputs == 0)
      return Validation::failure("node '" + connection.from + "' produces no output");
    if (to_type->inputs == 0)
      return Validation::failure("node '" + connection.to + "' accepts no input");
  }

  // Cycles are a compile error; feedback is expressed with an explicit delay node (§5.1). Kahn's
  // algorithm both detects a cycle and produces the topological order the compiler needs.
  const std::size_t n = graph.nodes.size();
  std::vector<std::vector<int>> successors(n);
  std::vector<int> indegree(n, 0);
  std::map<std::pair<int, int>, bool> seen;
  for (const Connection& connection : graph.connections)
  {
    const int from = indexed.position.at(connection.from);
    const int to = indexed.position.at(connection.to);
    if (seen.emplace(std::make_pair(from, to), true).second)
    {
      successors[static_cast<std::size_t>(from)].push_back(to);
      ++indegree[static_cast<std::size_t>(to)];
    }
  }

  std::vector<bool> placed(n, false);
  std::size_t placed_count = 0;
  for (std::size_t step = 0; step < n; ++step)
  {
    int chosen = -1;
    for (std::size_t i = 0; i < n; ++i)
    {
      if (!placed[i] && indegree[i] == 0)
      {
        chosen = static_cast<int>(i);
        break; // lowest index first: deterministic order for the same graph
      }
    }
    if (chosen < 0)
      return Validation::failure("graph contains a cycle; use an explicit delay node for feedback");
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
  std::vector<std::vector<int>> successors(n);
  std::vector<int> indegree(n, 0);
  std::map<std::pair<int, int>, bool> seen;
  for (const Connection& connection : graph.connections)
  {
    const int from = indexed.position.at(connection.from);
    const int to = indexed.position.at(connection.to);
    if (seen.emplace(std::make_pair(from, to), true).second)
    {
      successors[static_cast<std::size_t>(from)].push_back(to);
      ++indegree[static_cast<std::size_t>(to)];
    }
  }

  std::vector<int> order;
  order.reserve(n);
  std::vector<bool> placed(n, false);
  for (std::size_t step = 0; step < n; ++step)
  {
    int chosen = -1;
    for (std::size_t i = 0; i < n; ++i)
    {
      if (!placed[i] && indegree[i] == 0)
      {
        chosen = static_cast<int>(i);
        break;
      }
    }
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

std::unique_ptr<RenderPlan> RenderPlan::compile(const Graph& graph, int max_quantum, rt::Arena& arena,
                                                std::string& error)
{
  if (max_quantum < 1)
  {
    error = "max_quantum must be at least 1";
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
      base += indexed.types[static_cast<std::size_t>(i)]->outputs;
    return base + port;
  };
  int value_count = 0;
  for (const render::NodeType* type : indexed.types)
    value_count += type->outputs;

  std::vector<int> owner(value_count, -1);
  for (std::size_t node = 0; node < indexed.types.size(); ++node)
    for (int port = 0; port < indexed.types[node]->outputs; ++port)
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
  }

  // Interval colouring: walk values in definition order and reuse the first buffer whose previous
  // occupant died before this definition. This is the memory plan of §5.2 in miniature.
  std::vector<int> colour(value_count, -1);
  std::vector<int> free_at;
  int next_colour = 0;
  for (const int node : order)
  {
    const int def_position = position_of[static_cast<std::size_t>(node)];
    for (int port = 0; port < indexed.types[static_cast<std::size_t>(node)]->outputs; ++port)
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
    plan_node.state = type->create(spec, max_quantum, error);
    if (plan_node.state == nullptr && type->id != "nc.input" && type->id != "nc.output" && type->id != "nc.split" &&
        type->id != "nc.mix")
      return nullptr; // create() set `error`

    plan_node.inputs.assign(static_cast<std::size_t>(type->inputs), plan->buffers_[static_cast<std::size_t>(silence_buffer)]);
    plan_node.outputs.reserve(static_cast<std::size_t>(type->outputs) + 1);
    for (int port = 0; port < type->outputs; ++port)
      plan_node.outputs.push_back(plan->buffers_[static_cast<std::size_t>(colour[static_cast<std::size_t>(value_of(node, port))])]);
    plan_node.outputs.push_back(nullptr); // terminator: process_split walks until nullptr

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
      plan->input_buffer_ = colour[static_cast<std::size_t>(value_of(order[i], 0))];
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
    for (int port = 0; port < indexed.types[static_cast<std::size_t>(node)]->outputs; ++port)
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
  if (input_buffer_ >= 0)
  {
    float* destination = buffers_[static_cast<std::size_t>(input_buffer_)];
    if (input != nullptr)
      std::copy(input, input + frames, destination);
    else
      std::fill(destination, destination + frames, 0.0f);
  }

  for (PlanNode& node : nodes_)
    node.type->process(node.state, node.inputs.data(), node.outputs.data(), frames);

  if (output_buffer_ >= 0 && output != nullptr)
    std::copy(buffers_[static_cast<std::size_t>(output_buffer_)],
              buffers_[static_cast<std::size_t>(output_buffer_)] + frames, output);
}

} // namespace graph
} // namespace namchain
