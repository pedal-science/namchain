#pragma once

// flowEQ's equaliser, re-implemented in C++ from the MATLAB plugin.
//
// flowEQ (https://github.com/csteinmetz1/flowEQ) is a disentangled variational autoencoder over the
// thirteen parameters of a five band parametric EQ. Its decoder is a two-layer network -- three
// latent values, 1024 ReLU hidden units, thirteen sigmoids -- and it is small enough to run as a
// plain forward pass; the weights are the ones the plugin's `plugin/assets/vae3d_beta_0.0010.mat`
// carries, converted to a C++ table by `tools/floweq/generate_model.py`. The five filters are the
// plugin's own (JUCE-shaped low shelf, three peaking bands, high shelf), and so is the gain
// compensation.
//
// What is *not* here: the full plugin's semantic mode (embeddings, interpolation), its latent-dim
// and beta model selectors, manual mode, and the per-band active switches. This is flowEQ-lite: one
// model, three latent dimensions, strength, extend, in/out gain and auto gain -- which is what the
// pedal's one screen can show at once. docs/floweq.md is the design and the provenance; the node
// itself is nc.eq.flow (`src/nodes/floweq.cpp`).

#include "namchain/floweq/model.h"

namespace namchain
{
namespace render
{
struct NodeType;
/// The node type, registered in `src/nodes/builtin.cpp`.
extern const NodeType kFlowEq;
} // namespace render

namespace floweq
{

inline constexpr int kParamCount = 13;
/// Low shelf, three peaking bands, high shelf, in that order: the plugin's signal path.
inline constexpr int kSectionCount = 5;

/// \brief The thirteen parameters, in the decoder's (and the plugin's) order.
///
/// The gains carry the full range the decoder can produce; `strength` scales them before the
/// coefficients are built, so these stay the model's own values and a report can show both.
enum ParamIndex
{
  kLowShelfGain = 0,
  kLowShelfFreq,
  kBand1Gain,
  kBand1Freq,
  kBand1Q,
  kBand2Gain,
  kBand2Freq,
  kBand2Q,
  kBand3Gain,
  kBand3Freq,
  kBand3Q,
  kHighShelfGain,
  kHighShelfFreq,
};

/// \brief Run the decoder: a latent vector to the thirteen parameters.
///
/// \param latent The three latent values, already scaled for the plugin's "Extend" (x2).
/// \param params Receives the thirteen parameters, denormalised to their nominal ranges (a gain in
///        dB, a frequency in Hz, a Q). The plugin's ranges are in `model.h`.
///
/// Control thread or render path; no allocation, and roughly 18 000 multiply-adds.
void decode(const float latent[3], float params[kParamCount]);

/// One section's coefficients, normalised by a0: y = b0*x + s1, and so on, as `run_biquad` does.
struct Coeffs
{
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

/// \brief The five sections the decoder's parameters describe, at a sample rate.
///
/// The formulas are the plugin's (`flowEQ_lite.m`, "Coefficient Cooking", which follows JUCE's
/// shelf/peak design): the shelves take a Q of 0.71 and a gain, each peaking band takes its own Q
/// and gain, and \p strength multiplies every gain before it becomes a gain factor -- which is what
/// makes `strength = 0` the identity filter.
///
/// A frequency the decoder cannot produce is not a caller error here: at a low sample rate the top
/// band's own range reaches past Nyquist, so a frequency is clamped to (0, 0.45*rate) rather than
/// being allowed to design an aliased filter. The plugin assumes 44.1 or 48 kHz and has no guard;
/// the pedal runs at 48 kHz, and this is the one place the engine is stricter than the source.
void design(const float params[kParamCount], double strength, double rate, Coeffs sections[kSectionCount]);

/// \brief The magnitude response of those sections, in dB, at \p count log-spaced frequencies.
///
/// For a report -- the curve a client draws -- rather than for rendering: the node publishes it in
/// its `rig.state` entry. \p out is filled with \p count values from \p f_min to \p f_max.
void response_db(const float params[kParamCount], double strength, double rate, double f_min, double f_max,
                 int count, float* out);

} // namespace floweq
} // namespace namchain
