#include "namchain/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

#include "namchain/pcm.h"

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

namespace
{

uint32_t read_u32(const unsigned char* p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16(const unsigned char* p)
{
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

float decode_pcm(const unsigned char* p, unsigned bits)
{
  switch (bits)
  {
  case 16:
    return pcm_decode(p, PcmFormat::S16_LE);
  case 24:
    return pcm_decode(p, PcmFormat::S24_3LE);
  case 32:
    return pcm_decode(p, PcmFormat::S32_LE);
  default:
    return 0.0f;
  }
}

} // namespace

bool load_wav(const std::string& path, std::vector<float>& interleaved, unsigned& channels, double& sample_rate,
              std::string& error)
{
  interleaved.clear();
  channels = 0;
  sample_rate = 0.0;

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
  {
    error = "cannot open '" + path + "' for reading";
    return false;
  }

  unsigned char header[12] = {};
  in.read(reinterpret_cast<char*>(header), 12);
  if (in.gcount() != 12 || std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0)
  {
    error = "'" + path + "' is not a RIFF/WAVE file";
    return false;
  }

  bool have_format = false;
  unsigned bits = 0;
  unsigned format_code = 0;
  unsigned num_channels = 0;

  // Chunks are walked rather than assumed to be in the canonical order: a `LIST` or `fact` chunk
  // before `data` is common in the wild, and the fixed-offset readers fall over on exactly those.
  for (;;)
  {
    unsigned char chunk_header[8] = {};
    in.read(reinterpret_cast<char*>(chunk_header), 8);
    if (in.gcount() != 8)
    {
      error = "'" + path + "' has no data chunk";
      return false;
    }
    const uint32_t chunk_size = read_u32(chunk_header + 4);

    if (std::memcmp(chunk_header, "fmt ", 4) == 0)
    {
      std::vector<unsigned char> fmt(chunk_size);
      in.read(reinterpret_cast<char*>(fmt.data()), static_cast<std::streamsize>(chunk_size));
      if (in.gcount() != static_cast<std::streamsize>(chunk_size) || chunk_size < 16)
      {
        error = "'" + path + "' has a truncated fmt chunk";
        return false;
      }
      format_code = read_u16(fmt.data());
      num_channels = read_u16(fmt.data() + 2);
      sample_rate = static_cast<double>(read_u32(fmt.data() + 4));
      bits = read_u16(fmt.data() + 14);
      have_format = true;
    }
    else if (std::memcmp(chunk_header, "data", 4) == 0)
    {
      if (!have_format)
      {
        error = "'" + path + "' has a data chunk before its fmt chunk";
        return false;
      }
      const bool is_float = format_code == 3 || format_code == 0xFFFE;
      const bool is_pcm = format_code == 1 || format_code == 0xFFFE;
      if (!(is_float && bits == 32) && !(is_pcm && (bits == 16 || bits == 24 || bits == 32)))
      {
        error = "'" + path + "' uses unsupported WAV format code " + std::to_string(format_code) + " with " +
                std::to_string(bits) + " bits per sample";
        return false;
      }
      if (num_channels < 1 || num_channels > 64)
      {
        error = "'" + path + "' declares " + std::to_string(num_channels) + " channels";
        return false;
      }
      if (sample_rate <= 0.0)
      {
        error = "'" + path + "' declares an invalid sample rate";
        return false;
      }

      const unsigned sample_bytes = bits / 8u;
      const size_t frames = chunk_size / (sample_bytes * num_channels);
      std::vector<unsigned char> raw(static_cast<size_t>(frames) * sample_bytes * num_channels);
      in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
      const size_t got = static_cast<size_t>(in.gcount());
      const size_t usable_frames = got / (sample_bytes * num_channels);
      if (usable_frames < frames)
      {
        error = "'" + path + "' is truncated: expected " + std::to_string(frames) + " frames, found " +
                std::to_string(usable_frames);
        return false;
      }

      interleaved.resize(static_cast<size_t>(frames) * num_channels);
      for (size_t i = 0; i < frames * num_channels; ++i)
      {
        const unsigned char* source = raw.data() + i * sample_bytes;
        interleaved[i] = bits == 32 && format_code == 3 ? pcm_decode(source, PcmFormat::F32_LE)
                                                        : decode_pcm(source, bits);
      }
      channels = num_channels;
      return true;
    }
    else
    {
      // Skip anything else, respecting the padding byte odd-sized chunks carry.
      in.seekg(static_cast<std::streamoff>(chunk_size + (chunk_size & 1u)), std::ios::cur);
      if (!in.good())
      {
        error = "'" + path + "' has no data chunk";
        return false;
      }
    }
  }
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
