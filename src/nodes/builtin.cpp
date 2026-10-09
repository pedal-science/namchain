#include "namchain/graph/plan.h"

#include "namchain/rt/crossfade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#if defined(__GNUG__)
  #include <cxxabi.h>
#endif

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/linear.h"
#include "NAM/slimmable.h"
#include "dsp/Resample.h" // AudioDSPTools, as the vendored core pins it
#include "json.hpp"

#include "namchain/wav.h"

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

bool has_param(const graph::NodeSpec& spec, const std::string& name)
{
  return spec.params.count(name) != 0;
}

std::string string_param(const graph::NodeSpec& spec, const std::string& name)
{
  const auto it = spec.strings.find(name);
  return it == spec.strings.end() ? std::string{} : it->second;
}

// A rejection is reported as "nc.foo: <reason>" so the message says which node to fix. These
// helpers keep that prefix consistent instead of each node inventing its own phrasing.
std::string reject(const std::string& type, const std::string& reason)
{
  return type + ": " + reason;
}

std::string quoted(double value)
{
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  return buffer;
}

/// M_PI is not in the standard, and glibc only exposes it when it feels like it.
constexpr double kPi = 3.14159265358979323846;

// --- nc.input / nc.output --------------------------------------------------
// The plan hands the incoming block to the input node, which copies it into its output buffer at
// its own point in the schedule; the plan reads the output node's input buffer back out. Keeping
// them as real nodes means a chain always has an unambiguous entry and exit point (§5.1), and
// having the input node *write* rather than be pre-filled is what makes a block-delay node work.
// A null `create` in the vtable is what marks a type as stateless.

struct InputState
{
  const float* source = nullptr;
};

void* create_input(const graph::NodeSpec&, const CompileOptions&, std::string&)
{
  return new InputState();
}

void destroy_input(void* state)
{
  delete static_cast<InputState*>(state);
}

void set_block_input(void* raw, const float* source) noexcept
{
  static_cast<InputState*>(raw)->source = source;
}

void process_input(void* raw, const float* const*, float* const* out, int frames) noexcept
{
  const InputState* state = static_cast<const InputState*>(raw);
  if (state->source != nullptr)
    std::copy(state->source, state->source + frames, out[0]);
  else
    std::fill(out[0], out[0] + frames, 0.0f);
}

void process_marker(void*, const float* const*, float* const*, int) noexcept {}

// --- nc.gain ---------------------------------------------------------------
//
// The first `smooth`-class parameter of §5.3: a change never recompiles and never steps. A step
// clicks; a *linear* ramp would be cheap but has a slope discontinuity at each end, and the glitch
// detector measures a second difference, so it scores the corner. The smoothing is therefore a
// smoothstep, whose value and slope both match at each end:
//
//   g(t) = from + (to - from) * t*t*(3 - 2t),   t = ramp_pos / ramp_len
//
// The length is the rig's `ramp_ms` (default 5 ms) converted to samples at build time, because time
// to samples is control-thread work (§6.1). `ramp_ms = 0` is a legal instant change, which the
// timeline's negative control uses to show the detector can fail.

/// Parameter ids are positions in the type's schema array plus one (plan.h, \ref ParamDesc).
constexpr uint32_t kGainParamGainDb = 1;
constexpr uint32_t kGainParamGain = 2;
constexpr uint32_t kBiquadParamMode = 1;
constexpr uint32_t kBiquadParamFreq = 2;
constexpr uint32_t kBiquadParamQ = 3;
constexpr uint32_t kBiquadParamGainDb = 4;
constexpr uint32_t kDelayParamSamples = 1;
constexpr uint32_t kDelayParamMs = 2;
/// Per-port parameter ids are the index into \ref kMixPortParams, plus one, under a port.
constexpr uint32_t kMixPortParamLevelDb = 1;
constexpr uint32_t kMixPortParamMute = 2;
constexpr uint32_t kMixPortParamPhase = 3;

struct GainState
{
  float gain = 1.0f;             ///< the value the render uses right now
  float from = 1.0f;             ///< where the current ramp started
  float to = 1.0f;               ///< where it is heading
  int ramp_pos = 0;              ///< samples into the current ramp
  int ramp_len = 0;              ///< current ramp length; 0 means "settled"
  int smoothing_samples = 240;   ///< the ramp length a set_param starts
};

float gain_value(const graph::NodeSpec& spec)
{
  if (has_param(spec, "gain_db"))
    return static_cast<float>(std::pow(10.0, number_param(spec, "gain_db", 0.0) / 20.0));
  return static_cast<float>(number_param(spec, "gain", 1.0));
}

void* create_gain(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  auto state = std::make_unique<GainState>();
  state->gain = state->from = state->to = gain_value(spec);

  if (!std::isfinite(state->gain))
  {
    error = reject("nc.gain", "gain is not finite");
    return nullptr;
  }

  const double ramp_ms = number_param(spec, "ramp_ms", 5.0);
  if (!std::isfinite(ramp_ms) || ramp_ms < 0.0)
  {
    error = reject("nc.gain", "'ramp_ms' must be a finite number >= 0");
    return nullptr;
  }
  // A ramp shorter than a sample is a step, which is what ramp_ms = 0 asks for; the max(1) keeps a
  // sub-sample request from becoming a division by zero rather than a shorter-than-possible ramp.
  state->smoothing_samples =
    ramp_ms == 0.0 ? 1 : std::max(1, static_cast<int>(std::llround(ramp_ms * options.sample_rate / 1000.0)));
  return state.release();
}

void destroy_gain(void* state)
{
  delete static_cast<GainState*>(state);
}

// The declared parameters, in id order. `gain_db` is id 1 and is the canonical one -- dB is the unit a
// player thinks in; `gain` is the linear spelling, kept because rigs and timelines are written in it,
// and marked hidden so a generic inspector shows one knob rather than two that mean the same thing.
// `ramp_ms` is build-only: it is a smoothing time, so changing it changes how every later ramp
// behaves, which is a rebuild rather than a ramp.
const ParamDesc kGainParams[] = {
  {.id = "gain_db", .name = "Gain", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -60.0,
   .has_max = true, .max = 24.0, .rampable = true},
  {.id = "gain", .name = "Gain (linear)", .default_value = 1.0, .has_min = true, .min = -8.0,
   .has_max = true, .max = 16.0, .rampable = true, .hidden = true},
  {.id = "ramp_ms", .name = "Ramp", .unit = "ms", .default_value = 5.0, .has_min = true, .min = 0.0,
   .has_max = true, .max = 1000.0},
};

bool set_param_gain(void* raw, uint32_t param_id, float value) noexcept
{
  if (!std::isfinite(value))
    return false;
  GainState* state = static_cast<GainState*>(raw);
  switch (param_id)
  {
  case kGainParamGainDb:
    state->to = static_cast<float>(std::pow(10.0, static_cast<double>(value) / 20.0));
    break;
  case kGainParamGain:
    state->to = value;
    break;
  default:
    return false;
  }
  // From the *current* value, so a second change mid-ramp does not jump back to the old endpoint.
  state->from = state->gain;
  state->ramp_pos = 0;
  state->ramp_len = state->smoothing_samples;
  return true;
}

