// nc.eq.flow: flowEQ-lite, the decoder and the five band EQ from flowEQ, as one node.
//
// flowEQ (Christian Steinmetz, BSD-3-Clause) is a disentangled variational autoencoder that maps a
// three dimensional latent space onto the thirteen parameters of a five band parametric EQ. The
// plugin's "lite" version is exactly one decoder and one EQ: x, y and z traverse the space, Extend
// doubles the reach, Strength scales every gain, Auto Gain matches the output loudness to the input,
// and In/Out Gain sit either side. That is this node. docs/floweq.md is the design and the
// provenance; docs/schema.md is what the parameters mean to a client.
//
// Three things are worth knowing before reading the render path:
//
//  * **The decoder runs once per block, not once per parameter.** A client dragging three knobs
//    sends three set_param dispatches in one block; they all only mark the coefficients dirty, and
//    `process` decodes once. The forward pass is ~18 000 multiply-adds, which is a round-off next
//    to a NAM model, but doing it three times per block for one gesture is still worth not doing.
//  * **A coefficient change is a crossfade, not an interpolation.** Exactly as nc.eq.biquad: the old
//    five sections keep running in a second lane and the two lanes' outputs are equal-power mixed.
//    Interpolating b0..a2 between two stable filters is not itself guaranteed stable, and moving the
//    latent gives no reason to expect it to be.
//  * **Auto gain is a measurement, not a knob.** It compares the K-weighted mean square (BS.1770's
//    short-term loudness weighting) of the signal before the filters with the signal after them and
//    drives the output gain toward the difference, clamped to the plugin's default +/-12 dB. The
//    output gain therefore follows a one-pole rather than a fixed ramp: it is tracking something
//    that moves every block.

#include "namchain/floweq.h"

#include "namchain/floweq/model.h"
#include "namchain/graph/plan.h"
#include "namchain/rt/crossfade.h"

#include <algorithm>
#include <array>
#include <complex>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <json.hpp>

namespace namchain
{
namespace floweq
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kShelfQ = 0.71; // flowEQ_lite.m: the shelves are built with a fixed Q.
} // namespace

void decode(const float latent[3], float params[kParamCount])
{
  std::array<float, kHidden> hidden{};
  for (int j = 0; j < kHidden; ++j)
  {
    float total = kB1[j];
    for (int i = 0; i < kLatentDims; ++i)
      total += latent[i] * kW1[i * kHidden + j];
    hidden[static_cast<std::size_t>(j)] = total > 0.0f ? total : 0.0f; // ReLU, as the Dense layer's activation
  }
  for (int k = 0; k < kParams; ++k)
  {
    float total = kB2[k];
    for (int j = 0; j < kHidden; ++j)
      total += hidden[static_cast<std::size_t>(j)] * kW2[j * kParams + k];
    const float y = 1.0f / (1.0f + std::exp(-total)); // sigmoid
    params[k] = y * (kMax[k] - kMin[k]) + kMin[k];
  }
}

/// The frequency one section is built at: the decoder's own value, held inside what the rate can
/// carry. See the header for why a model output is clamped rather than refused.
double usable_frequency(double freq, double rate)
{
  const double highest = std::max(2.0, 0.45 * rate);
  return std::clamp(freq, 2.0, highest);
}

/// The gain factor `strength` leaves a band with. The plugin multiplies the dB value and then takes
/// `10^(dB/20)`; doing the multiplication first is the whole of what strength does.
double gain_factor(double gain_db, double strength)
{
  return std::pow(10.0, (gain_db * strength) / 20.0);
}

/// A section's coefficients from the plugin's snippets, normalised by a0.
bool low_shelf(double rate, double freq, double q, double factor, Coeffs& out)
{
  const double A = std::max(0.0, std::sqrt(factor));
  const double aminus1 = A - 1.0;
  const double aplus1 = A + 1.0;
  const double omega = (2.0 * kPi * usable_frequency(freq, rate)) / rate;
  const double coso = std::cos(omega);
  const double beta = std::sin(omega) * std::sqrt(A) / q;
  const double aminus1TimesCoso = aminus1 * coso;

  const double b0 = A * (aplus1 - aminus1TimesCoso + beta);
  const double b1 = A * 2.0 * (aminus1 - aplus1 * coso);
  const double b2 = A * (aplus1 - aminus1TimesCoso - beta);
  const double a0 = aplus1 + aminus1TimesCoso + beta;
  const double a1 = -2.0 * (aminus1 + aplus1 * coso);
  const double a2 = aplus1 + aminus1TimesCoso - beta;
  if (!std::isfinite(a0) || a0 == 0.0)
    return false;
  out = {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0), static_cast<float>(b2 / a0),
         static_cast<float>(a1 / a0), static_cast<float>(a2 / a0)};
  return true;
}

