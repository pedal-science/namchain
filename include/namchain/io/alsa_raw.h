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
  /// \brief Periods of already-processed audio the playback ring is allowed to hold: the
  /// latency-against-dropout-tolerance knob (§9.5 item 7).
  ///
  /// A **cap**, not a promise of cushion. The device plays from the ring, so its contents are the
  /// stall the loop can absorb -- but the contents are conserved (one period produced per period
  /// played), so the cap only binds a burst that tries to grow the queue, and how much a run actually
  /// holds is whatever \ref fill_output_queue established before the loop was timed, spent one period
  /// at a time by lateness. Measured at 64 frames: a cap of 3 periods held 1.4-2.1 periods, and a cap
  /// of 1 period is below what this device needs at all -- it underran 1629 times in 20 s.
  ///
  /// 0 means no cap, which is the behaviour every measurement up to and including the first hour was
  /// taken with: the loop writes whenever the kernel reports room, so the ring's contents wander
  /// between empty and full and the cushion is whatever the run happened to be holding at the moment
  /// it mattered.
  ///
  /// Must be less than \ref periods: a cap equal to the ring leaves no room for the period the policy
  /// is deciding about, so the loop could never write at all.
  unsigned output_queue_periods = 0;
  /// \brief Periods of queue below which the loop inserts a period of silence to hold the floor.
  ///
  /// The other end of the same trade as \ref output_queue_periods, and the answer to a problem the cap
  /// cannot solve: the queue is not renewable, because the loop produces one period per period, so once
  /// the queue is short only *something other than the signal* can refill it. Without this, a short
  /// queue ends in a playback underrun, and an underrun stops the stream and recovery discards the
  /// whole queued ring -- measured at 8 per hour for a configuration whose 5-minute runs had none.
  /// With it, the cost is one period (1.33 ms) of inserted silence per shortfall and the timeline is
  /// preserved rather than skipped.
  ///
  /// 0 disables the controller. Must be at most \ref output_queue_periods when that is set, and less
  /// than \ref periods. The inserted period is silence today, which is audible at both ends without a
  /// splice fade (plan.md §10.3); the counter is reported so the cost can be weighed before that lands.
  unsigned output_queue_floor_periods = 0;
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
  /// The output-queue target actually in force, in periods; 0 when there is none. Echoed from the
  /// configuration rather than recomputed, so that "the target the report states" and "the target the
  /// write path enforces" are the same number and cannot drift apart.
  unsigned output_queue_periods = 0;
  /// The output-queue floor actually in force, in periods; 0 when there is no controller.
  unsigned output_queue_floor_periods = 0;
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

  /// \brief Write silence until the playback ring holds the configured queue target.
  ///
  /// Must be called after the stream is running and immediately before the loop that is going to be
  /// timed, because the target cannot be established any earlier: the bring-up path (draining the
  /// capture side, the first untimed cycle, the first waits) consumes a period of the primed queue
  /// without writing anything back, and the ring's contents are otherwise conserved -- one period
  /// produced per period played -- so a queue that starts a period short stays a period short for
  /// ever. Measured: a target of 3 primed 192 frames and the timed loop then held 64..128, exactly one
  /// period less than asked for, with zero bursts and zero drops to explain the difference.
  ///
  /// Only \ref AlsaRawConfig::output_queue_periods can fail this, and only if the device refuses to
  /// take the frames.
  bool fill_output_queue(std::string& error);

  /// Frames of silence written by fill_output_queue(). Bring-up audio, like
  /// settle_discarded_frames(): it is the latency the target costs, paid once, before anything is
  /// timed, and reported so that "the queue was there from block 0" is a checkable statement.
  uint64_t prefill_frames() const { return prefill_frames_; }

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
  /// When AlsaRawConfig::output_queue_periods is set there is a second refusal, checked *before* the
  /// ring's own capacity: if the ring already holds more than the target, the period is dropped
  /// without asking the kernel anything. The ring's capacity is then a safety net rather than the
  /// policy, which is the point -- with a target the loop decides how much latency it is willing to
  /// carry, and a burst that would have raised the queue instead loses a period (§9.5 item 7).
  /// The two refusals are counted separately because they call for different responses.
  ///
  /// \return frames written (== \p frames), 0 when there was no room so the period was dropped and
  /// counted as a drop, or -1 on a real error.
  int try_write_playback(const void* src, unsigned frames, std::string& error);

  /// Count of `-EPIPE` conditions. Every one is counted and reported; the engine never silently
  /// re-prepares (plan.md §10.5.1). This is the sum of the two per-direction counts below, and the
  /// sum is asserted against them rather than assumed.
  ///
  /// The direction is worth keeping separate because the two have almost nothing in common and only
  /// one of them is a latency question:
  ///
  ///  - a **capture overrun** means the device produced a period the loop had not read yet. It is a
  ///    function of how promptly the loop reads, and no amount of output queue helps it;
  ///  - a **playback underrun** means the ring ran dry, which is exactly the event the output-queue
  ///    target exists to prevent, and its count is the number a target has to be chosen against.
  ///
  /// Reported as one number until the hour-long baseline, where 92 xruns could not be attributed and
  /// the whole question of what a queue target would buy turned on the split (§9.5 item 7).
  uint64_t driver_xruns() const { return driver_xruns_; }
  /// How many times recovery was performed (one per handled xrun, normally).
  uint64_t recoveries() const { return recoveries_; }
  /// Capture overruns: the device produced a period before the loop read the last one.
  uint64_t capture_overruns() const { return capture_xruns_; }
  /// Playback underruns: the ring ran dry before the loop wrote the next period. This is the count
  /// the output-queue target is chosen against.
  uint64_t playback_underruns() const { return playback_xruns_; }

  /// Periods try_write_playback() found no room for. Each one is a period of output audio the loop
  /// chose to lose rather than block on. Zero is the only acceptable steady-state value.
  uint64_t output_drops() const { return output_drops_; }
  /// \brief The subset of \ref output_drops refused by the queue target rather than by the kernel.
  ///
  /// The two refusals mean different things and have different fixes. "The kernel says the ring is
  /// full" is the ring being the binding constraint, which with a target set should be impossible
  /// (a target below the ring's capacity always leaves room for one more period) and therefore means
  /// something else went wrong. "The target says this period would take the queue past what we chose
  /// to hold" is the policy working: the audio is one period shorter and the latency did not grow.
  uint64_t output_target_drops() const { return output_target_drops_; }
  /// The queue target in force, in frames; 0 when there is none.
  unsigned target_frames() const { return target_frames_; }
  /// Periods of silence the floor controller has inserted to keep the queue off the bottom. Each one is
  /// 1.33 ms at q=64, audible without a splice fade, and the timeline cost of not underrunning: an
  /// underrun discards the whole queued ring in recovery, so a period is the cheaper mistake.
  uint64_t floor_insertions() const { return floor_insertions_; }
  /// The same in frames, which must equal `floor_insertions() * period_size`; asserted by the soak.
  uint64_t floor_inserted_frames() const { return floor_inserted_frames_; }
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
  /// Print one queue-target decision when NAMCHAIN_QUEUE_DEBUG is set. Kept because the failure it
  /// was written to find is invisible in every aggregate the report has: a target refusing a dead
  /// stream shows up as a large drop count and a healthy xrun count, which is exactly backwards.
  void queue_debug_trace(const AlsaRawOccupancy& occupancy, bool allowed);
  /// Write silence while the queue is below floor_frames_. Called after a successful write, so the
  /// decision is made on the occupancy the loop just produced rather than on a stale one.
  void maintain_output_floor();

  int capture_fd_ = -1;
  int playback_fd_ = -1;
  unsigned quantum_ = 64;
  unsigned periods_ = 4;
  /// The queue target in periods as requested, held until the device says what a period really is.
  unsigned requested_queue_periods_ = 0;
  /// The queue target in frames, or 0 for none. Derived from requested_queue_periods_ once the device
  /// has said what a period actually is (the requested quantum is not always granted).
  unsigned target_frames_ = 0;
  /// The queue floor in frames, or 0 for no controller; from requested_floor_periods_.
  unsigned floor_frames_ = 0;
  unsigned requested_floor_periods_ = 0;
  std::vector<unsigned> format_preference_;
  bool enable_timestamps_ = false;
  unsigned capture_bytes_per_frame_ = 2;
  unsigned playback_bytes_per_frame_ = 2;
  /// Silence used to prime the playback ring; preallocated so recovery does not allocate.
  std::vector<unsigned char> prime_silence_;
  /// Sink for drain_capture(), preallocated for the same reason and sized one period.
  std::vector<unsigned char> drain_buffer_;
  AlsaRawInfo info_;
  /// NAMCHAIN_QUEUE_DEBUG, read once at open rather than per period: getenv in the write path would
  /// be a libc call on every period, and a debug trace must not change the timing it is there to
  /// explain.
  bool queue_debug_ = false;
  unsigned queue_debug_lines_ = 0;
  uint64_t driver_xruns_ = 0;
  uint64_t recoveries_ = 0;
  uint64_t output_drops_ = 0;
  uint64_t output_target_drops_ = 0;
  uint64_t floor_insertions_ = 0;
  uint64_t floor_inserted_frames_ = 0;
  uint64_t capture_xruns_ = 0;
  uint64_t playback_xruns_ = 0;
  uint64_t capture_bursts_ = 0;
  uint64_t settle_discarded_frames_ = 0;
  uint64_t prefill_frames_ = 0;
  uint64_t gate_capture_xruns_ = 0;
  /// Mutable because query_occupancy() is const and this is a diagnostic it happens to compute.
  mutable uint64_t occupancy_mismatches_ = 0;
};

/// Resolve "hw:4,0", "4,0" or a "/dev/snd/..." path to a PCM device node.
bool resolve_pcm_node(const std::string& spec, bool capture, std::string& node, std::string& error);

} // namespace namchain

#endif // __linux__