void process_gain(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  GainState* state = static_cast<GainState*>(raw);
  for (int i = 0; i < frames; ++i)
  {
    if (state->ramp_pos < state->ramp_len)
    {
      const float t = static_cast<float>(state->ramp_pos + 1) / static_cast<float>(state->ramp_len);
      state->gain = state->from + (state->to - state->from) * t * t * (3.0f - 2.0f * t);
      ++state->ramp_pos;
      if (state->ramp_pos >= state->ramp_len)
        state->gain = state->to;
    }
    out[0][i] = in[0][i] * state->gain;
  }
}

// --- nc.split / nc.mix -----------------------------------------------------

void process_split(void*, const float* const* in, float* const* out, int frames) noexcept
{
  for (int port = 0; out[port] != nullptr; ++port)
    std::copy(in[0], in[0] + frames, out[port]);
}

/// One input's settings and the ramp that is moving them. Mirrors \ref GainState: the smoothing is
/// the same smoothstep, because a lane level is a gain and there is no reason for it to move
/// differently from any other gain in the engine.
struct MixPort
{
  float gain = 1.0f;
  float level = 1.0f; ///< the level the port is set to, before mute and polarity
  float from = 1.0f;
  float to = 1.0f;
  int ramp_pos = 0;
  int ramp_len = 0;
  bool muted = false;
  bool inverted = false;
};

struct MixState
{
  std::vector<MixPort> ports;
  int smoothing_samples = 240;
};

/// The gain a port is heading for: mute is a ramp to silence rather than a flag the render path
/// branches on, so muting and unmuting are click-free for the same reason a level change is.
float mix_target(const MixPort& port) noexcept
{
  if (port.muted)
    return 0.0f;
  return port.inverted ? -port.level : port.level;
}

void* create_mix(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  // The arity has already been resolved and validated (`resolve_ports`), so reading the param here
  // cannot disagree with the plan's port count: it is the same number, read the same way.
  const int inputs = static_cast<int>(std::lround(number_param(spec, "inputs", 2.0)));
  if (inputs < 1)
  {
    error = reject("nc.mix", "'inputs' must be at least 1");
    return nullptr;
  }

  const double ramp_ms = number_param(spec, "ramp_ms", 5.0);
  if (!std::isfinite(ramp_ms) || ramp_ms < 0.0)
  {
    error = reject("nc.mix", "'ramp_ms' must be a finite number >= 0");
    return nullptr;
  }

  auto state = std::make_unique<MixState>();
  state->ports.resize(static_cast<std::size_t>(inputs));
  state->smoothing_samples =
    ramp_ms == 0.0 ? 1 : std::max(1, static_cast<int>(std::llround(ramp_ms * options.sample_rate / 1000.0)));
  // Every port starts at unity, unmuted and in phase.
  //
  // A *document* cannot set a lane here: per-port parameters are addressed with a port at runtime
  // (`rig.patch`), and a rig that wants a lane at -6 dB says so with a node in the lane -- which is
  // what a client's flattening already produces -- or, once there is a lowering layer, with a macro
  // that sets the port itself. A naming convention like `level2` would have been a second way to say
  // it, with its own schema, its own bounds and its own drift.
  return state.release();
}

void destroy_mix(void* state)
{
  delete static_cast<MixState*>(state);
}

bool set_param_mix(void* raw, uint32_t param_id, float value) noexcept
{
  if (!std::isfinite(value))
    return false;
  MixState* state = static_cast<MixState*>(raw);

  const int port = port_of_param_id(param_id);
  if (port < 0 || static_cast<std::size_t>(port) >= state->ports.size())
    return false; // a port the mixer does not have: refuse rather than ignore
  const uint32_t index = param_id & ((1u << kPortParamShift) - 1u);
  MixPort& p = state->ports[static_cast<std::size_t>(port)];

  switch (index)
  {
  case kMixPortParamLevelDb:
    p.level = static_cast<float>(std::pow(10.0, static_cast<double>(value) / 20.0));
    break;
  case kMixPortParamMute:
    p.muted = value != 0.0f;
    break;
  case kMixPortParamPhase:
    p.inverted = value != 0.0f;
    break;
  default:
    return false;
  }

  // From the *current* value, so a second change mid-ramp does not jump back to the old endpoint.
  p.from = p.gain;
  p.to = mix_target(p);
  p.ramp_pos = 0;
  p.ramp_len = state->smoothing_samples;
  return true;
}

void process_mix(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  // N inputs, terminated by nullptr. Accumulating rather than writing in[0] + in[1] is what lets a
  // rig sum as many chains as its `inputs` param declares.
  std::fill(out[0], out[0] + frames, 0.0f);
  MixState* state = static_cast<MixState*>(raw);
  for (int port = 0; in[port] != nullptr; ++port)
  {
    const float* source = in[port];
    // An input with no port settings (the reference interpreter binds every declared port, and a
    // hand-written graph may connect more than the state was built for) sums at unity rather than
    // being dropped.
    MixPort* settings = static_cast<std::size_t>(port) < state->ports.size()
                          ? &state->ports[static_cast<std::size_t>(port)]
                          : nullptr;
    // The unity fast path is only for a port that is settled *at* unity: a pending ramp must run even
    // when its starting value is 1.0, which is the common case for a lane nobody has touched yet.
    if (settings == nullptr || (settings->gain == 1.0f && settings->ramp_pos >= settings->ramp_len))
    {
      for (int i = 0; i < frames; ++i)
        out[0][i] += source[i];
      continue;
    }
    for (int i = 0; i < frames; ++i)
    {
      if (settings->ramp_pos < settings->ramp_len)
      {
        const float t = static_cast<float>(settings->ramp_pos + 1) / static_cast<float>(settings->ramp_len);
        settings->gain = settings->from + (settings->to - settings->from) * t * t * (3.0f - 2.0f * t);
        ++settings->ramp_pos;
        if (settings->ramp_pos >= settings->ramp_len)
          settings->gain = settings->to;
      }
      out[0][i] += source[i] * settings->gain;
    }
  }
}

// --- nc.bypass -------------------------------------------------------------
//
// A 1->1 pass-through: an explicit dry path a rig can place in the chain. It is *not* how
// `bypass: true` is implemented -- a bypassed node keeps running and has its output replaced by its
// input (RenderPlan::execute), because keeping it running is what keeps a stateful node warm for the
// un-bypass (G14). This type is that same substitution for a node that has no state to keep.

void process_bypass(void*, const float* const* in, float* const* out, int frames) noexcept
{
  std::copy(in[0], in[0] + frames, out[0]);
}

// --- the DSP-backed nodes (nc.nam, nc.cab.ir) ------------------------------
//
// Both wrap a vendored-core `nam::DSP` and differ only in how that DSP is produced: a `.nam` file
// for nc.nam, an impulse response for nc.cab.ir. The conversion between the engine's float buffers
// and the core's NAM_SAMPLE happens in buffers allocated here, so `process` allocates nothing.