bool high_shelf(double rate, double freq, double q, double factor, Coeffs& out)
{
  const double A = std::max(0.0, std::sqrt(factor));
  const double aminus1 = A - 1.0;
  const double aplus1 = A + 1.0;
  const double omega = (2.0 * kPi * usable_frequency(freq, rate)) / rate;
  const double coso = std::cos(omega);
  const double beta = std::sin(omega) * std::sqrt(A) / q;
  const double aminus1TimesCoso = aminus1 * coso;

  const double b0 = A * (aplus1 + aminus1TimesCoso + beta);
  const double b1 = A * -2.0 * (aminus1 + aplus1 * coso);
  const double b2 = A * (aplus1 + aminus1TimesCoso - beta);
  const double a0 = aplus1 - aminus1TimesCoso + beta;
  const double a1 = 2.0 * (aminus1 - aplus1 * coso);
  const double a2 = aplus1 - aminus1TimesCoso - beta;
  if (!std::isfinite(a0) || a0 == 0.0)
    return false;
  out = {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0), static_cast<float>(b2 / a0),
         static_cast<float>(a1 / a0), static_cast<float>(a2 / a0)};
  return true;
}

bool peaking(double rate, double freq, double q, double factor, Coeffs& out)
{
  const double A = std::max(0.0, std::sqrt(factor));
  const double omega = (2.0 * kPi * usable_frequency(freq, rate)) / rate;
  const double alpha = std::sin(omega) / (std::max(q, 1e-3) * 2.0);
  const double c2 = -2.0 * std::cos(omega);
  const double alphaTimesA = alpha * A;
  const double alphaOverA = alpha / std::max(A, 1e-12);

  const double b0 = 1.0 + alphaTimesA;
  const double b1 = c2;
  const double b2 = 1.0 - alphaTimesA;
  const double a0 = 1.0 + alphaOverA;
  const double a1 = c2;
  const double a2 = 1.0 - alphaOverA;
  if (!std::isfinite(a0) || a0 == 0.0)
    return false;
  out = {static_cast<float>(b0 / a0), static_cast<float>(b1 / a0), static_cast<float>(b2 / a0),
         static_cast<float>(a1 / a0), static_cast<float>(a2 / a0)};
  return true;
}

void design(const float params[kParamCount], double strength, double rate, Coeffs sections[kSectionCount])
{
  // The two shelves, then the three peaking bands, in the order the decoder emits them. A filter that
  // cannot be designed at this rate (a degenerate a0) is left as the identity rather than as
  // whatever the arithmetic produced: silence from a corner of the latent space would be a worse
  // answer than "this band does nothing", and it is not reachable from the decoder's own ranges.
  if (!low_shelf(rate, params[kLowShelfFreq], kShelfQ, gain_factor(params[kLowShelfGain], strength), sections[0]))
    sections[0] = Coeffs{};
  if (!peaking(rate, params[kBand1Freq], params[kBand1Q], gain_factor(params[kBand1Gain], strength), sections[1]))
    sections[1] = Coeffs{};
  if (!peaking(rate, params[kBand2Freq], params[kBand2Q], gain_factor(params[kBand2Gain], strength), sections[2]))
    sections[2] = Coeffs{};
  if (!peaking(rate, params[kBand3Freq], params[kBand3Q], gain_factor(params[kBand3Gain], strength), sections[3]))
    sections[3] = Coeffs{};
  if (!high_shelf(rate, params[kHighShelfFreq], kShelfQ, gain_factor(params[kHighShelfGain], strength),
                  sections[4]))
    sections[4] = Coeffs{};
}

void response_db(const float params[kParamCount], double strength, double rate, double f_min, double f_max,
                 int count, float* out)
{
  if (count <= 0)
    return;
  Coeffs sections[kSectionCount];
  design(params, strength, rate, sections);
  const double low = std::max(1e-3, f_min);
  const double high = std::max(low, f_max);
  for (int i = 0; i < count; ++i)
  {
    // Log-spaced: the axis a client draws is log, and evenly spaced points on it are what make the
    // curve look like the one every EQ displays.
    const double t = count == 1 ? 0.0 : static_cast<double>(i) / static_cast<double>(count - 1);
    const double freq = low * std::pow(high / low, t);
    const double omega = 2.0 * kPi * freq / rate;
    const std::complex<double> z(std::cos(omega), -std::sin(omega));
    const std::complex<double> z2 = z * z;
    std::complex<double> response(1.0, 0.0);
    for (const Coeffs& section : sections)
    {
      const std::complex<double> numerator = double(section.b0) + double(section.b1) * z + double(section.b2) * z2;
      const std::complex<double> denominator = 1.0 + double(section.a1) * z + double(section.a2) * z2;
      response *= numerator / denominator;
    }
    out[i] = static_cast<float>(20.0 * std::log10(std::max(std::abs(response), 1e-9)));
  }
}

} // namespace floweq

