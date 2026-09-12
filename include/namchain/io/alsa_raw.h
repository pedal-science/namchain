#pragma once

// Raw ALSA backend: kernel UAPI ioctls on /dev/snd/pcmC*D*p, with no libasound anywhere. This is
// the appliance backend and the one that matters for latency on a Pi (plan.md §4.3), and it is why
// xrun detection is a first-class part of the design rather than an add-on: `-EPIPE` and
// SNDRV_PCM_STATE_XRUN come straight from the ioctls we already issue.
#if defined(__linux__)

  #include <cstdint>
  #include <memory>
  #include <sound/asound.h>
  #include <string>
  #include <vector>

namespace namchain
{

struct AlsaRawConfig
{
  /// "hw:4,0", "4,0", or an explicit "/dev/snd/pcmC4D0c".
  std::string capture;
  std::string playback;
  unsigned sample_rate = 48000;
  unsigned channels = 1;
  /// Requested period size in frames. The device period IS the engine quantum (§9.2).
  unsigned quantum = 64;
  /// Target buffer length in periods.
  unsigned periods = 4;
  /// Refuse to open when the device cannot give exactly `quantum`. This is the guard against
  /// the silent rounding measured in §9.5 (ask for 32, get 48), which would otherwise leave the
  /// engine's render plan and buffer accounting describing a period that does not exist.
  bool require_exact_period = true;
  /// Sample formats to try, most preferred first (SNDRV_PCM_FORMAT_* values). The default asks for
  /// 24-bit packing before 16-bit: the engine is float32 internally, and 16-bit at the boundary
  /// puts a floor near -96 dBFS uncomfortably close to the -60 dBFS state-continuity target (§1).
  /// The PCM2902 offers only S16_LE and falls through; 24-bit interfaces use the top entry.
  std::vector<unsigned> format_preference{SNDRV_PCM_FORMAT_S24_3LE, SNDRV_PCM_FORMAT_S32_LE,
                                          SNDRV_PCM_FORMAT_S16_LE};
};

struct AlsaRawInfo
{
  unsigned period_size = 0;
  unsigned buffer_size = 0;
  unsigned channels = 0;
  unsigned sample_rate = 0;
  /// The SNDRV_PCM_FORMAT_* the device agreed to, and its width in bytes.
  unsigned sample_format = 0;
  unsigned bytes_per_sample = 0;
  /// False when the device rounded the requested period (e.g. up to its 1 ms floor).
  bool period_exact = false;
  std::string capture_node;
  std::string playback_node;
};

/// Full-duplex PCM stream. Both directions are opened and configured together because the
/// PCM2902's capture endpoint is ASYNC with no explicit feedback endpoint, so the driver relies on
/// implicit feedback from the other direction (§9.5).
class AlsaRawDuplex
{
public:
  ~AlsaRawDuplex();

  AlsaRawDuplex(const AlsaRawDuplex&) = delete;
  AlsaRawDuplex& operator=(const AlsaRawDuplex&) = delete;

  /// Open and configure both directions. \return nullptr on failure, with \p error set.
  static std::unique_ptr<AlsaRawDuplex> open(const AlsaRawConfig& config, std::string& error);

  bool start(std::string& error);
  void stop();

  /// Wait until capture has a period available, without transferring anything. The engine's
  /// real-time loop is: wait (untimed) -> read, DSP, write (timed). Measuring from the start of
  /// the wait would just report the period length.
  bool wait_capture(int timeout_ms, std::string& error);

  /// Read exactly \p frames into \p dst, waiting up to \p timeout_ms. Returns frames transferred,
  /// or -1 on error. An xrun is counted, recovered from, and reported through driver_xruns().
  ///
  /// \p dst must hold `frames * info().bytes_per_sample * info().channels` BYTES. It is void*
  /// rather than a typed pointer on purpose: sizing it as a count of 16-bit samples is a heap
  /// overflow on any device wider than 16-bit, which is exactly the bug this signature prevents.
  int read_capture(void* dst, unsigned frames, int timeout_ms, std::string& error);

  /// Write exactly \p frames from \p src. Same sizing rule as read_capture().
  int write_playback(const void* src, unsigned frames, int timeout_ms, std::string& error);

  /// Count of `-EPIPE` conditions. Every one is counted and reported; the engine never silently
  /// re-prepares (plan.md §10.5.1).
  uint64_t driver_xruns() const { return driver_xruns_; }
  /// How many times recovery was performed (one per handled xrun, normally).
  uint64_t recoveries() const { return recoveries_; }

  const AlsaRawInfo& info() const { return info_; }

private:
  AlsaRawDuplex() = default;

  bool configure(int fd, const char* direction, std::string& error);
  /// \param playback when true the ring is re-primed with silence after the xrun.
  void recover(int fd, bool playback);
  bool prime_playback(std::string& error);
  int transfer(int fd, void* buf, unsigned frames, bool playback, int timeout_ms, std::string& error);

  int capture_fd_ = -1;
  int playback_fd_ = -1;
  unsigned quantum_ = 64;
  unsigned periods_ = 4;
  std::vector<unsigned> format_preference_;
  unsigned bytes_per_frame_ = 2;
  /// Silence used to prime the playback ring; preallocated so recovery does not allocate.
  std::vector<unsigned char> prime_silence_;
  AlsaRawInfo info_;
  uint64_t driver_xruns_ = 0;
  uint64_t recoveries_ = 0;
};

/// Resolve "hw:4,0", "4,0" or a "/dev/snd/..." path to a PCM device node.
bool resolve_pcm_node(const std::string& spec, bool capture, std::string& node, std::string& error);

} // namespace namchain

#endif // __linux__
