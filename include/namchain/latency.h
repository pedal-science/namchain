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
};

struct ChannelLatency
{
  unsigned capture_channel = 0;
  bool found = false;
  /// The delay from the stimulus being handed to the playback ring to it appearing in the capture,
  /// in frames. This is the round trip including both directions' buffering.
  long long delay_frames = 0;
  /// The same, refined by parabolic interpolation of the correlation peak. The analogue path adds a
  /// delay that is not a whole number of samples, so the integer figure is quantised by up to half
  /// a sample and this is the better estimate.
  double delay_frames_sub = 0.0;
  double delay_ms = 0.0;

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
  size_t captured_frames = 0;
  unsigned output_channel = 0;
  unsigned stimulus_position = 0;
  unsigned stimulus_length = 0;
  /// Frames between successive stimuli.
  unsigned repeat_spacing = 0;

  std::vector<ChannelLatency> channels;
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
