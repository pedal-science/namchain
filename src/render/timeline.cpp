#include "namchain/render/timeline.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <utility>

#include <json.hpp>

#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"
#include "namchain/wav.h"

namespace namchain
{
namespace render
{
namespace
{

const graph::NodeSpec* find_node(const graph::Graph& graph, const std::string& id)
{
  for (const graph::NodeSpec& spec : graph.nodes)
    if (spec.id == id)
      return &spec;
  return nullptr;
}

/// \return true when \p name is a parameter the node type can ramp on the render path.
bool settable(const graph::NodeSpec& spec, const std::string& name)
{
  const NodeType* type = find_node_type(spec.type);
  if (type == nullptr || type->set_param == nullptr)
    return false;
  for (int i = 0; i < type->param_count; ++i)
    if (name == type->params[i].id)
      return type->params[i].rampable;
  return false;
}

bool validate_against_graph(const graph::Graph& graph, const EditOp& op, std::string& error)
{
  if (op.kind != EditKind::SetParam)
  {
    error = "only the set_param op class is implemented";
    return false;
  }
  const graph::NodeSpec* spec = find_node(graph, op.node);
  if (spec == nullptr)
  {
    error = "unknown node '" + op.node + "'";
    return false;
  }
  if (!settable(*spec, op.param))
  {
    error = "node '" + op.node + "' (" + spec->type + ") has no settable param '" + op.param + "'";
    return false;
  }
  if (!std::isfinite(op.value))
  {
    error = "value for " + op.node + "." + op.param + " is not finite";
    return false;
  }
  return true;
}

/// How the driver names an op in a report. One kind so far, so it is a constant rather than a switch
/// that would have to grow a default nobody exercises.
const char* edit_op_name()
{
  return "set_param";
}

struct ResolvedOp
{
  size_t node_index = 0;
  uint32_t param_id = 0;
  float value = 0.0f;
  uint64_t at_sample = 0;
};

} // namespace

unsigned settable_param_count(const graph::Graph& graph)
{
  unsigned total = 0;
  for (const graph::NodeSpec& spec : graph.nodes)
  {
    const NodeType* type = find_node_type(spec.type);
    if (type == nullptr || type->set_param == nullptr)
      continue;
    for (int i = 0; i < type->param_count; ++i)
      total += type->params[i].rampable ? 1u : 0u;
  }
  return total;
}

bool parse_timeline_json(const std::string& text, const graph::Graph& graph, EditTimeline& timeline,
                         std::string& error)
{
  nlohmann::json doc;
  try
  {
    doc = nlohmann::json::parse(text);
  }
  catch (const std::exception& e)
  {
    error = std::string("not valid JSON: ") + e.what();
    return false;
  }

  // Accept either a bare array of edits or `{"edits": [...]}`, because both read naturally and
  // rejecting one of them would be a parser preference rather than a design decision.
  const nlohmann::json* edits = nullptr;
  if (doc.is_array())
    edits = &doc;
  else if (doc.is_object() && doc.contains("edits") && doc["edits"].is_array())
    edits = &doc["edits"];
  else
  {
    error = "a timeline is an array of edits, or an object with an 'edits' array";
    return false;
  }

  EditTimeline parsed;
  parsed.ops.reserve(edits->size());
  uint64_t previous = 0;
  for (std::size_t i = 0; i < edits->size(); ++i)
  {
    const std::string where = "edit " + std::to_string(i);
    const nlohmann::json& object = (*edits)[i];
    if (!object.is_object())
    {
      error = where + " must be an object";
      return false;
    }

    EditOp op;
    if (object.contains("op"))
    {
      if (!object["op"].is_string())
      {
        error = where + ": 'op' must be a string";
        return false;
      }
      const std::string name = object["op"].get<std::string>();
      if (name != edit_op_name())
      {
        error = where + ": op '" + name + "' is not implemented (only '" + edit_op_name() + "')";
        return false;
      }
    }
    if (!object.contains("at_sample") || !object["at_sample"].is_number_unsigned())
    {
      error = where + ": 'at_sample' must be a non-negative integer";
      return false;
    }
    op.at_sample = object["at_sample"].get<uint64_t>();
    if (!object.contains("node") || !object["node"].is_string() ||
        !object.contains("param") || !object["param"].is_string() ||
        !object.contains("value") || !object["value"].is_number())
    {
      error = where + ": needs string 'node' and 'param' and a numeric 'value'";
      return false;
    }
    op.node = object["node"].get<std::string>();
    op.param = object["param"].get<std::string>();
    op.value = object["value"].get<double>();
    op.kind = EditKind::SetParam;

    std::string reason;
    if (!validate_against_graph(graph, op, reason))
    {
      error = where + ": " + reason;
      return false;
    }
    // A timeline is a sequence. Sorting silently would hide a caller's bug, so disorder is an error
    // (the detector makes the same demand of its edit list).
    if (!parsed.ops.empty() && op.at_sample < previous)
    {
      error = where + ": at_sample " + std::to_string(op.at_sample) + " is before the previous edit (" +
              std::to_string(previous) + "); a timeline must be in order";
      return false;
    }
    previous = op.at_sample;
    parsed.ops.push_back(std::move(op));
  }

  timeline = std::move(parsed);
  return true;
}

bool load_timeline_json(const std::string& path, const graph::Graph& graph, EditTimeline& timeline,
                        std::string& error)
{
  std::ifstream file(path);
  if (!file)
  {
    error = "cannot open timeline '" + path + "'";
    return false;
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  if (!parse_timeline_json(contents.str(), graph, timeline, error))
  {
    error = "'" + path + "': " + error;
    return false;
  }
  return true;
}

EditTimeline random_timeline(const graph::Graph& graph, unsigned edits, uint64_t frames, uint64_t seed)
{
  EditTimeline timeline;
  if (edits == 0 || frames == 0)
    return timeline;

  struct Candidate
  {
    std::string node;
    std::string param;
    double low = 0.25;
    double high = 1.5;
  };
  std::vector<Candidate> candidates;
  for (const graph::NodeSpec& spec : graph.nodes)
  {
    const NodeType* type = find_node_type(spec.type);
    if (type == nullptr || type->set_param == nullptr)
      continue;
    for (int p = 0; p < type->param_count; ++p)
    {
      // Only what the node can ramp, and only what a value in the generator's range means: a
      // parameter with declared bounds gets values inside them, so a generated timeline is legal
      // against the same schema a client validates against.
      const ParamDesc& param = type->params[p];
      if (!param.rampable || param.kind != ParamKind::Number || param.hidden)
        continue;
      Candidate candidate;
      candidate.node = spec.id;
      candidate.param = param.id;
      // Inside the declared bounds where there are any, so a generated timeline is legal against the
      // same schema the compiler validates against rather than legal only by luck.
      if (param.has_min)
        candidate.low = param.min_inclusive ? param.min : param.min + (param.has_max ? (param.max - param.min) * 0.1 : 1.0);
      if (param.has_max)
        candidate.high = param.max_inclusive ? param.max : param.max - (param.has_max ? (param.max - (param.has_min ? param.min : 0.0)) * 0.1 : 1.0);
      if (candidate.high <= candidate.low)
        candidate.high = candidate.low + 1.0;
      candidates.push_back(std::move(candidate));
    }
  }
  if (candidates.empty())
    return timeline;

  std::mt19937_64 rng(seed);

  // Even slots with jitter, so consecutive edits are at least half a slot apart: the oracle model
  // needs the previous ramp to have settled before the next edit to the same parameter.
  const uint64_t slot = std::max<uint64_t>(1, frames / (static_cast<uint64_t>(edits) + 1));
  const uint64_t jitter = std::max<uint64_t>(1, slot / 4);
  uint64_t cursor = slot;
  for (unsigned i = 0; i < edits && cursor < frames; ++i)
  {
    EditOp op;
    op.kind = EditKind::SetParam;
    op.at_sample = cursor + (rng() % jitter);
    if (op.at_sample >= frames)
      op.at_sample = frames - 1;
    const Candidate& chosen = candidates[static_cast<std::size_t>(rng() % candidates.size())];
    op.node = chosen.node;
    op.param = chosen.param;
    op.value = std::uniform_real_distribution<double>(chosen.low, chosen.high)(rng);
    timeline.ops.push_back(std::move(op));
    cursor += slot;
  }
  std::sort(timeline.ops.begin(), timeline.ops.end(),
            [](const EditOp& a, const EditOp& b) { return a.at_sample < b.at_sample; });
  return timeline;
}

TimelineRenderResult render_timeline_to_wav(const graph::Graph& graph, const EditTimeline& timeline,
                                            const std::string& input_wav, const std::string& output_wav,
                                            const TimelineRenderOptions& options)
{
  TimelineRenderResult result;
  if (options.quantum < 1 || options.quantum > 4096)
  {
    result.error = "quantum must be in [1, 4096]";
    return result;
  }

  // The timeline is re-validated here rather than trusted: this is the entry point a caller can reach
  // without going through parse_timeline_json (a generated timeline), and applying an op that names a
  // missing node would otherwise be a silently dropped edit.
  for (std::size_t i = 0; i < timeline.ops.size(); ++i)
  {
    std::string reason;
    if (!validate_against_graph(graph, timeline.ops[i], reason))
    {
      result.error = "edit " + std::to_string(i) + ": " + reason;
      return result;
    }
  }

  std::vector<float> interleaved;
  unsigned channels = 0;
  double rate = 0.0;
  if (!load_wav(input_wav, interleaved, channels, rate, result.error))
    return result;
  if (channels == 0 || !(rate > 0.0))
  {
    result.error = "'" + input_wav + "' has no channels or no sample rate";
    return result;
  }
  const std::size_t frames = interleaved.size() / channels;
  if (frames == 0)
  {
    result.error = "'" + input_wav + "' has no frames";
    return result;
  }
  std::vector<float> input(frames);
  for (std::size_t i = 0; i < frames; ++i)
    input[i] = interleaved[i * channels]; // the render path is mono; channel 0 is the signal

  CompileOptions compile_options;
  compile_options.max_quantum = options.quantum;
  compile_options.sample_rate = rate;

  rt::Arena arena(1u << 20);
  std::unique_ptr<graph::RenderPlan> plan =
    graph::RenderPlan::compile(graph, compile_options, arena, result.error);
  if (plan == nullptr)
    return result;
  plan->prewarm();

  std::vector<ResolvedOp> ops;
  ops.reserve(timeline.ops.size());
  for (std::size_t i = 0; i < timeline.ops.size(); ++i)
  {
    const EditOp& op = timeline.ops[i];
    ResolvedOp resolved;
    resolved.at_sample = op.at_sample;
    resolved.value = static_cast<float>(op.value);
    if (!plan->resolve_param(op.node, op.param, resolved.node_index, resolved.param_id))
    {
      result.error = "edit " + std::to_string(i) + ": node '" + op.node + "' has no settable param '" +
                     op.param + "'";
      return result;
    }
    ops.push_back(resolved);
  }

  std::vector<float> output(frames, 0.0f);
  std::size_t pos = 0;
  std::size_t next = 0;
  while (pos < frames)
  {
    // Sample-accurate: render up to the next edit, not to the next block boundary, so an edit lands
    // at exactly the sample the timeline names (plan.md §5.1 item 4).
    uint64_t stop = frames;
    if (next < ops.size() && ops[next].at_sample < frames)
      stop = std::min<uint64_t>(frames, ops[next].at_sample);
    while (pos < stop)
    {
      const int chunk = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(options.quantum), stop - pos));
      plan->execute(input.data() + pos, output.data() + pos, chunk);
      pos += static_cast<std::size_t>(chunk);
    }
    while (next < ops.size() && ops[next].at_sample == pos)
    {
      plan->set_param(ops[next].node_index, ops[next].param_id, ops[next].value);
      ++next;
    }
  }

