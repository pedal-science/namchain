#pragma once

#include <cstdint>
#include <string>

namespace namchain
{

/// \brief Configuration for a soak run.
struct SoakOptions
{
  int quantum = 64;
  /// Audio seconds to process. Input is looped if it is shorter than this.
  double duration_seconds = 60.0;
  /// Skip this many seconds of the input before starting, and loop back to that point rather
  /// than to the file start.
  ///
  /// This matters more than it looks. The vendored example fixture is **1 s of silence followed
  /// by 1 s of signal**, and a soak that loops it from the top spends half its life on silence —
  /// where NAM's silence handling (§7.5) skips work and the CPU load is not the load you are
  /// trying to validate. Start at the signal.
  double input_offset_seconds = 0.0;
  /// Pace the loop against the wall clock, the way a real audio callback is paced. When
  /// false the loop runs as fast as possible, which measures throughput but cannot show
  /// lateness.
  bool realtime_paced = true;
  /// Compare the rendered output against an independent offline render of the same input.
  bool self_oracle = false;
  /// Compare the rendered output against this WAV instead.
  std::string oracle_wav;
  /// Write the rendered audio here (only needed when keeping it is useful).
  std::string output_wav;
  /// Tolerance passed to the oracle comparison (see namchain::compare_wav_mono).
  double oracle_tolerance = 1e-5;

  /// When non-empty, run against a real ALSA device in passthrough (capture -> playback) instead
  /// of a model: "hw:4,0", "4,0" or a /dev/snd path. This is the device half of the acceptance
  /// criterion, and the stage where `driver_xruns` stops being permanently zero.
  std::string device;
  unsigned device_channels = 1;
  unsigned device_periods = 4;
  unsigned device_sample_rate = 48000;
};

struct OracleResult
{
  bool checked = false;
  bool match = false;
  size_t frames = 0;
  double max_abs = 0.0;
  double rms = 0.0;
  std::string source; ///< what we compared against: "offline self-render" or a path
  std::string detail; ///< why it failed, when it did
};

/// \brief Outcome of a soak run. Everything the acceptance criterion needs to be judged.
struct SoakResult
{
  bool ok = false;
  std::string error;

  uint64_t blocks = 0;
  uint64_t frames = 0;

  /// Blocks whose processing time exceeded the period deadline. On a real device this is
  /// what becomes an xrun, and it is the number that lets us argue xruns *cannot* happen
  /// (p99 comfortably under the deadline) instead of waiting to observe one.
  uint64_t deadline_overruns = 0;
  /// Xruns reported by the driver (`-EPIPE`, `SNDRV_PCM_STATE_XRUN`). Always 0 until the
  /// `alsa_raw`/`pipewire` backends land: the counters and the reporting path exist now so
  /// that the acceptance criterion has somewhere to live, rather than being retrofitted.
  uint64_t driver_xruns = 0;

  uint64_t deadline_ns = 0;
  uint64_t p50_ns = 0;
  uint64_t p99_ns = 0;
  uint64_t max_ns = 0;
  double p99_percent_of_deadline = 0.0;

  double audio_seconds = 0.0;
  double wall_seconds = 0.0;
  double rtf = 0.0;
  /// Worst lateness behind the ideal schedule caused by processing alone (0 if the loop always
  /// kept up). Sleep/OS wake-up jitter is deliberately excluded: this measures whether the
  /// engine fits its deadline, not whether the scheduler is well behaved -- the latter shows up
  /// as driver xruns once a real device is attached.
  double max_lateness_ms = 0.0;

  // ---- device mode only ----
  bool device_mode = false;
  unsigned device_period_size = 0;
  unsigned device_buffer_size = 0;
  unsigned device_sample_format = 0;   ///< SNDRV_PCM_FORMAT_* the device agreed to
  unsigned device_bytes_per_sample = 0;
  /// False when the device rounded the requested period (e.g. to its 1 ms full-speed USB floor).
  bool device_period_exact = false;
  std::string device_capture_node;
  std::string device_playback_node;

  OracleResult oracle;
};

/// \brief Run a bounded soak over one model and report timing and correctness.
///
/// This is the M1 acceptance harness. It is deliberately backend-agnostic: with `null`/`file`
/// pacing it measures the engine's margin and proves sample-correctness against the offline
/// render, and the same counters will be driven by a real device through `driver_xruns` once
/// the ALSA backend exists. See plan.md §1 criterion 3 and §10.5.
SoakResult run_soak(const std::string& model_path, const std::string& input_wav_path, const SoakOptions& options);

} // namespace namchain
