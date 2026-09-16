#include "namchain/io/alsa_raw.h"

#if defined(__linux__)

  #include <cerrno>
  #include <chrono>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <fcntl.h>
  #include <poll.h>
  #include <sound/asound.h>
  #include <string>
  #include <sys/ioctl.h>
  #include <unistd.h>
  #include <vector>

  #include "namchain/io/alsa_params.h"
#include "namchain/rt/splice_fader.h"

namespace namchain
{
namespace
{

std::string errno_text(const std::string& what)
{
  return what + ": " + std::strerror(errno);
}

/// \brief The message a capture-gate timeout carries.
///
/// "capture timed out" alone cannot distinguish a device that stopped delivering from a gate that
/// stopped being satisfiable, and a timeout is what ends a long soak -- so the last state and ring
/// occupancy the gate saw, and how many xruns it recovered, travel with the error. The ring figure
/// matters most: an xrun freezes the pointer, so a ring that stopped growing is the signature.
std::string timeout_text(int timeout_ms, int state, long long ring, uint64_t gate_xruns)
{
  std::string text = "capture timed out after " + std::to_string(timeout_ms) + " ms (last state " +
                     std::to_string(state) + ", ring " + std::to_string(ring) + " frames";
  if (gate_xruns > 0)
    text += ", " + std::to_string(gate_xruns) + " capture xruns recovered at the gate";
  text += ")";
  return text;
}

bool parse_card_device(const std::string& text, unsigned& card, unsigned& device)
{
  unsigned c = 0;
  unsigned d = 0;
  if (std::sscanf(text.c_str(), "%u,%u", &c, &d) != 2)
    return false;
  card = c;
  device = d;
  return true;
}

} // namespace

bool resolve_pcm_node(const std::string& spec, bool capture, std::string& node, std::string& error)
{
  if (spec.rfind("/dev/", 0) == 0)
  {
    node = spec;
    return true;
  }

  std::string rest = spec;
  if (rest.rfind("hw:", 0) == 0)
    rest = rest.substr(3);

  unsigned card = 0;
  unsigned device = 0;
  if (!parse_card_device(rest, card, device))
  {
    error = "cannot parse PCM device '" + spec + "'; expected hw:C,D, C,D or /dev/snd/pcmC*D*";
    return false;
  }

  node = "/dev/snd/pcmC" + std::to_string(card) + "D" + std::to_string(device) + (capture ? "c" : "p");
  return true;
}

AlsaRawDuplex::~AlsaRawDuplex()
{
  stop();
  if (capture_fd_ >= 0)
    ::close(capture_fd_);
  if (playback_fd_ >= 0)
    ::close(playback_fd_);
}

std::unique_ptr<AlsaRawDuplex> AlsaRawDuplex::open(const AlsaRawConfig& config, std::string& error)
{
  // Devices expose fixed channel counts per altset (the SSL 2+ offers only 4), so this is a
  // sanity bound rather than a policy: the engine picks which of them it uses.
  constexpr unsigned kMaxChannels = 32;
  const unsigned capture_channels = config.capture_channels == 0 ? config.channels : config.capture_channels;
  const unsigned playback_channels = config.playback_channels == 0 ? config.channels : config.playback_channels;
  if (capture_channels < 1 || capture_channels > kMaxChannels || playback_channels < 1 ||
      playback_channels > kMaxChannels)
  {
    error = "channel count out of range [1, " + std::to_string(kMaxChannels) + "]";
    return nullptr;
  }
  if (config.quantum < 1)
  {
    error = "quantum must be >= 1";
    return nullptr;
  }

  std::unique_ptr<AlsaRawDuplex> self(new AlsaRawDuplex());
  self->quantum_ = config.quantum;
  self->periods_ = config.periods == 0 ? 4 : config.periods;
  // A target equal to the ring is not a tight bound, it is an impossible one: the policy would refuse
  // every period for ever, because the ring could never have room for one more. Caught here rather
  // than at the first write, where it would look like the device refusing to accept audio.
  if (config.output_queue_periods != 0 && config.output_queue_periods >= self->periods_)
  {
    error = "output queue target of " + std::to_string(config.output_queue_periods) +
            " periods does not fit a ring of " + std::to_string(self->periods_) +
            " periods (the target must be less than --periods, so that the ring can hold the target "
            "plus the period being decided about)";
    return nullptr;
  }
  self->requested_queue_periods_ = config.output_queue_periods;
  // The floor controller's configuration is checked here rather than at the first short queue, where a
  // contradictory pair would show up as inserted silence the cap then refuses to make room for.
  if (config.output_queue_floor_periods != 0)
  {
    if (config.output_queue_floor_periods >= self->periods_)
    {
      error = "output queue floor of " + std::to_string(config.output_queue_floor_periods) +
              " periods does not fit a ring of " + std::to_string(self->periods_) + " periods";
      return nullptr;
    }
    if (config.output_queue_periods != 0 && config.output_queue_floor_periods > config.output_queue_periods)
    {
      error = "output queue floor of " + std::to_string(config.output_queue_floor_periods) +
              " periods is above the cap of " + std::to_string(config.output_queue_periods) +
              " periods; the controller would insert silence the cap exists to refuse";
      return nullptr;
    }
  }
  self->requested_floor_periods_ = config.output_queue_floor_periods;
  self->queue_debug_ = std::getenv("NAMCHAIN_QUEUE_DEBUG") != nullptr;
  self->format_preference_ = config.format_preference;
  self->enable_timestamps_ = config.enable_timestamps;
  self->info_.capture_channels = capture_channels;
  self->info_.playback_channels = playback_channels;
  self->info_.sample_rate = config.sample_rate;

  if (!resolve_pcm_node(config.capture, true, self->info_.capture_node, error))
    return nullptr;
  if (!resolve_pcm_node(config.playback, false, self->info_.playback_node, error))
    return nullptr;

  // Both directions are opened before either is configured: the capture endpoint is ASYNC with no
  // explicit feedback endpoint, so the driver wants the playback stream present (§9.5).
  self->capture_fd_ = ::open(self->info_.capture_node.c_str(), O_RDWR | O_NONBLOCK);
  if (self->capture_fd_ < 0)
  {
    error = errno_text("cannot open " + self->info_.capture_node);
    return nullptr;
  }
  self->playback_fd_ = ::open(self->info_.playback_node.c_str(), O_RDWR | O_NONBLOCK);
  if (self->playback_fd_ < 0)
  {
    // Distinguish "the second direction was refused" from "this node is wrong or unusable".
    // EINVAL here, with the capture side already open, is not a bad path and not a busy
    // device (that is EBUSY) -- it is a driver refusing to have both directions of one PCM
    // open at once. Measured on tib's on-board tinker-codec (2026-09-16): either direction
    // alone opens, the pair is refused with EINVAL, and a 1.5 s delay between the two opens
    // does not change it, so it is topology rather than a race. Without this note the message
    // reads like a malformed argument, which sends the reader to the channel count and the
    // rate instead of to the device.
    const int saved = errno;
    error = errno_text("cannot open " + self->info_.playback_node);
    if (saved == EINVAL)
      error += " (the capture side opened, so the path is valid: this device appears not to"
               " support full-duplex capture+playback on the same PCM -- either direction"
               " alone works. Use a device that exposes the two directions separately, or a"
               " USB interface, which does)";
    return nullptr;
  }

  if (!self->configure(self->capture_fd_, true, "capture", error))
    return nullptr;
  if (!self->configure(self->playback_fd_, false, "playback", error))
    return nullptr;

  if (!self->info_.period_exact)
  {
    if (config.require_exact_period)
    {
      error = "device negotiated a period of " + std::to_string(self->info_.period_size) +
              " frames, not the requested " + std::to_string(config.quantum) +
              "; full-speed USB has a 1000 us period floor (plan.md §9.5). Pass the accepted quantum, or set "
              "require_exact_period = false to adopt the device's value.";
      return nullptr;
    }
  }

  return self;
}

bool AlsaRawDuplex::configure(int fd, bool capture, const char* direction, std::string& error)
{
  struct snd_pcm_hw_params params;
  alsa::params_set_any(params);
  alsa::params_narrow_basics(params, capture ? info_.capture_channels : info_.playback_channels,
                             info_.sample_rate);

  // Phase 1: let the kernel narrow our request to what the device can actually do. Leaving the
  // period and buffer sizes unconstrained here is what makes it report its real ranges -- notably
  // the 1000 us PERIOD_TIME floor that turns a request for 32 frames into 48.
  if (::ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0)
  {
    // The usual cause is a channel count the device does not offer: USB audio exposes one altset
    // per channel count, so asking for a subset of what the device has fails outright rather than
    // being trimmed. The SSL 2+ Mk II is 6 out / 8 in on firmware V1.16, and asking for 4 gets
    // EINVAL from here.
    error = errno_text("SNDRV_PCM_IOCTL_HW_REFINE failed") + " (" + direction + " with " +
            std::to_string(capture ? info_.capture_channels : info_.playback_channels) +
            " channels at " + std::to_string(info_.sample_rate) +
            " Hz; a channel count the device does not offer is the usual cause)";
    return false;
  }

  // Pick the best format the device actually offers, rather than demanding one. FORMAT was left
  // as "any" by params_narrow_basics precisely so that refine reports the device's real list here.
  unsigned chosen_format = 0;
  if (!alsa::params_pick_format(params, format_preference_.data(),
                                static_cast<unsigned>(format_preference_.size()), chosen_format))
  {
    error = std::string(direction) +
            ": device offers none of the requested sample formats (S24_3LE, S32_LE, S16_LE)";
    return false;
  }
  const unsigned format_width = alsa::bytes_per_sample(chosen_format);
  if (format_width == 0)
  {
    error = std::string(direction) + ": unsupported sample format " + std::to_string(chosen_format);
    return false;
  }
  alsa::params_set_format(params, chosen_format);

  unsigned period_min = 0;
  unsigned period_max = 0;
  if (!alsa::params_interval_bounds(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_min, period_max))
  {
    error = "device reports no usable period size range";
    return false;
  }

  // PERIOD_SIZE's range does not encode the rate-dependent floor: this device advertises
  // PERIOD_SIZE [32..48000] *and* PERIOD_TIME [1000..1000000] us, and the two only combine into
  // ">= 48 frames at 48 kHz". Pinning 32 directly makes the space unsatisfiable and HW_PARAMS
  // returns EINVAL, so the time bound has to be folded in here -- this is exactly the §9.5
  // rounding, and doing it explicitly is what turns a raw EINVAL into a clear message.
  const unsigned period_size_min_raw = period_min;
  const unsigned period_size_max_raw = period_max;

  unsigned period_time_min = 0;
  unsigned period_time_max = 0;
  const bool have_period_time =
    alsa::params_interval_bounds(params, SNDRV_PCM_HW_PARAM_PERIOD_TIME, period_time_min, period_time_max);
  if (have_period_time && period_time_min > 0)
  {
    const unsigned from_time = static_cast<unsigned>(
      (static_cast<unsigned long long>(period_time_min) * info_.sample_rate + 999999ull) / 1000000ull);
    if (from_time > period_min)
      period_min = from_time;
  }

  unsigned buffer_min = 0;
  unsigned buffer_max = 0;
  const bool have_buffer_bounds = alsa::params_interval_bounds(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, buffer_min, buffer_max);

  if (std::getenv("NAMCHAIN_ALSA_DEBUG") != nullptr)
  {
    std::fprintf(stderr,
                 "[alsa] %s refined: period_size [%u..%u], period_time [%s%u..%u] us, buffer_size [%u..%u]; "
                 "folded period floor %u at %u Hz\n",
                 direction, period_size_min_raw, period_size_max_raw, have_period_time ? "" : "unavailable ",
                 period_time_min, period_time_max, buffer_min, buffer_max, period_min, info_.sample_rate);
  }

  // The clamp is the whole point of the negotiation: a request for 32 becomes the device's floor.
  const unsigned period = alsa::clamp_to_bounds(quantum_, period_min == 0 ? 1 : period_min, period_max);
  alsa::params_set_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);