struct DspState
{
  std::unique_ptr<nam::DSP> model;
  std::vector<NAM_SAMPLE> input;
  std::vector<NAM_SAMPLE> output;
};

/// Shared tail of both constructors: check the shape, reset at the engine rate, allocate the
/// conversion buffers. `origin` names the thing for error messages ("nc.nam: ...").
std::unique_ptr<DspState> prepare_dsp(std::unique_ptr<nam::DSP> model, const CompileOptions& options,
                                      const std::string& origin, std::string& error)
{
  if (!model)
  {
    error = reject(origin, "the model could not be loaded");
    return nullptr;
  }
  if (model->NumInputChannels() != 1 || model->NumOutputChannels() != 1)
  {
    error = reject(origin, "only mono models are supported so far");
    return nullptr;
  }

  // A model trained at 44.1 kHz driven at 48 kHz is a silent, plausible-sounding lie: it runs, and
  // it is wrong. The core has no rate guard of its own, so the engine provides one (§7.1).
  const double expected = model->GetExpectedSampleRate();
  if (expected > 0.0 && std::abs(expected - options.sample_rate) > 0.5)
  {
    error = reject(origin, "the model expects " + quoted(expected) + " Hz but the engine runs at " +
                              quoted(options.sample_rate) + " Hz");
    return nullptr;
  }

  auto state = std::make_unique<DspState>();
  state->model = std::move(model);
  try
  {
    state->model->Reset(options.sample_rate, options.max_quantum);
  }
  catch (const std::exception& e)
  {
    error = reject(origin, std::string("Reset failed: ") + e.what());
    return nullptr;
  }

  state->input.assign(static_cast<std::size_t>(options.max_quantum), static_cast<NAM_SAMPLE>(0));
  state->output.assign(static_cast<std::size_t>(options.max_quantum), static_cast<NAM_SAMPLE>(0));
  return state;
}

void destroy_dsp(void* state)
{
  delete static_cast<DspState*>(state);
}

/// The demangled concrete type of the wrapped DSP. A2PlanarFull and A2FastModel<8> are a factor of
/// two apart on this host, so a timing report has to say which one it measured.
std::string describe_dsp(void* raw)
{
  const DspState* state = static_cast<const DspState*>(raw);
  if (state == nullptr || state->model == nullptr)
    return {};
#if defined(__GNUG__)
  int status = 0;
  std::unique_ptr<char, void (*)(void*)> name{
    abi::__cxa_demangle(typeid(*state->model).name(), nullptr, nullptr, &status), std::free};
  if (status == 0 && name)
    return name.get();
#endif
  return typeid(*state->model).name();
}

void process_dsp(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  DspState* state = static_cast<DspState*>(raw);
  for (int i = 0; i < frames; ++i)
    state->input[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(in[0][i]);

  NAM_SAMPLE* in_ptrs[] = {state->input.data()};
  NAM_SAMPLE* out_ptrs[] = {state->output.data()};
  state->model->process(in_ptrs, out_ptrs, frames);

  for (int i = 0; i < frames; ++i)
    out[0][i] = static_cast<float>(state->output[static_cast<std::size_t>(i)]);
}

void* create_nam(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  const std::string path = string_param(spec, "model");
  if (path.empty())
  {
    error = reject("nc.nam", "'model' must name a .nam file");
    return nullptr;
  }

  std::unique_ptr<nam::DSP> model;
  try
  {
    model = nam::get_dsp(std::filesystem::path(path));
  }
  catch (const std::exception& e)
  {
    error = reject("nc.nam", "cannot load '" + path + "': " + e.what());
    return nullptr;
  }
  // The model budget's choice (NodeSpec::slim), made before prepare_dsp() resets the model: a container
  // resets only its active submodel, so the full one never allocates its history or prewarms.
  if (spec.slim)
  {
    if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(model.get()))
    {
      try
      {
        slimmable->SetSlimmableSize(0.0);
      }
      catch (const std::exception& e)
      {
        error = reject("nc.nam", "cannot slim '" + path + "': " + e.what());
        return nullptr;
      }
    }
  }
  return prepare_dsp(std::move(model), options, "nc.nam", error).release();
}

// --- nc.cab.ir --------------------------------------------------------------
//
// Zero-latency convolution through the vendored core's `nam::Linear`, wrapped like nc.nam's DSP.
// Its Auto implementation is direct for short IRs and FFT for long ones, with every partition's
// multiply done in the callback that completes a block -- one heavy callback per block rather than
// work spread across them. On piv at 8192 taps that roughly doubles the cab's share of the block
// p99, from 8.68% to 15.39% of a 64-frame deadline, and is still far inside it
// (bench-results/piv-cab-ir.txt). The fork's spread convolver that removed the burst is deprecated.
//
// The IR is loaded the way the NAM plugin loads one, less its gain law: resampled to the engine
// rate with the same cubic resampler, then truncated at 8192 taps (170 ms at 48 kHz). What that
// cut discards is -59 dB median across a 1538-file library (NAMBench/IR-PATH.md, "Is a shorter IR
// good enough?"). The plugin also scales by -18 dB; namchain has always run IRs at unity and still
// does.

/// The ceiling itself lives in namchain/wav.h: the daemon's cost table has to know it too, so
/// that an IR is charged for the taps that reach the convolver rather than the file's own count.
constexpr std::size_t kCabIrMaxTaps = kMaxIrTaps;

/// The taps of a `Linear` .nam: the core's own container for an impulse response.
bool load_linear_nam(const std::string& path, std::vector<float>& taps, double& rate, std::string& error)
{
  nlohmann::json doc;
  try
  {
    std::ifstream file(path);
    if (!file)
    {
      error = "cannot open '" + path + "'";
      return false;
    }
    doc = nlohmann::json::parse(file);
    if (doc.value("architecture", std::string()) != "Linear")
    {
      error = "'" + path + "' is not a Linear model";
      return false;
    }
    const auto& config = doc.at("config");
    if (config.value("bias", false))
    {
      error = "'" + path + "' has a bias term, which an impulse response does not";
      return false;
    }
    if (config.value("in_channels", 1) != 1 || config.value("out_channels", 1) != 1)
    {
      error = "'" + path + "' is not mono";
      return false;
    }
    const std::size_t count = config.at("receptive_field").get<std::size_t>();
    const auto& weights = doc.at("weights");
    if (count == 0 || weights.size() < count)
    {
      error = "'" + path + "' has fewer weights than its receptive field";
      return false;
    }
    taps.resize(count);
    for (std::size_t i = 0; i < count; ++i)
      taps[i] = weights[i].get<float>();
    rate = doc.value("sample_rate", 48000.0);
  }
  catch (const std::exception& e)
  {
    error = "cannot load '" + path + "': " + e.what();
    return false;
  }
  return true;
}

