#include "namchain/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>

#include "wav.h" // AudioDSPTools' loader (include dir added privately by CMake)

namespace namchain
{

bool load_wav_mono(const std::string& path, std::vector<float>& samples, double& sample_rate, std::string& error)
{
  samples.clear();
  sample_rate = 0.0;

  const dsp::wav::LoadReturnCode rc = dsp::wav::Load(path.c_str(), samples, sample_rate);
  if (rc != dsp::wav::LoadReturnCode::SUCCESS)
  {
    error = "WAV load failed for '" + path + "': " + dsp::wav::GetMsgForLoadReturnCode(rc);
    return false;
  }
  if (samples.empty())
  {
    error = "WAV file '" + path + "' contains no samples";
    return false;
  }
  if (sample_rate <= 0.0)
  {
    error = "WAV file '" + path + "' reports an invalid sample rate";
    return false;
  }
  return true;
}

bool save_wav_float32(const std::string& path, const float* interleaved, size_t frames, int channels,
                      double sample_rate, std::string& error)
{
  if (channels < 1)
  {
    error = "cannot write a WAV with no channels";
    return false;
  }
  if (sample_rate <= 0.0)
  {
    error = "cannot write a WAV with an invalid sample rate";
    return false;
  }

  const uint64_t total_samples = static_cast<uint64_t>(frames) * static_cast<uint64_t>(channels);
  const uint64_t data_bytes = total_samples * sizeof(float);
  // RIFF sizes are 32-bit. Refuse rather than silently truncate a header.
  if (data_bytes > 0xFFFFFFFFull - 36ull)
  {
    error = "render is too long for a RIFF/WAVE header (>4 GiB of samples)";
    return false;
  }

  std::ofstream out(path, std::ios::binary);
  if (!out.is_open())
  {
    error = "cannot open '" + path + "' for writing";
    return false;
  }

  const uint32_t data_size = static_cast<uint32_t>(data_bytes);
  const uint32_t riff_size = 36u + data_size;
  const uint32_t fmt_size = 16u;
  const uint16_t audio_format = 3; // IEEE float
  const uint16_t num_channels = static_cast<uint16_t>(channels);
  const uint32_t rate = static_cast<uint32_t>(sample_rate);
  const uint32_t byte_rate = rate * num_channels * static_cast<uint32_t>(sizeof(float));
  const uint16_t block_align = static_cast<uint16_t>(num_channels * sizeof(float));
  const uint16_t bits_per_sample = 32;

  out.write("RIFF", 4);
  out.write(reinterpret_cast<const char*>(&riff_size), 4);
  out.write("WAVE", 4);

  out.write("fmt ", 4);
  out.write(reinterpret_cast<const char*>(&fmt_size), 4);
  out.write(reinterpret_cast<const char*>(&audio_format), 2);
  out.write(reinterpret_cast<const char*>(&num_channels), 2);
  out.write(reinterpret_cast<const char*>(&rate), 4);
  out.write(reinterpret_cast<const char*>(&byte_rate), 4);
  out.write(reinterpret_cast<const char*>(&block_align), 2);
  out.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

  out.write("data", 4);
  out.write(reinterpret_cast<const char*>(&data_size), 4);
  if (data_size > 0)
    out.write(reinterpret_cast<const char*>(interleaved), static_cast<std::streamsize>(data_size));

  if (!out.good())
  {
    error = "write failed for '" + path + "'";
    return false;
  }
  return true;
}

WavCompareStatus compare_wav_mono(const std::string& a_path, const std::string& b_path, double tolerance,
                                  WavDiff& diff, std::string& error)
{
  diff = WavDiff{};

  std::vector<float> a;
  std::vector<float> b;
  double a_rate = 0.0;
  double b_rate = 0.0;
  if (!load_wav_mono(a_path, a, a_rate, error) || !load_wav_mono(b_path, b, b_rate, error))
    return WavCompareStatus::Error;

  if (a.size() != b.size())
  {
    error = "sample count differs: " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    return WavCompareStatus::Differs;
  }
  if (std::abs(a_rate - b_rate) > 0.5)
  {
    error = "sample rate differs: " + std::to_string(static_cast<long long>(a_rate)) + " vs " +
            std::to_string(static_cast<long long>(b_rate));
    return WavCompareStatus::Differs;
  }

  double sum_sq = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
  {
    const double d = std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
    if (d > diff.max_abs)
    {
      diff.max_abs = d;
      diff.max_abs_index = i;
    }
    sum_sq += d * d;
    diff.peak_a = std::max(diff.peak_a, std::abs(static_cast<double>(a[i])));
  }
  diff.frames = a.size();
  diff.rms = a.empty() ? 0.0 : std::sqrt(sum_sq / static_cast<double>(a.size()));

  return diff.max_abs > tolerance ? WavCompareStatus::Differs : WavCompareStatus::Match;
}

} // namespace namchain
