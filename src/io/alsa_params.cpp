#include "namchain/io/alsa_params.h"

#if defined(__linux__)

  #include <cstring>
  #include <limits>

namespace namchain
{
namespace alsa
{
namespace
{

struct snd_mask* mask_of(struct snd_pcm_hw_params& params, unsigned param)
{
  return &params.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

const struct snd_mask* mask_of(const struct snd_pcm_hw_params& params, unsigned param)
{
  return &params.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

struct snd_interval* interval_of(struct snd_pcm_hw_params& params, unsigned param)
{
  return &params.intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

const struct snd_interval* interval_of(const struct snd_pcm_hw_params& params, unsigned param)
{
  return &params.intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

void mask_any(struct snd_mask& mask)
{
  std::memset(mask.bits, 0xff, sizeof(mask.bits));
}

void mask_clear(struct snd_mask& mask)
{
  std::memset(mask.bits, 0, sizeof(mask.bits));
}

void mask_add(struct snd_mask& mask, unsigned value)
{
  mask.bits[value / 32] |= 1u << (value % 32);
}

bool mask_is_single(const struct snd_mask& mask, unsigned value)
{
  for (unsigned i = 0; i < sizeof(mask.bits) / sizeof(mask.bits[0]); ++i)
  {
    const unsigned expected = (i == value / 32) ? (1u << (value % 32)) : 0u;
    if (mask.bits[i] != expected)
      return false;
  }
  return true;
}

void interval_any(struct snd_interval& interval)
{
  interval.min = 0;
  interval.max = std::numeric_limits<unsigned>::max();
  interval.openmin = 0;
  interval.openmax = 0;
  interval.integer = 0;
  interval.empty = 0;
}

void interval_pin(struct snd_interval& interval, unsigned value)
{
  interval.min = value;
  interval.max = value;
  interval.openmin = 0;
  interval.openmax = 0;
  interval.integer = 1;
  interval.empty = 0;
}

bool interval_is_single(const struct snd_interval& interval, unsigned& value)
{
  if (interval.empty || interval.openmin || interval.openmax || interval.min != interval.max)
    return false;
  value = interval.min;
  return true;
}

} // namespace

void params_set_any(struct snd_pcm_hw_params& params)
{
  std::memset(&params, 0, sizeof(params));
  for (unsigned p = SNDRV_PCM_HW_PARAM_FIRST_MASK; p <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++p)
    mask_any(*mask_of(params, p));
  for (unsigned p = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; p <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++p)
    interval_any(*interval_of(params, p));
  // rmask tells the kernel which parameters we are asking it to refine. Leaving it zero makes
  // SNDRV_PCM_IOCTL_HW_REFINE return the input space *unchanged* -- no error, no narrowing, and
  // every bound still reads as "any" -- so the negotiation appears to work while doing nothing.
  params.rmask = ~0u;
  params.cmask = 0;
}

void params_narrow_basics(struct snd_pcm_hw_params& params, unsigned channels, unsigned sample_rate)
{
  struct snd_mask* access = mask_of(params, SNDRV_PCM_HW_PARAM_ACCESS);
  mask_clear(*access);
  mask_add(*access, SNDRV_PCM_ACCESS_RW_INTERLEAVED);

  struct snd_mask* format = mask_of(params, SNDRV_PCM_HW_PARAM_FORMAT);
  mask_clear(*format);
  mask_add(*format, SNDRV_PCM_FORMAT_S16_LE);

  struct snd_mask* subformat = mask_of(params, SNDRV_PCM_HW_PARAM_SUBFORMAT);
  mask_clear(*subformat);
  mask_add(*subformat, SNDRV_PCM_SUBFORMAT_STD);

  interval_pin(*interval_of(params, SNDRV_PCM_HW_PARAM_CHANNELS), channels);
  interval_pin(*interval_of(params, SNDRV_PCM_HW_PARAM_RATE), sample_rate);
}

bool params_interval_bounds(const struct snd_pcm_hw_params& params, unsigned param, unsigned& min,
                            unsigned& max)
{
  if (param < SNDRV_PCM_HW_PARAM_FIRST_INTERVAL || param > SNDRV_PCM_HW_PARAM_LAST_INTERVAL)
    return false;
  const struct snd_interval& interval = *interval_of(params, param);
  if (interval.empty)
    return false;
  min = interval.min;
  max = interval.max;
  return true;
}

void params_set_interval(struct snd_pcm_hw_params& params, unsigned param, unsigned value)
{
  interval_pin(*interval_of(params, param), value);
}

bool params_format_is_exactly(const struct snd_pcm_hw_params& params, unsigned format)
{
  return mask_is_single(*mask_of(params, SNDRV_PCM_HW_PARAM_FORMAT), format);
}

bool params_single_value(const struct snd_pcm_hw_params& params, unsigned param, unsigned& value)
{
  if (param < SNDRV_PCM_HW_PARAM_FIRST_INTERVAL || param > SNDRV_PCM_HW_PARAM_LAST_INTERVAL)
    return false;
  return interval_is_single(*interval_of(params, param), value);
}

unsigned clamp_to_bounds(unsigned requested, unsigned min, unsigned max)
{
  if (requested < min)
    return min;
  if (requested > max)
    return max;
  return requested;
}

} // namespace alsa
} // namespace namchain

#endif // __linux__
