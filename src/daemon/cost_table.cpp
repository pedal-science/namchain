// The cost table: what a rig costs, and whether it may play. See namchain/daemon/cost_table.h.
#include "namchain/daemon/cost_table.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>
#include <type_traits>

#include <json.hpp>

#include "namchain/wav.h"
#include "namchain/working_set.h"

namespace namchain
{
namespace daemon
{
namespace
{

using json = nlohmann::json;

/// A cost comparison's slack: the rows are three significant figures, so a sum that is a hair over
/// the headroom because of the arithmetic is not over it.
constexpr double kEpsilon = 1e-9;

std::string string_param(const graph::NodeSpec& node, const std::string& name)
{
  const auto found = node.strings.find(name);
  return found == node.strings.end() ? std::string() : found->second;
}

double number_or(const json& object, const char* key, double fallback)
{
  const auto found = object.find(key);
  if (found == object.end() || !found->is_number())
    return fallback;
  return found->get<double>();
}

/// Every number in a rows object, with a caller-made key. Anything that is not a number is skipped:
/// a table the engine cannot read a row from is a row it does not have, which costs `unknown`.
template <typename Key>
void read_rows(const json& object, std::map<Key, double>& out)
{
  if (!object.is_object())
    return;
  for (auto it = object.begin(); it != object.end(); ++it)
  {
    if (!it.value().is_number())
      continue;
    const double value = it.value().get<double>();
    if (!(value > 0.0) || !std::isfinite(value))
      continue;
    if constexpr (std::is_same_v<Key, int>)
    {
      try
      {
        out[std::stoi(it.key())] = value;
      }
      catch (const std::exception&)
      {
        // Not a tap count: not a row.
      }
    }
    else
    {
      out[it.key()] = value;
    }
  }
}

} // namespace

std::vector<std::pair<std::string, double>> CostTable::rows() const
{
  std::vector<std::pair<std::string, double>> out;
  for (const auto& [family, cost] : nam)
    out.emplace_back("NAM." + family, cost);
  for (const auto& [taps, cost] : ir)
    out.emplace_back("IR." + std::to_string(taps), cost);
  for (const auto& [type, cost] : node)
    out.emplace_back("node." + type, cost);
  return out;
}

CostTable::Charge CostTable::charge_nam(const std::string& family, bool lite) const
{
  Charge charge;
  const std::string want = family + (lite ? ".lite" : ".full");
  const auto found = family.empty() ? nam.end() : nam.find(want);
  if (found != nam.end())
  {
    charge.cost = found->second;
    charge.row = "NAM." + want;
    charge.measured = true;
    return charge;
  }
  charge.cost = unknown_nam;
  charge.row = "unknown.NAM";
  return charge;
}

CostTable::Charge CostTable::charge_ir(std::size_t taps) const
{
  Charge charge;
  // The cheapest row that is not shorter than the IR: cost is not linear in taps, and charging a
  // 6000-tap cab as if it were a 512-tap one is the one direction that must not happen.
  for (const auto& [row_taps, cost] : ir)
  {
    if (static_cast<std::size_t>(row_taps) >= taps)
    {
      charge.cost = cost;
      charge.row = "IR." + std::to_string(row_taps);
      charge.measured = true;
      return charge;
    }
  }
  charge.cost = unknown_ir;
  charge.row = "unknown.IR";
  return charge;
}

CostTable::Charge CostTable::charge_node(const std::string& type) const
{
  Charge charge;
  const auto found = node.find(type);
  if (found != node.end())
  {
    charge.cost = found->second;
    charge.row = "node." + type;
    charge.measured = true;
    return charge;
  }
  charge.cost = unknown_node;
  charge.row = "unknown.node";
  return charge;
}

CostTable CostTable::from_json(const std::string& text, const std::string& path)
{
  CostTable table;
  table.path = path;
  json doc;
  try
  {
    doc = json::parse(text);
  }
  catch (const std::exception& e)
  {
    table.detail = std::string("JSON parse error: ") + e.what();
    return table;
  }
  if (!doc.is_object())
  {
    table.detail = "not a JSON object";
    return table;
  }

  const auto string_at = [&](const char* key) {
    const auto found = doc.find(key);
    return found != doc.end() && found->is_string() ? found->get<std::string>() : std::string();
  };
  table.host = string_at("host");
  table.measured = string_at("measured");
  table.engine_commit = string_at("engine_commit");
  table.kernel = string_at("kernel");
  table.bar = string_at("bar");
  table.quantum = static_cast<int>(number_or(doc, "quantum", table.quantum));
  table.deadline_us = number_or(doc, "deadline_us", 0.0);
  const double headroom = number_or(doc, "headroom", 0.0);
  if (headroom > 0.0 && headroom <= 1.0)
    table.headroom = headroom;

  // The coexistence charge (D3, 2026-10-09): {"1": 1.27, "2": 1.47} -- warm documents to the factor
  // the rig's cost is multiplied by. A table without it charges nothing, which is every table measured
  // before that date, and the engine then admits exactly as it always did.
  const auto coexistence = doc.find("coexistence");
  if (coexistence != doc.end() && coexistence->is_object())
  {
    const auto factors = coexistence->find("factors");
    if (factors != coexistence->end() && factors->is_object())
    {
      for (auto entry = factors->begin(); entry != factors->end(); ++entry)
      {
        if (!entry.value().is_number())
          continue;
        const int count = std::atoi(entry.key().c_str());
        const double value = entry.value().get<double>();
        if (count > 0 && value >= 1.0 && std::isfinite(value))
          table.coexistence_factors[count] = value;
      }
    }
    const auto text = [&](const char* key) {
      const auto found = coexistence->find(key);
      return found != coexistence->end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    table.coexistence_measured = text("measured");
    table.coexistence_note = text("note");
  }

  const auto core = doc.find("core_percent");
  if (core != doc.end() && core->is_object())
  {
    // NAM rows are "family.size" in the file's nested shape: {"NAM": {"WaveNet": {"full": ..,
    // "lite": ..}}}. Flattened here to "WaveNet.full", which is what a charge is keyed by.
    const auto nam_rows = core->find("NAM");
    if (nam_rows != core->end() && nam_rows->is_object())
    {
      for (auto family = nam_rows->begin(); family != nam_rows->end(); ++family)
      {
        if (!family.value().is_object())
          continue;
        for (auto size = family.value().begin(); size != family.value().end(); ++size)
        {
          if (size.key() != "full" && size.key() != "lite")
            continue;
          if (!size.value().is_number())
            continue;
          const double value = size.value().get<double>();
          if (value > 0.0 && std::isfinite(value))
            table.nam[family.key() + "." + size.key()] = value;
        }
      }
    }
    read_rows(core->contains("IR") ? (*core)["IR"] : json(), table.ir);
    read_rows(core->contains("node") ? (*core)["node"] : json(), table.node);
  }

  const auto unknown = doc.find("unknown");
  if (unknown != doc.end() && unknown->is_object())
  {
    table.unknown_nam = number_or(*unknown, "NAM", table.unknown_nam);
    table.unknown_ir = number_or(*unknown, "IR", table.unknown_ir);
    table.unknown_node = number_or(*unknown, "node", table.unknown_node);
  }

  table.ok = true;
  table.detail = "loaded";
  return table;
}

CostTable CostTable::load(const std::string& path)
{
  CostTable table;
  table.path = path;
  if (path.empty())
  {
    table.detail = "no cost table configured";
    return table;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    table.detail = "cannot open " + path;
    return table;
  }
  std::ostringstream text;
  text << in.rdbuf();
  return from_json(text.str(), path);
}

// --- the units a document is charged for ---------------------------------------------------------

UnitCache::Unit UnitCache::get(const std::string& path, bool is_ir, double rate, int max_quantum)
{
  Unit unit;
  unit.kind = is_ir ? "IR" : "NAM";
  if (path.empty())
  {
    unit.why = is_ir ? "no 'ir' was named" : "no 'model' was named";
    return unit;
  }

  std::error_code size_error;
  std::error_code time_error;
  const std::uintmax_t size = std::filesystem::file_size(path, size_error);
  const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, time_error);
  const bool statable = !size_error && !time_error;
  if (statable)
  {
    const auto found = entries_.find(path);
    if (found != entries_.end() && found->second.size == size && found->second.modified == modified)
      return found->second.unit;
  }

  ++reads_;
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    unit.why = "cannot open " + path;
  }
  else
  {
    std::ostringstream text;
    text << in.rdbuf();
    const std::string body = text.str();
    if (is_ir && std::filesystem::path(path).extension() != ".nam")
    {
      // A .wav cab: the taps that reach the convolver are the file's, resampled to the engine rate
      // and then truncated the way `nc.cab.ir` truncates (src/nodes/builtin.cpp).
      std::vector<float> samples;
      double file_rate = 0.0;
      std::string error;
      if (!load_wav_mono(path, samples, file_rate, error))
        unit.why = error;
      else if (samples.empty() || !(file_rate > 0.0))
        unit.why = path + " has no samples";
      else
      {
        const double resampled = std::abs(file_rate - rate) > 0.5
                                   ? static_cast<double>(samples.size()) * rate / file_rate
                                   : static_cast<double>(samples.size());
        unit.taps = std::min<std::size_t>(kMaxIrTaps, static_cast<std::size_t>(std::llround(resampled)));
        unit.ok = true;
        unit.why = "wav";
      }
    }
    else if (is_ir)
    {
      // A Linear .nam used as a cab: its weights are the taps, truncated the same way.
      try
      {
        const json doc = json::parse(body);
        std::size_t taps = 0;
        const auto config = doc.find("config");
        if (config != doc.end() && config->is_object() && config->contains("receptive_field") &&
            (*config)["receptive_field"].is_number())
          taps = static_cast<std::size_t>((*config)["receptive_field"].get<double>());
        else if (doc.contains("weights") && doc["weights"].is_array())
          taps = doc["weights"].size();
        if (taps == 0)
          unit.why = path + " names no taps";
        else
        {
          unit.taps = std::min<std::size_t>(kMaxIrTaps, taps);
          unit.ok = true;
          unit.why = "linear nam";
        }
      }
      catch (const std::exception& e)
      {
        unit.why = std::string("cannot read ") + path + ": " + e.what();
      }
    }
    else
    {
      unit.family = model_family_from_file(path);
      const WorkingSet estimate = working_set_from_file(path, max_quantum);
      if (unit.family.empty() && !estimate.ok)
        unit.why = estimate.detail;
      else
      {
        unit.ok = true;
        unit.slimmable = estimate.ok && estimate.slimmable;
        unit.full_bytes = estimate.ok ? estimate.full_bytes : 0;
        unit.slim_bytes = estimate.ok ? estimate.slim_bytes : 0;
        unit.why = estimate.ok ? estimate.detail : "family read, working set not";
      }
    }
  }