  unsigned buffer = period * periods_;
  if (have_buffer_bounds)
  {
    // The buffer must be a whole number of periods, or HW_PARAMS fails with EINVAL. That matters
    // precisely when the period was clamped (ask for 32, get 48): 4 x 32 = 128 is not a multiple
    // of 48. So derive the buffer from the *actual* period and round inside the device's range.
    if (buffer < buffer_min)
      buffer = ((buffer_min + period - 1) / period) * period;
    if (buffer_max >= period && buffer > buffer_max)
      buffer = (buffer_max / period) * period;
    if (buffer < period * 2)
      buffer = period * 2;
    alsa::params_set_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, buffer);
  }

  // Phase 2: commit, with both sizes pinned to single values. rmask is set again so the kernel
  // refines against what we just pinned rather than ignoring it.
  params.rmask = ~0u;
  params.cmask = 0;
  if (::ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0)
  {
    error = errno_text("SNDRV_PCM_IOCTL_HW_PARAMS failed for " + std::string(direction) + " (requested quantum " +
                       std::to_string(quantum_) + ", using period " + std::to_string(period) + ", buffer " +
                       std::to_string(buffer) + ")");
    return false;
  }

  unsigned actual_period = period;
  unsigned actual_buffer = 0;
  alsa::params_single_value(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, actual_period);
  if (!alsa::params_single_value(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, actual_buffer))
    actual_buffer = period * periods_;

  if (info_.period_size == 0)
  {
    info_.period_size = actual_period;
    info_.buffer_size = actual_buffer;
    info_.sample_format = chosen_format;
    info_.bytes_per_sample = format_width;
    info_.period_exact = (actual_period == quantum_);
    info_.capture_bytes_per_frame = info_.capture_channels * format_width;
    info_.playback_bytes_per_frame = info_.playback_channels * format_width;
    capture_bytes_per_frame_ = info_.capture_bytes_per_frame;
    playback_bytes_per_frame_ = info_.playback_bytes_per_frame;
    prime_silence_.assign(static_cast<size_t>(info_.buffer_size) * playback_bytes_per_frame_, 0);
    drain_buffer_.assign(static_cast<size_t>(info_.period_size) * capture_bytes_per_frame_, 0);
    last_frame_.assign(playback_bytes_per_frame_, 0);
    previous_frame_.assign(playback_bytes_per_frame_, 0);
    hold_period_.assign(static_cast<size_t>(info_.period_size) * playback_bytes_per_frame_, 0);
    // The target in frames, now that a period is a known number rather than a request. `info_` carries
    // it in periods too: the write path needs the frames and the report needs the periods, and
    // deriving either from the other at the point of use is how the two come to disagree.
    target_frames_ = requested_queue_periods_ == 0
                       ? 0
                       : static_cast<unsigned>(
                           alsa::queue_prime_frames(info_.period_size, requested_queue_periods_));
    floor_frames_ = requested_floor_periods_ == 0
                      ? 0
                      : static_cast<unsigned>(
                          alsa::queue_prime_frames(info_.period_size, requested_floor_periods_));
    info_.output_queue_periods = requested_queue_periods_;
    info_.output_queue_floor_periods = requested_floor_periods_;
  }
  else if (info_.sample_format != chosen_format)
  {
    error = std::string(direction) + " agreed sample format " + std::to_string(chosen_format) +
            " but the other direction agreed " + std::to_string(info_.sample_format) +
            "; both directions must agree";
    return false;
  }
  else if (info_.period_size != actual_period || info_.buffer_size != actual_buffer)
  {
    error = "capture negotiated " + std::to_string(actual_period) + "/" + std::to_string(actual_buffer) +
            " but playback negotiated " + std::to_string(info_.period_size) + "/" +
            std::to_string(info_.buffer_size) + " frames; both directions must agree";
    return false;
  }

  // Software parameters, which is where the transfer cadence actually lives.
  //
  // `avail_min` is what `poll` wakes on, and leaving it at whatever the kernel picked is how the loop
  // ended up paced by the wrong ring: with a threshold of one frame, `poll` returns almost
  // immediately in a running stream, so the loop free-runs until the playback ring is full and the
  // write becomes the clock. One period makes the wakeup land on the period boundary the loop
  // actually consumes at, which is what wait_capture() then checks against the device position.
  //
  // `start_threshold` for playback is one period, matching prime_playback()'s one-period prime: the
  // ring is deliberately not filled, so a write always has room and the I/O latency floor is a
  // period rather than a buffer (§9.5).
  struct snd_pcm_sw_params sw;
  std::memset(&sw, 0, sizeof(sw));
  sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
  sw.period_step = 1;
  sw.avail_min = actual_period;
  sw.start_threshold = capture ? 1 : actual_period;
  sw.stop_threshold = actual_buffer;
  sw.silence_threshold = 0;
  sw.silence_size = 0;
  if (::ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0)
  {
    error = errno_text("SNDRV_PCM_IOCTL_SW_PARAMS failed for " + std::string(direction));
    return false;
  }

  // Timestamps are off unless asked for, and both the enable and the clock choice can be refused.
  // Failure is not fatal: a caller that needs them can check info().timestamps_enabled.
  if (enable_timestamps_)
  {
    int enabled = 1;
    if (::ioctl(fd, SNDRV_PCM_IOCTL_TSTAMP, &enabled) == 0)
    {
      int clock_type = SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW;
      ::ioctl(fd, SNDRV_PCM_IOCTL_TTSTAMP, &clock_type);
      info_.timestamps_enabled = true;
    }
  }

  return true;
}