/// Two sources, because both are things people actually have: a `.wav` of the IR, or a `.nam`
/// containing a `Linear` model (the same data in the core's own container).
void* create_cab_ir(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  const std::string path = string_param(spec, "ir");
  if (path.empty())
  {
    error = reject("nc.cab.ir", "'ir' must name a .wav impulse response or a Linear .nam");
    return nullptr;
  }

  std::vector<float> taps;
  double ir_rate = 0.0;
  std::string load_error;
  const bool loaded = std::filesystem::path(path).extension() == ".nam"
                        ? load_linear_nam(path, taps, ir_rate, load_error)
                        : load_wav_mono(path, taps, ir_rate, load_error);
  if (!loaded)
  {
    error = reject("nc.cab.ir", load_error);
    return nullptr;
  }
  if (taps.empty())
  {
    error = reject("nc.cab.ir", "'" + path + "' contains no samples");
    return nullptr;
  }
  if (!(ir_rate > 0.0) || !std::isfinite(ir_rate))
  {
    error = reject("nc.cab.ir", "'" + path + "' has no usable sample rate");
    return nullptr;
  }
  for (const float tap : taps)
  {
    if (!std::isfinite(tap))
    {
      error = reject("nc.cab.ir", "'" + path + "' contains a non-finite sample");
      return nullptr;
    }
  }

  if (std::abs(ir_rate - options.sample_rate) > 0.5)
  {
    // As the plugin does it: one zero of padding each side, so the cubic has neighbours at the ends.
    std::vector<float> padded(taps.size() + 2, 0.0f);
    std::copy(taps.begin(), taps.end(), padded.begin() + 1);
    std::vector<float> resampled;
    try
    {
      dsp::ResampleCubic<float>(padded, ir_rate, options.sample_rate, 0.0, resampled);
    }
    catch (const std::exception& e)
    {
      error = reject("nc.cab.ir", "cannot resample '" + path + "': " + e.what());
      return nullptr;
    }
    taps = std::move(resampled);
  }
  if (taps.size() > kCabIrMaxTaps)
    taps.resize(kCabIrMaxTaps);

  std::unique_ptr<nam::DSP> model;
  try
  {
    model = std::make_unique<nam::Linear>(1, 1, static_cast<int>(taps.size()), false, taps, options.sample_rate);
  }
  catch (const std::exception& e)
  {
    error = reject("nc.cab.ir", std::string("cannot build the convolution: ") + e.what());
    return nullptr;
  }
  return prepare_dsp(std::move(model), options, "nc.cab.ir", error).release();
}

// --- nc.eq.biquad ----------------------------------------------------------
//
// RBJ audio-EQ-cookbook biquads, computed once at build. Direct Form II transposed: two state
// floats and no delay line, which is all a mono biquad needs.

enum class BiquadMode
{
  Peaking,
  LowShelf,
  HighShelf,
  LowPass,
  HighPass,
  BandPass,
  Notch
};

bool parse_biquad_mode(const std::string& name, BiquadMode& mode)
{
  if (name == "peaking" || name == "peak" || name == "eq")
  {
    mode = BiquadMode::Peaking;
    return true;
  }
  if (name == "lowshelf" || name == "low_shelf")
  {
    mode = BiquadMode::LowShelf;
    return true;
  }
  if (name == "highshelf" || name == "high_shelf")
  {
    mode = BiquadMode::HighShelf;
    return true;
  }
  if (name == "lowpass" || name == "low_pass" || name == "lp")
  {
    mode = BiquadMode::LowPass;
    return true;
  }
  if (name == "highpass" || name == "high_pass" || name == "hp")
  {
    mode = BiquadMode::HighPass;
    return true;
  }
  if (name == "bandpass" || name == "band_pass" || name == "bp")
  {
    mode = BiquadMode::BandPass;
    return true;
  }
  if (name == "notch")
  {
    mode = BiquadMode::Notch;
    return true;
  }
  return false;
}

/// Normalised coefficients of one biquad section: the difference equation is
///   y = b0*x + s1;  s1 = b1*x - a1*y + s2;  s2 = b2*x - a2*y
struct BiquadCoeffs
{
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

/// One running section: coefficients plus the two states they carry.
///
/// Two of these live in a node, because a parameter change crossfades between them rather than
/// interpolating coefficients (see \ref BiquadState).
struct BiquadLane
{
  BiquadCoeffs coeffs;
  float s1 = 0.0f;
  float s2 = 0.0f;
};

void run_biquad(BiquadLane& lane, const float* in, float* out, int frames) noexcept
{
  const float b0 = lane.coeffs.b0;
  const float b1 = lane.coeffs.b1;
  const float b2 = lane.coeffs.b2;
  const float a1 = lane.coeffs.a1;
  const float a2 = lane.coeffs.a2;
  float s1 = lane.s1;
  float s2 = lane.s2;
  for (int i = 0; i < frames; ++i)
  {
    const float x = in[i];
    const float y = b0 * x + s1;
    s1 = b1 * x - a1 * y + s2;
    s2 = b2 * x - a2 * y;
    out[i] = y;
  }
  lane.s1 = s1;
  lane.s2 = s2;
}

/// The RBJ cookbook coefficients for \p mode at these settings, normalised by a0.
///
/// Returns false with a reason for settings the filter cannot honour, so `create` can report them and
/// a render-path parameter change can refuse them: a `set_param` must not be able to build a
/// degenerate filter either.
bool biquad_coefficients(BiquadMode mode, double freq, double q, double gain_db, double rate,
                         BiquadCoeffs& out, std::string& error)
{
  if (!std::isfinite(freq) || !std::isfinite(q) || !std::isfinite(gain_db))
  {
    error = "freq, q and gain_db must be finite";
    return false;
  }
  // Reject rather than clamp: an EQ that silently ignores a 30 kHz setting is a bug report about
  // "the EQ does nothing", and one that silently wraps is a bug report about noise.
  if (freq <= 0.0 || freq >= 0.5 * rate)
  {
    error = "freq " + quoted(freq) + " Hz is outside (0, " + quoted(0.5 * rate) + ") Hz";
    return false;
  }
  if (q <= 0.0)
  {
    error = "q must be > 0, got " + quoted(q);
    return false;
  }
  // Shelves have no bandwidth, so RBJ parameterises them by slope instead. Using `q` as that slope
  // (clamped to the range that stays monotonic) beats accepting a parameter and ignoring it.
  const bool shelf = mode == BiquadMode::LowShelf || mode == BiquadMode::HighShelf;
  const double slope = std::clamp(q, 0.05, 1.0);

  const double w0 = 2.0 * kPi * freq / rate;
  const double cos_w0 = std::cos(w0);
  const double sin_w0 = std::sin(w0);
  const double A = std::pow(10.0, gain_db / 40.0);
  const double alpha = shelf ? sin_w0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0)
                             : sin_w0 / (2.0 * q);

  double b0 = 1.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double a0 = 1.0;
  double a1 = 0.0;
  double a2 = 0.0;

