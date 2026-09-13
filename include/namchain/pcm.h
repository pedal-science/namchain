#pragma once

#include <cstddef>

namespace namchain
{

/// Sample formats the device layer may present. Deliberately portable (no ALSA headers) so the
/// conversion arithmetic can be tested anywhere, including CI which has no audio device.
enum class PcmFormat
{
  Unknown = 0,
  S16_LE,   ///< signed 16-bit little-endian
  S24_3LE,  ///< signed 24-bit little-endian, packed into 3 bytes (not padded to 4)
  S32_LE,   ///< signed 32-bit little-endian
  F32_LE    ///< 32-bit IEEE float little-endian. The engine's own format: the backends that have no
            ///< wire to speak (`file`, `null`) use it rather than quantising for no reason.
};

std::size_t pcm_bytes_per_sample(PcmFormat format);
const char* pcm_format_name(PcmFormat format);

/// Decode one little-endian sample to the range [-1, 1].
float pcm_decode(const unsigned char* src, PcmFormat format);

/// Encode one sample to little-endian \p format, clamping outside [-1, 1].
///
/// Scaling is asymmetric (positive full scale is one step short of the negative extreme, as in
/// every fixed-point audio format), so the arithmetic cannot overflow on +1.0.
void pcm_encode(unsigned char* dst, PcmFormat format, float value);

} // namespace namchain