bool AlsaRawDuplex::prime_playback(std::string& error)
{
  if (info_.buffer_size == 0 || prime_silence_.empty())
    return true;
  // Prime ONE period by default, not the whole ring. Filling the ring (which is what aplay
  // effectively does for a file) leaves it permanently full, so every subsequent write then blocks
  // for a whole period and the loop's latency becomes one buffer rather than one period. Measured:
  // with a full-ring prime the write blocked 1416 us of a 1333 us period; the engine's actual work
  // was 70 us. One period is enough to reach the kernel's start threshold.
  //
  // With a queue target set, the prime IS the target: the cushion a target describes has to exist
  // before the first period is processed, and the loop cannot build it afterwards. It cannot, because
  // it receives one period of input per period and can therefore produce at most one period of output
  // per period -- the ring only grows when a capture burst lets the loop write twice in one period,
  // which is exactly the event that pushes it over the target and gets refused. So the target is
  // primed, then held, and never earned (§9.5 item 7).
  //
  // This is also why the original full-ring prime *looked* like it worked: it was an accidental queue
  // target of the whole ring, latency and all.
  const unsigned chunk = static_cast<unsigned>(
    alsa::queue_prime_frames(info_.period_size, requested_queue_periods_));
  const int written = write_playback(prime_silence_.data(), chunk, 1000, error);
  if (written < static_cast<int>(chunk))
  {
    error = "could not prime the playback buffer: " + error;
    return false;
  }
  return true;
}

