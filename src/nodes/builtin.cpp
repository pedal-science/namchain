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

struct GainState
{
  float gain = 1.0f;
};

void* create_gain(const graph::NodeSpec& spec, const CompileOptions&, std::string& error)
{
  auto state = std::make_unique<GainState>();
  if (has_param(spec, "gain_db"))
    state->gain = static_cast<float>(std::pow(10.0, number_param(spec, "gain_db", 0.0) / 20.0));
  else
    state->gain = static_cast<float>(number_param(spec, "gain", 1.0));

  if (!std::isfinite(state->gain))
  {
    error = reject("nc.gain", "gain is not finite");
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

// Field order matches NodeType: id, inputs, outputs, create, destroy, process, describe,
// set_block_input, block_delay.
const NodeType kInput{"nc.input", 0, 1, create_input, destroy_input, process_input, nullptr, set_block_input};
const NodeType kOutput{"nc.output", 1, 0, nullptr, nullptr, process_marker};
const NodeType kGain{"nc.gain", 1, 1, create_gain, destroy_gain, process_gain};
const NodeType kSplit{"nc.split", 1, 2, nullptr, nullptr, process_split};
const NodeType kMix{"nc.mix", 2, 1, nullptr, nullptr, process_mix};
const NodeType kNam{"nc.nam", 1, 1, create_nam, destroy_dsp, process_dsp, describe_dsp};
const NodeType kCabIr{"nc.cab.ir", 1, 1, create_cab_ir, destroy_dsp, process_dsp, describe_dsp};
const NodeType kBiquad{"nc.eq.biquad", 1, 1, create_biquad, destroy_biquad, process_biquad};
const NodeType kDelay{"nc.delay", 1, 1, create_delay, destroy_delay, process_delay};
const NodeType kUnitDelay{"nc.unit_delay", 1, 1, nullptr, nullptr, process_unit_delay, nullptr, nullptr, true};

} // namespace

const NodeType* find_node_type(const std::string& id)
{
  if (id == kInput.id) return &kInput;
  if (id == kOutput.id) return &kOutput;
  if (id == kGain.id) return &kGain;
  if (id == kSplit.id) return &kSplit;
  if (id == kMix.id) return &kMix;
  if (id == kNam.id) return &kNam;
  if (id == kCabIr.id) return &kCabIr;
  if (id == kBiquad.id) return &kBiquad;
  if (id == kDelay.id) return &kDelay;
  if (id == kUnitDelay.id) return &kUnitDelay;
  return nullptr;
}

} // namespace render
} // namespace namchain
