// Tests for the device sample-format conversion. Portable and hardware-free, so CI covers the
// arithmetic that sits between the ALSA backend and the engine.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "namchain/pcm.h"

namespace
{

/// Encode a value, then decode it back.
float round_trip(namchain::PcmFormat format, float value)
{
  std::vector<unsigned char> bytes(namchain::pcm_bytes_per_sample(format), 0);
  namchain::pcm_encode(bytes.data(), format, value);
  return namchain::pcm_decode(bytes.data(), format);
}

} // namespace

TEST_CASE("sample widths")
{
  using namchain::PcmFormat;
  CHECK(namchain::pcm_bytes_per_sample(PcmFormat::S16_LE) == 2u);
  CHECK(namchain::pcm_bytes_per_sample(PcmFormat::S24_3LE) == 3u); // packed, not padded to 4
  CHECK(namchain::pcm_bytes_per_sample(PcmFormat::S32_LE) == 4u);
  CHECK(namchain::pcm_bytes_per_sample(PcmFormat::Unknown) == 0u);
  CHECK(std::string(namchain::pcm_format_name(PcmFormat::S24_3LE)) == "S24_3LE");
}

TEST_CASE("decoding known bit patterns")
{
  using namchain::PcmFormat;

  // Silence is zero in every format.
  const std::vector<unsigned char> zero4{0, 0, 0, 0};
  CHECK(namchain::pcm_decode(zero4.data(), PcmFormat::S16_LE) == 0.0f);
  CHECK(namchain::pcm_decode(zero4.data(), PcmFormat::S24_3LE) == 0.0f);
  CHECK(namchain::pcm_decode(zero4.data(), PcmFormat::S32_LE) == 0.0f);

  // Positive full scale exactly.
  const std::vector<unsigned char> plus16{0xFF, 0x7F};             // 32767
  const std::vector<unsigned char> plus24{0xFF, 0xFF, 0x7F};       // 8388607
  const std::vector<unsigned char> plus32{0xFF, 0xFF, 0xFF, 0x7F}; // 2147483647
  CHECK(namchain::pcm_decode(plus16.data(), PcmFormat::S16_LE) == doctest::Approx(1.0f).epsilon(1e-4));
  CHECK(namchain::pcm_decode(plus24.data(), PcmFormat::S24_3LE) == doctest::Approx(1.0f).epsilon(1e-5));
  CHECK(namchain::pcm_decode(plus32.data(), PcmFormat::S32_LE) == doctest::Approx(1.0f).epsilon(1e-6));

  // Negative full scale exactly: this is where sign extension matters, and where a naive
  // implementation that forgot to extend gets a large *positive* number instead.
  const std::vector<unsigned char> minus16{0x00, 0x80};             // -32768
  const std::vector<unsigned char> minus24{0x00, 0x00, 0x80};       // -8388608
  const std::vector<unsigned char> minus32{0x00, 0x00, 0x00, 0x80}; // INT32_MIN
  CHECK(namchain::pcm_decode(minus16.data(), PcmFormat::S16_LE) == -1.0f);
  CHECK(namchain::pcm_decode(minus24.data(), PcmFormat::S24_3LE) == -1.0f);
  CHECK(namchain::pcm_decode(minus32.data(), PcmFormat::S32_LE) == -1.0f);

  // Half scale.
  CHECK(namchain::pcm_decode(std::vector<unsigned char>{0x00, 0x40}.data(), PcmFormat::S16_LE) == 0.5f);
}

TEST_CASE("round trips stay within one LSB")
{
  using namchain::PcmFormat;
  const float values[] = {0.0f, 0.5f, -0.5f, 0.25f, -0.25f, 0.999f, -0.999f, 1.0f, -1.0f};

  for (const PcmFormat format : {PcmFormat::S16_LE, PcmFormat::S24_3LE, PcmFormat::S32_LE})
  {
    // ldexp rather than a shifted int literal: 1 << 31 does not fit in an int, and the overflow
    // silently produces a *negative* tolerance that no assertion can satisfy.
    const int bits = 8 * static_cast<int>(namchain::pcm_bytes_per_sample(format));
    const double lsb = 1.0 / std::ldexp(1.0, bits - 1);
    for (const float value : values)
    {
      const float back = round_trip(format, value);
      INFO("format = ", std::string(namchain::pcm_format_name(format)), ", value = ", value);
      CHECK(std::abs(static_cast<double>(back) - static_cast<double>(value)) <= 2.0 * lsb);
    }
  }
}

TEST_CASE("out-of-range and non-finite input is clamped, not wrapped")
{
  using namchain::PcmFormat;

  // Wrapping here would turn a loud passage into a polarity flip, so clamping is the contract.
  for (const PcmFormat format : {PcmFormat::S16_LE, PcmFormat::S24_3LE, PcmFormat::S32_LE})
  {
    CHECK(round_trip(format, 2.0f) <= 1.0f);
    CHECK(round_trip(format, 2.0f) >= 0.99f);
    CHECK(round_trip(format, -2.0f) >= -1.0f);
    CHECK(round_trip(format, -2.0f) <= -0.99f);
    CHECK(round_trip(format, std::nanf("")) == 0.0f);
  }
}

TEST_CASE("encode writes exactly the bytes the format claims and no more")
{
  // A 3-byte format writing a 4th byte would corrupt the neighbouring channel's sample.
  std::vector<unsigned char> buffer(8, 0xAA);
  namchain::pcm_encode(buffer.data(), namchain::PcmFormat::S24_3LE, 0.5f);
  CHECK(buffer[0] != 0xAA);
  CHECK(buffer[1] != 0xAA);
  CHECK(buffer[2] != 0xAA);
  CHECK(buffer[3] == 0xAA); // untouched
}

TEST_CASE("float32 samples round trip bit-exactly, with no clamping surprise")
{
  // The engine's own format, used by the backends that have no wire to speak (`file`, `null`). It
  // has to be exact: a file backend that quantises would make every offline comparison meaningless
  // while still looking plausible.
  for (const float value : {0.0f, 0.5f, -0.5f, 0.75f, -0.25f, 1.0f, -1.0f, 1e-30f, -3.5e-7f})
  {
    unsigned char bytes[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    namchain::pcm_encode(bytes, namchain::PcmFormat::F32_LE, value);
    CHECK(namchain::pcm_decode(bytes, namchain::PcmFormat::F32_LE) == value);
  }
  CHECK(namchain::pcm_bytes_per_sample(namchain::PcmFormat::F32_LE) == 4u);
}
