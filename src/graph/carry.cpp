#include "namchain/graph/carry.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace namchain
{
namespace graph
{

namespace
{

/// A node's input or output port: (node id, port).
using Port = std::pair<std::string, int>;

/// One graph, indexed the ways the planner reads it.
struct View
{
  std::map<std::string, const NodeSpec*> nodes;
  std::vector<std::string> order;               ///< declaration order, which is what keeps output stable
  std::map<Port, Port> driver;                  ///< (to, to_port) -> (from, from_port)
  std::map<std::string, std::set<int>> driven;  ///< node -> its input ports that have a driver
};

View view_of(const Graph& graph)
{
  View view;
  for (const NodeSpec& node : graph.nodes)
  {
    view.nodes[node.id] = &node;
    view.order.push_back(node.id);
  }
  for (const Connection& connection : graph.connections)
  {
    view.driver[{connection.to, connection.to_port}] = {connection.from, connection.from_port};
    view.driven[connection.to].insert(connection.to_port);
  }
  return view;
}

/// What it takes to turn one node into another while keeping its instance.
enum class Delta
{
  Same,     ///< nothing the node reads changed
  Ramps,    ///< only values the node ramps changed: keep it and ramp them
  Bypass,   ///< its bypass flag changed (and perhaps values that ramp): keep it and fade wet <-> dry
  Replaced, ///< a type, a string, a value that does not ramp, or a key appearing or going: build anew
};

const render::ParamDesc* declared(const render::NodeType& type, const std::string& key)
{
  for (int i = 0; i < type.param_count; ++i)
  {
    if (type.params != nullptr && key == type.params[i].id)
      return &type.params[i];
  }
  return nullptr;
}

Delta delta_of(const NodeSpec& was, const NodeSpec& now, std::vector<CarriedRamp>& ramps)
{
  if (was.type != now.type || was.strings != now.strings)
    return Delta::Replaced;
  const render::NodeType* type = render::find_node_type(now.type);
  if (type == nullptr)
    return Delta::Replaced;

  // The same keys, or it is a different node: a key that appears or goes is a default coming or going,
  // and "the default" of a parameter in a `one_of` group is not a value anything can ramp to.
  if (was.params.size() != now.params.size())
    return Delta::Replaced;
  std::vector<CarriedRamp> moved;
  for (const auto& [key, value] : now.params)
  {
    const auto previous = was.params.find(key);
    if (previous == was.params.end())
      return Delta::Replaced;
    if (previous->second == value)
      continue;
    const render::ParamDesc* desc = declared(*type, key);
    if (desc == nullptr || !desc->rampable || !desc->patchable || desc->kind == render::ParamKind::Option)
      return Delta::Replaced;
    moved.push_back({now.id, key, value});
  }
  ramps.insert(ramps.end(), moved.begin(), moved.end());
  if (was.bypass != now.bypass)
    return Delta::Bypass;
  return moved.empty() ? Delta::Same : Delta::Ramps;
}

bool is_block_delay(const std::string& type_id)
{
  const render::NodeType* type = render::find_node_type(type_id);
  return type != nullptr && type->block_delay;
}

bool is_stateful(const std::string& type_id)
{
  const render::NodeType* type = render::find_node_type(type_id);
  return type != nullptr && type->create != nullptr;
}

std::string first_of_type(const Graph& graph, const char* type)
{
  for (const NodeSpec& node : graph.nodes)
  {
    if (node.type == type)
      return node.id;
  }
  return {};
}

/// The transitional graph for one choice of which nodes keep their instance.
struct Built
{
  Graph graph;
  std::map<std::string, std::string> carried_from;
  /// Kept nodes with at least one crossfaded input: the candidates to rebuild if the graph has a cycle.
  std::set<std::string> faded;
  int fades = 0;
  int retiring = 0;
  int created = 0;
};

Built build_transitional(const View& was, const View& now, const Graph& before, const Graph& after,
                         const std::set<std::string>& kept, const std::map<std::string, Delta>& delta,
                         int fade_frames)
{
  Built built;
  const auto is_kept = [&kept](const std::string& id) { return kept.count(id) != 0; };
  const auto bypassing = [&](const std::string& id) {
    const auto found = delta.find(id);
    return is_kept(id) && found != delta.end() && found->second == Delta::Bypass;
  };

  // Every id either graph uses, so a name minted here cannot collide with one of them.
  std::set<std::string> taken;
  for (const auto& entry : was.nodes)
    taken.insert(entry.first);
  for (const auto& entry : now.nodes)
    taken.insert(entry.first);
  const auto mint = [&taken](const std::string& stem) {
    std::string name = stem;
    for (int suffix = 2; taken.count(name) != 0; ++suffix)
      name = stem + "~" + std::to_string(suffix);
    taken.insert(name);
    return name;
  };

  // The inputs of a kept node that need a crossfade: its driver moved, or its driver is a node that is
  // not kept (new, or rebuilt), whose output is therefore a different signal from the one it had.
  std::vector<Port> faded_ports;
  for (const std::string& id : now.order)
  {
    if (!is_kept(id))
      continue;
    std::set<int> ports;
    if (const auto found = was.driven.find(id); found != was.driven.end())
      ports.insert(found->second.begin(), found->second.end());
    if (const auto found = now.driven.find(id); found != now.driven.end())
      ports.insert(found->second.begin(), found->second.end());
    for (const int port : ports)
    {
      const auto old_driver = was.driver.find({id, port});
      const auto new_driver = now.driver.find({id, port});
      const bool had = old_driver != was.driver.end();
      const bool has = new_driver != now.driver.end();
      const bool moved = had != has || (had && old_driver->second != new_driver->second);
      const bool rebuilt = has && !is_kept(new_driver->second.first);
      if (moved || rebuilt)
      {
        faded_ports.push_back({id, port});
        built.faded.insert(id);
      }
    }
  }

  // The nodes that are going away but must keep running until the fades close: whatever the old side
  // of a crossfade reads that is not kept, and what those read in turn.
  std::set<std::string> retiring;
  std::deque<std::string> work;
  const auto need_old = [&](const std::string& id) {
    if (!is_kept(id) && retiring.insert(id).second)
      work.push_back(id);
  };
  for (const Port& port : faded_ports)
  {
    const auto old_driver = was.driver.find(port);
    if (old_driver != was.driver.end())
      need_old(old_driver->second.first);
  }
  while (!work.empty())
  {
    const std::string id = work.front();
    work.pop_front();
    if (const auto found = was.driven.find(id); found != was.driven.end())
    {
      for (const int port : found->second)
        need_old(was.driver.at({id, port}).first);
    }
  }

  // Names. A retiring node is renamed because a new node may have its id; the crossfades are minted.
  std::map<std::string, std::string> old_name;
  for (const std::string& id : was.order)
  {
    if (retiring.count(id) != 0)
      old_name[id] = mint("~old:" + id);
  }
  std::map<Port, std::string> fade_name;
  for (const Port& port : faded_ports)
    fade_name[port] = mint("~fade:" + port.first + ":" + std::to_string(port.second));
  std::map<std::string, std::string> bypass_name;
  for (const std::string& id : now.order)
  {
    if (bypassing(id))
      bypass_name[id] = mint("~bypass:" + id);
  }

  const auto xfade = [fade_frames](const std::string& id) {
    NodeSpec spec;
    spec.id = id;
    spec.type = "nc.xfade";
    spec.params["frames"] = static_cast<double>(fade_frames);
    return spec;
  };

  // Nodes: the new graph's, then the retiring ones as they were, then the crossfades.
  for (const std::string& id : now.order)
  {
    NodeSpec spec = *now.nodes.at(id);
    if (is_kept(id))
    {
      built.carried_from[id] = id;
      // A node whose bypass is changing renders wet throughout; its own crossfade does the switching.
      if (bypassing(id))
        spec.bypass = false;
    }
    else
    {
      ++built.created;
    }
    built.graph.nodes.push_back(std::move(spec));
  }
  for (const std::string& id : was.order)
  {
    const auto renamed = old_name.find(id);
    if (renamed == old_name.end())
      continue;
    NodeSpec spec = *was.nodes.at(id);
    spec.id = renamed->second;
    built.carried_from[renamed->second] = id;
    built.graph.nodes.push_back(std::move(spec));
    ++built.retiring;
  }
  for (const Port& port : faded_ports)
    built.graph.nodes.push_back(xfade(fade_name.at(port)));
  for (const std::string& id : now.order)
  {
    if (bypassing(id))
      built.graph.nodes.push_back(xfade(bypass_name.at(id)));
  }

  // What a kept node presents to whatever reads it: its own output, or its bypass crossfade.
  const auto source = [&](const Port& from) -> Port {
    if (bypassing(from.first))
      return {bypass_name.at(from.first), 0};
    return from;
  };
  const auto connect = [&built](const Port& from, const Port& to) {
    Connection connection;
    connection.from = from.first;
    connection.from_port = from.second;
    connection.to = to.first;
    connection.to_port = to.second;
    built.graph.connections.push_back(std::move(connection));
  };
  const auto faded = [&fade_name](const Port& port) { return fade_name.count(port) != 0; };

  // The new graph's edges, with a crossfaded input taking its new signal through its crossfade.
  for (const Connection& connection : after.connections)
  {
    const Port from = source({connection.from, connection.from_port});
    const Port to{connection.to, connection.to_port};
    if (faded(to))
      connect(from, {fade_name.at(to), 1});
    else
      connect(from, to);
  }
  // The retiring nodes' edges as they were. What they read is kept, or retiring too.
  for (const Connection& connection : before.connections)
  {
    const auto to = old_name.find(connection.to);
    if (to == old_name.end())
      continue;
    const Port from = is_kept(connection.from) ? source({connection.from, connection.from_port})
                                               : Port{old_name.at(connection.from), connection.from_port};
    connect(from, {to->second, connection.to_port});
  }
  // Each crossfade: the old signal in, and out to the input it serves.
  for (const Port& port : faded_ports)
  {
    const auto old_driver = was.driver.find(port);
    if (old_driver != was.driver.end())
    {
      const Port& from = old_driver->second;
      connect(is_kept(from.first) ? source(from) : Port{old_name.at(from.first), from.second},
              {fade_name.at(port), 0});
    }
    connect({fade_name.at(port), 0}, port);
  }
  // Each bypass crossfade, between the node's own output and the signal at its input.
  for (const std::string& id : now.order)
  {
    if (!bypassing(id))
      continue;
    const Port wet{id, 0};
    bool has_dry = true;
    Port dry;
    if (faded({id, 0}))
      dry = {fade_name.at({id, 0}), 0};
    else if (const auto driver = now.driver.find({id, 0}); driver != now.driver.end())
      dry = source(driver->second);
    else
      has_dry = false;
    const bool was_bypassed = was.nodes.at(id)->bypass;
    const bool is_bypassed = now.nodes.at(id)->bypass;
    if (!was_bypassed || has_dry)
      connect(was_bypassed ? dry : wet, {bypass_name.at(id), 0});
    if (!is_bypassed || has_dry)
      connect(is_bypassed ? dry : wet, {bypass_name.at(id), 1});
  }

  built.fades = static_cast<int>(faded_ports.size() + bypass_name.size());
  return built;
}

/// The nodes a schedule cannot place: those on a cycle, or downstream of one. Edges into a block-delay
/// node are not dependencies, exactly as the compiler treats them.
std::set<std::string> unplaced(const Graph& graph)
{
  std::map<std::string, int> indegree;
  std::map<std::string, std::vector<std::string>> successors;
  for (const NodeSpec& node : graph.nodes)
    indegree[node.id] = 0;
  std::map<std::string, std::string> type_of;
  for (const NodeSpec& node : graph.nodes)
    type_of[node.id] = node.type;
  std::set<std::pair<std::string, std::string>> seen;
  for (const Connection& connection : graph.connections)
  {
    if (is_block_delay(type_of[connection.to]))
      continue;
    if (!seen.insert({connection.from, connection.to}).second)
      continue;
    successors[connection.from].push_back(connection.to);
    ++indegree[connection.to];
  }
  std::deque<std::string> ready;
  for (const auto& [id, count] : indegree)
  {
    if (count == 0)
      ready.push_back(id);
  }
  std::set<std::string> placed;
  while (!ready.empty())
  {
    const std::string id = ready.front();
    ready.pop_front();
    placed.insert(id);
    for (const std::string& next : successors[id])
    {
      if (--indegree[next] == 0)
        ready.push_back(next);
    }
  }
  std::set<std::string> left;
  for (const NodeSpec& node : graph.nodes)
  {
    if (placed.count(node.id) == 0)
      left.insert(node.id);
  }
  return left;
}

void add_copy(const RenderPlan& from, const std::string& from_id, const RenderPlan& to, const std::string& to_id,
              std::vector<std::pair<const float*, float*>>& copies)
{
  const int source = from.index_of(from_id);
  const int destination = to.index_of(to_id);
  if (source < 0 || destination < 0)
    return;
  const float* read = from.input_buffer(static_cast<std::size_t>(source), 0);
  float* write = to.input_buffer(static_cast<std::size_t>(destination), 0);
  if (read != nullptr && write != nullptr)
    copies.emplace_back(read, write);
}

} // namespace

bool plan_local_transition(const Graph& before, const Graph& after, const std::set<std::string>& unusable,
                           int fade_frames, LocalPlan& plan, std::string& reason)
{
  plan = LocalPlan{};
  if (before.nodes.empty() || after.nodes.empty())
  {
    reason = "there is no running graph to carry from";
    return false;
  }
  const View was = view_of(before);
  const View now = view_of(after);

  // The chain's ends run once in every plan: a chain has exactly one input and one output node, so a
  // transition that would need two of either is a whole-chain one.
  const std::string input = first_of_type(after, "nc.input");
  const std::string output = first_of_type(after, "nc.output");
  if (input.empty() || input != first_of_type(before, "nc.input"))
  {
    reason = "the chain's input node is not the same node";
    return false;
  }
  if (output.empty() || output != first_of_type(before, "nc.output"))
  {
    reason = "the chain's output node is not the same node";
    return false;
  }

  std::map<std::string, Delta> delta;
  std::map<std::string, std::vector<CarriedRamp>> ramps;
  std::set<std::string> kept;
  bool identity_changed = false;
  for (const std::string& id : now.order)
  {
    const auto old = was.nodes.find(id);
    if (old == was.nodes.end() || unusable.count(id) != 0)
    {
      delta[id] = Delta::Replaced;
      continue;
    }
    std::vector<CarriedRamp> moved;
    delta[id] = delta_of(*old->second, *now.nodes.at(id), moved);
    if (delta[id] == Delta::Replaced)
      continue;
    kept.insert(id);
    ramps[id] = std::move(moved);
    const NodeSpec& was_node = *old->second;
    const NodeSpec& now_node = *now.nodes.at(id);
    identity_changed = identity_changed || was_node.element != now_node.element || was_node.role != now_node.role;
  }
  if (kept.count(input) == 0 || kept.count(output) == 0)
  {
    reason = "the chain's input or output node changed";
    return false;
  }

  // Only values that ramp moved, or bypass flags flipped, on the same nodes wired the same way: no plan
  // at all. A bypass is then a crossfade inside the running plan, between the node's output and its
  // input, as a value ramps.
  bool structural = identity_changed || before.nodes.size() != after.nodes.size() || was.driver != now.driver;
  for (const auto& [id, kind] : delta)
    structural = structural || kind == Delta::Replaced;
  if (!structural)
  {
    plan.ramps_only = true;
    for (const std::string& id : now.order)
    {
      plan.ramps.insert(plan.ramps.end(), ramps[id].begin(), ramps[id].end());
      if (delta[id] == Delta::Bypass)
        plan.bypasses.push_back({id, now.nodes.at(id)->bypass});
    }
    plan.final_graph = after;
    return true;
  }

  // Keep everything that can be kept, then give up keeping a node for as long as the transitional
  // graph has a cycle. A cycle comes from two kept nodes trading places: each one's old signal is the
  // other's output. Rebuilding one of them breaks it -- its old self keeps running on the old side, its
  // new self on the new -- at the cost of that node's state.
  for (std::size_t attempt = 0; attempt <= after.nodes.size(); ++attempt)
  {
    Built built = build_transitional(was, now, before, after, kept, delta, fade_frames);
    const std::set<std::string> cyclic = unplaced(built.graph);
    if (cyclic.empty())
    {
      int stateful_kept = 0;
      for (const std::string& id : kept)
      {
        if (id != input && is_stateful(now.nodes.at(id)->type))
          ++stateful_kept;
      }
      if (stateful_kept == 0)
      {
        reason = "nothing with state would be kept";
        return false;
      }

      plan.instant = built.fades == 0 && built.retiring == 0;
      plan.final_graph = after;
      plan.carried_from = std::move(built.carried_from);
      plan.fades = built.fades;
      plan.kept = static_cast<int>(kept.size());
      plan.created = built.created;
      plan.retiring = built.retiring;
      for (const std::string& id : now.order)
      {
        if (kept.count(id) != 0)
          plan.ramps.insert(plan.ramps.end(), ramps[id].begin(), ramps[id].end());
      }
      const Graph& first = plan.instant ? plan.final_graph : built.graph;
      for (const NodeSpec& node : first.nodes)
      {
        const auto from = plan.carried_from.find(node.id);
        if (from != plan.carried_from.end() && is_block_delay(node.type))
          plan.buffers_from_running.emplace_back(from->second, node.id);
      }
      if (!plan.instant)
        plan.transitional = std::move(built.graph);
      return true;
    }

    std::string victim;
    for (const std::string& id : now.order)
    {
      if (kept.count(id) != 0 && cyclic.count(id) != 0 && built.faded.count(id) != 0 && id != input &&
          id != output)
      {
        victim = id;
        break;
      }
    }
    if (victim.empty())
    {
      reason = "the transition cannot be scheduled";
      return false;
    }
    kept.erase(victim);
    delta[victim] = Delta::Replaced;
  }
  reason = "the transition cannot be scheduled";
  return false;
}

bool build_carried_plans(const RenderPlan& running, const LocalPlan& plan, const render::CompileOptions& options,
                         rt::Arena& transitional_arena, rt::Arena& final_arena, CarriedPlans& plans,
                         std::string& error)
{
  plans = CarriedPlans{};

  CarriedInstances from_running;
  for (const auto& [id, running_id] : plan.carried_from)
  {
    NodeInstance instance = running.instance_of(running_id);
    if (instance != nullptr)
      from_running[id] = std::move(instance);
  }

  if (plan.instant)
  {
    plans.final_plan = RenderPlan::compile(plan.final_graph, options, final_arena, error, &from_running);
    if (plans.final_plan == nullptr)
      return false;
    plans.final_plan->prewarm_fresh();
    for (const auto& [running_id, id] : plan.buffers_from_running)
      add_copy(running, running_id, *plans.final_plan, id, plans.on_adopt);
    return true;
  }

  plans.transitional = RenderPlan::compile(plan.transitional, options, transitional_arena, error, &from_running);
  if (plans.transitional == nullptr)
    return false;
  plans.transitional->prewarm_fresh();

  // The final plan runs the transitional plan's instances for every node it has: the kept ones, and the
  // ones the transitional plan created. So it creates nothing, and has nothing to prewarm.
  CarriedInstances from_transitional;
  for (const NodeSpec& node : plan.final_graph.nodes)
  {
    NodeInstance instance = plans.transitional->instance_of(node.id);
    if (instance != nullptr)
      from_transitional[node.id] = std::move(instance);
  }
  plans.final_plan = RenderPlan::compile(plan.final_graph, options, final_arena, error, &from_transitional);
  if (plans.final_plan == nullptr)
    return false;

  for (const auto& [running_id, id] : plan.buffers_from_running)
    add_copy(running, running_id, *plans.transitional, id, plans.on_adopt);
  for (const NodeSpec& node : plan.final_graph.nodes)
  {
    if (is_block_delay(node.type))
      add_copy(*plans.transitional, node.id, *plans.final_plan, node.id, plans.on_final);
  }
  return true;
}

} // namespace graph
} // namespace namchain
