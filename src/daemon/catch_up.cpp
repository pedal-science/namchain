#include "namchain/daemon/catch_up.h"

#include <algorithm>
#include <cmath>

#include "namchain/graph/macro.h"
#include "namchain/lookback.h"

namespace namchain
{
namespace daemon
{
namespace
{

/// A node's string parameter, or an empty string when it has none. The same spelling the daemon's
/// own cost walk uses: a NAM node names `model`, a cab names `ir`.
std::string string_param(const graph::NodeSpec& node, const char* name)
{
  const auto found = node.strings.find(name);
  return found == node.strings.end() ? std::string() : found->second;
}

/// The engine frames that correspond to \p samples of the model's own domain: the core resamples the
/// engine's input to the model's rate, so a model at 44.1 kHz has heard `frames * 44100 / 48000` of
/// the frames the engine recorded. Rounded up, and never below the model's own count when the file
/// names no rate (which is the case the engine assumed before this existed).
std::size_t engine_frames(const LookbackResult& lookback, double engine_rate)
{
  const double rate = lookback.sample_rate > 0.0 ? lookback.sample_rate : engine_rate;
  const double scaled = engine_rate > 0.0 && rate > 0.0
                          ? static_cast<double>(lookback.samples) * engine_rate / rate
                          : static_cast<double>(lookback.samples);
  const double rounded = std::ceil(scaled - 1e-9);
  return rounded <= 0.0 ? 0 : static_cast<std::size_t>(rounded);
}

/// What one node's memory is, for the catch-up.
struct NodeMemory
{
  std::size_t frames = 0;
  bool exact = true;   ///< a replay restores this node's state exactly
  /// The node's memory *cannot* be replayed at all (an LSTM's, an architecture this cannot size):
  /// the whole catch-up is refused rather than run and disbelieved.
  bool refused = false;
  std::string detail;
};

/// One node's memory: what a replay has to feed for this node's state to be the state a running plan
/// would have.
///
/// The default for a type this does not know is "cannot promise": a node the engine has not taught
/// this walk about is exactly the case where a silent approximation would be wrong, and D8's rule --
/// an unmeasured unit is expensive, never free -- has the same shape.
NodeMemory node_memory(const graph::NodeSpec& node, double engine_rate, int max_quantum, UnitCache* units)
{
  NodeMemory memory;
  const auto withhold = [&](bool refused, const std::string& why) {
    memory.exact = false;
    memory.refused = refused;
    memory.detail = "node '" + node.id + "' (" + node.type + "): " + why;
  };

  if (node.type == "nc.nam")
  {
    const std::string model = string_param(node, "model");
    const LookbackResult lookback = compute_lookback_from_file(model);
    if (!lookback.ok())
    {
      // `Unbounded` is an LSTM: its memory cannot be replayed exactly at any length, and the engine
      // says so rather than approximating. The other failures are the same answer with a different
      // reason -- nothing here can size the state, so nothing promises it.
      withhold(true, "'" + model + "': " + std::string(to_string(lookback.status)) + " -- " + lookback.detail);
      return memory;
    }
    memory.frames = engine_frames(lookback, engine_rate);
    return memory;
  }

  if (node.type == "nc.cab.ir")
  {
    const std::string ir = string_param(node, "ir");
    // A `.nam` cab carries its own receptive field, and that is the number the lookback calculator
    // reports for a `Linear`. A `.wav` cab has none, and the taps that actually reach the convolver
    // -- after the resample and the truncation `nc.cab.ir` does -- come from the unit cache, which is
    // the same number the cost table charges by.
    const LookbackResult lookback = compute_lookback_from_file(ir);
    if (lookback.ok())
    {
      memory.frames = engine_frames(lookback, engine_rate);
      return memory;
    }
    if (units != nullptr)
    {
      const UnitCache::Unit unit = units->get(ir, /*is_ir=*/true, engine_rate, max_quantum);
      if (unit.ok && unit.taps > 0)
      {
        memory.frames = unit.taps;
        return memory;
      }
      withhold(false, "'" + ir + "': " + (unit.why.empty() ? std::string("no tap count") : unit.why));
      return memory;
    }
    withhold(true, "'" + ir + "': " + std::string(to_string(lookback.status)) + " -- " + lookback.detail);
    return memory;
  }

  // Nodes whose state is a function of the parameters rather than of the input: replaying input into
  // them changes nothing, so they add nothing. `nc.input` and `nc.unit_delay` are here on purpose --
  // their state is one *block* of the plan's own buffer, and a replay that feeds whole blocks in order
  // leaves that buffer holding the previous block, which is exactly what a running plan holds.
  if (node.type == "nc.input" || node.type == "nc.unit_delay" || node.type == "nc.output" ||
      node.type == "nc.gain" || node.type == "nc.split" || node.type == "nc.mix" ||
      node.type == "nc.bypass" || node.type == "nc.xfade")
    return memory;

  // A biquad's state is an IIR tail, and a delay's line depends on when its time last moved: a replay
  // of the input alone restores neither *exactly*, so the models may still be replayed but the promise
  // is withheld and the node is named.
  withhold(false, "its state is not restored by replaying input alone");
  return memory;
}

} // namespace

CatchUpPlan catch_up_plan(const graph::Document& resolved, double engine_rate, int max_quantum, UnitCache* units)
{
  CatchUpPlan plan;
  std::size_t any_memory = 0;
  std::string withheld_detail;
  bool refused = false;
  bool too_long = false;

  for (const graph::Chain& chain : resolved.chains)
  {
    CatchUpChain result;
    result.id = chain.id;

    // Macros are lowered first, because that is where a model reference lives: the resolver rewrites
    // the parameter on the macro, and the compiler reads it on the primitive the lowering mints.
    graph::Graph lowered;
    std::string why;
    if (!graph::lower_graph(chain.graph, lowered, why))
    {
      result.exact = false;
      result.detail = "chain '" + chain.id + "': " + why;
      plan.chains.push_back(std::move(result));
      continue;
    }

    for (const graph::NodeSpec& node : lowered.nodes)
    {
      const NodeMemory memory = node_memory(node, engine_rate, max_quantum, units);
      if (memory.frames > 0)
        result.frames += memory.frames;
      if (!memory.exact)
      {
        result.exact = false;
        // The first node that cannot be promised is the one named, and a node that refuses the replay
        // outright outranks one the replay merely does not cover.
        if (result.detail.empty() || memory.refused)
          result.detail = memory.detail;
        refused = refused || memory.refused;
      }
    }

    if (result.frames > kMaxCatchUpFrames)
      too_long = true;
    if (!result.exact && withheld_detail.empty())
      withheld_detail = result.detail;
    any_memory += result.frames;
    plan.window_frames = std::max(plan.window_frames, result.frames);
    plan.chains.push_back(std::move(result));
  }

  if (resolved.chains.empty())
  {
    plan.state = "empty";
    plan.detail = "the document has no chains";
    return plan;
  }
  if (too_long)
  {
    plan.state = "too_long";
    plan.detail = "a model's memory is longer than the " + std::to_string(kMaxCatchUpFrames) +
                  " frames a catch-up replays";
    plan.window_frames = 0;
    return plan;
  }
  if (refused)
  {
    // A model whose memory cannot be replayed at all: no replay is done, and the reply says why
    // rather than letting a cold Full arrive unexplained (P8's rule for the `Unbounded` case).
    plan.state = "unbounded";
    plan.detail = withheld_detail;
    plan.window_frames = 0;
    return plan;
  }
  if (!withheld_detail.empty())
  {
    // The models can be replayed; something else in the chain cannot be restored by it. The replay
    // still runs -- the models' state is then exact -- and the promise is withheld, with the node.
    plan.state = "inexact";
    plan.detail = withheld_detail;
    plan.replayable = any_memory > 0;
    return plan;
  }
  if (any_memory == 0)
  {
    plan.state = "empty";
    plan.detail = "nothing in this document has memory to replay";
    return plan;
  }

  plan.state = "exact";
  plan.replayable = true;
  return plan;
}

ReplayResult replay_window(graph::RenderPlan& plan, const InputWindow& window, unsigned channel,
                           std::size_t frames, float* scratch)
{
  ReplayResult result;
  if (frames == 0 || !window.configured() || scratch == nullptr)
  {
    result.covered = frames == 0;
    return result;
  }

  const std::size_t block_frames = static_cast<std::size_t>(window.block_frames());
  if (block_frames == 0 || block_frames > static_cast<std::size_t>(plan.max_quantum()))
  {
    // A plan may not be handed a block longer than the quantum it was compiled for. Saying so is the
    // only safe answer: a short block would restore a block-delay node's buffer to the wrong length.
    return result;
  }

  const std::size_t want = (frames + block_frames - 1) / block_frames;
  // **All of it or none of it.** A window that does not hold the whole receptive field contiguously
  // cannot make the state exact, and feeding the part it does hold would leave a plan whose state is
  // *nearly* right -- the one outcome this whole mechanism exists to avoid, and one that could not be
  // corrected afterwards (the missing samples are gone). The caller waits for a window that covers.
  if (want == 0 || !window.covers(frames))
    return result;

  const std::size_t first = window.held() - want;
  for (std::size_t i = first; i < window.held(); ++i)
  {
    const float* in = window.block(i, channel);
    if (in == nullptr)
    {
      result.blocks = 0;
      result.frames = 0;
      result.last_seq = 0;
      return result;
    }
    plan.execute(in, scratch, static_cast<int>(block_frames));
    ++result.blocks;
    result.frames += block_frames;
    result.last_seq = window.seq_of(i);
  }
  result.covered = true;
  return result;
}

} // namespace daemon
} // namespace namchain
