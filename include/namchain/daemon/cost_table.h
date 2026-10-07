#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "namchain/graph/rig.h"

namespace namchain
{
namespace daemon
{

/// \brief One device's measured cost table: what a unit costs on one core of one Core device.
///
/// pedal.project docs/managing-processing-power-and-optimising-for-fast-switching.md, D2 and D9. The
/// format is **the engine's** -- this is where "what a unit is and what a cost means" is decided --
/// and the numbers are the device's: one file per device, measured on that device by
/// `namchain-warm-bench measure` (P1) and installed where the engine reads it (P2).
///
/// A unit is a NAM model at a submodel size (`NAM.<family>.<full|lite>`), an IR at a tap count
/// (`IR.<taps>`), or a node type (`node.<nc.type>`). Its cost is `1/n`, where `n` is the largest
/// number of copies of it that rendered a block within the deadline on one core, and costs **add**
/// (D2). A rig is admitted when its total is at most \ref headroom (D3, 0.8).
///
/// A unit with no row costs \ref unknown (D8): never nothing, so a rig cannot be waved through on the
/// grounds that nobody had measured its parts. A table that is missing or unreadable is not an error
/// the engine can refuse work over -- it is every unit unmeasured, which is the same conservative
/// answer -- and \ref ok says so for the status report.
struct CostTable
{
  /// What one unit costs, which row charged for it, and whether that row was measured.
  struct Charge
  {
    double cost = 0;
    std::string row; ///< "NAM.WaveNet.full", "IR.8192", "node.nc.gain", "unknown.NAM", ...
    bool measured = false;
  };

  bool ok = false;    ///< false when there is no usable table: every unit then costs `unknown`
  std::string detail; ///< why not, when !ok
  std::string path;
  std::string host;
  std::string measured;
  std::string engine_commit;
  std::string kernel;
  std::string bar;
  int quantum = 64;
  double deadline_us = 0;
  /// The largest cost a rig may have and still be admitted (D3: 0.8). A table that names none keeps
  /// this, which is the value the rules settled on.
  double headroom = 0.8;

  std::map<std::string, double> nam;  ///< "WaveNet.full" -> 0.125
  std::map<int, double> ir;           ///< taps -> cost
  std::map<std::string, double> node; ///< "nc.gain" -> cost
  double unknown_nam = 0.25;
  double unknown_ir = 0.1;
  double unknown_node = 0.05;

  /// The rows, for a status report: "NAM.WaveNet.full" -> 0.125, and so on.
  std::vector<std::pair<std::string, double>> rows() const;

  Charge charge_nam(const std::string& family, bool lite) const;
  /// A cab costs the cheapest row that is not shorter than it: a 6000-tap IR pays the 8192 row, and
  /// one longer than every row (which the engine's own truncation makes impossible for a `.wav`
  /// cab, but not for a `Linear` `.nam` one) costs `unknown.IR`.
  Charge charge_ir(std::size_t taps) const;
  Charge charge_node(const std::string& type) const;

  static CostTable load(const std::string& path);
  static CostTable from_json(const std::string& text, const std::string& path);
};

/// \brief What a document's units are, read from their files and cached.
///
/// A publish must not reparse a library, and a model's family and an IR's taps both need a file read,
/// so both are cached by path, size and mtime -- the same discipline the model budget used. The
/// working-set estimate comes along because it is read from the same parse and is what a status
/// report can show beside a measured cost (T2's falsifier is the working set against L2).
class UnitCache
{
public:
  struct Unit
  {
    bool ok = false;
    std::string kind;     ///< "NAM" or "IR"
    std::string family;   ///< NAM: the architecture family its row is keyed by
    bool slimmable = false; ///< NAM: has a smaller submodel for D6's trimming to select
    std::size_t taps = 0; ///< IR: the taps that reach the convolver, after resample and truncation
    std::size_t full_bytes = 0; ///< the model's working set at the size it loads at
    std::size_t slim_bytes = 0;
    std::string why;      ///< why there is no answer
  };

  /// \param path the model or IR file; \param is_ir whether it is an `nc.cab.ir`; \param rate the
  /// engine rate, which is what an IR is resampled to before its taps are counted.
  Unit get(const std::string& path, bool is_ir, double rate, int max_quantum);

  /// How many files this has read: what a test says "cached" with.
  std::size_t reads() const { return reads_; }

private:
  struct Entry
  {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    Unit unit;
  };
  std::map<std::string, Entry> entries_;
  std::size_t reads_ = 0;
};

/// One node's share of a rig's cost.
struct CostedNode
{
  std::string chain;
  std::string node;    ///< the node's id
  std::string element; ///< what a client keys a block on
  std::string reference; ///< the model or IR reference as the client wrote it
  std::string row;     ///< the row that charged for it
  std::size_t chain_index = 0; ///< where it is in the resolved document, for writing `slim` back
  std::size_t node_index = 0;
  double cost = 0;     ///< as it stands now, after any trimming
  double cost_full = 0;///< before trimming: what this node costs if it plays its own size
  double cost_lite = 0;///< 0 when nothing can be trimmed
  bool slimmable = false;
  bool measured = true;
  bool slimmed = false; ///< this is what trimming changed
  std::size_t working_set_bytes = 0;
};

/// What a rig costs, and what the engine did about it.
struct CostOutcome
{
  bool has_table = false;
  std::string table;    ///< the table's path, or "" when there was none
  double cost = 0;      ///< as it stands, after trimming
  double full_cost = 0; ///< as written, before trimming
  double headroom = 0.8;
  bool over = false;    ///< over the headroom: a client's publish is refused
  bool trimmed = false; ///< D6: at least one model plays its Lite submodel to fit
  std::string verdict;  ///< "fits", "trimmed" or "over"
  std::vector<CostedNode> nodes;
};

/// \brief Cost a resolved document against a table and trim it until it fits.
///
/// Writes `node.slim` on the **resolved** copy and nowhere else, exactly as the model budget did: no
/// rig or patch spells it and `rig.get` never returns it. D6: trimming is Full -> Lite, applied only
/// as far as it needs to be, dearest first -- the node whose Lite saves the most, until the total is
/// at most the table's headroom or nothing slimmable is left.
CostOutcome apply_cost_table(graph::Document& resolved, const CostTable& table, UnitCache& units, double rate,
                             int max_quantum);

/// A cost as the messages and the status report write it: "0.66 of a core".
std::string format_cost(double cost);

/// The nodes named one per line, for a refusal message.
std::string describe_costs(const CostOutcome& outcome, bool with_chain);

} // namespace daemon
} // namespace namchain
