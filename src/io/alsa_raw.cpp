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

namespace namchain
{
namespace
{

std::string errno_text(const std::string& what)
{
  return what + ": " + std::strerror(errno);
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
    error = errno_text("cannot open " + self->info_.playback_node);
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
  // Prime ONE period, not the whole ring. Filling the ring (which is what aplay effectively does
  // for a file) leaves it permanently full, so every subsequent write then blocks for a whole
  // period and the loop's latency becomes one buffer rather than one period. Measured: with a
  // full-ring prime the write blocked 1416 us of a 1333 us period; the engine's actual work was
  // 70 us. One period is enough to reach the kernel's start threshold.
  const unsigned chunk = info_.period_size > 0 ? info_.period_size : info_.buffer_size;
  const int written = write_playback(prime_silence_.data(), chunk, 1000, error);
  if (written < static_cast<int>(chunk))
  {
    error = "could not prime the playback buffer: " + error;
    return false;
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
  // returns -EPIPE (observed on the PCM2902). Prime the whole ring with silence first -- this is
  // what aplay does, and it is also what makes the kernel's start threshold reachable. The cost is
  // that the I/O latency floor is one buffer; tightening that with sw_params thresholds is a
  // deliberate follow-up, not an accident.
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
  return true;
}

void AlsaRawDuplex::stop()
{
  if (playback_fd_ >= 0)
    ::ioctl(playback_fd_, SNDRV_PCM_IOCTL_DROP);
  if (capture_fd_ >= 0)
    ::ioctl(capture_fd_, SNDRV_PCM_IOCTL_DROP);
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
    if (query_occupancy(true, status, ignored) &&
        status.delay_frames >= static_cast<long long>(info_.period_size))
      return true;

    const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    const int remaining = timeout_ms - static_cast<int>(elapsed);
    if (remaining <= 0)
    {
      error = "capture timed out after " + std::to_string(timeout_ms) + " ms";
      return false;
    }

    struct pollfd pfd;
    pfd.fd = capture_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    const int ready = ::poll(&pfd, 1, remaining);
    if (ready == 0)
    {
      error = "capture timed out after " + std::to_string(timeout_ms) + " ms";
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
  return transfer(playback_fd_, const_cast<void*>(src), frames, true, timeout_ms, error);
}

bool AlsaRawDuplex::query_occupancy(bool capture, AlsaRawOccupancy& out, std::string& error) const
{
  const int fd = capture ? capture_fd_ : playback_fd_;
  if (fd < 0)
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
  out.hw_ptr = static_cast<unsigned long long>(status.hw_ptr);
  out.appl_ptr = static_cast<unsigned long long>(status.appl_ptr);
  out.state = static_cast<int>(status.state);
  out.tstamp_ns = static_cast<long long>(status.tstamp.tv_sec) * 1000000000LL + status.tstamp.tv_nsec;
  out.audio_tstamp_ns =
    static_cast<long long>(status.audio_tstamp.tv_sec) * 1000000000LL + status.audio_tstamp.tv_nsec;
  out.audio_tstamp_valid = status.audio_tstamp_data != 0;
  out.audio_tstamp_accuracy = status.audio_tstamp_accuracy;
  return true;
}

} // namespace namchain

#endif // __linux__
