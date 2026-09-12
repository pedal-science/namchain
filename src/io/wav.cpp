#include "namchain/wav.h"

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

} // namespace namchain
