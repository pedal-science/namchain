#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace namchain
{

/// \brief The longest impulse response `nc.cab.ir` convolves.
///
/// A loaded IR is resampled to the engine rate and then truncated to this many taps (170 ms at
/// 48 kHz), which is what the pedal supports: "IRs longer than 8192 taps. Not supported; measured as
/// such rather than left undefined"
/// (pedal.project docs/managing-processing-power-and-optimising-for-fast-switching.md, "Not doing").
/// The cost table has no IR row above it, because no longer IR reaches the convolver.
constexpr std::size_t kMaxIrTaps = 8192;

/// \brief Load a mono WAV file as float samples.
///
/// Reading is delegated to AudioDSPTools' loader (the same one the vendored core's own
/// `render` tool uses), so both paths decode identical float values -- which is what makes
/// the golden comparison in tests/golden mean anything.
///
/// Only mono input is supported at M0; multichannel input is a job for the `file` backend
/// in M1. `error` is set on failure.
bool load_wav_mono(const std::string& path, std::vector<float>& samples, double& sample_rate, std::string& error);

/// \brief Load a WAV of any channel count as interleaved float samples.
///
/// Our own reader, rather than the core's, because the core's refuses anything but mono
/// (`ERROR_NOT_MONO`) and the `file` backend has to be able to read what it wrote. PCM 16/24/32-bit
/// and 32-bit IEEE float are supported, which is what the engine's own writer produces plus what a
/// device capture is likely to be.
///
/// \ref load_wav_mono deliberately still delegates to the core's loader: the golden test compares
/// our render against the core's own `render` tool, so the two paths must decode identical floats.
/// This one exists for the cases the core cannot serve, and is not used for that comparison.
bool load_wav(const std::string& path, std::vector<float>& interleaved, unsigned& channels, double& sample_rate,
              std::string& error);

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