bool AlsaRawDuplex::drain_capture(std::string& error)
{
  // Settle the capture side before the timed loop. The device produces from capture START, and
  // however many periods accumulated before the loop was ready would otherwise be read back to back
  // -- and, because the loop writes one period per read, written back to back into a playback ring
  // that was primed with exactly one. That burst is the first step of the ratchet in plan.md §9.5
  // item 4: each backlog period becomes an extra queued period, the ring reaches its ceiling, and
  // the write's wait for space then becomes the reported block time for ever after. Draining here
  // makes the loop's starting occupancy a known number instead of however long bring-up took.
  //
  // Each discarded period is replaced with a period of silence on the playback side, when the ring
  // has room for it. That is not cosmetic: the ring is primed and nothing else feeds it while this
  // runs, so discarding without writing would drain the prime and hand the loop less queue than it
  // was promised. With the replacement the ring is exactly as full as the prime left it, and the
  // discarded audio is bring-up capture the engine never had a graph for. It is counted so the report
  // can say how much. The room check is what keeps the replacement from blocking: with a queue target
  // the ring may already hold the target, and waiting for the device here would stall the very
  // bring-up this function exists to make predictable.
  //
  // Both streams must already be running before this is called. An earlier version drained between
  // capture START and playback START, which leaves the capture stream running with no playback
  // stream beside it -- on a device whose capture endpoint takes implicit feedback from playback,
  // that is a stream behaving unlike the one the loop will see.
  const unsigned max_blocks = info_.period_size == 0 ? 0 : info_.buffer_size / info_.period_size + 2;
  for (unsigned i = 0; i < max_blocks; ++i)
  {
    AlsaRawOccupancy status;
    std::string ignored;
    if (!query_occupancy(true, status, ignored))
      return true; // nothing to drain if the device will not tell us what it holds
    if (status.ring_frames < static_cast<long long>(info_.period_size))
      return true;
    if (read_capture(drain_buffer_.data(), info_.period_size, 1000, ignored) < 0)
    {
      // Not fatal: the loop's own gate and the xrun counter handle a device that will not give us a
      // period. Leaving the backlog is the old behaviour, not a new fault.
      error.clear();
      return true;
    }
    settle_discarded_frames_ += info_.period_size;
    // Only replace the discarded period if the playback ring can actually take it. With a queue
    // target the ring may already be sitting at the target, and a blocking write there would wait for
    // the device instead of bringing the loop up -- a stall during bring-up, which is the one thing
    // this function exists to prevent. The occupancy is re-read rather than reused because the read
    // above moved it.
    AlsaRawOccupancy playback;
    if (query_occupancy(false, playback, ignored) &&
        playback.ring_frames + static_cast<long long>(info_.period_size) <=
          static_cast<long long>(info_.buffer_size))
    {
      if (write_playback(prime_silence_.data(), info_.period_size, 1000, ignored) < 0)
      {
        error.clear();
        return true;
      }
    }
  }
  return true;
}

