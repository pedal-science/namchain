// The model budget. See namchain/daemon/model_budget.h for what it is for.
#include "namchain/daemon/model_budget.h"

#include <cstdio>
#include <system_error>

namespace namchain
{
namespace daemon
{

WorkingSet WorkingSetCache::get(const std::string& path, int max_quantum)
{
  std::error_code size_error;
  std::error_code time_error;
  const std::uintmax_t size = std::filesystem::file_size(path, size_error);
  const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, time_error);
  if (size_error || time_error)
  {
    // Nothing to key on, so nothing to cache: the estimator reports why it cannot read the file.
    ++parses_;
    return working_set_from_file(path, max_quantum);
  }

  const auto found = entries_.find(path);
  if (found != entries_.end() && found->second.size == size && found->second.modified == modified &&
      found->second.max_quantum == max_quantum)
    return found->second.estimate;

  ++parses_;
  Entry entry;
  entry.size = size;
  entry.modified = modified;
  entry.max_quantum = max_quantum;
  entry.estimate = working_set_from_file(path, max_quantum);
  const WorkingSet estimate = entry.estimate;
  entries_[path] = std::move(entry);
  return estimate;
}

BudgetOutcome apply_model_budget(graph::Document& resolved, std::size_t per_chain_bytes,
                                 std::size_t document_bytes, int max_quantum, WorkingSetCache& cache)
{
  BudgetOutcome outcome;
  outcome.per_chain_bytes = per_chain_bytes;
  outcome.budget_bytes = document_bytes;
  if (per_chain_bytes == 0 || document_bytes == 0)
    return outcome;

  for (std::size_t c = 0; c < resolved.chains.size(); ++c)
  {
    graph::Chain& chain = resolved.chains[c];
    for (std::size_t n = 0; n < chain.graph.nodes.size(); ++n)
    {
      graph::NodeSpec& node = chain.graph.nodes[n];
      if (node.type != "nc.nam")
        continue;
      // Decided here and nowhere else: whatever a previous publish chose, this one chooses again.
      node.slim = false;

      BudgetedModel model;
      model.chain = chain.id;
      model.node = node.id;
      model.element = node.element.empty() ? node.id : node.element;
      model.chain_index = c;
      model.node_index = n;
      const auto path = node.strings.find("model");
      if (path != node.strings.end())
        model.path = path->second;

      const WorkingSet estimate = cache.get(model.path, max_quantum);
      if (estimate.ok)
      {
        model.full_bytes = estimate.full_bytes;
        model.slim_bytes = estimate.slim_bytes;
        model.slimmable = estimate.slimmable;
      }
      else
      {
        model.estimated = false;
        model.why = estimate.detail;
      }
      outcome.full_bytes += model.full_bytes;
      outcome.models.push_back(std::move(model));
    }
  }

  // A chain is executed by one thread on one core, so no chain may exceed the per-core ceiling --
  // whatever the document as a whole is allowed. Slim that chain's own models first, last declared
  // first within it, and only then look at the document.
  for (std::size_t c = 0; c < resolved.chains.size(); ++c)
  {
    std::size_t chain_bytes = 0;
    for (const BudgetedModel& model : outcome.models)
    {
      if (model.chain_index == c)
        chain_bytes += model.bytes();
    }
    for (auto it = outcome.models.rbegin(); it != outcome.models.rend() && chain_bytes > per_chain_bytes; ++it)
    {
      if (it->chain_index != c || !it->slimmable || it->slimmed)
        continue;
      it->slimmed = true;
      chain_bytes -= it->full_bytes - it->slim_bytes;
      resolved.chains[it->chain_index].graph.nodes[it->node_index].slim = true;
    }
    if (chain_bytes > outcome.heaviest_chain_bytes)
    {
      outcome.heaviest_chain_bytes = chain_bytes;
      outcome.heaviest_chain = c;
    }
  }

  // The last declared first. Nothing about the order is measured -- slimming either of two equal
  // amps saves the same -- but it is predictable, and it leaves the first amp of a chain as written.
  outcome.used_bytes = 0;
  for (const BudgetedModel& model : outcome.models)
    outcome.used_bytes += model.bytes();
  for (auto it = outcome.models.rbegin(); it != outcome.models.rend() && outcome.used_bytes > document_bytes;
       ++it)
  {
    if (!it->slimmable || it->slimmed)
      continue;
    it->slimmed = true;
    outcome.used_bytes -= it->full_bytes - it->slim_bytes;
    resolved.chains[it->chain_index].graph.nodes[it->node_index].slim = true;
  }
  outcome.over = outcome.used_bytes > document_bytes || outcome.heaviest_chain_bytes > per_chain_bytes;
  return outcome;
}

BudgetOutcome apply_model_budget(graph::Document& resolved, std::size_t budget_bytes, int max_quantum,
                                 WorkingSetCache& cache)
{
  return apply_model_budget(resolved, budget_bytes, budget_bytes, max_quantum, cache);
}

std::string format_kib(std::size_t bytes)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
  return text;
}

} // namespace daemon
} // namespace namchain
