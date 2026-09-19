#include "namchain/graph/plan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

/// Parameter ids are positions in \ref kGainParams plus one (plan.h, \ref ParamDesc).
constexpr uint32_t kGainParamGainDb = 1;
constexpr uint32_t kGainParamGain = 2;

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

void process_mix(void*, const float* const* in, float* const* out, int frames) noexcept
{
  // N inputs, terminated by nullptr. Accumulating rather than writing in[0] + in[1] is what lets a
  // rig sum as many chains as its `inputs` param declares.
  std::fill(out[0], out[0] + frames, 0.0f);
  for (int port = 0; in[port] != nullptr; ++port)
  {
    const float* source = in[port];
    for (int i = 0; i < frames; ++i)
      out[0][i] += source[i];
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
  return prepare_dsp(std::move(model), options, "nc.nam", error).release();
}

/// Load a mono impulse response and wrap it as a zero-latency `nam::Linear` (§6.2).
///
/// Two sources, because both are things people actually have: a `.wav` of the IR, or a `.nam`
/// containing a `Linear` model (which is the same data in the core's own container).
void* create_cab_ir(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  const std::string path = string_param(spec, "ir");
  if (path.empty())
  {
    error = reject("nc.cab.ir", "'ir' must name a .wav impulse response or a Linear .nam");
    return nullptr;
  }

  const std::filesystem::path fs_path(path);
  std::unique_ptr<nam::DSP> model;
  if (fs_path.extension() == ".nam")
  {
    try
    {
      model = nam::get_dsp(fs_path);
    }
    catch (const std::exception& e)
    {
      error = reject("nc.cab.ir", "cannot load '" + path + "': " + e.what());
      return nullptr;
    }
  }
  else
  {
    std::vector<float> taps;
    double ir_rate = 0.0;
    std::string wav_error;
    if (!load_wav_mono(path, taps, ir_rate, wav_error))
    {
      error = reject("nc.cab.ir", wav_error);
      return nullptr;
    }
    if (taps.empty())
    {
      error = reject("nc.cab.ir", "'" + path + "' contains no samples");
      return nullptr;
    }
    if (std::abs(ir_rate - options.sample_rate) > 0.5)
    {
      error = reject("nc.cab.ir", "'" + path + "' is " + quoted(ir_rate) + " Hz but the engine runs at " +
                                    quoted(options.sample_rate) + " Hz; there is no resampler yet");
      return nullptr;
    }
    try
    {
      model = std::make_unique<nam::Linear>(1, 1, static_cast<int>(taps.size()), false, taps, ir_rate);
    }
    catch (const std::exception& e)
    {
      error = reject("nc.cab.ir", std::string("cannot build the convolution: ") + e.what());
      return nullptr;
    }
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

struct BiquadState
{
  // Normalised by a0, so the difference equation is
  //   y = b0*x + s1;  s1 = b1*x - a1*y + s2;  s2 = b2*x - a2*y
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float s1 = 0.0f;
  float s2 = 0.0f;
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

  const double rate = options.sample_rate;
  const double freq = number_param(spec, "freq", 1000.0);
  const double q = number_param(spec, "q", 0.707);
  const double gain_db = number_param(spec, "gain_db", 0.0);

  if (!std::isfinite(freq) || !std::isfinite(q) || !std::isfinite(gain_db))
  {
    error = reject(type, "freq, q and gain_db must be finite");
    return nullptr;
  }
  // Reject rather than clamp: an EQ that silently ignores a 30 kHz setting is a bug report about
  // "the EQ does nothing", and one that silently wraps is a bug report about noise.
  if (freq <= 0.0 || freq >= 0.5 * rate)
  {
    error = reject(type, "freq " + quoted(freq) + " Hz is outside (0, " + quoted(0.5 * rate) + ") Hz");
    return nullptr;
  }
  if (q <= 0.0)
  {
    error = reject(type, "q must be > 0, got " + quoted(q));
    return nullptr;
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
    error = reject(type, "the coefficients are degenerate at these settings");
    return nullptr;
  }

  auto state = std::make_unique<BiquadState>();
  state->b0 = static_cast<float>(b0 / a0);
  state->b1 = static_cast<float>(b1 / a0);
  state->b2 = static_cast<float>(b2 / a0);
  state->a1 = static_cast<float>(a1 / a0);
  state->a2 = static_cast<float>(a2 / a0);
  return state.release();
}

void destroy_biquad(void* state)
{
  delete static_cast<BiquadState*>(state);
}

void process_biquad(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  BiquadState* state = static_cast<BiquadState*>(raw);
  const float b0 = state->b0;
  const float b1 = state->b1;
  const float b2 = state->b2;
  const float a1 = state->a1;
  const float a2 = state->a2;
  float s1 = state->s1;
  float s2 = state->s2;
  for (int i = 0; i < frames; ++i)
  {
    const float x = in[0][i];
    const float y = b0 * x + s1;
    s1 = b1 * x - a1 * y + s2;
    s2 = b2 * x - a2 * y;
    out[0][i] = y;
  }
  state->s1 = s1;
  state->s2 = s2;
}

// --- nc.delay --------------------------------------------------------------
//
// A plain fixed delay, which is also the only way to write feedback: §5.1 makes a cycle a compile
// error, so a feedback loop is "split, delay, gain, mix" with the delay breaking the cycle.

struct DelayState
{
  std::vector<float> line;
  std::size_t write = 0;
  std::size_t read = 0;

  void advance() noexcept
  {
    if (++write == line.size())
      write = 0;
    if (++read == line.size())
      read = 0;
  }
};

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

  // A delay shorter than a sample is not a delay, and a longer one is a user error worth naming --
  // silently allocating 10 minutes of ring for a typo'd 'ms' is worse than refusing.
  const double max_samples = 60.0 * options.sample_rate;
  if (!std::isfinite(samples) || samples < 1.0 || samples > max_samples)
  {
    error = reject(type, "delay must be between 1 sample and " + quoted(max_samples) + " samples, got " +
                           quoted(samples));
    return nullptr;
  }

  auto state = std::make_unique<DelayState>();
  // One more than the delay so that read and write can never be the same slot: read-before-write at
  // a shared index would also work, but this way the read pointer is visibly `delay` behind.
  state->line.assign(static_cast<std::size_t>(samples) + 1, 0.0f);
  state->write = 0;
  state->read = state->line.size() - static_cast<std::size_t>(samples); // == 1
  return state.release();
}

void destroy_delay(void* state)
{
  delete static_cast<DelayState*>(state);
}

void process_delay(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  DelayState* state = static_cast<DelayState*>(raw);
  for (int i = 0; i < frames; ++i)
  {
    out[0][i] = state->line[state->read];
    state->line[state->write] = in[0][i];
    state->advance();
  }
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
  {.id = "outputs", .name = "Outputs", .kind = ParamKind::Flag, .default_value = 2.0, .has_min = true,
   .min = 1.0, .has_max = true, .max = 64.0, .step = 1.0, .snap = true},
};

const ParamDesc kMixParams[] = {
  {.id = "inputs", .name = "Inputs", .kind = ParamKind::Flag, .default_value = 2.0, .has_min = true,
   .min = 1.0, .has_max = true, .max = 64.0, .step = 1.0, .snap = true},
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
  // Not rampable yet, and deliberately: interpolating a biquad's *coefficients* is not the same thing
  // as interpolating a value, so a smooth freq/q/gain needs an in-node crossfade between the old and
  // new coefficient sets. Until that exists, a change here is the structural class -- dual-rendered
  // and crossfaded at the plan level, which is glitch-free by construction
  // (docs/ui-integration-plan.md §2, slice B3).
  // No maximum: the real limit is Nyquist at whatever rate the rig runs at, which the schema does not
  // know and a fixed 20 kHz would get wrong at 96 kHz. The node's own check owns that bound and says
  // so, which is the difference between a schema limit and an engine limit.
  {.id = "freq", .name = "Frequency", .unit = "Hz", .default_value = 1000.0, .has_min = true,
   .min = 10.0, .min_inclusive = false, .scale = ParamScale::Log},
  {.id = "q", .name = "Q", .default_value = 0.707, .has_min = true, .min = 0.0,
   .min_inclusive = false, .has_max = true, .max = 20.0, .scale = ParamScale::Log},
  {.id = "gain_db", .name = "Gain", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -30.0,
   .has_max = true, .max = 30.0},
};

const ParamDesc kDelayParams[] = {
  // Same reasoning as the biquad: moving a read pointer needs a crossfade between two taps rather
  // than an interpolated value, so changing either spelling is the structural class for now.
  // Bounded by the engine, not by the schema: 60 seconds at the rig's rate, in samples. A client
  // showing a slider picks its own display range when a parameter is unbounded here.
  {.id = "samples", .name = "Delay", .unit = "samples", .has_min = true, .min = 1.0, .step = 1.0,
   .snap = true, .one_of = "delay time"},
  {.id = "ms", .name = "Delay", .unit = "ms", .has_min = true, .min = 0.021, .scale = ParamScale::Log,
   .one_of = "delay time"},
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
                    .process = process_mix, .arity_param = "inputs", .min_ports = 1, .max_ports = 64,
                    .params = kMixParams, .param_count = 1};
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
                       .process = process_biquad, .params = kBiquadParams, .param_count = 4};
const NodeType kDelay{.id = "nc.delay", .name = "Delay", .category = "delay", .inputs = 1, .outputs = 1,
                      .create = create_delay, .destroy = destroy_delay, .process = process_delay,
                      .params = kDelayParams, .param_count = 2};
// A scheduling primitive rather than a pedal: `block_delay` is what breaks a feedback cycle, and a
// palette should leave it out, which is what the flag is for.
const NodeType kUnitDelay{"nc.unit_delay", "Unit delay", "utility", 1, 1, nullptr, nullptr,
                          process_unit_delay, nullptr, nullptr, true};

} // namespace

/// Every registered type, in the order `engine.schema` reports them: the palette's order, grouped by
/// what a player is looking for rather than by when each was written.
const NodeType* const kNodeTypes[] = {&kInput,  &kOutput, &kGain,  &kBiquad, &kDelay, &kNam,
                                      &kCabIr,  &kSplit,  &kMix,   &kBypass, &kUnitDelay};

const NodeType* find_node_type(const std::string& id)
{
  for (const NodeType* type : kNodeTypes)
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