  switch (mode)
  {
  case BiquadMode::Peaking:
    b0 = 1.0 + alpha * A;
    b1 = -2.0 * cos_w0;
    b2 = 1.0 - alpha * A;
    a0 = 1.0 + alpha / A;
    a1 = -2.0 * cos_w0;
    a2 = 1.0 - alpha / A;
    break;
  case BiquadMode::LowPass:
    b0 = (1.0 - cos_w0) / 2.0;
    b1 = 1.0 - cos_w0;
    b2 = (1.0 - cos_w0) / 2.0;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cos_w0;
    a2 = 1.0 - alpha;
    break;
  case BiquadMode::HighPass:
    b0 = (1.0 + cos_w0) / 2.0;
    b1 = -(1.0 + cos_w0);
    b2 = (1.0 + cos_w0) / 2.0;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cos_w0;
    a2 = 1.0 - alpha;
    break;
  case BiquadMode::BandPass:
    b0 = alpha;
    b1 = 0.0;
    b2 = -alpha;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cos_w0;
    a2 = 1.0 - alpha;
    break;
  case BiquadMode::Notch:
    b0 = 1.0;
    b1 = -2.0 * cos_w0;
    b2 = 1.0;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cos_w0;
    a2 = 1.0 - alpha;
    break;
  case BiquadMode::LowShelf:
  {
    const double sqrt_A = std::sqrt(A);
    b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha);
    b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
    b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha);
    a0 = (A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha;
    a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
    a2 = (A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha;
    break;
  }
  case BiquadMode::HighShelf:
  {
    const double sqrt_A = std::sqrt(A);
    b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha);
    b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
    b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha);
    a0 = (A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrt_A * alpha;
    a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
    a2 = (A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrt_A * alpha;
    break;
  }
  }

  if (!(a0 != 0.0) || !std::isfinite(a0))
  {
    error = "the coefficients are degenerate at these settings";
    return false;
  }

  out.b0 = static_cast<float>(b0 / a0);
  out.b1 = static_cast<float>(b1 / a0);
  out.b2 = static_cast<float>(b2 / a0);
  out.a1 = static_cast<float>(a1 / a0);
  out.a2 = static_cast<float>(a2 / a0);
  return true;
}

/// \brief A biquad whose parameters can change while it runs.
///
/// The interesting decision: a parameter change does **not** interpolate coefficients. Linearly
/// interpolating b0..a2 between two stable filters is not itself guaranteed stable, and even when it
/// is, it is not the response anyone asked for at the midpoint. Instead the node keeps the old
/// coefficients in a second lane, runs both sections over the ramp and equal-power crossfades their
/// outputs (`rt::Crossfade`, the same law and the same code as a plan-level transition). Two filters
/// for the duration of a ramp costs one extra biquad per sample and cannot click, because both
/// signals it is mixing are individually valid.
struct BiquadState
{
  explicit BiquadState(int max_fade) : fader(max_fade) {}

  BiquadLane lane;      ///< the coefficients the output is following
  BiquadLane outgoing;  ///< the previous ones, still running while a ramp closes
  rt::Crossfade fader;  ///< equal power between the two lanes; inactive when settled
  /// The parameters behind \ref lane, so a change ramps from where the filter actually is rather than
  /// from whatever a client last sent.
  double freq = 1000.0;
  double q = 0.707;
  double gain_db = 0.0;
  BiquadMode mode = BiquadMode::Peaking;
  double rate = 48000.0;
  int smoothing_samples = 240;
  /// Sized at build: the render path may not allocate, and both lanes run during a ramp.
  std::vector<float> old_buf;
  std::vector<float> new_buf;
};

void* create_biquad(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  const std::string type = "nc.eq.biquad";

  BiquadMode mode = BiquadMode::Peaking;
  const std::string mode_name = string_param(spec, "mode");
  if (!mode_name.empty() && !parse_biquad_mode(mode_name, mode))
  {
    error = reject(type, "unknown mode '" + mode_name + "'");
    return nullptr;
  }

  const double freq = number_param(spec, "freq", 1000.0);
  const double q = number_param(spec, "q", 0.707);
  const double gain_db = number_param(spec, "gain_db", 0.0);

  BiquadCoeffs coeffs;
  std::string reason;
  if (!biquad_coefficients(mode, freq, q, gain_db, options.sample_rate, coeffs, reason))
  {
    error = reject(type, reason);
    return nullptr;
  }

  const double ramp_ms = number_param(spec, "ramp_ms", 5.0);
  if (!std::isfinite(ramp_ms) || ramp_ms < 0.0)
  {
    error = reject(type, "'ramp_ms' must be a finite number >= 0");
    return nullptr;
  }
  // A ramp shorter than a sample is a step, which is what ramp_ms = 0 asks for, and it is also the
  // negative control the glitch tests use.
  const int smoothing =
    ramp_ms == 0.0 ? 1 : std::max(1, static_cast<int>(std::llround(ramp_ms * options.sample_rate / 1000.0)));

  auto state = std::make_unique<BiquadState>(smoothing);
  state->lane.coeffs = coeffs;
  state->freq = freq;
  state->q = q;
  state->gain_db = gain_db;
  state->mode = mode;
  state->rate = options.sample_rate;
  state->smoothing_samples = smoothing;
  state->old_buf.assign(static_cast<std::size_t>(std::max(1, options.max_quantum)), 0.0f);
  state->new_buf.assign(static_cast<std::size_t>(std::max(1, options.max_quantum)), 0.0f);
  return state.release();
}

void destroy_biquad(void* state)
{
  delete static_cast<BiquadState*>(state);
}

bool set_param_biquad(void* raw, uint32_t param_id, float value) noexcept
{
  if (!std::isfinite(value))
    return false;
  BiquadState* state = static_cast<BiquadState*>(raw);

  double freq = state->freq;
  double q = state->q;
  double gain_db = state->gain_db;
  switch (param_id)
  {
  case kBiquadParamFreq: freq = value; break;
  case kBiquadParamQ: q = value; break;
  case kBiquadParamGainDb: gain_db = value; break;
  default: return false;
  }

  BiquadCoeffs coeffs;
  std::string reason;
  if (!biquad_coefficients(state->mode, freq, q, gain_db, state->rate, coeffs, reason))
    return false; // out of range: leave the filter exactly as it was rather than jump somewhere else

  if (!state->fader.active())
  {
    // Start from where the filter actually is, not from the last value a client sent.
    state->outgoing = state->lane;
    // And give the incoming section the outgoing one's memory. The states are the filter's record of
    // the signal, not of the coefficients, so a section that starts from zero is a section that has
    // forgotten the last few milliseconds -- audible as a thump on a high-Q setting, and measurably
    // not the response the parameter asked for until the filter has rung itself back in.
    state->lane.s1 = state->outgoing.s1;
    state->lane.s2 = state->outgoing.s2;
    state->fader.start(state->smoothing_samples);
  }
  // A second change while a ramp is still closing retargets the incoming lane and lets the fade
  // continue: restarting it would jump the mix weights, which is the click this exists to avoid.
  state->lane.coeffs = coeffs;
  state->freq = freq;
  state->q = q;
  state->gain_db = gain_db;
  return true;
}