namespace render
{

namespace
{

using floweq::kParamCount;
using floweq::kSectionCount;

double number_param(const graph::NodeSpec& spec, const std::string& name, double fallback)
{
  const auto it = spec.params.find(name);
  return it == spec.params.end() ? fallback : it->second;
}

// Parameter ids are positions in the schema array plus one (plan.h, ParamDesc), and the tables below
// are written in that order.
constexpr uint32_t kFlowParamX = 1;
constexpr uint32_t kFlowParamY = 2;
constexpr uint32_t kFlowParamZ = 3;
constexpr uint32_t kFlowParamStrength = 4;
constexpr uint32_t kFlowParamInDb = 5;
constexpr uint32_t kFlowParamOutDb = 6;
constexpr uint32_t kFlowParamExtend = 7;
constexpr uint32_t kFlowParamAutoGain = 8;

/// The latent range the plugin's sliders reach, and therefore the schema's.
constexpr float kLatentLimit = 2.0f;
/// The plugin's default gain range for the automatic compensation (GainRange "12dB").
constexpr double kAutoGainLimitDb = 12.0;
/// BS.1770-4's short-term loudness window, which is what flowEQ's loudness meter reports.
constexpr double kLoudnessWindowSeconds = 3.0;
/// The frequency range the published curve covers: what flowEQ's own visualiser draws.
constexpr double kCurveMinHz = 20.0;
constexpr double kCurveMaxHz = 20000.0;
constexpr int kCurvePoints = 96;

/// One running section: the coefficients and the two states a direct-form-II-transposed biquad keeps.
struct Section
{
  floweq::Coeffs coeffs;
  float s1 = 0.0f;
  float s2 = 0.0f;
};

/// The five sections in series, in place: y = b0*x + s1 and so on, the same recurrence nc.eq.biquad
/// runs over its one section.
struct Chain
{
  Section sections[kSectionCount];
  /// \brief Every section is y = x, so running the chain would be running nothing.
  ///
  /// `strength = 0` scales every gain to 0 dB, which gives each section a0 == b0 and a1 == b1 and
  /// a2 == b2 exactly; the node then skips the filters rather than re-deriving the input from them.
  /// That is not only cheaper: under FMA contraction the all-identity recurrence is not *bit* exact
  /// (`b1*x - a1*y + s2` fuses to `fma(b1, x, s2) - a1*y`, which rounds differently even when the
  /// two products are equal), so a strength of zero would leave a -110 dB noise floor the plugin does
  /// not have. nc.mix has the same fast path for the same reason: a control at unity should be
  /// silent, not nearly silent.
  bool identity = true;

  /// Take new coefficients (never a new state) and re-derive \ref identity.
  void set_coefficients(const floweq::Coeffs* coefficients) noexcept
  {
    bool all_identity = true;
    for (int i = 0; i < kSectionCount; ++i)
    {
      sections[i].coeffs = coefficients[i];
      all_identity = all_identity && coefficients[i].b0 == 1.0f && coefficients[i].b1 == coefficients[i].a1 &&
                     coefficients[i].b2 == coefficients[i].a2;
    }
    identity = all_identity;
  }

