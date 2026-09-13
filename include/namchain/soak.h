#pragma once

#include <cstdint>
#include <string>

#include "namchain/io/backend.h"

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
  /// Device mode only: how many seconds of the run the self-oracle compares.
  ///
  /// The comparison has to keep the captured input, and an hour of it is 1.4 GB, so it is bounded.
  /// The bound is reported rather than left implicit: "the first N seconds are sample-identical to
  /// the offline render" is a weaker claim than "the whole run is", and the number belongs next to
  /// it. The rest of the run is still covered by the xrun and timing gates.
  double oracle_window_seconds = 60.0;

  /// When non-empty, run against a real ALSA device in passthrough (capture -> playback) instead
  /// of a model: "hw:4,0", "4,0" or a /dev/snd path. This is the device half of the acceptance
  /// criterion, and the stage where `driver_xruns` stops being permanently zero.
  std::string device;
  /// Which I/O backend drives the stream half of the harness (plan.md §9.1). The default is the
  /// device backend, which is what the acceptance criterion means by "on the SSL"; `null` paces
  /// against the wall clock with no device so CI can run the same timing gates, and `file` replays a
  /// WAV through the same loop with neither a device nor a clock.
  ///
  /// Choosing a backend other than \ref device's also selects this path without a \ref device, which
  /// is what makes `--backend null` usable on a machine with no audio at all.
  BackendKind backend = BackendKind::AlsaRaw;
  /// When set, run this model between the device's capture read and playback write. Several
  /// instances are chained in series, which is the CPU cost of that many concurrent models.
  std::string model;
  /// A rig file (plan.md §5). Mutually exclusive with \ref model: this is how an arbitrary graph
  /// reaches the audio thread, and it is what makes the compiler -- not just a model -- the thing
  /// being measured.
  std::string graph_path;
  unsigned instances = 1;
  unsigned input_channel = 0;
  unsigned output_channel = 0;
  unsigned device_channels = 1;
  /// 0 means "the same as device_channels". Needed wherever the device is asymmetric:
  /// the SSL 2+ Mk II is 6 out / 8 in on firmware V1.16, and USB audio only offers the
  /// channel counts it has, so a subset fails to open rather than being trimmed.
  unsigned capture_channels = 0;
  unsigned playback_channels = 0;
  unsigned device_periods = 4;
  /// The engine's rate, and therefore the rate the device is asked for and that every input file
  /// and model is checked against. Fixed rather than inferred: this is a 48 kHz engine at M1, and a
  /// harness that quietly follows whichever file it was handed cannot compare two runs.
  unsigned sample_rate = 48000;
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
  /// Allocations and deallocations observed *inside the render-path scope*. Both must be zero;
  /// rt_instrumented says whether the interposer was compiled in at all, because "zero" and "not
  /// measured" are very different claims.
  uint64_t rt_allocations = 0;
  uint64_t rt_deallocations = 0;
  bool rt_instrumented = false;

  uint64_t deadline_overruns = 0;
  /// Xruns reported by the driver (`-EPIPE`, `SNDRV_PCM_STATE_XRUN`). Always 0 until the
  /// `alsa_raw`/`pipewire` backends land: the counters and the reporting path exist now so
  /// that the acceptance criterion has somewhere to live, rather than being retrofitted.
  uint64_t driver_xruns = 0;

  uint64_t deadline_ns = 0;
  /// Time spent blocked waiting for the device to offer a period, separate from the work we do
  /// on it. This is what distinguishes "the device is pacing us slower than nominal" from "we are
  /// too slow": if wait + work exceeds the deadline, the run cannot keep real time.
  uint64_t wait_p50_ns = 0;
  /// Phase breakdown of the timed section, device mode only: backend read, engine chain, backend
  /// write. A single "block time" cannot distinguish blocking in the backend from working.
  uint64_t read_p50_ns = 0;
  uint64_t chain_p50_ns = 0;
  uint64_t write_p50_ns = 0;
  uint64_t wait_p99_ns = 0;
  uint64_t wait_max_ns = 0;
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
  /// True when the run was driven by an \ref AudioBackend a period at a time, rather than by the
  /// flat-out offline path. Covers the device, the paced clock and the WAV file alike.
  bool stream_mode = false;
  /// Name of the backend that drove it, so a number can never be quoted without saying where it
  /// came from.
  std::string backend;
  /// Whether mlockall succeeded: without it the measurement includes page-fault stalls.
  bool memory_locked = false;
  /// Index of the first block that exceeded the deadline; UINT64_MAX if none. Distinguishes a
  /// startup transient from steady-state overload, which need very different responses.
  uint64_t first_overrun_block = UINT64_MAX;
  /// Driver xruns seen during the untimed bring-up cycle, before steady state was reached.
  uint64_t startup_driver_xruns = 0;
  unsigned device_period_size = 0;
  unsigned device_buffer_size = 0;
  /// The sample format the backend presented, by PcmFormat name. A backend rather than a device
  /// now, so this is the portable name and not an SNDRV_PCM_FORMAT_* value.
  std::string device_sample_format;
  unsigned device_bytes_per_sample = 0;
  /// False when the device rounded the requested period (e.g. to its 1 ms full-speed USB floor).
  bool device_period_exact = false;
  std::string device_capture_node;
  std::string device_playback_node;
  unsigned chain_instances = 0;
  std::string chain_model;
  /// The concrete C++ type the factory returned (e.g. A2PlanarFull vs A2FastModel<8>). A timing
  /// number is only meaningful alongside the implementation it measured, and the two A2 paths
  /// differ by ~2x.
  std::string model_type;

  // ---- the compiled plan ----
  /// Filled in both modes, because both now render through the same `RenderPlan`: a soak with
  /// `--model` is a chain graph, so the compiler is always in the measurement path.
  unsigned plan_nodes = 0;
  unsigned plan_buffers = 0;
  unsigned plan_buffer_bytes = 0;
  /// Buffers a naive one-per-value interpreter would need. The ratio is the memory plan's point,
  /// and reporting both is what keeps "the colouring works" from being an assertion.
  unsigned reference_buffers = 0;
  bool graph_mode = false;
  std::string graph_source; ///< the rig path, or "single model chain"

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