bool AlsaRawDuplex::start(std::string& error)
{
  if (::ioctl(capture_fd_, SNDRV_PCM_IOCTL_PREPARE) < 0)
  {
    error = errno_text("capture PREPARE failed");
    return false;
  }
  if (::ioctl(playback_fd_, SNDRV_PCM_IOCTL_PREPARE) < 0)
  {
    error = errno_text("playback PREPARE failed");
    return false;
  }

  // Playback must not be STARTed empty: the stream underruns immediately and the ioctl itself
  // returns -EPIPE (observed on the PCM2902). One period of silence is enough to reach the kernel's
  // start threshold (sw.start_threshold), and deliberately not a whole ring -- filling it is how the
  // write's wait for space became the clock in the first measurement round (§9.5 item 2).
  if (!prime_playback(error))
    return false;

  if (::ioctl(capture_fd_, SNDRV_PCM_IOCTL_START) < 0)
  {
    error = errno_text("capture START failed");
    return false;
  }

  // The priming write above may already have reached the kernel's start threshold, in which case
  // the stream is RUNNING and START reports EBADFD. That is success, not failure -- but confirm it
  // by asking the state rather than assuming.
  if (::ioctl(playback_fd_, SNDRV_PCM_IOCTL_START) < 0)
  {
    struct snd_pcm_status status;
    std::memset(&status, 0, sizeof(status));
    const bool already_running =
      ::ioctl(playback_fd_, SNDRV_PCM_IOCTL_STATUS, &status) == 0 && status.state == SNDRV_PCM_STATE_RUNNING;
    if (!already_running)
    {
      error = errno_text("playback START failed");
      return false;
    }
  }

  return drain_capture(error);
}

void AlsaRawDuplex::stop()
{
  if (playback_fd_ >= 0)
    ::ioctl(playback_fd_, SNDRV_PCM_IOCTL_DROP);
  if (capture_fd_ >= 0)
    ::ioctl(capture_fd_, SNDRV_PCM_IOCTL_DROP);
}

bool AlsaRawDuplex::fill_output_queue(std::string& error)
{
  if (info_.buffer_size == 0 || info_.period_size == 0)
    return true;
  // Whichever end of the trade is configured decides what the timed loop starts with: a cap is a level
  // to hold, and a floor is a level to be at least. With both, the cap is the higher of the two --
  // starting at the floor when a cap was asked for would leave the run below the level it was told to
  // hold, and the controller would then be inserting silence from block 0 for no reason.
  const unsigned target = std::max(target_frames_, floor_frames_);
  if (target == 0)
    return true;
  // A short loop rather than one write, because the thing being chased is a queue the device is
  // draining: the deficit is measured again each time instead of being written once and hoped for.
  // It cannot overshoot -- the ring only ever loses frames while this runs, so each write lands at or
  // below the target -- and it cannot spin, because each attempt is bounded by the device's own rate:
  // writing a period takes microseconds against a 1333 us period.
  for (unsigned attempt = 0; attempt < 4; ++attempt)
  {
    AlsaRawOccupancy occupancy;
    std::string ignored;
    if (!query_occupancy(false, occupancy, ignored))
    {
      error = "cannot read the playback ring to establish the output queue: " + ignored;
      return false;
    }
    if (occupancy.ring_frames >= static_cast<long long>(target))
      return true;
    const long long missing = static_cast<long long>(target) - occupancy.ring_frames;
    const long long room = static_cast<long long>(info_.buffer_size) - occupancy.ring_frames;
    const unsigned chunk = static_cast<unsigned>(std::min(missing, std::max(0LL, room)));
    if (chunk == 0)
      return true; // the ring is as full as it can be, which is a different report's problem
    if (write_playback(prime_silence_.data(), chunk, 1000, ignored) < 0)
    {
      error = "could not establish the output queue of " + std::to_string(target) + " frames: " + ignored;
      return false;
    }
    prefill_frames_ += chunk;
  }
  return true;
}

void AlsaRawDuplex::recover(int fd, bool playback)
{
  // Recovering is right for a soak (we want to keep running); hiding it is not, which is why
  // driver_xruns() is reported alongside recoveries().
  if (::ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) == 0)
  {
    // PREPARE empties the playback ring, so restarting without re-priming it underruns again
    // immediately -- which showed up as hundreds of xruns per run with a median block cost of
    // only 8% of the deadline. Re-prime before restarting.
    if (playback)
    {
      std::string ignored;
      prime_playback(ignored);
    }
    ::ioctl(fd, SNDRV_PCM_IOCTL_START);
  }
  ++recoveries_;
}