void process_biquad(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  BiquadState* state = static_cast<BiquadState*>(raw);
  if (!state->fader.active())
  {
    run_biquad(state->lane, in[0], out[0], frames);
    return;
  }

  run_biquad(state->outgoing, in[0], state->old_buf.data(), frames);
  run_biquad(state->lane, in[0], state->new_buf.data(), frames);
  state->fader.mix(state->old_buf.data(), state->new_buf.data(), out[0], frames);
  if (!state->fader.active())
    state->outgoing = BiquadLane{}; // the old section is stale once the fade has closed
}

// --- nc.delay --------------------------------------------------------------
//
// A plain fixed delay, which is also the only way to write feedback: §5.1 makes a cycle a compile
// error, so a feedback loop is "split, delay, gain, mix" with the delay breaking the cycle.

/// \brief A fixed delay whose time can change while it runs, by crossfading between two taps.
///
/// The ring itself is shared; only the read offset moves. Moving it in one step would splice two
/// unrelated points of the signal together, which is a click, and interpolating the read position
/// would be a varispeed pitch slide rather than a new delay time. So the node reads the old tap and
/// the new tap for the length of a ramp and equal-power crossfades them -- the tape-machine answer,
/// and the same `rt::Crossfade` law a plan-level transition uses.
struct DelayState
{
  explicit DelayState(int max_fade) : fader(max_fade) {}

  std::vector<float> line;
  std::size_t write = 0;
  std::size_t read = 0;          ///< the tap the output is following
  std::size_t outgoing_read = 0; ///< the previous tap, read while a ramp closes
  rt::Crossfade fader;           ///< equal power between the two taps; inactive when settled
  /// The longest delay this line can hold, in samples. A `set_param` beyond it is refused: the ring
  /// cannot be grown on the render path, and reading past its capacity would wrap into recent samples
  /// and sound like a short delay rather than like a mistake.
  double capacity = 1.0;
  double rate = 48000.0;
  int smoothing_samples = 240;
  std::vector<float> old_buf;
  std::vector<float> new_buf;

  void advance(std::size_t& index) noexcept
  {
    if (++index == line.size())
      index = 0;
  }
};

/// Where the read pointer has to be for a delay of \p samples behind the write pointer.
std::size_t delay_tap(std::size_t write, std::size_t size, double samples) noexcept
{
  const std::size_t back = static_cast<std::size_t>(samples) % size;
  return (write + size - back) % size;
}

bool delay_in_range(double samples, double rate, std::string& error)
{
  // A delay shorter than a sample is not a delay, and a longer one is a user error worth naming --
  // silently allocating 10 minutes of ring for a typo'd 'ms' is worse than refusing.
  const double max_samples = 60.0 * rate;
  if (!std::isfinite(samples) || samples < 1.0 || samples > max_samples)
  {
    error = "delay must be between 1 sample and " + quoted(max_samples) + " samples, got " + quoted(samples);
    return false;
  }
  return true;
}

void* create_delay(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  const std::string type = "nc.delay";

  double samples = 0.0;
  if (has_param(spec, "samples"))
  {
    samples = std::round(number_param(spec, "samples", 0.0));
  }
  else if (has_param(spec, "ms"))
  {
    samples = std::round(number_param(spec, "ms", 0.0) * options.sample_rate / 1000.0);
  }
  else
  {
    error = reject(type, "one of 'samples' or 'ms' is required");
    return nullptr;
  }

  std::string reason;
  if (!delay_in_range(samples, options.sample_rate, reason))
  {
    error = reject(type, reason);
    return nullptr;
  }

  // How much line to allocate. A delay that can be *lengthened* while it runs needs a ring long enough
  // for the longest time it will be asked for, and the render path may not allocate, so the rig
  // declares the maximum -- exactly as a real delay's memory is sized by its longest setting rather
  // than by where the knob happens to be. Without one, the ring is the built time and a later
  // `set_param` beyond it is refused rather than reading stale samples from a too-short line.
  double capacity = samples;
  if (has_param(spec, "max_samples"))
  {
    capacity = std::round(number_param(spec, "max_samples", samples));
  }
  else if (has_param(spec, "max_ms"))
  {
    capacity = std::round(number_param(spec, "max_ms", samples * 1000.0 / options.sample_rate) *
                          options.sample_rate / 1000.0);
  }
  if (!delay_in_range(capacity, options.sample_rate, reason))
  {
    error = reject(type, reason);
    return nullptr;
  }
  if (capacity < samples)
  {
    error = reject(type, "the delay is " + quoted(samples) + " samples and the line is only " +
                           quoted(capacity) + "; raise 'max_ms' or 'max_samples'");
    return nullptr;
  }

  const double ramp_ms = number_param(spec, "ramp_ms", 5.0);
  if (!std::isfinite(ramp_ms) || ramp_ms < 0.0)
  {
    error = reject(type, "'ramp_ms' must be a finite number >= 0");
    return nullptr;
  }
  const int smoothing =
    ramp_ms == 0.0 ? 1 : std::max(1, static_cast<int>(std::llround(ramp_ms * options.sample_rate / 1000.0)));

  auto state = std::make_unique<DelayState>(smoothing);
  // One more than the longest delay so that read and write can never be the same slot: read-before-
  // write at a shared index would also work, but this way the read pointer is visibly `delay` behind.
  state->line.assign(static_cast<std::size_t>(capacity) + 1, 0.0f);
  state->capacity = capacity;
  state->write = 0;
  state->read = delay_tap(0, state->line.size(), samples);
  state->outgoing_read = state->read;
  state->rate = options.sample_rate;
  state->smoothing_samples = smoothing;
  state->old_buf.assign(static_cast<std::size_t>(std::max(1, options.max_quantum)), 0.0f);
  state->new_buf.assign(static_cast<std::size_t>(std::max(1, options.max_quantum)), 0.0f);
  return state.release();
}

void destroy_delay(void* state)
{
  delete static_cast<DelayState*>(state);
}

bool set_param_delay(void* raw, uint32_t param_id, float value) noexcept
{
  if (!std::isfinite(value))
    return false;
  DelayState* state = static_cast<DelayState*>(raw);

  double samples = 0.0;
  switch (param_id)
  {
  case kDelayParamSamples: samples = std::round(value); break;
  case kDelayParamMs: samples = std::round(static_cast<double>(value) * state->rate / 1000.0); break;
  default: return false;
  }

  std::string reason;
  if (!delay_in_range(samples, state->rate, reason) || samples > state->capacity)
    return false; // out of range or longer than the line: leave the delay exactly as it was

  if (!state->fader.active())
  {
    // Both taps start at the current one; the ramp is what separates them.
    state->outgoing_read = state->read;
    state->fader.start(state->smoothing_samples);
  }
  // A second change mid-ramp retargets the incoming tap and lets the fade continue, for the same
  // reason the biquad does: restarting would jump the mix weights.
  state->read = delay_tap(state->write, state->line.size(), samples);
  return true;
}

