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

/// \brief Sample-by-sample difference between two mono WAV files.
struct WavDiff
{
  size_t frames = 0;
  double max_abs = 0.0;
  size_t max_abs_index = 0;
  double rms = 0.0;      ///< RMS of the difference, in linear units.
  double peak_a = 0.0;   ///< Peak of the first file, for putting \ref rms in context.
};

enum class WavCompareStatus
{
  Match,  ///< Within tolerance.
  Differs,///< Over tolerance, or a length/rate mismatch.
  Error   ///< A file could not be read.
};

/// \brief Compare two mono WAV files.
///
/// One implementation shared by `namchain-wavcmp` (golden test) and the soak harness's oracle
/// check, so the two can never disagree about what "the same audio" means.
WavCompareStatus compare_wav_mono(const std::string& a_path, const std::string& b_path, double tolerance,
                                  WavDiff& diff, std::string& error);

} // namespace namchain
