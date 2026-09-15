#include "namchain/rt/splice_fader.h"

#include <algorithm>
#include <cstddef>

namespace namchain
{
namespace rt
{
namespace
{

/// Hermite bases for a bridge that starts at (value 1, slope 0) and ends at (value 0, slope 0).
float h00(float t) { return ((2.0f * t - 3.0f) * t) * t + 1.0f; }
/// Starts at (value 0, slope 1) and ends at (value 0, slope 0). Scaled by the window length where it is
/// used, because its slope is in units of the normalised parameter.
float h10(float t) { return ((t - 2.0f) * t + 1.0f) * t; }

} // namespace

SpliceFader::SpliceFader(unsigned channels, unsigned fade_frames)
  : channels_(channels == 0 ? 1 : channels),
    fade_frames_(fade_frames == 0 ? 0u : std::max(3u, fade_frames))
{
  anchor_.assign(channels_, 0.0f);
  previous_.assign(channels_, 0.0f);
  ramp_from_.assign(channels_, 0.0f);
  ramp_slope_.assign(channels_, 0.0f);
  bridge_value_.assign(channels_, 0.0f);
  bridge_slope_.assign(channels_, 0.0f);
}

void SpliceFader::arm(Cause cause)
{
  if (!enabled())
    return;
  ramp_from_ = anchor_;
  for (unsigned c = 0; c < channels_; ++c)
    ramp_slope_[c] = cause == Cause::Dropped ? anchor_[c] - previous_[c] : 0.0f;
  remaining_ = fade_frames_;
  ++fades_;
}

void SpliceFader::apply(const float* in, float* out, unsigned frames)
{
  if (frames == 0 || channels_ == 0)
    return;

  const unsigned channels = channels_;
  // The bridge needs the new signal's first two samples to know the slope it is bridging *to*. A period
  // of one sample cannot express a slope, so the bridge simply does not start there.
  if (remaining_ == 0 || frames < 2)
  {
    // Idle: a plain copy, so a run with the fader enabled but no splices is bit-identical to one
    // without it. This is the path that makes "the fader costs nothing when it is not needed" a
    // statement about the samples rather than about the arithmetic.
    if (out != in)
      std::copy(in, in + static_cast<size_t>(frames) * channels, out);
  }
  else
  {
    // Index into the bridge, not into the buffer: a bridge longer than a period continues across as
    // many periods as it needs, which is why `remaining_` and the anchor are state rather than locals.
    const unsigned window = fade_frames_;
    const float inverse_last = 1.0f / static_cast<float>(window - 1);
    unsigned bridge_index = window - remaining_;
    for (unsigned f = 0; f < frames; ++f)
    {
      const float* src = in + static_cast<size_t>(f) * channels;
      float* dst = out + static_cast<size_t>(f) * channels;
      if (remaining_ > 0)
      {
        if (bridge_index == 0)
        {
          // First period of the bridge: the mismatches are measured once, here, against the signal the
          // bridge is actually joining. Everything after this reads them.
          for (unsigned c = 0; c < channels; ++c)
          {
            const float x0 = in[c];
            const float slope_new = in[channels + c] - x0;
            const float slope_old = ramp_slope_[c];
            // Value mismatch against where the device was *heading*, not where it was: that is what
            // makes the second difference at the join zero rather than |slope_old|.
            bridge_value_[c] = (ramp_from_[c] + slope_old) - x0;
            bridge_slope_[c] = (slope_old - slope_new) * static_cast<float>(window - 1);
          }
        }
        const float t = static_cast<float>(bridge_index) * inverse_last;
        const float b00 = h00(t);
        const float b10 = h10(t);
        for (unsigned c = 0; c < channels; ++c)
          dst[c] = src[c] + bridge_value_[c] * b00 + bridge_slope_[c] * b10;
        ++bridge_index;
        --remaining_;
      }
      else
      {
        // The bridge finished inside this period; the rest is the signal, untouched.
        for (unsigned c = 0; c < channels; ++c)
          dst[c] = src[c];
      }
    }
  }

}

void SpliceFader::commit(const float* emitted, unsigned frames)
{
  if (emitted == nullptr || frames == 0 || channels_ == 0)
    return;
  for (unsigned c = 0; c < channels_; ++c)
  {
    const float newest = emitted[static_cast<size_t>(frames - 1) * channels_ + c];
    previous_[c] = frames >= 2 ? emitted[static_cast<size_t>(frames - 2) * channels_ + c] : anchor_[c];
    anchor_[c] = newest;
  }
}

} // namespace rt
} // namespace namchain
