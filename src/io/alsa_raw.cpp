#include "namchain/io/alsa_raw.h"

#if defined(__linux__)

  #include <cerrno>
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
  if (config.channels < 1 || config.channels > 2)
  {
    error = "only 1 or 2 channels are supported";
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
  self->bytes_per_frame_ = config.channels * 2; // S16_LE
  self->info_.channels = config.channels;
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

  if (!self->configure(self->capture_fd_, "capture", error))
    return nullptr;
  if (!self->configure(self->playback_fd_, "playback", error))
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

bool AlsaRawDuplex::configure(int fd, const char* direction, std::string& error)
{
  struct snd_pcm_hw_params params;
  alsa::params_set_any(params);
  alsa::params_narrow_basics(params, info_.channels, info_.sample_rate);

  // Phase 1: let the kernel narrow our request to what the device can actually do. Leaving the
  // period and buffer sizes unconstrained here is what makes it report its real ranges -- notably
  // the 1000 us PERIOD_TIME floor that turns a request for 32 frames into 48.
  if (::ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0)
  {
    error = errno_text("SNDRV_PCM_IOCTL_HW_REFINE failed");
    return false;
  }

  if (!alsa::params_format_is_exactly(params, SNDRV_PCM_FORMAT_S16_LE))
  {
    error = "device does not accept S16_LE exclusively";
    return false;
  }

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
    info_.period_exact = (actual_period == quantum_);
  }
  else if (info_.period_size != actual_period || info_.buffer_size != actual_buffer)
  {
    error = "capture negotiated " + std::to_string(actual_period) + "/" + std::to_string(actual_buffer) +
            " but playback negotiated " + std::to_string(info_.period_size) + "/" +
            std::to_string(info_.buffer_size) + " frames; both directions must agree";
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
  if (info_.buffer_size > 0)
  {
    std::vector<int16_t> silence(static_cast<size_t>(info_.buffer_size) * info_.channels, 0);
    std::string prime_error;
    const int written = write_playback(silence.data(), info_.buffer_size, 1000, prime_error);
    if (written < static_cast<int>(info_.buffer_size))
    {
      error = "could not prime the playback buffer: " + prime_error;
      return false;
    }
  }

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

void AlsaRawDuplex::recover(int fd)
{
  // Recovering is right for a soak (we want to keep running); hiding it is not, which is why
  // driver_xruns() is reported alongside recoveries().
  if (::ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) == 0)
    ::ioctl(fd, SNDRV_PCM_IOCTL_START);
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
    xfer.buf = static_cast<char*>(buf) + static_cast<size_t>(done) * bytes_per_frame_;
    xfer.frames = frames - done;

    const int rc = ::ioctl(fd, playback ? SNDRV_PCM_IOCTL_WRITEI_FRAMES : SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
    if (rc < 0)
    {
      if (errno == EPIPE)
      {
        ++driver_xruns_;
        recover(fd);
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

int AlsaRawDuplex::read_capture(int16_t* dst, unsigned frames, int timeout_ms, std::string& error)
{
  return transfer(capture_fd_, dst, frames, false, timeout_ms, error);
}

bool AlsaRawDuplex::wait_capture(int timeout_ms, std::string& error)
{
  for (;;)
  {
    struct pollfd pfd;
    pfd.fd = capture_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    const int ready = ::poll(&pfd, 1, timeout_ms);
    if (ready > 0)
      return true;
    if (ready == 0)
    {
      error = "capture timed out after " + std::to_string(timeout_ms) + " ms";
      return false;
    }
    if (errno == EINTR)
      continue;
    error = errno_text("poll failed");
    return false;
  }
}

int AlsaRawDuplex::write_playback(const int16_t* src, unsigned frames, int timeout_ms, std::string& error)
{
  return transfer(playback_fd_, const_cast<int16_t*>(src), frames, true, timeout_ms, error);
}

} // namespace namchain

#endif // __linux__
