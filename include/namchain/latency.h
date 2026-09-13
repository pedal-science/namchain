#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace namchain
{

/// \brief Configuration for a round-trip latency measurement.
///
/// The measurement is the one thing about an interface that cannot be derived from its descriptors:
/// how long audio takes to leave the host, cross the converters, and come back. It needs a physical
/// loop (an output patched to an input) or an internal one (a loopback channel), and it is what
/// turns "1.33 ms I/O + 0 algorithmic" from an accounting identity into an observation.
struct LatencyOptions
{
  /// "hw:4,0", "4,0", or a /dev/snd path.
  std::string device;
  unsigned sample_rate = 48000;
  /// Requested period size. Reported alongside the result because the round trip is bounded below
  /// by the device's buffering, which is `periods x quantum` frames at least.
  unsigned quantum = 64;
  unsigned periods = 4;
  /// Device channel counts. 0 asks the backend to use \ref channels for both.
  unsigned channels = 1;
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
  /// Which playback channel carries the stimulus, and which capture channels to analyse.
  unsigned output_channel = 0;
  std::vector<unsigned> measure_channels;
  /// Peak amplitude of the stimulus, linear relative to full scale. The correlation buys back
  /// roughly 10*log10(length) dB of SNR, so this does not need to be loud.
  double stimulus_level = 0.25;
  unsigned stimulus_length = 4096;
  /// How many times to send the stimulus. More than one is what makes "the delay is stable"
  /// an observation rather than an assumption.
  unsigned repeats = 4;
  /// Upper bound on the delay searched for, which sets the run length. A USB device at a 64-frame
  /// quantum is nowhere near this; it is generous because a measurement that silently misses the
  /// peak would report a confidently wrong number.
  double max_delay_ms = 100.0;
  /// How many times to repeat the *whole* measurement, stream start included. This is what turns
  /// "the figure is reproducible" into something the tool shows: the spread across runs is where the
  /// time that the occupancy correction cannot account for actually lives. One run is the default
  /// because it is the expensive part (each run restarts the device).
  unsigned runs = 1;
};

/// One measurement's summary for one capture channel, kept compact so that LatencyResult::runs
/// can hold many without duplicating the whole per-run detail.
struct ChannelRun
{
  unsigned capture_channel = 0;
  bool found = false;
  long long delay_frames = 0;
  long long path_frames = 0;
  long long queue_correction_frames = 0;
};

/// One repetition of the whole measurement.
struct LatencyRun
{
  bool ok = false;
  uint64_t driver_xruns = 0;
  std::vector<ChannelRun> channels;
};

struct ChannelLatency
{
  unsigned capture_channel = 0;
  bool found = false;
  /// The delay from the stimulus being handed to the playback ring to it appearing in the capture,
  /// in frames, counted from the start of the captured stream. This is the round trip including both
  /// directions' buffering *and* whatever happened to be sitting in the rings when the stimulus was
  /// written, which is why it is not reproducible between runs. Prefer \ref path_frames.
  long long delay_frames = 0;
  /// The same, refined by parabolic interpolation of the correlation peak. The analogue path adds a
  /// delay that is not a whole number of samples, so the integer figure is quantised by up to half
  /// a sample and this is the better estimate.
  double delay_frames_sub = 0.0;
  double delay_ms = 0.0;

  /// \brief `delay_frames` with the ring occupancy removed: the delay the device contributes,
  /// rather than the delay this particular run happened to produce.
  ///
  /// The occupancy at the moment of the write is measured with `SNDRV_PCM_IOCTL_STATUS` and
  /// subtracted, which removes a real and large term -- it ranged over ~100 frames between runs and
  /// configurations on the SSL 2+ Mk II. It is not the whole of the irreproducibility, though, and
  /// this figure is **not** frame-exact between runs: what is left is tens of frames, coming from the
  /// unexplained part of the relationship between the two streams' start points. The pointer
  /// interface cannot resolve that (the pointers wrap) and the driver will not timestamp it -- it
  /// accepts `SNDRV_PCM_IOCTL_TSTAMP` and still reports zeros. See plan.md §9.5.
  ///
  /// The figure that *is* exact is the difference between two capture channels measured in the same
  /// run, because both share everything that varies. \ref LatencyResult::runs shows the spread of
  /// this one rather than leaving it to be guessed.
  long long path_frames = 0;
  double path_frames_sub = 0.0;
  double path_ms = 0.0;

  /// Level of the returned burst, measured inside the window the correlation found rather than
  /// across the whole capture -- otherwise one unrelated transient becomes "the level".
  ///
  /// RMS is the figure to compare against the stimulus: a broadband stimulus comes back band-limited,
  /// and band-limiting moves energy from the RMS into ringing, so the peak of a filtered PN sequence
  /// sits far above its RMS and would overstate the path's gain by ~10 dB.
  double captured_rms = 0.0;
  double captured_peak = 0.0;
  uint64_t clipped_frames = 0;
  /// Ratio of the correlation peak to the largest correlation outside it, in dB. This is the
  /// measurement's own error bar: a low figure means the peak is not distinguishable from the
  /// noise, and the delay should not be believed.
  double peak_to_sidelobe_db = 0.0;

  /// Per-repeat delays, in frames. They should all be equal; a spread means the device is
  /// resampling, dropping or repeating frames, which is a fault in its own right.
  std::vector<long long> per_repeat_frames;
  /// Per-repeat \ref path_frames. This spread is the honest uncertainty on the path figure, and it
  /// is the first thing to look at when the result is surprising.
  std::vector<long long> per_repeat_path_frames;

  /// The subtraction, term by term, so the arithmetic can be audited and a surprising figure traced
  /// to whichever term caused it. All in frames, all sampled at the instant of the write.
  long long frames_read_before_write = 0;
  long long capture_delay_frames = 0;
  long long playback_delay_frames = 0;
  /// Where inside its period the stimulus started.
  long long block_offset_frames = 0;
  /// The two hardware pointers at the instant of the write, as ring offsets. Their difference,
  /// modulo the buffer size, is the phase between the device's input and output streams -- the term
  /// that the ring occupancy correction cannot remove, because it is not ring occupancy.
  unsigned long long capture_hw_ptr = 0;
  unsigned long long playback_hw_ptr = 0;
  /// Timestamps the driver reported with those pointers (nanoseconds). `audio_tstamp` is in the
  /// device's own clock when the driver provides one; the flag says whether it did.
  long long capture_tstamp_ns = 0;
  long long playback_tstamp_ns = 0;
  long long capture_audio_tstamp_ns = 0;
  long long playback_audio_tstamp_ns = 0;
  bool playback_audio_tstamp_valid = false;
  /// The sum of the four terms above, which is what was taken off `delay_frames`.
  long long queue_correction_frames = 0;
};

struct LatencyResult
{
  bool ok = false;
  std::string error;

  unsigned sample_rate = 0;
  unsigned period_size = 0;
  unsigned buffer_size = 0;
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
  /// Minimum round trip the device's own buffering implies, in frames: one full buffer for playback
  /// plus one for capture. The measurement should not come in under this, and if it does, one of
  /// the two numbers is wrong.
  unsigned buffer_floor_frames = 0;
  std::string capture_node;
  std::string playback_node;

  uint64_t driver_xruns = 0;
  uint64_t blocks = 0;
  /// Whether the driver agreed to record period timestamps at all. The SSL 2+ Mk II reports none of
  /// the status timestamps, with or without this, which is why the absolute figure cannot be related
  /// to a common clock between the two streams (plan.md §9.5).
  bool timestamps_enabled = false;
  size_t captured_frames = 0;
  unsigned output_channel = 0;
  unsigned stimulus_position = 0;
  unsigned stimulus_length = 0;
  /// Frames between successive stimuli.
  unsigned repeat_spacing = 0;

  std::vector<ChannelLatency> channels;
  /// Every run's summary, in order, when \ref LatencyOptions::runs is greater than one. \ref channels
  /// holds the first run in full.
  std::vector<LatencyRun> runs;
};

/// \brief The delay of a known stimulus inside a capture, by normalised cross-correlation.
struct DelayEstimate
{
  bool found = false;
  /// Lag, in frames, at which the stimulus best matches the capture.
  long long lag = 0;
  /// The peak refined by parabolic interpolation: the analogue path's delay is not a whole number
  /// of samples, so the integer peak is quantised by up to half a sample (10 us at 48 kHz).
  double lag_sub = 0.0;
  /// Correlation at the peak, in [-1, 1].
  double peak = 0.0;
  /// Ratio of the peak to the largest correlation at least one stimulus length away, in dB. This is
  /// the estimate's own error bar: a low figure means the peak is not distinguishable from noise.
  double peak_to_sidelobe_db = 0.0;
};

/// \brief Correlate `stimulus` against `capture` over lags in `[first_lag, last_lag)`.
///
/// Exposed rather than private because this is where the measurement can be wrong in a way the
/// hardware will never reveal: an ambiguous or aliased peak still returns a number, and it looks
/// exactly like a measurement. Testing it against synthetic captures with known delays is the only
/// way to know that a reported latency means what it says.
DelayEstimate estimate_delay(const std::vector<float>& capture, const std::vector<float>& stimulus,
                             long long first_lag, long long last_lag);

/// \brief Send a known stimulus out of one channel and time its return.
///
/// Full duplex, one stimulus repeated \ref LatencyOptions::repeats times. The delay falls out of the
/// cross-correlation between what was sent and what came back, so it needs no assumption about the
/// device's internal buffering -- which is the point, since that is what is being measured.
LatencyResult measure_round_trip_latency(const LatencyOptions& options);

} // namespace namchain