int AlsaRawDuplex::transfer(int fd, void* buf, unsigned frames, bool playback, int timeout_ms, std::string& error)
{
  unsigned done = 0;
  while (done < frames)
  {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = static_cast<short>(playback ? POLLOUT : POLLIN);
    pfd.revents = 0;

    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready == 0)
    {
      error = std::string(playback ? "playback" : "capture") + " timed out after " + std::to_string(timeout_ms) + " ms";
      return done == 0 ? -1 : static_cast<int>(done);
    }
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      error = errno_text("poll failed");
      return -1;
    }

    struct snd_xferi xfer;
    std::memset(&xfer, 0, sizeof(xfer));
    xfer.buf = static_cast<char*>(buf) +
              static_cast<size_t>(done) * (playback ? playback_bytes_per_frame_ : capture_bytes_per_frame_);
    xfer.frames = frames - done;

    const int rc = ::ioctl(fd, playback ? SNDRV_PCM_IOCTL_WRITEI_FRAMES : SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
    if (rc < 0)
    {
      if (errno == EPIPE)
      {
        ++driver_xruns_;
        if (playback)
          ++playback_xruns_;
        else
          ++capture_xruns_;
        recover(fd, playback);
        continue; // retry the remainder after re-preparing
      }
      if (errno == EAGAIN || errno == EINTR)
        continue;
      error = errno_text(playback ? "write failed" : "read failed");
      return -1;
    }
    if (xfer.result <= 0)
      continue; // nothing moved; poll again rather than spin
    done += static_cast<unsigned>(xfer.result);
  }
  return static_cast<int>(done);
}

int AlsaRawDuplex::read_capture(void* dst, unsigned frames, int timeout_ms, std::string& error)
{
  return transfer(capture_fd_, dst, frames, false, timeout_ms, error);
}

bool AlsaRawDuplex::wait_capture(int timeout_ms, std::string& error)
{
  const auto started = std::chrono::steady_clock::now();
  // Reported in the timeout message. "capture timed out" on its own cannot distinguish a device that
  // stopped delivering from a gate that stopped being satisfiable, and the timeout is what ends a
  // long soak -- so the state and the ring that produced it travel with the error.
  int last_state = SNDRV_PCM_STATE_OPEN;
  long long last_ring = -1;

  for (;;)
  {
    // One period *available to read*, which is a question about the device pointer, not about how
    // much happens to be queued: reading a period takes the availability back below a period, so the
    // gate throttles the loop to the device's own rate however far behind it once fell.
    //
    // "Has a period of production happened since my last read" is the tempting phrasing and it is
    // wrong: production continues while the engine works, so that gate charges the work to the
    // deadline as well and the loop runs (work / period) short of real time. Measured with that gate:
    // RTF 1.175 and 39 170 xruns in five minutes, against RTF 0.9999 for this one.
    AlsaRawOccupancy status;
    std::string ignored;
    if (query_occupancy(true, status, ignored))
    {
      last_state = status.state;
      last_ring = status.ring_frames;
      // An xrun on the capture stream stops the hardware pointer advancing, so the gate below would
      // wait for a period that can never arrive: the stream is dead, not slow. This is the one place
      // the loop can notice, because the gate is exactly what would have let a read through, and a
      // read is what reports -EPIPE. Without this the soak aborts on a two-second "capture timed
      // out" -- observed at block 52 443 of an hour-long run, with the ring frozen at 13 frames --
      // which is a stall the run could have survived and reported instead of dying of.
      if (status.state == SNDRV_PCM_STATE_XRUN)
      {
        ++driver_xruns_;
        ++capture_xruns_;
        ++gate_capture_xruns_;
        last_state = status.state;
        last_ring = status.ring_frames;
        recover(capture_fd_, false);
        continue; // re-check against the remaining timeout, now on a prepared stream
      }
      if (status.state == SNDRV_PCM_STATE_SUSPENDED || status.state == SNDRV_PCM_STATE_DISCONNECTED)
      {
        error = std::string("capture stream is ") +
                (status.state == SNDRV_PCM_STATE_SUSPENDED ? "suspended" : "disconnected");
        return false;
      }

      if (status.ring_frames >= static_cast<long long>(info_.period_size))
      {
        // A release with two periods already waiting means the loop is a period behind and will read
        // twice almost together. Counting it is the whole diagnostic for the ratchet in §9.5 item 4:
        // the burst is what turns one late period into a permanently full playback ring.
        if (status.ring_frames >= static_cast<long long>(2 * info_.period_size))
          ++capture_bursts_;
        return true;
      }
    }

    const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    const int remaining = timeout_ms - static_cast<int>(elapsed);
    if (remaining <= 0)
    {
      error = timeout_text(timeout_ms, last_state, last_ring, gate_capture_xruns_);
      return false;
    }

    struct pollfd pfd;
    pfd.fd = capture_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    const int ready = ::poll(&pfd, 1, remaining);
    if (ready == 0)
    {
      error = timeout_text(timeout_ms, last_state, last_ring, gate_capture_xruns_);
      return false;
    }
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      error = errno_text("poll failed");
      return false;
    }
  }
}

int AlsaRawDuplex::write_playback(const void* src, unsigned frames, int timeout_ms, std::string& error)
{
  const int written = transfer(playback_fd_, const_cast<void*>(src), frames, true, timeout_ms, error);
  if (written == static_cast<int>(frames))
    remember_last_frame(src, frames);
  return written;
}

