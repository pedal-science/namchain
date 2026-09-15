#pragma once

// The I/O layer's one interface (plan.md §9.1).
//
// Four very different things have to look the same to the engine: a WAV file, a paced clock with no
// device, raw ALSA ioctls, and PipeWire. Before this existed, `alsa_raw` was a concrete class and
// `offline` was an unrelated sibling, so the soak harness had exactly two modes -- a real device, or
// flat out -- and neither CI nor a laptop could exercise the timing machinery. The point of the
// interface is that everything above it (the render plan, the block timers, the oracle, the xrun
// accounting) is written once and runs against all of them.
#include <cstdint>
#include <memory>
#include <string>

#include "namchain/pcm.h"

namespace namchain
{

enum class BackendKind
{
  File,    ///< WAV in, WAV out, as fast as the disk allows. The offline instance of the interface.
  Null,    ///< Real-time paced against the wall clock, with no device: silence in, discarded out.
  AlsaRaw, ///< /dev/snd PCM ioctls, no libasound (Linux only, §4.3).
  PipeWire ///< Not implemented; refused by name rather than silently falling back.
};

const char* backend_kind_name(BackendKind kind);

/// Parse "file", "null", "alsa", "alsa_raw", "pipewire". \return false on an unknown name, so a
/// typo is an error rather than a silent fallback to a different backend.
bool parse_backend_kind(const std::string& text, BackendKind& kind);

struct BackendConfig
{
  unsigned sample_rate = 48000;
  /// Requested period size. Every backend reports what it actually got in \ref BackendInfo.
  unsigned period_size = 64;
  /// Requested buffer length in periods.
  unsigned periods = 4;
  unsigned capture_channels = 1;
  unsigned playback_channels = 1;
  /// Device spec for the device backends: "hw:4,0", "4,0", or a /dev/snd path.
  std::string device;
  /// `file` only.
  std::string input_path;
  std::string output_path;
  /// `null` only: pace against the wall clock the way a device would. False runs flat out, which is
  /// what a throughput measurement wants and a lateness measurement cannot use.
  bool realtime_paced = true;
  /// Refuse a period size other than the one requested. On a device this is the guard against the
  /// silent rounding measured in §9.5.
  bool require_exact_period = true;
  /// \brief Periods of already-processed audio the output ring is allowed to hold: the
  /// latency-against-dropout-tolerance knob (§9.5 item 7).
  ///
  /// A **cap**, not a promise: the loop refuses a period that would take the ring past it, so a burst
  /// cannot buy latency beyond the number. The ring's contents are conserved, so how much a run
  /// actually holds is established by \ref AudioBackend::prefill_output_queue before the loop is timed
  /// and spent by lateness afterwards. 0 means no cap, which is what every measurement before this
  /// looked like. Must be less than \ref periods.
  unsigned output_queue_periods = 0;
  /// \brief Periods of queue below which a period of silence is inserted to hold the floor.
  ///
  /// The other end of the same trade, and the answer to a problem a cap cannot solve: the queue is not
  /// renewable (one period produced per period played), so only something other than the signal can
  /// refill it. Without it a short queue ends in a playback underrun, and recovery from an underrun
  /// discards the *whole* queued ring -- so a period of inserted silence is the cheaper mistake. 0
  /// disables it; must be at most \ref output_queue_periods and less than \ref periods. The inserted
  /// period is silence today, audible at both ends without a splice fade (plan.md §10.3).
  unsigned output_queue_floor_periods = 0;
};

struct BackendInfo
{
  unsigned period_size = 0;
  unsigned buffer_size = 0;
  unsigned sample_rate = 0;
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
  /// The format on the wire. The backends with no wire use F32_LE.
  PcmFormat format = PcmFormat::Unknown;
  unsigned bytes_per_sample = 0;
  /// Bytes per frame, per direction. Two fields rather than one because the counts differ on an
  /// asymmetric device -- the SSL 2+ Mk II is 6 out / 8 in -- and sizing both directions from one
  /// number is how a capture buffer ends up 25 % short.
  unsigned capture_bytes_per_frame = 0;
  unsigned playback_bytes_per_frame = 0;
  /// False when the backend could not give the requested period exactly.
  bool period_exact = false;
  /// The output-queue target this backend is enforcing, in periods; 0 when it enforces none. Echoed
  /// rather than re-read from the configuration so that the number a report states and the number the
  /// write path acts on are the same one.
  unsigned output_queue_periods = 0;
  /// The output-queue floor this backend is enforcing, in periods; 0 when it enforces none.
  unsigned output_queue_floor_periods = 0;
  std::string capture_name;
  std::string playback_name;
  /// The minimum a round trip through this backend can take, in frames: what its own buffers add
  /// before any converter is involved. 0 for a backend with no rings.
  uint64_t buffer_floor_frames = 0;
  /// Set once capture has no more frames to give. A caller that loops until this is set will stop
  /// at end of input rather than reading silence forever.
  bool capture_exhausted = false;
  /// True when the first period after start is an artefact of starting and the caller should run one
  /// untimed cycle before beginning to measure -- a device's ring is empty at START, and its first
  /// period costs a fill rather than a period's work.
  ///
  /// False for the backends with no rings, and it has to be: for `file` that "bring-up" cycle is a
  /// period of silence unconditionally written to the output WAV, which showed up as an output one
  /// period longer than the renderer's and would have been blamed on the DSP.
  bool needs_bringup_cycle = false;
};

/// \brief How the loop's queue control behaved, for the backends that can say.
///
/// These are the three numbers that tell a *paced* loop from a *blocked* one, and they are collected
/// in one place because they are read together and mean nothing apart. `output_drops` is the loop
/// choosing to lose a period rather than wait for the device; `capture_bursts` is the loop having
/// fallen a period behind; `settle_discarded_frames` is bring-up audio that the engine was never
/// going to see. Zero, zero, and a small number is a healthy run (plan.md §9.5 item 4).
///
/// The device's own health counters travel with them because they are read at the same moment and
/// answer the same question -- how much of this run's loss was the loop's decision and how much was
/// the device's -- and because separating them is what a queue target has to be chosen against.
struct StreamQueueStats
{
  uint64_t output_drops = 0;
  /// The subset of `output_drops` the queue target refused, as against the ring being full. The two
  /// mean different things: a target refusal is the policy working (one period lost, latency held),
  /// a ring-full refusal is the ring being the binding constraint, which a target below the ring's
  /// capacity makes unreachable.
  uint64_t output_target_drops = 0;
  /// The output-queue target in force, in periods; 0 when there is none.
  unsigned output_queue_periods = 0;
  /// The output-queue floor in force, in periods; 0 when there is no controller.
  unsigned output_queue_floor_periods = 0;
  /// \brief Periods of silence the floor controller inserted to keep the queue off the bottom.
  ///
  /// Each one is 1.33 ms at q=64 and is audible without a splice fade, so this is the price of the
  /// floor and it belongs next to the xruns it prevented: an underrun discards the whole queued ring in
  /// recovery, so a period of silence is the cheaper mistake -- but it is not free, and a controller
  /// that inserts constantly is a configuration stating that its floor is too high for its host.
  uint64_t floor_insertions = 0;
  /// The same in frames: must equal `floor_insertions * period_size`, and the soak asserts it.
  uint64_t floor_inserted_frames = 0;
  uint64_t capture_bursts = 0;
  uint64_t settle_discarded_frames = 0;
  /// \brief Silence written to establish \ref output_queue_periods before the timed loop started.
  ///
  /// The cushion cannot be built by the loop itself -- it produces one period of output per period of
  /// input, so the ring only grows when a burst recovers a late period, which the target refuses -- and
  /// the bring-up path spends a period of the primed queue. So the target is written in once, at the
  /// last moment before the loop is timed and counted here, because "the queue was there from block 0"
  /// has to be a checkable claim rather than a comment.
  uint64_t prefill_frames = 0;
  /// Capture xruns the gate found rather than a read reporting them. A subset of driver_xruns(), and
  /// the interesting one: an xrun freezes the hardware pointer, so without this the gate waits for a
  /// period that can never arrive and the run dies of a stall it could have survived.
  uint64_t gate_capture_xruns = 0;
  /// Samples where the backend's own idea of its queue disagreed with its pointer pair. Not a
  /// behaviour of the loop at all, but it is what says whether an occupancy figure can be trusted.
  uint64_t occupancy_mismatches = 0;
  /// \brief Driver xruns by direction, and together they are driver_xruns() exactly.
  ///
  /// Kept apart because only one of them is a latency question: a capture overrun is the device
  /// having produced a period the loop had not read yet (a function of how promptly the loop reads,
  /// which an output queue cannot help), while a playback underrun is the output ring running dry --
  /// precisely the event the queue target exists to prevent. The hour-long baseline reported 92
  /// xruns as one number, and what a target would buy turned entirely on the split (§9.5 item 7).
  uint64_t capture_overruns = 0;
  uint64_t playback_underruns = 0;
};

/// \brief One full-duplex audio stream, whatever is behind it.
///
/// Deliberately byte-oriented and direction-symmetric, matching what the ioctls actually do: the
/// caller converts with namchain/pcm.h. Making the interface float-typed would push a conversion
/// into every backend and hide the one place it belongs.
///
/// Not thread-safe. `read_capture`/`write_playback`/`wait_capture` are called from the audio thread
/// and must not allocate, lock or (for the device backends) do anything the caller has not put
/// outside its real-time scope.
class AudioBackend
{
public:
  virtual ~AudioBackend() = default;