  void run(float* buffer, int frames) noexcept
  {
    if (identity)
      return;
    for (Section& section : sections)
    {
      const float b0 = section.coeffs.b0;
      const float b1 = section.coeffs.b1;
      const float b2 = section.coeffs.b2;
      const float a1 = section.coeffs.a1;
      const float a2 = section.coeffs.a2;
      float s1 = section.s1;
      float s2 = section.s2;
      for (int i = 0; i < frames; ++i)
      {
        const float x = buffer[i];
        const float y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        buffer[i] = y;
      }
      section.s1 = s1;
      section.s2 = s2;
    }
  }
};

bool same_sections(const Chain& a, const Chain& b) noexcept
{
  for (int i = 0; i < kSectionCount; ++i)
  {
    const floweq::Coeffs& x = a.sections[i].coeffs;
    const floweq::Coeffs& y = b.sections[i].coeffs;
    if (x.b0 != y.b0 || x.b1 != y.b1 || x.b2 != y.b2 || x.a1 != y.a1 || x.a2 != y.a2)
      return false;
  }
  return true;
}

/// \brief BS.1770-4's K-weighting: the shelf and the high-pass a loudness meter runs first.
///
/// The same numbers libebur128 and pyloudnorm derive (the standard's coefficients are specified at
/// 48 kHz and these formulas reproduce them to the last digit at 48 kHz, and give the right filters
/// at any other rate). The mid/side and channel weights of the standard are not here: the engine is
/// mono, and a gain comparison needs no absolute loudness -- only the same weighting on both sides.
struct KWeighting
{
  floweq::Coeffs shelf;
  floweq::Coeffs highpass;
};

void design_k_weighting(double rate, KWeighting& out)
{
  // Stage 1: the high shelf.
  {
    constexpr double f0 = 1681.974450955533;
    constexpr double gain_db = 3.999843853973347;
    constexpr double q = 0.7071752369554196;
    const double k = std::tan(3.14159265358979323846 * f0 / rate);
    const double vh = std::pow(10.0, gain_db / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    out.shelf = {static_cast<float>((vh + vb * k / q + k * k) / a0),
                 static_cast<float>(2.0 * (k * k - vh) / a0),
                 static_cast<float>((vh - vb * k / q + k * k) / a0),
                 static_cast<float>(2.0 * (k * k - 1.0) / a0),
                 static_cast<float>((1.0 - k / q + k * k) / a0)};
  }
  // Stage 2: the RLB high-pass.
  {
    constexpr double f0 = 38.13547087602444;
    constexpr double q = 0.5003270373238773;
    const double k = std::tan(3.14159265358979323846 * f0 / rate);
    const double a0 = 1.0 + k / q + k * k;
    out.highpass = {1.0f, -2.0f, 1.0f, static_cast<float>(2.0 * (k * k - 1.0) / a0),
                    static_cast<float>((1.0 - k / q + k * k) / a0)};
  }
}

/// One channel's K-weighted mean square, folded sample by sample.
///
/// A one-pole over the squared, weighted signal rather than a ring of the last three seconds: the
/// mean square is what the meter needs, and an exponential window has the same long memory without
/// 576 KB of history per meter. The time constant is the standard's short-term window.
struct LoudnessMeter
{
  KWeighting weighting;
  float shelf_s1 = 0.0f;
  float shelf_s2 = 0.0f;
  float highpass_s1 = 0.0f;
  float highpass_s2 = 0.0f;
  double mean_square = 0.0;
  double coefficient = 1.0;

  void prepare(double rate)
  {
    design_k_weighting(rate, weighting);
    const double samples = std::max(1.0, kLoudnessWindowSeconds * rate);
    coefficient = 1.0 - std::exp(-1.0 / samples);
  }

  void process(const float* signal, int frames) noexcept
  {
    const floweq::Coeffs& shelf = weighting.shelf;
    const floweq::Coeffs& highpass = weighting.highpass;
    float s1 = shelf_s1;
    float s2 = shelf_s2;
    float h1 = highpass_s1;
    float h2 = highpass_s2;
    double ms = mean_square;
    for (int i = 0; i < frames; ++i)
    {
      const float x = signal[i];
      const float y = shelf.b0 * x + s1;
      s1 = shelf.b1 * x - shelf.a1 * y + s2;
      s2 = shelf.b2 * x - shelf.a2 * y;
      const float z = highpass.b0 * y + h1;
      h1 = highpass.b1 * y - highpass.a1 * z + h2;
      h2 = highpass.b2 * y - highpass.a2 * z;
      ms += (static_cast<double>(z) * static_cast<double>(z) - ms) * coefficient;
    }
    shelf_s1 = s1;
    shelf_s2 = s2;
    highpass_s1 = h1;
    highpass_s2 = h2;
    mean_square = ms;
  }
};

/// What a control-thread client can see of a running node. Written by the render thread under
/// \ref FlowEqState::published (an epoch, odd while writing), read through \ref read_report.
struct Report
{
  float latent[3] = {0.0f, 0.0f, 0.0f};
  float strength = 1.0f;
  float in_db = 0.0f;
  float out_db = 0.0f;
  float gain_db = 0.0f;
  float params[kParamCount] = {};
  bool extend = false;
  bool auto_gain = false;
};

struct FlowEqState
{
  // The report is written on the render path and read on the control thread; an atomic that took a
  // lock would be a lock on the render path.
  static_assert(std::atomic<float>::is_always_lock_free, "the flowEQ report must be lock-free");
  static_assert(std::atomic<uint32_t>::is_always_lock_free, "the flowEQ report's epoch must be lock-free");

  explicit FlowEqState(int max_fade) : fade(max_fade) {}

  // --- the parameters, as last set -------------------------------------------------------------
  float latent[3] = {0.0f, 0.0f, 0.0f};
  float strength = 1.0f;
  bool extend = false;
  bool auto_gain = false;
  float in_db = 0.0f;
  float out_db = 0.0f;
  /// The decoded parameters the coefficients were built from: the report carries them, and the
  /// "did anything actually move" comparison is made on the sections rather than on these.
  float current_params[kParamCount] = {};
  /// A set_param touched a parameter the coefficients depend on; `process` rebuilds them once.
  bool rebuild = true;

  // --- filters: the lane the output follows, and the one it is leaving --------------------------
  Chain current;
  Chain previous;
  rt::Crossfade fade;
  bool fading = false;

  // --- gains ------------------------------------------------------------------------------------
  float in_gain = 1.0f;
  float in_target = 1.0f;
  float out_gain = 1.0f;
  /// The one-pole coefficient both gains move with: `ramp_ms` is the time constant.
  float gain_coefficient = 1.0f;
  int smoothing_samples = 240; ///< the crossfade length, for a coefficient change
  double rate = 48000.0;

  // --- loudness compensation -------------------------------------------------------------------
  LoudnessMeter pre_meter;
  LoudnessMeter post_meter;

  // --- render-path scratch, sized at build -----------------------------------------------------
  std::vector<float> old_buf;
  std::vector<float> new_buf;

  // --- the report (see \ref Report) -------------------------------------------------------------
  std::atomic<uint32_t> published{0};
  std::atomic<float> report_latent[3];
  std::atomic<float> report_strength{1.0f};
  std::atomic<float> report_in_db{0.0f};
  std::atomic<float> report_out_db{0.0f};
  std::atomic<float> report_gain_db{0.0f};
  std::atomic<float> report_params[kParamCount];
  std::atomic<uint32_t> report_flags{0};

  FlowEqState(const FlowEqState&) = delete;
  FlowEqState& operator=(const FlowEqState&) = delete;
};

/// Render path: publish what the render thread has derived, so a control-thread reader sees either
/// the state before or the state after a change, never a mixture of the two.
void publish(FlowEqState& state) noexcept
{
  const uint32_t epoch = state.published.load(std::memory_order_relaxed);
  state.published.store(epoch + 1, std::memory_order_relaxed); // odd: a write is in progress
  std::atomic_thread_fence(std::memory_order_release);
  for (int i = 0; i < 3; ++i)
    state.report_latent[i].store(state.latent[i], std::memory_order_relaxed);
  state.report_strength.store(state.strength, std::memory_order_relaxed);
  state.report_in_db.store(state.in_db, std::memory_order_relaxed);
  state.report_out_db.store(state.out_db, std::memory_order_relaxed);
  state.report_gain_db.store(state.out_gain > 0.0f ? static_cast<float>(20.0 * std::log10(state.out_gain)) : -120.0f,
                             std::memory_order_relaxed);
  for (int i = 0; i < kParamCount; ++i)
    state.report_params[i].store(state.current_params[i], std::memory_order_relaxed);
  state.report_flags.store((state.extend ? 1u : 0u) | (state.auto_gain ? 2u : 0u), std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  state.published.store(epoch + 2, std::memory_order_relaxed); // even: settled
}

bool read_report(const FlowEqState& state, Report& out)
{
  for (int attempt = 0; attempt < 8; ++attempt)
  {
    const uint32_t before = state.published.load(std::memory_order_acquire);
    if (before & 1u)
      continue; // a write is in progress
    for (int i = 0; i < 3; ++i)
      out.latent[i] = state.report_latent[i].load(std::memory_order_relaxed);
    out.strength = state.report_strength.load(std::memory_order_relaxed);
    out.in_db = state.report_in_db.load(std::memory_order_relaxed);
    out.out_db = state.report_out_db.load(std::memory_order_relaxed);
    out.gain_db = state.report_gain_db.load(std::memory_order_relaxed);
    for (int i = 0; i < kParamCount; ++i)
      out.params[i] = state.report_params[i].load(std::memory_order_relaxed);
    const uint32_t flags = state.report_flags.load(std::memory_order_relaxed);
    out.extend = (flags & 1u) != 0;
    out.auto_gain = (flags & 2u) != 0;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (before == state.published.load(std::memory_order_relaxed))
      return true;
  }
  return false;
}

/// Render path: the decoder and the five sections, from the parameters as they stand.
///
/// Called at most once a block, and only when a set_param asked for it. The coefficients go into the
/// incoming lane; whether that needs a crossfade is \ref update_coefficients' question.
void decode_and_design(FlowEqState& state, floweq::Coeffs sections[kSectionCount])
{
  float latent[3] = {state.latent[0], state.latent[1], state.latent[2]};
  if (state.extend)
  {
    for (float& value : latent)
      value *= 2.0f;
  }
  floweq::decode(latent, state.current_params);
  floweq::design(state.current_params, state.strength, state.rate, sections);
}

float gain_from_db(float db) noexcept
{
  return std::pow(10.0f, db / 20.0f);
}

/// What the output gain is heading for: the Out knob, or the loudness match when Auto Gain is on.
float output_target(const FlowEqState& state) noexcept
{
  if (!state.auto_gain)
    return gain_from_db(state.out_db);
  const double pre = state.pre_meter.mean_square;
  const double post = state.post_meter.mean_square;
  // Nothing to compare yet (silence, or the first block of a plan): hold the gain rather than move it
  // toward a ratio of two numbers that are both round-off.
  if (!(pre > 1e-20) || !(post > 1e-20))
    return state.out_gain;
  double db = 10.0 * std::log10(pre / post);
  db = std::clamp(db, -kAutoGainLimitDb, kAutoGainLimitDb);
  return static_cast<float>(std::pow(10.0, db / 20.0));
}

void rebuild_coefficients(FlowEqState& state) noexcept
{
  state.rebuild = false;
  floweq::Coeffs sections[kSectionCount];
  decode_and_design(state, sections);

  Chain incoming = state.current;
  incoming.set_coefficients(sections);
  if (same_sections(incoming, state.current))
    return; // nothing moved (a duplicate value): no fade, nothing to mix

  if (!state.fade.active())
  {
    // The old lane takes the signal the current one has been carrying, and the new lane starts with
    // the old lane's *memory*: the two states are the filter's record of the signal, not of the
    // coefficients, so a section that starts from zero would forget the last few milliseconds and
    // thump. This is the same choice nc.eq.biquad makes, for the same reason.
    state.previous = state.current;
    // `incoming` already carries the states: it was copied from `current`, and only the coefficients
    // were replaced.
    state.current = incoming;
    state.fade.start(state.smoothing_samples);
    state.fading = true;
  }
  else
  {
    // A second change while a crossfade is running retargets the incoming lane and lets the fade
    // continue: restarting it would jump the mix weights.
    state.current = incoming;
  }
}

void* create_floweq(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
{
  const std::string type = "nc.eq.flow";

  const double ramp_ms = number_param(spec, "ramp_ms", 5.0);
  if (!std::isfinite(ramp_ms) || ramp_ms < 0.0)
  {
    error = type + ": 'ramp_ms' must be a finite number >= 0";
    return nullptr;
  }
  const int smoothing =
    ramp_ms == 0.0 ? 1 : std::max(1, static_cast<int>(std::llround(ramp_ms * options.sample_rate / 1000.0)));
  // The gain's one-pole has the same time constant as the coefficient crossfade, which is what makes
  // `ramp_ms = 0` mean "everything immediate" rather than "immediate filters, slow gains".
  const float gain_coefficient =
    ramp_ms == 0.0 ? 1.0f
                   : static_cast<float>(1.0 - std::exp(-1.0 / (ramp_ms / 1000.0 * options.sample_rate)));

  auto state = std::make_unique<FlowEqState>(smoothing);
  state->rate = options.sample_rate;
  state->smoothing_samples = smoothing;
  state->gain_coefficient = gain_coefficient;

  state->latent[0] = static_cast<float>(std::clamp(number_param(spec, "x", 0.0), -2.0, 2.0));
  state->latent[1] = static_cast<float>(std::clamp(number_param(spec, "y", 0.0), -2.0, 2.0));
  state->latent[2] = static_cast<float>(std::clamp(number_param(spec, "z", 0.0), -2.0, 2.0));
  state->strength = static_cast<float>(std::clamp(number_param(spec, "strength", 1.0), 0.0, 1.0));
  state->in_db = static_cast<float>(number_param(spec, "in_db", 0.0));
  state->out_db = static_cast<float>(number_param(spec, "out_db", 0.0));
  state->extend = number_param(spec, "extend", 0.0) != 0.0;
  state->auto_gain = number_param(spec, "auto_gain", 0.0) != 0.0;
  state->in_target = gain_from_db(state->in_db);
  state->in_gain = state->in_target;
  // The plugin starts its loudness compensation at unity and lets it settle, whatever Out says: with
  // Auto Gain on, Out is not in the path.
  state->out_gain = state->auto_gain ? 1.0f : gain_from_db(state->out_db);

  state->pre_meter.prepare(options.sample_rate);
  state->post_meter.prepare(options.sample_rate);
  state->old_buf.assign(static_cast<std::size_t>(std::max(1, options.max_quantum)), 0.0f);
  state->new_buf.assign(static_cast<std::size_t>(std::max(1, options.max_quantum)), 0.0f);

  for (std::atomic<float>& value : state->report_params)
    value.store(0.0f);
  // The first build is settled: there is no previous lane to crossfade from, and the node starts
  // playing the curve the document asked for rather than fading into it from the identity.
  floweq::Coeffs sections[kSectionCount];
  decode_and_design(*state, sections);
  state->current.set_coefficients(sections);
  state->rebuild = false;
  publish(*state);
  return state.release();
}

void destroy_floweq(void* raw)
{
  delete static_cast<FlowEqState*>(raw);
}

bool set_param_floweq(void* raw, uint32_t param_id, float value) noexcept
{
  if (!std::isfinite(value))
    return false;
  FlowEqState* state = static_cast<FlowEqState*>(raw);
  switch (param_id)
  {
  case kFlowParamX: state->latent[0] = std::clamp(value, -kLatentLimit, kLatentLimit); state->rebuild = true; break;
  case kFlowParamY: state->latent[1] = std::clamp(value, -kLatentLimit, kLatentLimit); state->rebuild = true; break;
  case kFlowParamZ: state->latent[2] = std::clamp(value, -kLatentLimit, kLatentLimit); state->rebuild = true; break;
  case kFlowParamStrength: state->strength = std::clamp(value, 0.0f, 1.0f); state->rebuild = true; break;
  case kFlowParamInDb: state->in_db = value; state->in_target = gain_from_db(value); break;
  case kFlowParamOutDb: state->out_db = value; break;
  case kFlowParamExtend: state->extend = value != 0.0f; state->rebuild = true; break;
  case kFlowParamAutoGain: state->auto_gain = value != 0.0f; break;
  default: return false;
  }
  return true;
}

void process_floweq(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  FlowEqState* state = static_cast<FlowEqState*>(raw);
  if (state->rebuild)
    rebuild_coefficients(*state);

  const float* input = in[0];
  float* output = out[0];

  if (state->fading)
  {
    // Input gain into both lanes, so the two filters see the same signal and only their coefficients
    // differ; the input meter reads the same buffer, before either of them.
    for (int i = 0; i < frames; ++i)
    {
      state->in_gain += (state->in_target - state->in_gain) * state->gain_coefficient;
      const float x = input[i] * state->in_gain;
      state->old_buf[static_cast<std::size_t>(i)] = x;
      state->new_buf[static_cast<std::size_t>(i)] = x;
    }
    state->pre_meter.process(state->old_buf.data(), frames);
    state->previous.run(state->old_buf.data(), frames);
    state->current.run(state->new_buf.data(), frames);
    state->fade.mix(state->old_buf.data(), state->new_buf.data(), output, frames);
    if (!state->fade.active())
      state->fading = false;
    state->post_meter.process(output, frames);
  }
  else
  {
    for (int i = 0; i < frames; ++i)
    {
      state->in_gain += (state->in_target - state->in_gain) * state->gain_coefficient;
      output[i] = input[i] * state->in_gain;
    }
    state->pre_meter.process(output, frames);
    state->current.run(output, frames);
    state->post_meter.process(output, frames);
  }

  // The output gain: one pole, because with Auto Gain on the target is a measurement that moves a
  // little every block. Snap when it is close, so a settled node does not accumulate round-off.
  const float target = output_target(*state);
  float gain = state->out_gain;
  for (int i = 0; i < frames; ++i)
  {
    gain += (target - gain) * state->gain_coefficient;
    output[i] *= gain;
  }
  if (std::abs(target - gain) < 1e-6f)
    gain = target;
  state->out_gain = gain;

  publish(*state);
}

const ParamDesc kFlowEqParams[] = {
  // The latent space. `-2..2` is the plugin's slider range; Extend doubles what that reaches.
  {.id = "x", .name = "X", .default_value = 0.0, .has_min = true, .min = -2.0, .has_max = true, .max = 2.0,
   .rampable = true},
  {.id = "y", .name = "Y", .default_value = 0.0, .has_min = true, .min = -2.0, .has_max = true, .max = 2.0,
   .rampable = true},
  {.id = "z", .name = "Z", .default_value = 0.0, .has_min = true, .min = -2.0, .has_max = true, .max = 2.0,
   .rampable = true},
  // A fraction, published as a percentage: it scales every band's gain, so 0 is the identity filter.
  {.id = "strength", .name = "Strength", .unit = "%", .default_value = 1.0, .has_min = true, .min = 0.0,
   .has_max = true, .max = 1.0, .rampable = true},
  // The plugin's In/Out Gain, in dB. In is before the decoder's filters, Out after them; with Auto
  // Gain on, Out is not in the path (the loudness match replaces it), as in the plugin.
  {.id = "in_db", .name = "In", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -60.0,
   .has_max = true, .max = 12.0, .rampable = true},
  {.id = "out_db", .name = "Out", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -60.0,
   .has_max = true, .max = 12.0, .rampable = true},
  // Extend: the latent values are doubled before the decoder, which reaches further into the space.
  {.id = "extend", .name = "Extend", .kind = ParamKind::Flag, .default_value = 0.0, .rampable = true},
  {.id = "auto_gain", .name = "Auto gain", .kind = ParamKind::Flag, .default_value = 0.0, .rampable = true},
  // The smoothing time: a coefficient change crossfades over it, and the gains move with it. Hidden
  // from an inspector on purpose -- flowEQ's lite panel has no such control, and a generic "Ramp"
  // knob on a tone box is noise. A document may still set it, which is how it is tuned.
  {.id = "ramp_ms", .name = "Ramp", .unit = "ms", .default_value = 5.0, .has_min = true, .min = 0.0,
   .has_max = true, .max = 1000.0, .hidden = true},
};

/// \brief What the running node has derived, for a client that wants to draw it (docs/daemon.md,
/// `rig.state`).
///
/// Control thread. The values come from the render thread's report, and the curve is computed here,
/// from the same parameters the report carries, so the node has one answer for "what is running"
/// rather than two.
std::string describe_floweq(void* raw)
{
  auto* state = static_cast<FlowEqState*>(raw);
  if (state == nullptr)
    return {};
  Report report;
  if (!read_report(*state, report))
    return {};

  const double rate = state->rate;
  const double f_max = std::min(kCurveMaxHz, 0.45 * rate);
  float points[kCurvePoints];
  floweq::response_db(report.params, report.strength, rate, kCurveMinHz, f_max, kCurvePoints, points);

  // The gains in force, after strength: what the curve shows and what a band readout should say.
  const auto gain = [&report](int index) {
    return static_cast<double>(report.params[index]) * static_cast<double>(report.strength);
  };
  nlohmann::json bands = nlohmann::json::array();
  const auto band = [&bands, &report, &gain](const char* kind, int gain_index, int freq_index, double q) {
    bands.push_back({{"type", kind},
                     {"gain_db", std::round(gain(gain_index) * 100.0) / 100.0},
                     {"freq", std::round(static_cast<double>(report.params[freq_index]) * 100.0) / 100.0},
                     {"q", std::round(q * 1000.0) / 1000.0}});
  };
  band("lowshelf", floweq::kLowShelfGain, floweq::kLowShelfFreq, 0.71);
  band("peaking", floweq::kBand1Gain, floweq::kBand1Freq, report.params[floweq::kBand1Q]);
  band("peaking", floweq::kBand2Gain, floweq::kBand2Freq, report.params[floweq::kBand2Q]);
  band("peaking", floweq::kBand3Gain, floweq::kBand3Freq, report.params[floweq::kBand3Q]);
  band("highshelf", floweq::kHighShelfGain, floweq::kHighShelfFreq, 0.71);

  nlohmann::json curve_points = nlohmann::json::array();
  for (int i = 0; i < kCurvePoints; ++i)
    curve_points.push_back(std::round(static_cast<double>(points[i]) * 100.0) / 100.0);

  nlohmann::json state_json;
  state_json["model"] = floweq::kModelName;
  state_json["latent"] = {report.latent[0], report.latent[1], report.latent[2]};
  state_json["strength"] = std::round(static_cast<double>(report.strength) * 1000.0) / 1000.0;
  state_json["extend"] = report.extend;
  state_json["auto_gain"] = report.auto_gain;
  state_json["in_db"] = std::round(static_cast<double>(report.in_db) * 100.0) / 100.0;
  state_json["out_db"] = std::round(static_cast<double>(report.out_db) * 100.0) / 100.0;
  // What the output gain actually is, which with Auto Gain on is not Out.
  state_json["gain_db"] = std::round(static_cast<double>(report.gain_db) * 100.0) / 100.0;
  state_json["bands"] = std::move(bands);
  state_json["curve"] = {{"f_min", kCurveMinHz}, {"f_max", f_max}, {"points", std::move(curve_points)}};
  return state_json.dump();
}

} // namespace

// Field order matches NodeType (plan.h). `describe_state` is the control thread's view of what this
// node has derived -- see describe_floweq.
const NodeType kFlowEq{.id = "nc.eq.flow",
                       .name = "flowEQ",
                       .category = "eq",
                       .inputs = 1,
                       .outputs = 1,
                       .create = create_floweq,
                       .destroy = destroy_floweq,
                       .process = process_floweq,
                       .params = kFlowEqParams,
                       .param_count = 9,
                       .set_param = set_param_floweq,
                       .describe_state = describe_floweq};

} // namespace render
} // namespace namchain