int AlsaRawDuplex::try_write_playback(const void* src, unsigned frames, std::string& error)
{
  // The queue target's decision, taken before the kernel is asked anything. This is the difference
  // between a loop that holds a stated latency and one whose latency is whatever the run happened to
  // accumulate: a burst (two capture periods released together, the loop's recovery from a late
  // period) would otherwise add a period to the ring, and the ring's capacity -- not the design --
  // would decide how much latency the run carries. See alsa::queue_allows_write for why the bound is
  // `<=` and why refusing above it is stable rather than a coin flip.
  //
  // Only queried when a target is set, so the default path is byte-for-byte the path every earlier
  // measurement took, including the cost of the extra STATUS ioctl it would otherwise add per period.
  if (target_frames_ != 0)
  {
    AlsaRawOccupancy occupancy;
    std::string ignored;
    if (!query_occupancy(false, occupancy, ignored))
      return -1;
    // Only a RUNNING stream has an occupancy. An xrun freezes hw_ptr, and the driver then reports an
    // `avail` past the end of the ring -- 260 frames for a 256-frame buffer, measured -- so the pointer
    // pair is describing a ring that no longer exists. Refusing on that figure starves the stream of
    // the very write that would report the xrun and recover it, and the loop then drops every period
    // for ever while the device sits stopped. Measured at target 1: one underrun at decision 5215, then
    // 2285 refusals in a row with hw_ptr frozen, and a report claiming zero underruns for the whole
    // run. So the target governs a running stream; anything else goes through to the driver, which is
    // the only thing that can say what is actually wrong.
    const bool running = occupancy.state == SNDRV_PCM_STATE_RUNNING;
    const bool allowed =
      running && alsa::queue_allows_write(static_cast<unsigned long long>(occupancy.ring_frames), target_frames_);
    if (queue_debug_)
      queue_debug_trace(occupancy, allowed);
    if (!allowed && running)
    {
      ++output_drops_;
      ++output_target_drops_;
      return 0;
    }
  }

  // Ask the kernel whether the ring can take a period *now*, and do not wait if it cannot. This is
  // the same readiness condition the blocking path would sleep on (avail_min is one period, set in
  // configure()), so a "yes" here means the transfer below cannot block, and a "no" is exactly the
  // state that used to cost 699 us of every period (plan.md §9.5 item 4). With a target set and the
  // stream running it should be unreachable -- the cap always leaves room for one more period -- but a
  // stream that is not running bypasses the cap above, so a refusal here is counted separately from a
  // target refusal rather than folded into it.
  struct pollfd pfd;
  pfd.fd = playback_fd_;
  pfd.events = POLLOUT;
  pfd.revents = 0;

  const int ready = ::poll(&pfd, 1, 0);
  if (ready == 0)
  {
    ++output_drops_;
    return 0;
  }
  if (ready < 0)
  {
    if (errno == EINTR)
    {
      ++output_drops_;
      return 0;
    }
    error = errno_text("poll failed for playback");
    return -1;
  }

  const int written = transfer(playback_fd_, const_cast<void*>(src), frames, true, 1000, error);
  if (written < 0)
    return -1;
  if (written != static_cast<int>(frames))
  {
    // Room for a period was advertised and then not delivered. Partial delivery cannot be dropped
    // quietly (it is half a period of audio in the ring), so this is an error rather than a drop.
    error = "playback took " + std::to_string(written) + " of " + std::to_string(frames) +
            " frames after POLLOUT reported room for a period";
    return -1;
  }
  remember_last_frame(src, frames);
  // After the signal, never before: the controller's job is to notice that the queue is short once this
  // period is in it, so its decision is made on the occupancy the loop actually produced.
  if (floor_frames_ != 0)
    maintain_output_floor();
  return written;
}

void AlsaRawDuplex::queue_debug_trace(const AlsaRawOccupancy& occupancy, bool allowed)
{
  // The first decisions show the queue being established; after that only the abnormal ones, because
  // the interesting event is a state that is not RUNNING and a healthy run is 100 % RUNNING. Bounded
  // so a dead stream cannot turn a soak into gigabytes of stderr.
  const bool abnormal = occupancy.state != SNDRV_PCM_STATE_RUNNING;
  if (queue_debug_lines_ >= 200 || (queue_debug_lines_ >= 12 && !abnormal))
    return;
  ++queue_debug_lines_;
  std::fprintf(stderr,
               "[queue] decision %u: ring %lld frames, avail %lld, state %d, hw %llu, appl %llu, "
               "target %u, %s\n",
               queue_debug_lines_, occupancy.ring_frames, occupancy.avail_frames, occupancy.state,
               occupancy.hw_ptr, occupancy.appl_ptr, target_frames_,
               allowed ? "write" : "refuse");
}

void AlsaRawDuplex::remember_last_frame(const void* src, unsigned frames)
{
  if (frames == 0 || last_frame_.empty())
    return;
  const unsigned char* bytes = static_cast<const unsigned char*>(src);
  // One frame of history is not enough for a payload that continues the *slope*: keep the frame before
  // this one too. Copied rather than assigned, so nothing here can allocate.
  std::copy(last_frame_.begin(), last_frame_.end(), previous_frame_.begin());
  std::memcpy(last_frame_.data(), bytes + static_cast<size_t>(frames - 1) * playback_bytes_per_frame_,
              playback_bytes_per_frame_);
}

