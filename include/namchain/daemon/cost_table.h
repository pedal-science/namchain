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

  /// \brief D3's coexistence charge: what a warm document beside the audible one costs it (2026-10-09).
  ///
  /// The unit rows are measured on a quiet core, and the fit bar assumes a rig has the core to itself.
  /// It does not: with a warm document rendering on another core the same rig takes longer -- shared L3
  /// and DRAM -- and T5 measured what that costs on vip. A rig admitted at 0.735 holds the deadline
  /// with nothing warm and misses it 4 to 72 times a minute with a neighbour, so `fits` was promising
  /// the wrong thing and the promise is now charged for.
  ///
  /// `factor` is indexed by how many warm documents the engine holds (1 today, 2 when the engine holds
  /// a rig per inactive core) and multiplies the rig's cost: `effective = cost * factor`. It is a
  /// device property -- memory traffic, not model arithmetic -- so it lives here, at the top level,
  /// rather than in a unit row, and a table that names none charges nothing (factor 1.0), which is
  /// what every table measured before 2026-10-09 does.
  std::map<int, double> coexistence_factors;
  std::string coexistence_measured;
  std::string coexistence_note;

  /// The multiplier for this many warm documents: 1.0 when the table has no charge for it, the
  /// largest measured factor when the count is beyond what was measured, and never below 1.0.
  double coexistence_factor(int warm_documents) const;

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
  /// The charge for rendering beside the warm documents held at the time, and what the rig costs with
  /// it: `effective = cost * coexistence`. 1.0 and `cost` when nothing is warm or the table charges
  /// nothing, which is the case for every table measured before 2026-10-09.
  double coexistence = 1.0;
  int warm_documents = 0;
  double effective = 0; ///< what the deadline actually sees, and what `over` is decided on
  bool over = false;    ///< over the headroom: a client's publish is refused
  bool trimmed = false; ///< D6: at least one model plays its Lite submodel to fit
  /// P8's first step: this document was trimmed to Lite *because it was asked for*, not because it
  /// had to be -- the rig is on its way to Full and the caller says so (the engine reports it).
  bool lite_first = false;
  std::string verdict;  ///< "fits", "trimmed" or "over"
  std::vector<CostedNode> nodes;
};

/// \brief Cost a resolved document against a table and trim it until it fits.
///
/// Writes `node.slim` on the **resolved** copy and nowhere else, exactly as the model budget did: no
/// rig or patch spells it and `rig.get` never returns it. D6: trimming is Full -> Lite, applied only
/// as far as it needs to be, dearest first -- the node whose Lite saves the most, until the total is
/// at most the table's headroom or nothing slimmable is left.
///
/// \param warm_documents the warm documents the engine is holding while this rig will play. Their cost
/// to it is the table's coexistence charge, and it is what the trimming and the admission test are
/// against: with a warm rig beside it a document is trimmed further, or refused, where it would have
/// fitted alone (D3, 2026-10-09).
/// \param lite_first trim **every** slimmable model to Lite whatever the budget says (P8). The point
/// is a rig that can start playing now -- a Lite compiles in about a third of the time -- with the
/// Full compiled behind it and crossfaded to when it is ready; E2's listening verdict is that a Lite
/// standing in for a Full is not audible on the models it tested, and D6 still requires it to be
/// *visible*, which is what \ref CostOutcome::lite_first is for.
CostOutcome apply_cost_table(graph::Document& resolved, const CostTable& table, UnitCache& units, double rate,
                             int max_quantum, int warm_documents = 0, bool lite_first = false);

/// A cost as the messages and the status report write it: "0.66 of a core".
std::string format_cost(double cost);

/// The nodes named one per line, for a refusal message.
std::string describe_costs(const CostOutcome& outcome, bool with_chain);

} // namespace daemon
} // namespace namchain
