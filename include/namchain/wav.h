#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace namchain
{

/// \brief Load a mono WAV file as float samples.
///
/// Reading is delegated to AudioDSPTools' loader (the same one the vendored core's own
/// `render` tool uses), so both paths decode identical float values -- which is what makes
/// the golden comparison in tests/golden mean anything.
///
/// Only mono input is supported at M0; multichannel input is a job for the `file` backend
/// in M1. `error` is set on failure.
bool load_wav_mono(const std::string& path, std::vector<float>& samples, double& sample_rate, std::string& error);

/// \brief Write interleaved float samples as a 32-bit IEEE-float WAV (format code 3).
///
/// Own implementation rather than the core's, which is a mono-only helper local to its
/// `render.cpp`: the engine needs a channel-count-aware writer for the `file` backend, and
/// writing it here keeps the WAV dependency inside libnamchain.
bool save_wav_float32(const std::string& path, const float* interleaved, size_t frames, int channels,
                      double sample_rate, std::string& error);

} // namespace namchain
