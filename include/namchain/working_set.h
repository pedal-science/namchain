#pragma once

#include <cstddef>
#include <string>

namespace namchain
{

/// \brief An estimate of the memory a model's render reads every block: its weights, and for each
/// convolution the history a block reaches back into -- lookback plus the block itself, per channel.
///
/// It exists for the engine's model budget (docs/daemon.md, "Model budget"). On a Cortex-A76, two
/// A2-Full models together overflow the core's 512 KB L2, and the render's worst blocks follow the
/// shared L3 and DRAM from then on (pedal.project, bench-results/2026-10-02-two-amp-tail-vip). This
/// is a figure from the model's config, not a measurement: what a kernel allocates and a block does
/// not read (a planar ring's rewind room, per-block scratch) is not counted, and neither is the
/// layer-to-layer traffic, which stays in L1.
struct WorkingSet
{
  bool ok = false;
  /// The architecture the answer came from, or why there is none.
  std::string detail;
  /// Bytes at the size the model loads at. A SlimmableContainer loads its last submodel.
  std::size_t full_bytes = 0;
  /// Bytes once slimmed: the submodel `SetSlimmableSize(0.0)` selects. Equal to \ref full_bytes for a
  /// model that offers one size.
  std::size_t slim_bytes = 0;
  /// True when slimming makes the model smaller: a SlimmableContainer whose `SetSlimmableSize(0.0)`
  /// submodel is smaller than the one it loads. A SlimmableWaveNet is not counted as slimmable: its
  /// smaller sizes are not described by its config.
  bool slimmable = false;
};

/// \brief Estimate a `.nam` document's working set at blocks of up to \p max_quantum frames.
///
/// Mirrors the architectures `compute_lookback_from_json` understands (WaveNet, SlimmableWaveNet,
/// SlimmableContainer, Linear, LSTM), plus ConvNet and Sequential. Never throws: an unreadable or
/// unfamiliar model comes back with `ok == false` and the reason in `detail`.
WorkingSet working_set_from_json(const std::string& json_text, int max_quantum);

/// \brief Same as working_set_from_json(), reading the file at \p path.
WorkingSet working_set_from_file(const std::string& path, int max_quantum);

/// \brief The architecture family a model's cost row keys on: the model's own `architecture`, or --
/// for a `SlimmableContainer`, which is a wrapper rather than a network -- its submodels'.
///
/// The pedal's cost table (pedal.project
/// docs/managing-processing-power-and-optimising-for-fast-switching.md, D2 and D9) keys its NAM rows
/// by `NAM.<family>.<full|lite>`, and D9 gives the engine the question "what is a unit". This is that
/// answer: the architecture the file declares, so a row cannot be keyed by a name no model carries.
/// Empty when the file cannot be read or declares no architecture.
///
/// It is deliberately coarse: two WaveNets of different shapes are one family here, which is the
/// honest limit of what the file says. A finer taxonomy is future work -- the table's `unknown` (D8)
/// is what an unrecognised model costs until then.
std::string model_family_from_json(const std::string& json_text);

/// \brief Same as model_family_from_json(), reading the file at \p path.
std::string model_family_from_file(const std::string& path);

} // namespace namchain
