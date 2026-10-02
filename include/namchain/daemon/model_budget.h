#pragma once

// The model budget: a ceiling on what a document's models read per block, together, and the rule
// that keeps a document under it by slimming models that can be slimmed (docs/daemon.md, "Model
// budget").
//
// Why it exists: every chain renders on one thread, on one core, so the models of a whole document
// share that core's L2. On a Cortex-A76 (512 KB) two A2-Full models do not fit together, and the
// render's worst blocks follow the shared L3 and DRAM from then on; one Full and one Lite fit
// (pedal.project, bench-results/2026-10-02-two-amp-tail-vip). The figures come from
// namchain/working_set.h, which estimates from a model's config rather than measuring.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "namchain/graph/rig.h"
#include "namchain/working_set.h"

namespace namchain
{
namespace daemon
{

/// \brief Working-set estimates by file, so a publish does not parse every model of a document again.
///
/// An entry is reused while the file's size and modification time are what they were when it was
/// read; a model replaced in the library is read again. Control thread only.
class WorkingSetCache
{
public:
  /// The estimate for the model at \p path, at blocks of up to \p max_quantum frames.
  WorkingSet get(const std::string& path, int max_quantum);
  /// How many files have been parsed rather than served from the cache. For tests.
  std::size_t parses() const { return parses_; }

private:
  struct Entry
  {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    int max_quantum = 0;
    WorkingSet estimate;
  };
  std::map<std::string, Entry> entries_;
  std::size_t parses_ = 0;
};

/// One `nc.nam` node as the budget saw it.
struct BudgetedModel
{
  std::string chain;
  std::string node;
  /// The node's element (its id when the document gave none): what a client keys a block on.
  std::string element;
  /// Where the node sits in the document, so a caller can find the same node in the document as
  /// written (resolution changes values, never the order).
  std::size_t chain_index = 0;
  std::size_t node_index = 0;
  /// The model file, as resolved.
  std::string path;
  /// The reference as the document wrote it. Not filled in by apply_model_budget(), which sees only
  /// the resolved document.
  std::string reference;
  std::size_t full_bytes = 0;
  std::size_t slim_bytes = 0;
  bool slimmable = false;
  /// Set by the budget: the node runs its smallest submodel (graph::NodeSpec::slim).
  bool slimmed = false;
  /// False when the estimator could not read the model: it is counted as nothing, and \ref why says
  /// what went wrong.
  bool estimated = true;
  std::string why;

  /// What the node reads per block as the budget left it.
  std::size_t bytes() const { return slimmed ? slim_bytes : full_bytes; }
};

/// What the budget made of one document.
struct BudgetOutcome
{
  /// The budget in bytes; 0 when it is off, and then nothing else is filled in.
  std::size_t budget_bytes = 0;
  /// What the document's models read per block as published, and what they would read unslimmed.
  std::size_t used_bytes = 0;
  std::size_t full_bytes = 0;
  /// Still over the budget with every slimmable model slimmed. The caller decides what that means:
  /// a client's publish is refused, the configured rig is admitted with a warning.
  bool over = false;
  /// Every `nc.nam` node, in document order: chains in order, each chain's nodes as declared.
  std::vector<BudgetedModel> models;
};

/// \brief Estimate every `nc.nam` node of \p resolved, and if together they read more than
/// \p budget_bytes per block, slim the slimmable ones -- the last declared first -- until they fit or
/// nothing slimmable is left.
///
/// Slimming sets graph::NodeSpec::slim on \p resolved, which must be the resolved copy of a document
/// (model references already turned into paths), never the document a client wrote. A budget of 0 is
/// off: nothing is estimated, nothing is slimmed, and the outcome is empty. Never refuses anything:
/// \ref BudgetOutcome::over is for the caller to act on.
///
/// Only `nc.nam` nodes are counted. A missing model's stand-in is an `nc.bypass` by now and counts
/// as nothing; a bypassed `nc.nam` still renders (graph::NodeSpec::bypass) and counts in full.
BudgetOutcome apply_model_budget(graph::Document& resolved, std::size_t budget_bytes, int max_quantum,
                                 WorkingSetCache& cache);

/// A byte count as KiB with one decimal, the way the budget's messages state sizes.
std::string format_kib(std::size_t bytes);

} // namespace daemon
} // namespace namchain