  AudioBackend(const AudioBackend&) = delete;
  AudioBackend& operator=(const AudioBackend&) = delete;
  virtual const BackendInfo& info() const = 0;

  virtual bool start(std::string& error) = 0;
  virtual void stop() = 0;

  /// Wait until a period of capture is available. The engine's loop is wait (untimed) -> read, DSP,
  /// write (timed), so this is where a real-time backend blocks.
  virtual bool wait_capture(int timeout_ms, std::string& error) = 0;

  /// Read exactly \p frames, or fewer only at end of input. \return frames transferred, or -1 on
  /// error. \p dst must hold `frames * bytes_per_sample * capture_channels` BYTES.
  virtual int read_capture(void* dst, unsigned frames, int timeout_ms, std::string& error) = 0;

  /// Write exactly \p frames from \p src. Same sizing rule, playback direction.
  virtual int write_playback(const void* src, unsigned frames, int timeout_ms, std::string& error) = 0;

  /// \brief Write \p frames only if they can go now; return 0 (a dropped period) if they cannot.
  ///
  /// This exists because a loop that *may* block in the write cannot bound its own latency: on the
  /// SSL, an hour of identical 295 us engine work was reported as 993 us blocks, of which 699 us was
  /// the write waiting for space in a saturated playback ring (§9.5 item 4). Asking first and
  /// dropping converts an unbounded wait into a bounded, counted loss, and the count is what lets a
  /// gap in the audio be a reported number instead of an inference from percentiles.
  ///
  /// A backend with a ring may also refuse on \ref BackendConfig::output_queue_periods, which is the
  /// other half of the same decision: the ring's capacity answers "can this period go anywhere at
  /// all", and the target answers "should this run carry another period of latency". The two refusals
  /// are counted separately (see StreamQueueStats) because only the second is the policy working.
  ///
  /// The default always writes: a backend with no ring cannot be full, and telling `file` to drop a
  /// period would silently remove audio from the output WAV.
  ///
  /// \return frames written, 0 when the period was dropped for want of room, or -1 on a real error.
  virtual int try_write_playback(const void* src, unsigned frames, std::string& error)
  {
    const int written = write_playback(src, frames, 1000, error);
    return written < 0 ? -1 : written;
  }