void process_delay(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  DelayState* state = static_cast<DelayState*>(raw);
  if (!state->fader.active())
  {
    for (int i = 0; i < frames; ++i)
    {
      out[0][i] = state->line[state->read];
      state->line[state->write] = in[0][i];
      state->advance(state->write);
      state->advance(state->read);
    }
    return;
  }

  // Both taps are read before the input is written, so neither sees the sample being recorded now.
  for (int i = 0; i < frames; ++i)
  {
    state->old_buf[static_cast<std::size_t>(i)] = state->line[state->outgoing_read];
    state->new_buf[static_cast<std::size_t>(i)] = state->line[state->read];
    state->line[state->write] = in[0][i];
    state->advance(state->write);
    state->advance(state->read);
    state->advance(state->outgoing_read);
  }
  state->fader.mix(state->old_buf.data(), state->new_buf.data(), out[0], frames);
  if (!state->fader.active())
    state->outgoing_read = state->read; // the old tap is stale once the fade has closed
}

// --- nc.unit_delay ---------------------------------------------------------
//
// The feedback primitive of §5.1: one block of delay, which is what turns a cycle into a DAG. The
// node itself is a pass-through -- it copies whatever is in its input buffer to its output buffer.
// The delay is a property of *when* it runs: the compiler schedules block-delay nodes before
// everything else, so their input buffers still hold the previous block's value. It is marked
// `block_delay` in its vtable, which is what tells the scheduler and the memory plan so.
//
// This is also why the node has no state: a stateful version would add a block on top of the one
// the schedule already provides, giving two.

void process_unit_delay(void*, const float* const* in, float* const* out, int frames) noexcept
{
  std::copy(in[0], in[0] + frames, out[0]);
}

// --- the declared parameters of every built-in (docs/ui-integration-plan.md §1.4) ----------------
//
// One table per type, in id order, and the only place a parameter is described: the compiler validates
// against these, `engine.schema` publishes them, and a param's id is its position plus one. A
// parameter that is declared but ignored is a knob that does nothing, so `tests/test_schema.cpp`
// checks each one against what the node actually reads.

const ParamDesc kSplitParams[] = {
  {.id = "outputs", .name = "Outputs", .kind = ParamKind::Number, .default_value = 2.0, .has_min = true,
   .min = 1.0, .has_max = true, .max = 64.0, .step = 1.0, .snap = true},
};

const ParamDesc kMixParams[] = {
  {.id = "inputs", .name = "Inputs", .kind = ParamKind::Number, .default_value = 2.0, .has_min = true,
   .min = 1.0, .has_max = true, .max = 64.0, .step = 1.0, .snap = true},
  {.id = "ramp_ms", .name = "Ramp", .unit = "ms", .default_value = 5.0, .has_min = true, .min = 0.0,
   .has_max = true, .max = 1000.0},
};

/// What every input of a mixer has: a level, a mute and a polarity. Declared once and addressed with a
/// port, so a client reads one set of shapes and applies them per lane rather than learning a naming
/// convention like `level3`.
const ParamDesc kMixPortParams[] = {
  {.id = "level_db", .name = "Level", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -60.0,
   .has_max = true, .max = 12.0, .rampable = true},
  {.id = "mute", .name = "Mute", .kind = ParamKind::Flag, .default_value = 0.0, .rampable = true},
  {.id = "phase", .name = "Invert", .kind = ParamKind::Flag, .default_value = 0.0, .rampable = true},
};

const ParamDesc kNamParams[] = {
  {.id = "model", .name = "Model", .kind = ParamKind::Option, .required = true, .options_from = "models"},
};

/// The modes `nc.eq.biquad` accepts. The labels are what a client shows; the values are what it sends.
const OptionDesc kBiquadModes[] = {
  {"peaking", "Peaking"},   {"lowshelf", "Low shelf"}, {"highshelf", "High shelf"},
  {"lowpass", "Low pass"},  {"highpass", "High pass"}, {"bandpass", "Band pass"},
  {"notch", "Notch"},
};

const char* const kBiquadModeAliases[] = {"peak",  "eq",        "low_shelf",  "high_shelf",
                                           "low_pass", "high_pass", "band_pass", "bp"};

const ParamDesc kBiquadParams[] = {
  {.id = "mode", .name = "Mode", .kind = ParamKind::Option, .default_value = 0.0,
   .options = kBiquadModes, .option_count = 7, .aliases = kBiquadModeAliases, .alias_count = 8},
  // Rampable, and the ramp is an in-node crossfade between two coefficient sets rather than an
  // interpolated one: linearly interpolating b0..a2 between two stable filters is not itself
  // guaranteed stable, and it is not the response anyone asked for at the midpoint either.
  //
  // No maximum: the real limit is Nyquist at whatever rate the rig runs at, which the schema does not
  // know and a fixed 20 kHz would get wrong at 96 kHz. The node's own check owns that bound and says
  // so, which is the difference between a schema limit and an engine limit.
  {.id = "freq", .name = "Frequency", .unit = "Hz", .default_value = 1000.0, .has_min = true,
   .min = 10.0, .min_inclusive = false, .scale = ParamScale::Log, .rampable = true},
  {.id = "q", .name = "Q", .default_value = 0.707, .has_min = true, .min = 0.0,
   .min_inclusive = false, .has_max = true, .max = 20.0, .scale = ParamScale::Log, .rampable = true},
  {.id = "gain_db", .name = "Gain", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -30.0,
   .has_max = true, .max = 30.0, .rampable = true},
  {.id = "ramp_ms", .name = "Ramp", .unit = "ms", .default_value = 5.0, .has_min = true, .min = 0.0,
   .has_max = true, .max = 1000.0},
};

const ParamDesc kDelayParams[] = {
  // Bounded by the engine, not by the schema: 60 seconds at the rig's rate, in samples. A client
  // showing a slider picks its own display range when a parameter is unbounded here.
  //
  // Rampable, by crossfading between the old tap and the new one rather than by interpolating the read
  // position -- interpolating would be a varispeed slide, which is a different effect from a new delay
  // time.
  {.id = "samples", .name = "Delay", .unit = "samples", .has_min = true, .min = 1.0, .step = 1.0,
   .snap = true, .rampable = true, .one_of = "delay time", .one_of_required = true},
  {.id = "ms", .name = "Delay", .unit = "ms", .has_min = true, .min = 0.021, .scale = ParamScale::Log,
   .rampable = true, .one_of = "delay time", .one_of_required = true},
  // How long *this* node takes to move between two delay times. Not rampable: it is the smoothing
  // time, so changing it is a rebuild, and a rebuild is the only honest way to change it.
  {.id = "ramp_ms", .name = "Ramp", .unit = "ms", .default_value = 5.0, .has_min = true, .min = 0.0,
   .has_max = true, .max = 1000.0},
  // How much line to allocate, and therefore how far the delay can be *lengthened* while it runs.
  // Build-only: the render path may not allocate, so this is the rig's declaration of its own memory.
  // Absent, the ring is the built time and a longer `set_param` is refused.
  {.id = "max_samples", .name = "Max delay", .unit = "samples", .has_min = true, .min = 1.0,
   .step = 1.0, .snap = true, .one_of = "max delay"},
  {.id = "max_ms", .name = "Max delay", .unit = "ms", .has_min = true, .min = 0.021,
   .scale = ParamScale::Log, .one_of = "max delay"},
};

