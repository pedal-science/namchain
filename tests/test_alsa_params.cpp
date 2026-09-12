// Tests for the raw-ALSA parameter arithmetic and device resolution.
//
// These deliberately need no audio hardware, so they run in CI (which has none) -- they cover the
// logic that has to be right for the negotiation to be trustworthy: the parameter space, the
// clamping that turns a 32-frame request into the device's floor, and node resolution.
#include <doctest.h>

#include <string>

#include "namchain/io/alsa_params.h"
#include "namchain/io/alsa_raw.h"

#if defined(__linux__)

TEST_CASE("a fresh parameter space is 'any', with rmask set so the kernel will refine it")
{
  struct snd_pcm_hw_params params;
  namchain::alsa::params_set_any(params);

  // rmask must be set: zero makes SNDRV_PCM_IOCTL_HW_REFINE return the input unchanged, which
  // looks like success while the negotiation silently does nothing.
  CHECK(params.rmask == ~0u);
  CHECK(params.cmask == 0u);

  unsigned min = 0;
  unsigned max = 0;
  REQUIRE(namchain::alsa::params_interval_bounds(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, min, max));
  CHECK(min == 0u);
  CHECK(max == 4294967295u);

  // Every mask bit set: all formats acceptable before narrowing.
  CHECK_FALSE(namchain::alsa::params_format_is_exactly(params, SNDRV_PCM_FORMAT_S16_LE));
}

TEST_CASE("narrowing pins channels and rate but deliberately leaves the format open")
{
  struct snd_pcm_hw_params params;
  namchain::alsa::params_set_any(params);
  namchain::alsa::params_narrow_basics(params, 2, 48000);

  // FORMAT stays wide so HW_REFINE reports what the device really offers; the backend then picks
  // from that with a preference list. Pinning S16_LE here would make a 24-bit interface look
  // 16-bit and silently throw away precision.
  CHECK(namchain::alsa::params_format_available(params, SNDRV_PCM_FORMAT_S16_LE));
  CHECK(namchain::alsa::params_format_available(params, SNDRV_PCM_FORMAT_S24_3LE));
  CHECK(namchain::alsa::params_format_available(params, SNDRV_PCM_FORMAT_S32_LE));
  CHECK_FALSE(namchain::alsa::params_format_is_exactly(params, SNDRV_PCM_FORMAT_S16_LE));

  unsigned value = 0;
  REQUIRE(namchain::alsa::params_single_value(params, SNDRV_PCM_HW_PARAM_CHANNELS, value));
  CHECK(value == 2u);
  REQUIRE(namchain::alsa::params_single_value(params, SNDRV_PCM_HW_PARAM_RATE, value));
  CHECK(value == 48000u);

  // Period size is deliberately left unconstrained so HW_REFINE can report the device's range.
  unsigned min = 0;
  unsigned max = 0;
  REQUIRE(namchain::alsa::params_interval_bounds(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, min, max));
  CHECK(max == 4294967295u);
}

TEST_CASE("format preference picks the best the device offers, and fails when none is offered")
{
  const unsigned preferred[] = {SNDRV_PCM_FORMAT_S24_3LE, SNDRV_PCM_FORMAT_S32_LE, SNDRV_PCM_FORMAT_S16_LE};
  const unsigned count = 3;
  unsigned chosen = 0;

  // A 24-bit interface: the top preference wins.
  struct snd_pcm_hw_params params;
  namchain::alsa::params_set_any(params);
  namchain::alsa::params_set_format(params, SNDRV_PCM_FORMAT_S24_3LE);
  REQUIRE(namchain::alsa::params_pick_format(params, preferred, count, chosen));
  CHECK(chosen == SNDRV_PCM_FORMAT_S24_3LE);

  // The PCM2902: only S16_LE, so the list falls through rather than failing.
  namchain::alsa::params_set_any(params);
  namchain::alsa::params_set_format(params, SNDRV_PCM_FORMAT_S16_LE);
  REQUIRE(namchain::alsa::params_pick_format(params, preferred, count, chosen));
  CHECK(chosen == SNDRV_PCM_FORMAT_S16_LE);

  // A device offering only something we do not support must be refused, not coerced.
  namchain::alsa::params_set_any(params);
  namchain::alsa::params_set_format(params, SNDRV_PCM_FORMAT_FLOAT_LE);
  CHECK_FALSE(namchain::alsa::params_pick_format(params, preferred, count, chosen));
}

