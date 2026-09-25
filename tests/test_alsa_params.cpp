// Tests for the raw-ALSA parameter arithmetic and device resolution.
//
// These deliberately need no audio hardware, so they run in CI (which has none) -- they cover the
// logic that has to be right for the negotiation to be trustworthy: the parameter space, the
// clamping that turns a 32-frame request into the device's floor, and node resolution.
#include <doctest.h>

#include <string>
#include <unistd.h>
#include <cstdlib>

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

TEST_CASE("PCM device specifications resolve card ids through /proc/asound")
{
  // A stand-in for /proc/asound as tib has it: each card id is a symlink to its cardN directory.
  // "II" is the SSL 2+ Mk II; ALSA derives the id from the product name.
  char root_template[] = "/tmp/namchain-asound-XXXXXX";
  const char* made = ::mkdtemp(root_template);
  REQUIRE(made != nullptr);
  const std::string root = made;
  REQUIRE(::symlink("card1", (root + "/II").c_str()) == 0);
  REQUIRE(::symlink("card2", (root + "/rockchiptinkerc").c_str()) == 0);

  std::string node;
  std::string error;
  CHECK(namchain::resolve_pcm_node("hw:CARD=II,DEV=0", true, node, error, root));
  CHECK(node == "/dev/snd/pcmC1D0c");
  CHECK(namchain::resolve_pcm_node("hw:CARD=II", false, node, error, root));
  CHECK(node == "/dev/snd/pcmC1D0p");
  CHECK(namchain::resolve_pcm_node("hw:II,1", false, node, error, root));
  CHECK(node == "/dev/snd/pcmC1D1p");
  CHECK(namchain::resolve_pcm_node("rockchiptinkerc", false, node, error, root));
  CHECK(node == "/dev/snd/pcmC2D0p");
  // An index still works, and never touches the lookup.
  CHECK(namchain::resolve_pcm_node("hw:4", true, node, error, root));
  CHECK(node == "/dev/snd/pcmC4D0c");

  // A missing id is an error that says which ids exist, not a guess at another card.
  error.clear();
  CHECK_FALSE(namchain::resolve_pcm_node("hw:CARD=WM8960", true, node, error, root));
  CHECK(error.find("WM8960") != std::string::npos);
  CHECK(error.find("II") != std::string::npos);
  CHECK(error.find("rockchiptinkerc") != std::string::npos);

  CHECK_FALSE(namchain::resolve_pcm_node("hw:CARD=II,0", true, node, error, root));
  CHECK_FALSE(namchain::resolve_pcm_node("hw:II,x", true, node, error, root));
  CHECK_FALSE(namchain::resolve_pcm_node("hw:CARD=", true, node, error, root));

  ::unlink((root + "/II").c_str());
  ::unlink((root + "/rockchiptinkerc").c_str());
  ::rmdir(root.c_str());
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

TEST_CASE("ring occupancy comes from the pointers, and matches the kernel's own avail")
{
  // Fixtures read from /proc/asound/card2/pcm0[cp]/sub0/status while a stream ran on the SSL 2+ Mk
  // II, buffer 256 frames. Each triple is (hw_ptr, appl_ptr, the kernel's `avail`), and they are
  // worth keeping as a test for three reasons: the raw pointers run far past the buffer length (up
  // to 87 616 against a 256-frame ring), so the modulo has to be taken on the subtraction rather
  // than on each pointer; `avail` is an independent check in both directions; and the field that
  // disagrees, `delay`, is in the same snapshot to show why it is not the one to use.
  struct Sample
  {
    unsigned long long hw_ptr;
    unsigned long long appl_ptr;
    long long avail;
  };
  const Sample capture[] = {{7177, 7168, 9},   {15170, 15168, 2},  {23235, 23232, 3},
                            {31276, 31232, 44}, {39294, 39232, 62}, {47311, 47296, 15},
                            {55328, 55296, 32}, {63321, 63296, 25}, {71338, 71296, 42},
                            {79403, 79360, 43}, {87420, 87360, 60}};
  for (const Sample& s : capture)
    CHECK(namchain::alsa::pcm_ring_frames(s.hw_ptr, s.appl_ptr, 256, true) ==
          static_cast<unsigned long long>(s.avail));

  // Playback has the opposite polarity, and `avail` there is the *room*, i.e. buffer - ring.
  const Sample playback[] = {{7225, 7360, 121},  {15218, 15424, 50}, {23283, 23488, 51},
                             {31324, 31488, 92}, {39342, 39488, 110}, {47359, 47552, 63},
                             {55352, 55552, 56}, {63393, 63552, 97},  {71434, 71616, 74},
                             {79475, 79616, 115}, {87468, 87616, 108}};
  for (const Sample& s : playback)
    CHECK(namchain::alsa::pcm_ring_frames(s.hw_ptr, s.appl_ptr, 256, false) ==
          256ull - static_cast<unsigned long long>(s.avail));

  // The case a per-pointer modulo gets wrong: appl_ptr has wrapped past a buffer boundary while
  // hw_ptr has not, so the pointers are in different buffer epochs. 205 frames are queued; reducing
  // each pointer first yields 141, which is still inside the ring and would have been believed.
  CHECK(namchain::alsa::pcm_ring_frames(23283, 23488, 256, false) == 205);

  // Degenerate inputs are answered rather than divided by.
  CHECK(namchain::alsa::pcm_ring_frames(1000, 1000, 256, false) == 0);
  CHECK(namchain::alsa::pcm_ring_frames(1000, 1000, 0, true) == 0);
}

TEST_CASE("the queue target refuses only the periods that would exceed it")
{
  // The output-queue policy (plan.md §9.5 item 7) is one comparison, and the whole reason it is a
  // function is that the comparison's direction is the difference between a loop that holds its queue
  // and one that drops half the audio to hold it. The device consumes one period while the loop
  // produces one, so a write made when the ring holds exactly the target leaves the ring holding the
  // target again: `== target` is a fixed point and must be *allowed*. Refusing there instead drops
  // every other period for ever, which is the failure this test exists to prevent from coming back.
  const unsigned long long period = 64;
  const unsigned long long target = 2 * period;

  CHECK(namchain::alsa::queue_allows_write(0, target));
  CHECK(namchain::alsa::queue_allows_write(period, target));
  CHECK(namchain::alsa::queue_allows_write(target, target));       // the fixed point
  CHECK_FALSE(namchain::alsa::queue_allows_write(target + 1, target)); // one frame over: refuse
  CHECK_FALSE(namchain::alsa::queue_allows_write(3 * period, target));

  // Zero means no target, and then the ring's own capacity is the only bound. This is the behaviour
  // every measurement taken before the knob existed ran with, so it has to stay exactly "always write"
  // rather than becoming "write only into an empty ring".
  CHECK(namchain::alsa::queue_allows_write(0, 0));
  CHECK(namchain::alsa::queue_allows_write(period, 0));
  CHECK(namchain::alsa::queue_allows_write(4 * period, 0));
}

TEST_CASE("the prime is the target, and at least one period")
{
  // The cushion cannot be built after the fact -- the loop produces one period per period, so the
  // ring only grows when a burst recovers a late period, which is what the target refuses -- so the
  // prime is where the target comes from. One period is the floor whatever is asked for, because the
  // kernel's start threshold is one period and a stream started empty underruns immediately.
  CHECK(namchain::alsa::queue_prime_frames(64, 0) == 64); // no target: one period, as before the knob
  CHECK(namchain::alsa::queue_prime_frames(64, 1) == 64);
  CHECK(namchain::alsa::queue_prime_frames(64, 2) == 128);
  CHECK(namchain::alsa::queue_prime_frames(64, 3) == 192);
  // A rounded period must be used, not the requested one: the ring is measured in the device's frames.
  CHECK(namchain::alsa::queue_prime_frames(48, 3) == 144);
}

TEST_CASE("a queue target that cannot fit the ring is refused before touching the device")
{
  // The failure this prevents is a target the policy can never satisfy: if the ring may hold T periods
  // and the target is T, there is never room for the period being decided about, so every period is
  // refused for ever and the run looks like a device that will not accept audio.
  namchain::AlsaRawConfig config;
  config.capture = "hw:4,0";
  config.playback = "hw:4,0";
  config.periods = 4;
  config.output_queue_periods = 4;

  std::string error;
  CHECK(namchain::AlsaRawDuplex::open(config, error) == nullptr);
  CHECK(error.find("output queue target") != std::string::npos);

  // One period less than the ring is the largest target that fits, and it must get as far as trying
  // to open the device (which does not exist here) rather than being refused by this check.
  config.output_queue_periods = 3;
  error.clear();
  CHECK(namchain::AlsaRawDuplex::open(config, error) == nullptr);
  CHECK(error.find("output queue target") == std::string::npos);
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

TEST_CASE("the floor controller tops up only when the queue is short and a period fits")
{
  // The other end of the cap's policy (plan.md §9.5 item 9). A short queue can only be refilled by
  // something other than the signal, so the decision has to be exactly "short, and a whole period fits"
  // -- an insertion that does not fit would block in the write, which is the one thing the loop may not
  // do, and an insertion that happens when the queue is *not* short is latency added for nothing.
  const unsigned long long period = 64;
  const unsigned long long buffer = 384; // a 6-period ring
  const unsigned long long floor = 3 * period;

  CHECK(namchain::alsa::queue_needs_top_up(0, floor, period, buffer));
  CHECK(namchain::alsa::queue_needs_top_up(period, floor, period, buffer));
  CHECK(namchain::alsa::queue_needs_top_up(floor - 1, floor, period, buffer));
  CHECK_FALSE(namchain::alsa::queue_needs_top_up(floor, floor, period, buffer)); // at the floor: done
  CHECK_FALSE(namchain::alsa::queue_needs_top_up(floor + period, floor, period, buffer));

  // No floor means no controller, which is what keeps the default path exactly the path every earlier
  // measurement took.
  CHECK_FALSE(namchain::alsa::queue_needs_top_up(0, 0, period, buffer));

  // Room is the condition that keeps the insertion from blocking. It is a guard rather than a live path:
  // the configuration refuses a floor at or above the ring's length, so a queue below the floor always
  // leaves room for a period -- which is exactly why the controller can never block in the write. The
  // case is still pinned here, because "cannot happen" is a claim and this is what makes it one.
  const unsigned long long greedy_floor = buffer;                       // more than the ring can hold
  CHECK(namchain::alsa::queue_needs_top_up(buffer - period, greedy_floor, period, buffer)); // fits exactly
  CHECK_FALSE(namchain::alsa::queue_needs_top_up(buffer - period + 1, greedy_floor, period, buffer));
  CHECK_FALSE(namchain::alsa::queue_needs_top_up(0, floor, period, 0)); // no ring at all
}

#endif // __linux__
