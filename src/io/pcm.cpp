#include "namchain/pcm.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace namchain
{
namespace
{

/// Read 1..4 little-endian bytes and sign-extend to int32.
int32_t decode_signed_le(const unsigned char* src, int bytes)
{
  uint32_t raw = 0;
  for (int i = 0; i < bytes; ++i)
    raw |= static_cast<uint32_t>(src[i]) << (8 * i);

  const int bits = 8 * bytes;
  if (bits < 32)
  {
    const uint32_t sign_bit = 1u << (bits - 1);
    if ((raw & sign_bit) != 0)
      raw |= ~((1u << bits) - 1u); // sign-extend the unused high bits
  }
  return static_cast<int32_t>(raw);
}

void encode_signed_le(unsigned char* dst, int bytes, int32_t value)
{
  const uint32_t raw = static_cast<uint32_t>(value);
  for (int i = 0; i < bytes; ++i)
    dst[i] = static_cast<unsigned char>((raw >> (8 * i)) & 0xFFu);
}

int32_t clamp_to(int32_t value, int32_t low, int32_t high)
{
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

/// Scale a clamped [-1, 1] value to an integer range whose positive extreme is `positive_max`
/// (one step short of the negative extreme, as in every fixed-point audio format).
int32_t scale_to_int(float value, double positive_max, int32_t low, int32_t high)
{
  const double scaled = std::isnan(value) ? 0.0 : static_cast<double>(value) * positive_max;
  const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
  return clamp_to(static_cast<int32_t>(rounded), low, high);
}

} // namespace

std::size_t pcm_bytes_per_sample(PcmFormat format)
{
  switch (format)
  {
  case PcmFormat::S16_LE: return 2;
  case PcmFormat::S24_3LE: return 3;
  case PcmFormat::S32_LE: return 4;
  case PcmFormat::F32_LE: return 4;
  case PcmFormat::Unknown: return 0;
  }
  return 0;
}

const char* pcm_format_name(PcmFormat format)
{
  switch (format)
  {
  case PcmFormat::S16_LE: return "S16_LE";
  case PcmFormat::S24_3LE: return "S24_3LE";
  case PcmFormat::S32_LE: return "S32_LE";
  case PcmFormat::F32_LE: return "F32_LE";
  case PcmFormat::Unknown: return "unknown";
  }
  return "unknown";
}

float pcm_decode(const unsigned char* src, PcmFormat format)
{
  switch (format)
  {
  case PcmFormat::S16_LE:
    return static_cast<float>(decode_signed_le(src, 2)) / 32768.0f;
  case PcmFormat::S24_3LE:
    return static_cast<float>(decode_signed_le(src, 3)) / 8388608.0f;
  case PcmFormat::S32_LE:
    return static_cast<float>(decode_signed_le(src, 4)) / 2147483648.0f;
  case PcmFormat::F32_LE:
  {
    // Assembled byte by byte rather than aliased, so this is correct on a big-endian host too and
    // does not depend on the compiler agreeing that a float can be read out of a char buffer.
    uint32_t raw = 0;
    for (int i = 0; i < 4; ++i)
      raw |= static_cast<uint32_t>(src[i]) << (8 * i);
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    // A NaN on the wire is not a signal the engine should propagate; it is a fault, and silence is
    // the least damaging reading of it.
    return std::isnan(value) ? 0.0f : value;
  }
  case PcmFormat::Unknown:
    break;
  }
  return 0.0f;
}

void pcm_encode(unsigned char* dst, PcmFormat format, float value)
{
  const float clamped = std::isnan(value) ? 0.0f : (value > 1.0f ? 1.0f : (value < -1.0f ? -1.0f : value));

  switch (format)
  {
  case PcmFormat::S16_LE:
    encode_signed_le(dst, 2, scale_to_int(clamped, 32767.0, -32768, 32767));
    return;
  case PcmFormat::S24_3LE:
    encode_signed_le(dst, 3, scale_to_int(clamped, 8388607.0, -8388608, 8388607));
    return;
  case PcmFormat::S32_LE:
    encode_signed_le(dst, 4,
                     scale_to_int(clamped, 2147483647.0, std::numeric_limits<int32_t>::min(),
                                  std::numeric_limits<int32_t>::max()));
    return;
  case PcmFormat::F32_LE:
  {
    const uint32_t raw = [&] {
      uint32_t bits = 0;
      std::memcpy(&bits, &clamped, sizeof(bits));
      return bits;
    }();
    for (int i = 0; i < 4; ++i)
      dst[i] = static_cast<unsigned char>((raw >> (8 * i)) & 0xFFu);
    return;
  }
  case PcmFormat::Unknown:
    return;
  }
}

} // namespace namchain