  /// Queue-control counters, where the backend has any. Zeroes for the backends with no rings.
  virtual StreamQueueStats queue_stats() const { return StreamQueueStats{}; }

  /// \brief Bring the output queue up to its configured target, immediately before the timed loop.
  ///
  /// A no-op for every backend with no ring, and for a run with no target. It exists as a separate
  /// step rather than being folded into `start` because the target has to be established *after* the
  /// bring-up path, which spends part of whatever `start` primed: the ring's contents are conserved,
  /// so a queue that begins a period short stays a period short (§9.5 item 7).
  virtual bool prefill_output_queue(std::string& error)
  {
    (void)error;
    return true;
  }

  /// Unrecoverable breaks reported by whatever is underneath: ALSA `-EPIPE`, and nothing else.
  /// Always zero for the backends with no device, which is a fact about the measurement worth
  /// reporting rather than a reason to omit the field (§10.5.1).
  virtual uint64_t driver_xruns() const = 0;

  /// \brief Frames sitting in each ring right now, when the backend has rings to report.
  ///
  /// This is here because a soak can be paced by the wrong ring and its percentiles will not say so.
  /// A single late period leaves a capture backlog; `poll` then returns immediately for ever, the
  /// loop free-runs until the playback ring fills, and from then on the block time being measured is
  /// the write's wait for space rather than the engine's work. That state is invisible in p50/p99 and
  /// unmistakable here -- which is the whole reason it cost an hour to find (plan.md §9.5).
  ///
  /// \return false when there is no ring to report, which is every backend that is not a device.
  ///
  /// Deliberately the ring's contents and not the driver's `delay`: on the SSL 2+ Mk II the playback
  /// delay also counts the frames already handed to the USB host controller, so it reported 544
  /// frames for a 256-frame ring. `StreamQueueStats::occupancy_mismatches` says how often a backend's
  /// own delay field disagreed with the pointer pair behind this figure.
  virtual bool ring_occupancy(long long& /*capture_frames*/, long long& /*playback_frames*/) const { return false; }

  /// Flush anything buffered. A no-op for every backend that is not `file`, where it writes the
  /// output WAV.
  virtual bool finish(std::string& error)
  {
    (void)error;
    return true;
  }

protected:
  /// Protected and defaulted: declaring the copy operations deleted above would otherwise suppress
  /// it, and every concrete backend needs it.
  AudioBackend() = default;
};

/// \brief Open a backend, or return nullptr with \p error set.
///
/// A backend that does not exist (PipeWire today) fails by name. Falling back to another one would
/// make "I measured it on PipeWire" a claim nobody could check.
std::unique_ptr<AudioBackend> open_backend(const BackendConfig& config, BackendKind kind, std::string& error);

} // namespace namchain