const ParamDesc kCabIrParams[] = {
  {.id = "ir", .name = "Impulse response", .kind = ParamKind::Option, .required = true,
   .options_from = "irs"},
};

// Field order matches NodeType: id, name, category, inputs, outputs, create, destroy, process,
// describe, set_block_input, block_delay, arity_param, min_ports, max_ports, params, param_count,
// channels, set_param.
const NodeType kInput{"nc.input", "Input", "io", 0, 1, create_input, destroy_input, process_input,
                      nullptr, set_block_input};
const NodeType kOutput{"nc.output", "Output", "io", 1, 0, nullptr, nullptr, process_marker};
// `nc.gain` is the first node with a parameter a client can ramp: `params`/`set_param` are the
// `smooth` op class of §5.3, and every later settable node reuses the same two vtable slots.
const NodeType kGain{.id = "nc.gain", .name = "Gain", .category = "utility", .inputs = 1, .outputs = 1,
                     .create = create_gain, .destroy = destroy_gain, .process = process_gain,
                     .params = kGainParams, .param_count = 3, .set_param = set_param_gain};
// `nc.split`/`nc.mix` are the per-spec arity nodes (plan.md §6.1): the fan-out is the rig's
// `"outputs"`/`"inputs"` param rather than a fixed 2, which is what makes N parallel chains a
// first-class shape. The default stays 2, so a rig written before this means exactly what it did.
const NodeType kSplit{.id = "nc.split", .name = "Split", .category = "utility", .inputs = 1,
                      .outputs = 2, .process = process_split, .arity_param = "outputs", .min_ports = 1,
                      .max_ports = 64, .params = kSplitParams, .param_count = 1};
const NodeType kMix{.id = "nc.mix", .name = "Mix", .category = "utility", .inputs = 2, .outputs = 1,
                    .create = create_mix, .destroy = destroy_mix, .process = process_mix,
                    .arity_param = "inputs", .min_ports = 1, .max_ports = 64, .params = kMixParams,
                    .param_count = 2, .per_port_params = kMixPortParams, .per_port_param_count = 3,
                    .set_param = set_param_mix};
const NodeType kBypass{"nc.bypass", "Bypass", "utility", 1, 1, nullptr, nullptr, process_bypass};
const NodeType kNam{.id = "nc.nam", .name = "NAM model", .category = "amp", .inputs = 1, .outputs = 1,
                    .create = create_nam, .destroy = destroy_dsp, .process = process_dsp,
                    .describe = describe_dsp, .arity_param = nullptr, .min_ports = 1, .max_ports = 64,
                    .params = kNamParams, .param_count = 1};
const NodeType kCabIr{.id = "nc.cab.ir", .name = "Cabinet IR", .category = "cab", .inputs = 1,
                      .outputs = 1, .create = create_cab_ir, .destroy = destroy_dsp,
                      .process = process_dsp, .describe = describe_dsp, .arity_param = nullptr,
                      .min_ports = 1, .max_ports = 64, .params = kCabIrParams, .param_count = 1};
const NodeType kBiquad{.id = "nc.eq.biquad", .name = "Biquad EQ", .category = "eq", .inputs = 1,
                       .outputs = 1, .create = create_biquad, .destroy = destroy_biquad,
                       .process = process_biquad, .params = kBiquadParams, .param_count = 5,
                       .set_param = set_param_biquad};
const NodeType kDelay{.id = "nc.delay", .name = "Delay", .category = "delay", .inputs = 1, .outputs = 1,
                      .create = create_delay, .destroy = destroy_delay, .process = process_delay,
                      .params = kDelayParams, .param_count = 5, .set_param = set_param_delay};
// A scheduling primitive rather than a pedal: `block_delay` is what breaks a feedback cycle, and a
// palette should leave it out, which is what the flag is for.
const NodeType kUnitDelay{"nc.unit_delay", "Unit delay", "utility", 1, 1, nullptr, nullptr,
                          process_unit_delay, nullptr, nullptr, true};

// --- nc.xfade (engine-internal) ----------------------------------------------
//
// The crossfade inside a local transition (docs/transitions.md, "Local transitions"). Input 0 is the
// signal as it was and input 1 the signal as it is becoming; the output moves from one to the other
// over `frames` samples from the node's first block, with the same equal-power law and the same exact
// landing as the whole-chain fade (rt::Crossfade), then passes input 1. Only the engine places one,
// at the point in a chain where an edit's old and new signals meet. It is left out of prewarm, which
// would otherwise spend the fade on silence before the plan is heard.

struct XfadeState
{
  explicit XfadeState(int frames) : fade(frames) { fade.start(frames); }
  rt::Crossfade fade;
};

void* create_xfade(const graph::NodeSpec& spec, const CompileOptions&, std::string& error)
{
  const auto found = spec.params.find("frames");
  const double frames = found == spec.params.end() ? 0.0 : found->second;
  if (!std::isfinite(frames) || frames < 1.0 || frames > 1048576.0)
  {
    error = reject("nc.xfade", "'frames' must be between 1 and 1048576");
    return nullptr;
  }
  return new XfadeState(static_cast<int>(frames));
}

void destroy_xfade(void* raw)
{
  delete static_cast<XfadeState*>(raw);
}

void process_xfade(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  static_cast<XfadeState*>(raw)->fade.mix(in[0], in[1], out[0], frames);
}

const ParamDesc kXfadeParams[] = {
  {.id = "frames", .name = "Frames", .kind = ParamKind::Number, .default_value = 240.0, .has_min = true,
   .min = 1.0, .step = 1.0, .snap = true, .required = true},
};

const NodeType kXfade{.id = "nc.xfade", .name = "Transition crossfade", .category = "utility", .inputs = 2,
                      .outputs = 1, .create = create_xfade, .destroy = destroy_xfade, .process = process_xfade,
                      .params = kXfadeParams, .param_count = 1, .internal = true, .skip_prewarm = true};

} // namespace

/// Every registered type, in the order `engine.schema` reports them: the palette's order, grouped by
/// what a player is looking for rather than by when each was written.
const NodeType* const kNodeTypes[] = {&kInput,  &kOutput, &kGain,  &kBiquad, &kDelay, &kNam,
                                      &kCabIr,  &kSplit,  &kMix,   &kBypass, &kUnitDelay};

/// Types only the engine places. Found by the compiler like any other, never listed in the catalogue.
const NodeType* const kInternalNodeTypes[] = {&kXfade};

const NodeType* find_node_type(const std::string& id)
{
  for (const NodeType* type : kNodeTypes)
  {
    if (id == type->id)
      return type;
  }
  for (const NodeType* type : kInternalNodeTypes)
  {
    if (id == type->id)
      return type;
  }
  return nullptr;
}

std::vector<const NodeType*> all_node_types()
{
  return std::vector<const NodeType*>(std::begin(kNodeTypes), std::end(kNodeTypes));
}

} // namespace render
} // namespace namchain