const void* AlsaRawDuplex::insert_payload()
{
  const void* payload = prime_silence_.data();
  if (!last_frame_.empty() &&
      hold_period_.size() == static_cast<size_t>(info_.period_size) * playback_bytes_per_frame_ &&
      info_.period_size >= 2)
  {
    // A shaped payload, not a hold: it continues the signal's value *and* slope into the inserted
    // period and arrives flat, which is what the loop's bridge expects to leave from. Both joints are
    // then continuous, and the entry -- which the loop can never reach, because it does not write this
    // period -- stops being a slope break.
    const PcmFormat format = alsa::pcm_format(info_.sample_format);
    const unsigned channels = info_.playback_channels;
    const unsigned width = info_.bytes_per_sample;
    if (format != PcmFormat::Unknown && channels > 0 && width > 0)
    {
      for (unsigned c = 0; c < channels; ++c)
      {
        const float last = pcm_decode(last_frame_.data() + static_cast<size_t>(c) * width, format);
        const float previous = pcm_decode(previous_frame_.data() + static_cast<size_t>(c) * width, format);
        const float slope = last - previous;
        for (unsigned f = 0; f < info_.period_size; ++f)
          pcm_encode(hold_period_.data() + (static_cast<size_t>(f) * channels + c) * width, format,
                     rt::splice_insert_sample(last, slope, f, info_.period_size));
      }
      payload = hold_period_.data();
    }
    else
    {
      for (unsigned f = 0; f < info_.period_size; ++f)
        std::memcpy(hold_period_.data() + static_cast<size_t>(f) * playback_bytes_per_frame_,
                    last_frame_.data(), playback_bytes_per_frame_);
      payload = hold_period_.data();
    }
  }
  return payload;
}

bool AlsaRawDuplex::insert_output_period(std::string& error)
{
  if (info_.period_size == 0)
    return true;
  const int written = write_playback(insert_payload(), info_.period_size, 1000, error);
  return written == static_cast<int>(info_.period_size);
}

void AlsaRawDuplex::maintain_output_floor()
{
  // One period at a time, and only while the queue is still short. A stall that cost two periods is
  // covered by two insertions, which is the honest meaning of catching up here: the loop cannot produce
  // audio it never received, so the timeline is preserved by delaying it rather than by skipping it.
  //
  // Bounded at two attempts: the device drains a period every 1333 us and an insertion costs
  // microseconds, so needing more than two means something other than a short queue is wrong -- and a
  // controller that spins inside the render path would be a worse fault than the dropout it prevents.
  for (unsigned attempt = 0; attempt < 2; ++attempt)
  {
    AlsaRawOccupancy occupancy;
    std::string ignored;
    if (!query_occupancy(false, occupancy, ignored))
      return;
    if (!alsa::queue_needs_top_up(static_cast<unsigned long long>(occupancy.ring_frames), floor_frames_,
                                  info_.period_size, info_.buffer_size))
      return;
    // A whole period, not the deficit: everything above this writes period-sized buffers, and a partial
    // period would leave the queue at a level that is not a whole number of periods -- a state nothing
    // else in this file has to reason about.
    //
    // The payload is the last frame written, repeated, not silence. That is what lets the loop's splice
    // bridge leave from the right place: the bridge is anchored on the last sample it emitted, and a
    // hold continues that sample, where silence would step to zero first and leave a click the bridge
    // cannot reach. Measured with plan.md §10.3's continuity rule: a held entry is a break in *slope*
    // (about a tenth of the signal's first difference), a silence entry is a step in *value*.
    const void* payload = insert_payload();
    if (write_playback(payload, info_.period_size, 1000, ignored) < 0)
      return;
    ++floor_insertions_;
    floor_inserted_frames_ += info_.period_size;
  }
}

bool AlsaRawDuplex::query_occupancy(bool capture, AlsaRawOccupancy& out, std::string& error) const
{
  const int fd = capture ? capture_fd_ : playback_fd_;  if (fd < 0)
  {
    error = std::string("query_occupancy: ") + (capture ? "capture" : "playback") + " stream is not open";
    return false;
  }

  // STATUS rather than DELAY: one ioctl returns the delay and the pointers that produced it, and
  // the pointers are what make the figure auditable when it turns out not to be stable.
  struct snd_pcm_status status;
  std::memset(&status, 0, sizeof(status));
  if (::ioctl(fd, SNDRV_PCM_IOCTL_STATUS, &status) < 0)
  {
    error = errno_text(std::string("SNDRV_PCM_IOCTL_STATUS failed for ") + (capture ? "capture" : "playback"));
    return false;
  }

  out.delay_frames = static_cast<long long>(status.delay);
  out.avail_frames = static_cast<long long>(status.avail);
  out.hw_ptr = static_cast<unsigned long long>(status.hw_ptr);
  out.appl_ptr = static_cast<unsigned long long>(status.appl_ptr);
  out.state = static_cast<int>(status.state);

  // The pointer pair is the authority on how much is in the ring; `delay` is not. On the SSL 2+
  // Mk II, playback `delay` is the pointer gap plus the frames already in the USB host controller's
  // hands (a constant +72 frames in a measured sample, larger when the ring is full), which is why
  // the old occupancy figure could exceed the ring it was describing. Capture has no such offset.
  if (info_.buffer_size != 0)
  {
    out.ring_frames = static_cast<long long>(alsa::pcm_ring_frames(out.hw_ptr, out.appl_ptr, info_.buffer_size, capture));
    if (out.ring_frames != out.delay_frames)
      ++occupancy_mismatches_;
  }
  else
  {
    out.ring_frames = out.delay_frames;
  }
  out.tstamp_ns = static_cast<long long>(status.tstamp.tv_sec) * 1000000000LL + status.tstamp.tv_nsec;
  out.audio_tstamp_ns =
    static_cast<long long>(status.audio_tstamp.tv_sec) * 1000000000LL + status.audio_tstamp.tv_nsec;
  out.audio_tstamp_valid = status.audio_tstamp_data != 0;
  out.audio_tstamp_accuracy = status.audio_tstamp_accuracy;
  return true;
}

} // namespace namchain

#endif // __linux__