  if (statable)
  {
    Entry entry;
    entry.size = size;
    entry.modified = modified;
    entry.unit = unit;
    entries_[path] = std::move(entry);
  }
  return unit;
}

CostOutcome apply_cost_table(graph::Document& resolved, const CostTable& table, UnitCache& units, double rate,
                             int max_quantum, int warm_documents, bool lite_first)
{
  CostOutcome outcome;
  outcome.has_table = table.ok;
  outcome.table = table.path;
  outcome.headroom = table.headroom;
  // D3's coexistence charge (2026-10-09): the rows were measured on a quiet core, and a rig does not
  // get one while a warm document renders beside it. The charge multiplies the rig's cost, and every
  // decision below -- what trimming has to reach, and whether the rig is admitted at all -- is made
  // against the charged figure rather than the raw one.
  outcome.warm_documents = warm_documents;
  outcome.coexistence = table.coexistence_factor(warm_documents);
  const double budget = outcome.coexistence > 0.0 ? table.headroom / outcome.coexistence : table.headroom;

  for (std::size_t c = 0; c < resolved.chains.size(); ++c)
  {
    graph::Chain& chain = resolved.chains[c];
    for (std::size_t n = 0; n < chain.graph.nodes.size(); ++n)
    {
      graph::NodeSpec& node = chain.graph.nodes[n];
      CostedNode costed;
      costed.chain = chain.id;
      costed.node = node.id;
      costed.element = node.element.empty() ? node.id : node.element;
      costed.chain_index = c;
      costed.node_index = n;

      if (node.type == "nc.nam")
      {
        // Decided here and nowhere else: whatever a previous publish chose, this one chooses again.
        node.slim = false;
        const std::string path = string_param(node, "model");
        costed.reference = path;
        const UnitCache::Unit unit = units.get(path, false, rate, max_quantum);
        const CostTable::Charge full = table.charge_nam(unit.family, false);
        const CostTable::Charge lite = table.charge_nam(unit.family, true);
        costed.row = full.row;
        costed.measured = full.measured;
        costed.cost = costed.cost_full = full.cost;
        costed.slimmable = unit.ok && unit.slimmable && lite.cost < full.cost;
        costed.cost_lite = costed.slimmable ? lite.cost : 0.0;
        costed.working_set_bytes = unit.full_bytes;
      }
      else if (node.type == "nc.cab.ir")
      {
        const std::string path = string_param(node, "ir");
        costed.reference = path;
        const UnitCache::Unit unit = units.get(path, true, rate, max_quantum);
        const CostTable::Charge charge = table.charge_ir(unit.ok ? unit.taps : 0);
        costed.row = charge.row;
        costed.measured = charge.measured;
        costed.cost = costed.cost_full = charge.cost;
      }
      else
      {
        // Every other node type, including a bypassed one: a bypassed node still runs, which is what
        // keeps its state warm (NodeSpec::bypass).
        const CostTable::Charge charge = table.charge_node(node.type);
        costed.row = charge.row;
        costed.measured = charge.measured;
        costed.cost = costed.cost_full = charge.cost;
      }

      outcome.cost += costed.cost;
      outcome.full_cost += costed.cost_full;
      outcome.nodes.push_back(std::move(costed));
    }
  }

  outcome.lite_first = lite_first;
  // D6: Full -> Lite, applied only as far as it needs to be, and the dearest saving first -- "on as
  // few NAM nodes as it takes, largest first". What a save is worth is the difference between the
  // node's own size and its Lite one, both from the table, so a model whose Lite row is not measured
  // saves nothing and is not trimmed for show. The target is the charge-adjusted budget: with a warm
  // rig beside it a document is trimmed as far as the coercion charge makes necessary, which is how
  // the pedal keeps the deadline instead of promising it.
  // `lite_first` (P8) is the same loop with nothing left to reach: every slimmable model goes to
  // Lite, dearest first, so the document can be compiled and published now and the Full published
  // over it later. A model with no Lite row still saves nothing and is not trimmed for show.
  while (outcome.cost > (lite_first ? -1.0 : budget) + kEpsilon)
  {
    CostedNode* best = nullptr;
    double best_saving = 0.0;
    for (CostedNode& candidate : outcome.nodes)
    {
      if (!candidate.slimmable || candidate.slimmed)
        continue;
      const double saving = candidate.cost_full - candidate.cost_lite;
      // Dearest first; a tie goes to the later-declared node, which is what the model budget did and
      // leaves the first amp of a chain as written.
      if (best == nullptr || saving >= best_saving)
      {
        best = &candidate;
        best_saving = saving;
      }
    }
    if (best == nullptr || best_saving <= 0.0)
      break;
    best->slimmed = true;
    best->cost = best->cost_lite;
    const std::string suffix = ".full";
    if (best->row.size() > suffix.size() &&
        best->row.compare(best->row.size() - suffix.size(), suffix.size(), suffix) == 0)
      best->row = best->row.substr(0, best->row.size() - suffix.size()) + ".lite";
    outcome.cost -= best_saving;
    outcome.trimmed = true;
  }

  // The one thing trimming changes in the document: the resolved copy's `slim`, on the nodes that
  // were chosen -- and on no others, so a client's own document is never rewritten.
  for (const CostedNode& costed : outcome.nodes)
  {
    if (!costed.slimmed)
      continue;
    resolved.chains[costed.chain_index].graph.nodes[costed.node_index].slim = true;
  }

  outcome.effective = outcome.cost * outcome.coexistence;
  // A lite_first document is over only if it is still over with everything trimmed: the caller asked
  // for the fast compile, not for an exemption from the budget.
  outcome.over = outcome.effective > table.headroom + kEpsilon;
  outcome.verdict = outcome.over ? "over" : (outcome.trimmed ? "trimmed" : "fits");
  return outcome;
}

double CostTable::coexistence_factor(int warm_documents) const
{
  if (warm_documents <= 0 || coexistence_factors.empty())
    return 1.0;
  // The largest measured factor at or below this many warm documents. Holding more than anyone
  // measured is charged what the worst measured case costs, never less.
  double factor = 1.0;
  for (const auto& entry : coexistence_factors)
  {
    if (entry.first <= warm_documents && entry.second > factor)
      factor = entry.second;
  }
  return factor < 1.0 ? 1.0 : factor;
}

std::string format_cost(double cost)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%.3f", cost);
  return text;
}

std::string describe_costs(const CostOutcome& outcome, bool with_chain)
{
  std::string out;
  for (const CostedNode& node : outcome.nodes)
  {
    if (!out.empty())
      out += ", ";
    if (with_chain)
      out += node.chain + "/";
    out += "'" + node.node + "' " + format_cost(node.cost);
    if (!node.measured)
      out += " (unmeasured)";
    if (node.slimmed)
      out += " (Lite)";
  }
  return out;
}

} // namespace daemon
} // namespace namchain
