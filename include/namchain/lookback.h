#pragma once

#include <string>

namespace namchain
{

/// \brief Outcome of a lookback (receptive-field) computation.
enum class LookbackStatus
{
  Ok,                  ///< A finite, exact replay length was derived.
  Unbounded,           ///< Model has unbounded memory (e.g. LSTM); replay cannot be exact.
  UnknownArchitecture, ///< Architecture not understood by this calculator.
  Malformed            ///< The JSON did not match the schema we can reason about.
};

/// \brief Result of compute_lookback_*().
struct LookbackResult
{
  LookbackStatus status = LookbackStatus::Malformed;
  /// Number of input samples required to reach exact steady state. This matches the
  /// semantics of nam::DSP::GetPrewarmSamples() in the vendored core, i.e. it is
  /// `receptive_field + 1` (the sample being produced plus the samples it depends on).
  /// Valid only when status == Ok.
  int samples = 0;
  /// \brief The rate the model declares, or 0 when the file names none.
  ///
  /// `samples` is in the model's own domain, and the core resamples the engine's input to it, so a
  /// caller replaying *engine* samples has to scale: a 44.1 kHz model needs `samples * rate / 44100`
  /// frames of 48 kHz input to have seen the same amount of music. Zero means "the same as the
  /// engine", which is what a file written at the engine's rate means and what the engine assumed
  /// before this field existed.
  double sample_rate = 0.0;
  /// Architecture that produced the answer, or a human-readable reason on failure.
  std::string detail;

  bool ok() const { return status == LookbackStatus::Ok; }
};

/// \brief Compute lookback by parsing a `.nam` document ourselves.
///
/// This mirrors the vendored core exactly (NAM/wavenet/model.cpp mPrewarmSamples,
/// LayerArray::get_receptive_field(), detail::Head::receptive_field()) but does not
/// depend on the core's virtuals, so it works for cases where the core reports 0 or
/// hides its submodels:
///   * `SlimmableContainer`  -> max over submodels (the core reports only the *active*
///                              submodel, which is wrong for planning a switch)
///   * `SlimmableWaveNet`    -> computed from config (the core hardcodes `return 0`)
///   * nested `condition_dsp`-> recursed into, matching the core's `mPrewarmSamples`
///                              base value
///   * `LSTM`                -> Unbounded (half-second heuristic in the core is not exact)
///
/// `samples` is the same quantity as `GetPrewarmSamples()`: the number of samples that
/// must be replayed for a WaveNet's state to be exact (see plan.md G5/G7/G8).
LookbackResult compute_lookback_from_json(const std::string& json_text);

/// \brief Same as compute_lookback_from_json(), reading the file at \p path.
LookbackResult compute_lookback_from_file(const std::string& path);

/// \brief Stable, human-readable name for a status.
const char* to_string(LookbackStatus status);

} // namespace namchain
