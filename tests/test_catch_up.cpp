// P8's catch-up: the replay, and the claim it exists for.
//
// The measurable claim (pedal.project docs/managing-processing-power-and-optimising-for-fast-
// switching.md, "P8"; docs/daemon.md, "Lite-first publishes") is that a plan replayed with the input
// the device has just been playing is **the same plan** as one that has been running all along: its
// output for the next block is bit-identical, not merely close. That is what makes it legitimate to
// crossfade a Full in mid-note instead of starting it from silence, and it is the only reason the
// Lite can stand in while the Full is compiled.
//
// The falsifier is in the same case: the same plan without the replay, whose first block after the
// hand-over is *not* the running plan's block. Without that, a test that compared the replay against
// nothing would pass just as well.
#include <doctest.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "namchain/daemon/catch_up.h"
#include "namchain/daemon/input_history.h"
#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/lookback.h"
#include "namchain/rt/arena.h"

namespace
{

using namchain::daemon::CatchUpPlan;
using namchain::daemon::InputWindow;
using namchain::daemon::ReplayResult;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::graph::RenderPlan;

constexpr int kRate = 48000;
constexpr int kQuantum = 64;
constexpr std::size_t kArenaBytes = 1u << 22;

std::string model_path(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

/// in -> nc.nam(model) -> out: the smallest rig with memory worth replaying.
Graph amp_graph(const std::string& model)
{
  Graph graph;
  NodeSpec amp = node("amp", "nc.nam");
  amp.strings["model"] = model;
  graph.nodes = {node("in", "nc.input"), amp, node("out", "nc.output")};
  graph.connections = {{"in", 0, "amp", 0}, {"amp", 0, "out", 0}};
  return graph;
}

/// A deterministic signal with content at several frequencies: a replay's correctness must not depend
/// on the input being simple, and a sine alone can hide a state that is off by a block.
std::vector<float> signal(std::size_t frames)
{
  std::vector<float> samples(frames);
  unsigned state = 0x12345678u;
  for (std::size_t i = 0; i < frames; ++i)
  {
    state = state * 1664525u + 1013904223u;
    const float noise = (static_cast<float>((state >> 8) & 0xffffu) / 32768.0f - 1.0f) * 0.05f;
    const float t = static_cast<float>(i) / static_cast<float>(kRate);
    samples[i] = 0.35f * std::sin(2.0f * 3.14159265358979f * 196.0f * t) +
                 0.2f * std::sin(2.0f * 3.14159265358979f * 440.0f * t) + noise;
  }
  return samples;
}

std::unique_ptr<RenderPlan> compile(const Graph& graph, namchain::rt::Arena& arena)
{
  namchain::render::CompileOptions options;
  options.max_quantum = kQuantum;
  options.sample_rate = kRate;
  options.levels = false;
  std::string error;
  std::unique_ptr<RenderPlan> plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  plan->prewarm();
  return plan;
}

/// Run a plan over one block of \p in, writing into \p out.
void run(RenderPlan& plan, const float* in, float* out)
{
  plan.execute(in, out, kQuantum);
}

} // namespace

TEST_CASE("a replayed plan is bit-identical to one that has been running")
{
  const std::string model = model_path("A2-Full.nam");
  const namchain::LookbackResult lookback = namchain::compute_lookback_from_file(model);
  REQUIRE(lookback.ok());
  const std::size_t need = static_cast<std::size_t>(lookback.samples);

  // 200 blocks of input: far longer than the model's receptive field, so the reference plan's state is
  // exactly what the input determines rather than what its own cold start left behind.
  constexpr std::size_t kBlocks = 200;
  const std::vector<float> x = signal(kBlocks * kQuantum);

  namchain::rt::Arena reference_arena(kArenaBytes);
  namchain::rt::Arena cold_arena(kArenaBytes);
  std::unique_ptr<RenderPlan> reference = compile(amp_graph(model), reference_arena);
  std::unique_ptr<RenderPlan> cold = compile(amp_graph(model), cold_arena);

  std::vector<float> out(kQuantum, 0.0f);
  for (std::size_t b = 0; b < kBlocks; ++b)
    run(*reference, x.data() + b * kQuantum, out.data());

  // The history the device thread would have recorded, block for block. The window is filled with the
  // whole signal, and holds the newest `lookback` frames of it.
  InputWindow window;
  window.configure(need, /*channels=*/1, kQuantum);
  for (std::size_t b = 0; b < kBlocks; ++b)
    window.append(x.data() + b * kQuantum, b + 1);
  REQUIRE(window.covers(need));

  std::vector<float> scratch(kQuantum, 0.0f);
  const ReplayResult replay = namchain::daemon::replay_window(*cold, window, /*channel=*/0, need, scratch.data());
  CHECK(replay.covered);
  CHECK(replay.blocks * kQuantum >= need);
  CHECK(replay.last_seq == kBlocks);

  // The claim: the next blocks are the same samples, not the same to within a tolerance.
  std::vector<float> expected(kQuantum, 0.0f);
  std::vector<float> replayed(kQuantum, 0.0f);
  for (std::size_t b = 0; b < 8; ++b)
  {
    const float* in = x.data() + (kBlocks + b) * kQuantum;
    run(*reference, in, expected.data());
    run(*cold, in, replayed.data());
    CHECK_MESSAGE(std::memcmp(expected.data(), replayed.data(), kQuantum * sizeof(float)) == 0,
                  "block " << b << " after the hand-over differs from the running plan");
  }

  // The falsifier: a plan that was *not* replayed is not the running plan. If this ever passed, the
  // comparison above would be measuring nothing.
  namchain::rt::Arena fresh_arena(kArenaBytes);
  std::unique_ptr<RenderPlan> fresh = compile(amp_graph(model), fresh_arena);
  std::vector<float> unreplayed(kQuantum, 0.0f);
  run(*fresh, x.data() + kBlocks * kQuantum, unreplayed.data());
  CHECK(std::memcmp(expected.data(), unreplayed.data(), kQuantum * sizeof(float)) != 0);
}

TEST_CASE("the replay runs whole blocks, so a block-delay node's state comes back right")
{
  // in -> unit_delay -> amp -> out. The delay node's state is the plan's *block* buffer, which only a
  // replay that feeds blocks in the device's own alignment can restore: this is the case that makes
  // `replay_window` refuse a window shorter than a block and a plan whose quantum is too small.
  const std::string model = model_path("A2-Lite.nam");
  const namchain::LookbackResult lookback = namchain::compute_lookback_from_file(model);
  REQUIRE(lookback.ok());
  const std::size_t need = static_cast<std::size_t>(lookback.samples) + kQuantum;

  Graph graph;
  NodeSpec amp = node("amp", "nc.nam");
  amp.strings["model"] = model;
  graph.nodes = {node("in", "nc.input"), node("d", "nc.unit_delay"), amp, node("out", "nc.output")};
  graph.connections = {{"in", 0, "d", 0}, {"d", 0, "amp", 0}, {"amp", 0, "out", 0}};

  constexpr std::size_t kBlocks = 200;
  const std::vector<float> x = signal(kBlocks * kQuantum);

  namchain::rt::Arena reference_arena(kArenaBytes);
  namchain::rt::Arena cold_arena(kArenaBytes);
  std::unique_ptr<RenderPlan> reference = compile(graph, reference_arena);
  std::unique_ptr<RenderPlan> cold = compile(graph, cold_arena);

  std::vector<float> out(kQuantum, 0.0f);
  for (std::size_t b = 0; b < kBlocks; ++b)
    run(*reference, x.data() + b * kQuantum, out.data());

  InputWindow window;
  window.configure(need, 1, kQuantum);
  for (std::size_t b = 0; b < kBlocks; ++b)
    window.append(x.data() + b * kQuantum, b + 1);

  std::vector<float> scratch(kQuantum, 0.0f);
  const ReplayResult replay = namchain::daemon::replay_window(*cold, window, 0, need, scratch.data());
  REQUIRE(replay.covered);

  std::vector<float> expected(kQuantum, 0.0f);
  std::vector<float> replayed(kQuantum, 0.0f);
  for (std::size_t b = 0; b < 4; ++b)
  {
    const float* in = x.data() + (kBlocks + b) * kQuantum;
    run(*reference, in, expected.data());
    run(*cold, in, replayed.data());
    CHECK(std::memcmp(expected.data(), replayed.data(), kQuantum * sizeof(float)) == 0);
  }
}

TEST_CASE("a window with a hole in it is refused rather than replayed short")
{
  const std::string model = model_path("A2-Lite.nam");
  const namchain::LookbackResult lookback = namchain::compute_lookback_from_file(model);
  REQUIRE(lookback.ok());
  const std::size_t need = static_cast<std::size_t>(lookback.samples);

  namchain::rt::Arena arena(kArenaBytes);
  std::unique_ptr<RenderPlan> plan = compile(amp_graph(model), arena);

  const std::vector<float> x = signal(4 * kQuantum);
  InputWindow window;
  window.configure(need, 1, kQuantum);
  window.append(x.data(), 1);
  window.append(x.data() + kQuantum, 2);
  // A block the producer dropped: the sequence jumps, so the frames either side of it are not one
  // continuous stretch of input and nothing may be promised from them.
  window.append(x.data() + 2 * kQuantum, 9);
  window.append(x.data() + 3 * kQuantum, 10);
  CHECK_FALSE(window.covers(need));

  std::vector<float> scratch(kQuantum, 0.0f);
  const ReplayResult replay = namchain::daemon::replay_window(*plan, window, 0, need, scratch.data());
  CHECK_FALSE(replay.covered);
  CHECK(replay.blocks == 0);
}

TEST_CASE("what a document's catch-up is comes from lookback, per node")
{
  // A rig with a NAM model whose memory is finite: exact, and the length is the model's.
  namchain::daemon::UnitCache units;
  namchain::graph::Document document;
  namchain::graph::Chain chain;
  chain.id = "main";
  chain.graph = amp_graph(model_path("A2-Full.nam"));
  document.chains.push_back(chain);

  const namchain::LookbackResult lookback = namchain::compute_lookback_from_file(model_path("A2-Full.nam"));
  REQUIRE(lookback.ok());

  CatchUpPlan plan = namchain::daemon::catch_up_plan(document, kRate, kQuantum, &units);
  CHECK(plan.state == "exact");
  CHECK(plan.replayable);
  REQUIRE(plan.chains.size() == 1);
  CHECK(plan.chains[0].frames == static_cast<std::size_t>(lookback.samples) * kRate /
                                    static_cast<std::size_t>(lookback.sample_rate == 0 ? kRate
                                                                                       : lookback.sample_rate));
  CHECK(plan.window_frames == plan.chains[0].frames);

  // A model that says its memory cannot be replayed: the engine refuses to promise, by name.
  namchain::graph::Document lstm;
  namchain::graph::Chain lstm_chain;
  lstm_chain.id = "main";
  lstm_chain.graph = amp_graph(std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/lstm.nam");
  lstm.chains.push_back(lstm_chain);

  const CatchUpPlan refused = namchain::daemon::catch_up_plan(lstm, kRate, kQuantum, &units);
  CHECK(refused.state == "unbounded");
  CHECK_FALSE(refused.replayable);
  CHECK(refused.detail.find("amp") != std::string::npos);
  CHECK(refused.detail.find("unbounded") != std::string::npos);

  // A node whose state a replay cannot restore exactly: the models may still be replayed, and the
  // promise is withheld with the node named.
  namchain::graph::Document delayed;
  namchain::graph::Chain delayed_chain;
  delayed_chain.id = "main";
  delayed_chain.graph.nodes = {node("in", "nc.input"), node("d", "nc.delay"), node("out", "nc.output")};
  delayed_chain.graph.connections = {{"in", 0, "d", 0}, {"d", 0, "out", 0}};
  delayed.chains.push_back(delayed_chain);

  const CatchUpPlan inexact = namchain::daemon::catch_up_plan(delayed, kRate, kQuantum, &units);
  CHECK(inexact.state == "inexact");
  CHECK(inexact.detail.find("nc.delay") != std::string::npos);
}