TEST_CASE("bytes per sample treats S24_3LE as packed, not padded")
{
  CHECK(namchain::alsa::bytes_per_sample(SNDRV_PCM_FORMAT_S16_LE) == 2u);
  CHECK(namchain::alsa::bytes_per_sample(SNDRV_PCM_FORMAT_S24_3LE) == 3u);
  CHECK(namchain::alsa::bytes_per_sample(SNDRV_PCM_FORMAT_S32_LE) == 4u);
  CHECK(namchain::alsa::bytes_per_sample(SNDRV_PCM_FORMAT_FLOAT_LE) == 0u);
}

TEST_CASE("interval pinning and single-value readback round-trip")
{
  struct snd_pcm_hw_params params;
  namchain::alsa::params_set_any(params);
  namchain::alsa::params_set_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 64);

  unsigned value = 0;
  REQUIRE(namchain::alsa::params_single_value(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, value));
  CHECK(value == 64u);

  unsigned min = 0;
  unsigned max = 0;
  REQUIRE(namchain::alsa::params_interval_bounds(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, min, max));
  CHECK(min == 64u);
  CHECK(max == 64u);

  // A mask parameter is not an interval parameter.
  CHECK_FALSE(namchain::alsa::params_single_value(params, SNDRV_PCM_HW_PARAM_FORMAT, value));
}

TEST_CASE("clamping is what turns a 32-frame request into the device floor")
{
  // The PCM2902 reports period_size [48..48000] at 48 kHz (measured; plan.md §9.5).
  CHECK(namchain::alsa::clamp_to_bounds(32, 48, 48000) == 48u);
  CHECK(namchain::alsa::clamp_to_bounds(64, 48, 48000) == 64u);
  CHECK(namchain::alsa::clamp_to_bounds(96000, 48, 48000) == 48000u);
  CHECK(namchain::alsa::clamp_to_bounds(48, 48, 48000) == 48u);
}

TEST_CASE("PCM device specifications resolve to the right node")
{
  std::string node;
  std::string error;

  CHECK(namchain::resolve_pcm_node("hw:4,0", true, node, error));
  CHECK(node == "/dev/snd/pcmC4D0c");
  CHECK(namchain::resolve_pcm_node("hw:4,0", false, node, error));
  CHECK(node == "/dev/snd/pcmC4D0p");
  CHECK(namchain::resolve_pcm_node("0,1", false, node, error));
  CHECK(node == "/dev/snd/pcmC0D1p");
  CHECK(namchain::resolve_pcm_node("/dev/snd/pcmC9D2c", true, node, error));
  CHECK(node == "/dev/snd/pcmC9D2c");

  node.clear();
  error.clear();
  CHECK_FALSE(namchain::resolve_pcm_node("not-a-device", true, node, error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(namchain::resolve_pcm_node("hw:", true, node, error));
}

TEST_CASE("opening a device that does not exist fails cleanly")
{
  // No hardware is needed for this: the failure path is what matters, and it is the one CI can
  // exercise. A real device is covered by the device-mode soak on the target.
  namchain::AlsaRawConfig config;
  config.capture = "/dev/snd/definitely-not-a-pcm-device";
  config.playback = "/dev/snd/definitely-not-a-pcm-device";

  std::string error;
  const std::unique_ptr<namchain::AlsaRawDuplex> device = namchain::AlsaRawDuplex::open(config, error);
  CHECK(device == nullptr);
  CHECK_FALSE(error.empty());
}

TEST_CASE("an invalid channel count is rejected before touching the device")
{
  namchain::AlsaRawConfig config;
  config.capture = "hw:4,0";
  config.playback = "hw:4,0";
  config.channels = 33;

  std::string error;
  CHECK(namchain::AlsaRawDuplex::open(config, error) == nullptr);
  CHECK(error.find("channel") != std::string::npos);
}

#endif // __linux__
