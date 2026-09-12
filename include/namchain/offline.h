#pragma once

#include <string>

namespace namchain
{

/// \brief Options for an offline (file-to-file) render.
struct OfflineRenderOptions
{
  /// Block size the model is driven with. Must be in [1, 4096]; the core asserts
  /// `frames <= maxBufferSize`, and `A2FastModel` silently reallocates if exceeded (G3).
  int quantum = 64;
  /// Apply `SetSlimmableSize` before rendering (needs a `nam::SlimmableModel`).
  bool apply_slim = false;
  /// Slimmable size in [0, 1] when \ref apply_slim is set.
  double slim_size = 1.0;
};

struct OfflineRenderStats
{
  int frames = 0;
  int input_channels = 0;
  int output_channels = 0;
  double sample_rate = 0.0;
  int quantum = 0;
};

struct OfflineRenderResult
{
  bool ok = false;
  std::string error;
  OfflineRenderStats stats;
};

/// \brief Render a `.nam` model over a WAV file and write the result to another WAV file.
///
/// This is the M0 offline path: one model, mono input, no graph. It exists to make the
/// engine's DSP results checkable against the vendored core's own `render` tool, and it is
/// the ancestor of the engine's `file` backend (M1). It is deliberately *not* the engine
/// render path: no arena, no graph, no transitions.
///
/// The engine rate is the model's expected rate; a WAV whose rate does not match is an
/// error rather than a silent resample, because the vendored core has no resampler (G11)
/// and resampling is an M1 I/O-layer concern (§9.2).
OfflineRenderResult render_model_to_wav(const std::string& model_path, const std::string& input_wav_path,
                                        const std::string& output_wav_path,
                                        const OfflineRenderOptions& options = {});

} // namespace namchain
