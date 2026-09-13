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
  /// Channel count for both directions. Overridden by \ref capture_channels / \ref playback_channels
  /// when the device is asymmetric: the SSL 2+ Mk II reports 6 playback and 8 capture channels on
  /// firmware V1.16 (2 analogue inputs + three stereo loopbacks), and requesting one count for both
  /// directions simply fails to open.
  unsigned channels = 1;
  /// 0 means "the same as \ref channels".
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
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
  /// Ask the driver to timestamp each period. Off by default: the engine never reads the timestamps,
  /// and a clock read per period in the render path is exactly the kind of cost this project exists
  /// to avoid. A latency measurement turns it on because it is the only route to a common timebase
  /// between the two streams.
  bool enable_timestamps = false;
};

struct AlsaRawInfo
{
  unsigned period_size = 0;
  unsigned buffer_size = 0;
  unsigned sample_rate = 0;
  /// Per-direction channel counts. These are not one number: the SSL 2+ Mk II is 6 out / 8 in.
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
  /// Bytes per frame, per direction. Derived from the channel count, so they differ whenever the
  /// channel counts do -- which is why buffer sizing must ask the direction it is sizing for.
  unsigned capture_bytes_per_frame = 0;
  unsigned playback_bytes_per_frame = 0;
  /// The SNDRV_PCM_FORMAT_* the device agreed to, and its width in bytes.
  unsigned sample_format = 0;
  unsigned bytes_per_sample = 0;
  /// False when the device rounded the requested period (e.g. up to its 1 ms floor).
  bool period_exact = false;
  /// True when the driver agreed to record timestamps. Without this the status timestamps are all
  /// zero, which is why a measurement that wants to relate the two streams to one clock has to ask.
  bool timestamps_enabled = false;
  std::string capture_node;
  std::string playback_node;
};

/// \brief How much audio is sitting in one direction's ring right now.
///
/// This is the quantity that made the first round-trip latency figures irreproducible (§9.5): what
/// you measure from "I wrote the stimulus" to "it came back" includes however much was already
/// queued when you wrote, and that settles differently from run to run. Reading it at the moment of
/// the write is what lets the measurement subtract it and report something that is a property of the
/// device rather than of the run.
struct AlsaRawOccupancy
{
  /// For playback: frames written but not yet played. For capture: frames the device has produced
  /// that the application has not read. Either way, it is the gap between the application pointer
  /// and the hardware pointer, which is exactly what has to be removed from a delay measurement.
  long long delay_frames = 0;
  /// The pointers behind the delay, for diagnostics. Both are ring offsets, not absolute counters,
  /// so they are only meaningful next to the buffer size.
  unsigned long long hw_ptr = 0;
  unsigned long long appl_ptr = 0;
  int state = 0;
  /// The wall-clock instant the driver associated with that pointer, in nanoseconds, and the
  /// device-clock timestamp for it. `audio_tstamp_valid` is false when the driver does not fill the
  /// latter in. These are what would let a measurement relate the two streams to one clock, which
  /// the pointers alone cannot do -- they are ring offsets and wrap.
  long long tstamp_ns = 0;
  long long audio_tstamp_ns = 0;
  bool audio_tstamp_valid = false;
  unsigned audio_tstamp_accuracy = 0;
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

  /// Wait until the device has one period of capture available to read.
  ///
  /// Not "until some data is available". The difference is what `sw_params.avail_min` decides, and
  /// leaving it at the kernel's default is how the loop came to be paced by the wrong ring: `poll`
  /// returns almost immediately in a running stream, the loop free-runs until the playback ring is
  /// full, and from then on the write blocks for the best part of a period -- which a soak then
  /// reports as engine block time. Measured here: a 20 s run had `write` p50 7.8 us, and the same
  /// binary over an hour settled at 699 us with `device wait` down to 8 us.
  ///
  /// The gate is checked against the device's own pointer rather than trusting the wakeup, so the
  /// cadence holds even if a driver wakes early.
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

  /// \brief Read the ring occupancy and pointers for one direction.
  ///
  /// Cheap (one ioctl) and, unlike the transfer calls, non-blocking: it reports what is in the ring
  /// at the instant it is called, which is the whole point. \return false with \p error set.
  bool query_occupancy(bool capture, AlsaRawOccupancy& out, std::string& error) const;

  const AlsaRawInfo& info() const { return info_; }

private:
  AlsaRawDuplex() = default;

  bool configure(int fd, bool capture, const char* direction, std::string& error);
  /// \param playback when true the ring is re-primed with silence after the xrun.
  void recover(int fd, bool playback);
  bool prime_playback(std::string& error);
  int transfer(int fd, void* buf, unsigned frames, bool playback, int timeout_ms, std::string& error);

  int capture_fd_ = -1;
  int playback_fd_ = -1;
  unsigned quantum_ = 64;
  unsigned periods_ = 4;
  std::vector<unsigned> format_preference_;
  bool enable_timestamps_ = false;
  unsigned capture_bytes_per_frame_ = 2;
  unsigned playback_bytes_per_frame_ = 2;
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