  result.frames = frames;
  result.edits = static_cast<unsigned>(ops.size());
  if (!save_wav_float32(output_wav, output.data(), frames, 1, rate, result.error))
    return result;
  result.ok = true;

  if (!options.glitch_checks || ops.empty())
    return result;

  // --- judge each edit against before/after oracles ------------------------
  const double frames_per_ms = rate / 1000.0;
  const std::size_t guard = static_cast<std::size_t>(std::llround(options.glitch_options.guard_ms * frames_per_ms));
  // Conservative: the window must cover the whole ramp, so a rig with a longer ramp raises this.
  const std::size_t ramp = static_cast<std::size_t>(std::llround(20.0 * frames_per_ms));
  const std::size_t margin = ramp + 2 * guard;
  const std::size_t span_len = 2 * margin + ramp + guard;

  // Render one oracle: the chain driven to the parameter snapshot that applies at edit `e`, with
  // every earlier op applied first. Rendering from frame 0 is what makes its state at the span the
  // state the live chain had; the set_param ramp it starts with is settled long before the span.
  const auto oracle_slice = [&](std::size_t e, bool include_edit, std::size_t span_start,
                                std::size_t span_end, std::vector<float>& slice, std::string& error) {
    rt::Arena oracle_arena(1u << 20);
    std::unique_ptr<graph::RenderPlan> oracle =
      graph::RenderPlan::compile(graph, compile_options, oracle_arena, error);
    if (oracle == nullptr)
      return false;
    oracle->prewarm();
    const std::size_t last = include_edit ? e + 1 : e;
    for (std::size_t j = 0; j < last; ++j)
    {
      std::size_t index = 0;
      uint32_t id = 0;
      if (!oracle->resolve_param(timeline.ops[j].node, timeline.ops[j].param, index, id))
      {
        error = "oracle: node '" + timeline.ops[j].node + "' has no settable param '" +
                timeline.ops[j].param + "'";
        return false;
      }
      oracle->set_param(index, id, static_cast<float>(timeline.ops[j].value));
    }
    std::vector<float> prefix(span_end, 0.0f);
    std::size_t at = 0;
    while (at < span_end)
    {
      const int chunk =
        static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(options.quantum), span_end - at));
      oracle->execute(input.data() + at, prefix.data() + at, chunk);
      at += static_cast<std::size_t>(chunk);
    }
    slice.assign(prefix.begin() + static_cast<std::ptrdiff_t>(span_start),
                 prefix.begin() + static_cast<std::ptrdiff_t>(span_end));
    return true;
  };

  for (std::size_t e = 0; e < ops.size(); ++e)
  {
    const std::size_t at = static_cast<std::size_t>(ops[e].at_sample);
    // Enough settled signal before the span that the oracle's own build ramp is over, and room for
    // the detector's guard after the fade.
    if (at < margin + ramp + guard)
      continue;
    const std::size_t span_start = at - margin;
    const std::size_t span_end = span_start + span_len;
    if (span_end > frames)
      continue;

    std::vector<float> before;
    std::vector<float> after;
    std::string error;
    if (!oracle_slice(e, false, span_start, span_end, before, error) ||
        !oracle_slice(e, true, span_start, span_end, after, error))
    {
      result.error = "oracle render for edit " + std::to_string(e) + ": " + error;
      result.glitch_ok = false;
      return result;
    }

    std::vector<float> y(output.begin() + static_cast<std::ptrdiff_t>(span_start),
                         output.begin() + static_cast<std::ptrdiff_t>(span_end));
    const std::vector<glitch::Edit> edits = {{margin, margin + ramp}};
    glitch::Options glitch_options = options.glitch_options;
    glitch_options.sample_rate = rate;
    std::string analysis_error;
    const glitch::Report report =
      glitch::analyse(y.data(), before.data(), after.data(), y.size(), edits, glitch_options, analysis_error);
    if (!analysis_error.empty())
    {
      result.error = "edit " + std::to_string(e) + ": " + analysis_error;
      result.glitch_ok = false;
      return result;
    }
    ++result.judged;
    if (!report.ok)
    {
      result.glitch_ok = false;
      for (glitch::Finding finding : report.findings)
      {
        finding.frame += span_start;
        result.findings.push_back(std::move(finding));
      }
    }
  }

  return result;
}

} // namespace render
} // namespace namchain
