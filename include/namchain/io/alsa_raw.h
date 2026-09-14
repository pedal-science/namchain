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
  ///
  /// **Not to be used as the ring occupancy.** On the SSL 2+ Mk II the driver's playback `delay` is
  /// the pointer gap *plus the frames already handed to the USB host controller*, measured as a
  /// constant +72 frames (1.5 ms) at three separate instants via `/proc/asound/card2/pcm0p/sub0/status`,
  /// and larger when the ring is full -- which is how a 256-frame ring came to report 544 frames of
  /// "occupancy". Capture has no such offset: `delay == hw_ptr - appl_ptr` exactly, at every sample.
  long long delay_frames = 0;
  /// The true ring occupancy, derived from the pointer pair and therefore free of the driver's
  /// in-flight offset: `appl - hw` for playback, `hw - appl` for capture, modulo the buffer.
  /// This is the field to pace, gate and report with.
  long long ring_frames = 0;
  /// `status.avail`: room to write (playback) or frames to read (capture). Agrees with the pointer
  /// pair in both directions on this device, unlike delay, and is what the kernel's own poll()
  /// readiness is computed from -- so it is the authority the non-blocking write relies on.
  long long avail_frames = 0;
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
  ///
  /// The gate is rate-correct but not burst-free: it releases whenever a period is waiting, so a
  /// backlog of two periods releases two reads almost together and therefore two writes. That burst
  /// is how one late period used to turn into a permanently full playback ring (§9.5 item 4), so
  /// every release that finds two or more periods waiting is counted -- see capture_bursts().
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

  /// \brief Write \p frames only if the ring can take them *now*; drop the period if it cannot.
  ///
  /// The whole point is that this never waits. A blocking write is not a failure of the engine, but
  /// it *is* the queue deciding the block time: on the SSL, an hour of identical 295 us engine work
  /// was reported as 993 us blocks because 699 us of every period was the write waiting for space
  /// (§9.5 item 4). A loop that can block in the write cannot keep a bound on its own deadline,
  /// and it stalls long enough for the capture ring to accumulate the backlog that makes the next
  /// write worse. Asking "is there room for a period" and dropping when there is not converts an
  /// unbounded wait into a bounded, counted one: the caller knows it lost a period, and can say so.
  ///
  /// The question is put to the kernel's own poll() rather than computed from `status.delay`,
  /// because `delay` is the field that reads 544 frames against a 256-frame buffer at saturation
  /// and is therefore not fit to decide anything with yet.
  ///
  /// \return frames written (== \p frames), 0 when there was no room so the period was dropped and
  /// counted as a drop, or -1 on a real error.
  int try_write_playback(const void* src, unsigned frames, std::string& error);

  /// Count of `-EPIPE` conditions. Every one is counted and reported; the engine never silently
  /// re-prepares (plan.md §10.5.1).
  uint64_t driver_xruns() const { return driver_xruns_; }
  /// How many times recovery was performed (one per handled xrun, normally).
  uint64_t recoveries() const { return recoveries_; }

  /// Periods try_write_playback() found no room for. Each one is a period of output audio the loop
  /// chose to lose rather than block on. Zero is the only acceptable steady-state value.
  uint64_t output_drops() const { return output_drops_; }
  /// Reads released while two or more periods were waiting. Zero means the loop never fell a period
  /// behind; a positive count is the mechanism that fills the playback ring, so it is reported even
  /// when the drop guard absorbs the consequence.
  uint64_t capture_bursts() const { return capture_bursts_; }
  /// Frames read and discarded before the timed loop to establish a known-empty capture ring.
  /// This is bring-up audio the engine was never going to process, not a dropped period.
  uint64_t settle_discarded_frames() const { return settle_discarded_frames_; }
  /// Capture overruns found by the *gate* rather than by a read returning `-EPIPE`. These are xruns
  /// that would otherwise have stalled the loop for its whole timeout, because an xrun freezes the
  /// hardware pointer and the gate is the only thing waiting on it. Counted separately from
  /// driver_xruns() only so the report can distinguish "the read told us" from "the gate found a
  /// dead stream": both are driver xruns and both are included in driver_xruns().
  uint64_t gate_capture_xruns() const { return gate_capture_xruns_; }
  /// Samples where `status.delay` disagreed with the pointer-derived ring occupancy. On this device
  /// that is *expected* for playback and never happens for capture: playback delay carries the
  /// driver's in-flight offset (see AlsaRawOccupancy). Kept as a counter because "the field we were
  /// about to control the loop with disagrees with the pointers" is a fact the report should state
  /// rather than an implementation detail, and because a change in this number would mean the
  /// driver's behaviour changed under us. Measured to settle the 544-against-256 question in §9.5.
  uint64_t occupancy_mismatches() const { return occupancy_mismatches_; }

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
  /// Read and discard capture until less than one period is waiting, so the timed loop starts from a
  /// known occupancy rather than from however long the device ran before the loop was ready.
  bool drain_capture(std::string& error);
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
  /// Sink for drain_capture(), preallocated for the same reason and sized one period.
  std::vector<unsigned char> drain_buffer_;
  AlsaRawInfo info_;
  uint64_t driver_xruns_ = 0;
  uint64_t recoveries_ = 0;
  uint64_t output_drops_ = 0;
  uint64_t capture_bursts_ = 0;
  uint64_t settle_discarded_frames_ = 0;
  uint64_t gate_capture_xruns_ = 0;
  /// Mutable because query_occupancy() is const and this is a diagnostic it happens to compute.
  mutable uint64_t occupancy_mismatches_ = 0;
};

/// Resolve "hw:4,0", "4,0" or a "/dev/snd/..." path to a PCM device node.
bool resolve_pcm_node(const std::string& spec, bool capture, std::string& node, std::string& error);

} // namespace namchain

#endif // __linux__
