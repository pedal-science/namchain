#include "namchain/graph/plan.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace namchain
{
namespace render
{
namespace
{

double number_param(const graph::NodeSpec& spec, const std::string& name, double fallback)
{
  const auto it = spec.params.find(name);
  return it == spec.params.end() ? fallback : it->second;
}

std::string string_param(const graph::NodeSpec& spec, const std::string& name)
{
  const auto it = spec.strings.find(name);
  return it == spec.strings.end() ? std::string{} : it->second;
}

// --- nc.input / nc.output --------------------------------------------------
// The plan seeds the input node's buffer from the graph input and reads the output node's input
// buffer back out, so both nodes are markers with nothing to do per block. Keeping them as real
// nodes means a chain always has an unambiguous entry and exit point (§5.1).

void* create_marker(const graph::NodeSpec&, int, std::string&)
{
  return nullptr;
}

void process_marker(void*, const float* const*, float* const*, int) noexcept {}

// --- nc.gain ---------------------------------------------------------------

struct GainState
{
  float gain = 1.0f;
};

void* create_gain(const graph::NodeSpec& spec, int, std::string& error)
{
  auto state = std::make_unique<GainState>();
  if (spec.params.count("gain_db") != 0)
    state->gain = static_cast<float>(std::pow(10.0, number_param(spec, "gain_db", 0.0) / 20.0));
  else
    state->gain = static_cast<float>(number_param(spec, "gain", 1.0));

  if (!std::isfinite(state->gain))
  {
    error = "nc.gain: gain is not finite";
    return nullptr;
  }
  return state.release();
}

void destroy_gain(void* state)
{
  delete static_cast<GainState*>(state);
}

void process_gain(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  const GainState* state = static_cast<const GainState*>(raw);
  const float gain = state->gain;
  for (int i = 0; i < frames; ++i)
    out[0][i] = in[0][i] * gain;
}

// --- nc.split / nc.mix -----------------------------------------------------

void process_split(void*, const float* const* in, float* const* out, int frames) noexcept
{
  for (int port = 0; out[port] != nullptr; ++port)
    std::copy(in[0], in[0] + frames, out[port]);
}

void process_mix(void*, const float* const* in, float* const* out, int frames) noexcept
{
  for (int i = 0; i < frames; ++i)
    out[0][i] = in[0][i] + in[1][i];
}

// --- nc.nam ----------------------------------------------------------------

/// Wraps a vendored-core model. The conversion between the engine's float buffers and the core's
/// NAM_SAMPLE is done in preallocated buffers, so `process` allocates nothing.
struct NamState
{
  std::unique_ptr<nam::DSP> model;
  std::vector<NAM_SAMPLE> input;
  std::vector<NAM_SAMPLE> output;
};

void* create_nam(const graph::NodeSpec& spec, int max_quantum, std::string& error)
{
  const std::string path = string_param(spec, "model");
  if (path.empty())
  {
    error = "nc.nam: 'model' must name a .nam file";
    return nullptr;
  }

  auto state = std::make_unique<NamState>();
  try
  {
    state->model = nam::get_dsp(std::filesystem::path(path));
  }
  catch (const std::exception& e)
  {
    error = std::string("nc.nam: cannot load '") + path + "': " + e.what();
    return nullptr;
  }
  if (!state->model)
  {
    error = "nc.nam: cannot load '" + path + "'";
    return nullptr;
  }
  if (state->model->NumInputChannels() != 1 || state->model->NumOutputChannels() != 1)
  {
    error = "nc.nam: only mono models are supported so far";
    return nullptr;
  }

  try
  {
    state->model->Reset(state->model->GetExpectedSampleRate(), max_quantum);
  }
  catch (const std::exception& e)
  {
    error = std::string("nc.nam: Reset failed: ") + e.what();
    return nullptr;
  }

  state->input.assign(static_cast<std::size_t>(max_quantum), static_cast<NAM_SAMPLE>(0));
  state->output.assign(static_cast<std::size_t>(max_quantum), static_cast<NAM_SAMPLE>(0));
  return state.release();
}

void destroy_nam(void* state)
{
  delete static_cast<NamState*>(state);
}

void process_nam(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  NamState* state = static_cast<NamState*>(raw);
  for (int i = 0; i < frames; ++i)
    state->input[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(in[0][i]);

  NAM_SAMPLE* in_ptrs[] = {state->input.data()};
  NAM_SAMPLE* out_ptrs[] = {state->output.data()};
  state->model->process(in_ptrs, out_ptrs, frames);

  for (int i = 0; i < frames; ++i)
    out[0][i] = static_cast<float>(state->output[static_cast<std::size_t>(i)]);
}

const NodeType kInput{"nc.input", 0, 1, create_marker, nullptr, process_marker};
const NodeType kOutput{"nc.output", 1, 0, create_marker, nullptr, process_marker};
const NodeType kGain{"nc.gain", 1, 1, create_gain, destroy_gain, process_gain};
const NodeType kSplit{"nc.split", 1, 2, create_marker, nullptr, process_split};
const NodeType kMix{"nc.mix", 2, 1, create_marker, nullptr, process_mix};
const NodeType kNam{"nc.nam", 1, 1, create_nam, destroy_nam, process_nam};

} // namespace

const NodeType* find_node_type(const std::string& id)
{
  if (id == kInput.id) return &kInput;
  if (id == kOutput.id) return &kOutput;
  if (id == kGain.id) return &kGain;
  if (id == kSplit.id) return &kSplit;
  if (id == kMix.id) return &kMix;
  if (id == kNam.id) return &kNam;
  return nullptr;
}

} // namespace render
} // namespace namchain
