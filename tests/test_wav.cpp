// Tests for the WAV layer, especially the multichannel reader the `file` backend needs.
//
// The core's own loader refuses anything but mono (ERROR_NOT_MONO), so reading a device capture back
// through the engine needs a reader of our own -- and a reader of our own is a second chance to
// disagree with the core's about what a sample is worth. The golden test only holds because
// `load_wav_mono` still delegates to the core; everything that goes through `load_wav` is checked
// here instead.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "namchain/wav.h"

namespace
{

std::string temp_path(const char* name)
{
  return std::string("/tmp/namchain-test-") + name + ".wav";
}

void append_u16(std::vector<unsigned char>& out, uint16_t value)
{
  out.push_back(static_cast<unsigned char>(value & 0xFFu));
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
}

void append_u32(std::vector<unsigned char>& out, uint32_t value)
{
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
}

void append_bytes(std::vector<unsigned char>& out, const char* text, size_t count)
{
  for (size_t i = 0; i < count; ++i)
    out.push_back(static_cast<unsigned char>(text[i]));
}

/// A minimal hand-built WAV, so the reader is tested against bytes rather than against our own
/// writer. `extra_chunk` inserts a LIST chunk before `data`, which is what a real editor's file looks
/// like and what a reader that assumes a fixed layout gets wrong.
std::vector<unsigned char> build_wav(unsigned channels, unsigned bits, unsigned rate,
                                     const std::vector<int32_t>& samples, bool extra_chunk)
{
  const unsigned sample_bytes = bits / 8u;
  std::vector<unsigned char> data;
  for (const int32_t value : samples)
    for (unsigned b = 0; b < sample_bytes; ++b)
      data.push_back(static_cast<unsigned char>((static_cast<uint32_t>(value) >> (8 * b)) & 0xFFu));

  std::vector<unsigned char> body;
  append_bytes(body, "WAVE", 4);
  append_bytes(body, "fmt ", 4);
  append_u32(body, 16);
  append_u16(body, 1); // PCM
  append_u16(body, static_cast<uint16_t>(channels));
  append_u32(body, rate);
  append_u32(body, rate * channels * sample_bytes);
  append_u16(body, static_cast<uint16_t>(channels * sample_bytes));
  append_u16(body, static_cast<uint16_t>(bits));
  if (extra_chunk)
  {
    append_bytes(body, "LIST", 4);
    append_u32(body, 10);
    append_bytes(body, "INFOhello", 10);
  }
  append_bytes(body, "data", 4);
  append_u32(body, static_cast<uint32_t>(data.size()));
  body.insert(body.end(), data.begin(), data.end());

  std::vector<unsigned char> file;
  append_bytes(file, "RIFF", 4);
  append_u32(file, static_cast<uint32_t>(body.size()));
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

void write_file(const std::string& path, const std::vector<unsigned char>& bytes)
{
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("a float WAV round-trips through our writer and our reader")
{
  const std::string path = temp_path("roundtrip");
  const std::vector<float> frames{0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f};
  const unsigned channels = 2;

  std::string error;
  REQUIRE(namchain::save_wav_float32(path, frames.data(), frames.size() / channels, channels, 48000.0, error));

  std::vector<float> loaded;
  unsigned loaded_channels = 0;
  double rate = 0.0;
  REQUIRE(namchain::load_wav(path, loaded, loaded_channels, rate, error));
  CHECK(loaded_channels == channels);
  CHECK(rate == doctest::Approx(48000.0));
  REQUIRE(loaded.size() == frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i)
    CHECK(loaded[i] == frames[i]); // bit-exact: float in, float out

  std::remove(path.c_str());
}

TEST_CASE("the reader decodes 24-bit PCM the way the golden test's loader does")
{
  // The example fixture is 24-bit, and the whole golden comparison rests on both paths decoding it
  // to identical floats. The core divides by 2^23; this checks ours does too, sign included.
  const std::vector<int32_t> samples{0, 1, -1, 8388607, -8388608, 4194304, -4194304};
  const std::string path = temp_path("pcm24");
  write_file(path, build_wav(1, 24, 48000, samples, /*extra_chunk=*/false));

  std::vector<float> loaded;
  unsigned channels = 0;
  double rate = 0.0;
  std::string error;
  REQUIRE(namchain::load_wav(path, loaded, channels, rate, error));
  CHECK(channels == 1u);
  REQUIRE(loaded.size() == samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i)
    CHECK(loaded[i] == doctest::Approx(static_cast<double>(samples[i]) / 8388608.0).epsilon(1e-12));

  std::remove(path.c_str());
}

TEST_CASE("the reader decodes 16-bit PCM and honouring the sign")
{
  const std::vector<int32_t> samples{0, 32767, -32768, 16384, -16384};
  const std::string path = temp_path("pcm16");
  write_file(path, build_wav(1, 16, 48000, samples, false));

  std::vector<float> loaded;
  unsigned channels = 0;
  double rate = 0.0;
  std::string error;
  REQUIRE(namchain::load_wav(path, loaded, channels, rate, error));
  REQUIRE(loaded.size() == samples.size());
  CHECK(loaded[0] == 0.0f);
  CHECK(loaded[1] == doctest::Approx(32767.0 / 32768.0).epsilon(1e-6));
  CHECK(loaded[2] == -1.0f);

  std::remove(path.c_str());
}

TEST_CASE("a chunk before data is skipped and the interleaving survives")
{
  // Three channels of a known ramp, with a LIST chunk in the way. A reader that assumes fmt is
  // immediately followed by data reads the chunk header as audio and produces noise that still
  // looks like a waveform, which is why this is checked sample by sample.
  const unsigned channels = 3;
  const std::vector<int32_t> samples{100, 200, 300, -100, -200, -300};
  const std::string path = temp_path("chunked");
  write_file(path, build_wav(channels, 24, 48000, samples, /*extra_chunk=*/true));

  std::vector<float> loaded;
  unsigned loaded_channels = 0;
  double rate = 0.0;
  std::string error;
  REQUIRE(namchain::load_wav(path, loaded, loaded_channels, rate, error));
  CHECK(loaded_channels == channels);
  REQUIRE(loaded.size() == samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i)
    CHECK(loaded[i] == doctest::Approx(static_cast<double>(samples[i]) / 8388608.0).epsilon(1e-12));

  std::remove(path.c_str());
}

TEST_CASE("a file with no data chunk is an error rather than silence")
{
  std::vector<unsigned char> body;
  append_bytes(body, "WAVE", 4);
  append_bytes(body, "fmt ", 4);
  append_u32(body, 16);
  append_u16(body, 1);
  append_u16(body, 1);
  append_u32(body, 48000);
  append_u32(body, 96000);
  append_u16(body, 2);
  append_u16(body, 16);

  std::vector<unsigned char> file;
  append_bytes(file, "RIFF", 4);
  append_u32(file, static_cast<uint32_t>(body.size()));
  file.insert(file.end(), body.begin(), body.end());

  const std::string path = temp_path("nodata");
  write_file(path, file);

  std::vector<float> loaded;
  unsigned channels = 0;
  double rate = 0.0;
  std::string error;
  CHECK_FALSE(namchain::load_wav(path, loaded, channels, rate, error));
  CHECK_FALSE(error.empty());

  std::remove(path.c_str());
}
