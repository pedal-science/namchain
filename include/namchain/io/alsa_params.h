#pragma once

// Pure manipulation of the ALSA kernel parameter space (the `snd_pcm_hw_params` struct from
// <sound/asound.h>). No device and no libasound: this is the arithmetic behind "give me exactly
// this, or tell me what you can actually do", which is the part that has to be right.
//
// Kept separate from the device class so it can be unit-tested without hardware. The specific
// lesson it exists to encode (plan.md §9.5): requesting a 32-frame period on a full-speed USB
// device silently yields 48, because PERIOD_TIME has a 1000 us floor. The engine's quantum must
// equal the device period, so the negotiation result is verified rather than assumed.
#if defined(__linux__)

  #include <sound/asound.h>

namespace namchain
{
namespace alsa
{

/// Reset every parameter to "any": the widest space the kernel will accept as a starting point.
void params_set_any(struct snd_pcm_hw_params& params);

/// Narrow to the properties we actually depend on, leaving period/buffer sizes unconstrained so
/// that a subsequent SNDRV_PCM_IOCTL_HW_REFINE reports the device's real ranges.
void params_narrow_basics(struct snd_pcm_hw_params& params, unsigned channels, unsigned sample_rate);

/// Bounds of an interval parameter, e.g. SNDRV_PCM_HW_PARAM_PERIOD_SIZE. Returns false if the
/// parameter is not an interval parameter or the interval is empty.
bool params_interval_bounds(const struct snd_pcm_hw_params& params, unsigned param, unsigned& min,
                            unsigned& max);

/// Pin an interval parameter to a single value.
void params_set_interval(struct snd_pcm_hw_params& params, unsigned param, unsigned value);

/// True when exactly `format` is permitted (and nothing else).
bool params_format_is_exactly(const struct snd_pcm_hw_params& params, unsigned format);

/// Read back a single-valued interval parameter. Returns false if it is not exactly one value.
bool params_single_value(const struct snd_pcm_hw_params& params, unsigned param, unsigned& value);

/// Clamp `requested` into [min, max], which is what the kernel does to a too-small period.
unsigned clamp_to_bounds(unsigned requested, unsigned min, unsigned max);

} // namespace alsa
} // namespace namchain

#endif // __linux__
