// The cost table: what a unit costs, what a rig costs, and what happens at the boundary.
//
// pedal.project docs/managing-processing-power-and-optimising-for-fast-switching.md, D2, D3, D6 and
// D8, and the boundary test T4. The table's own format is devices/common/cost.schema.json in the
// pedal repo; what is tested here is the engine's half of D9 -- what a unit is, what a cost means,
// how a rig's cost is summed, and the fit verdict.
#include <doctest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "namchain/daemon/cost_table.h"
#include "namchain/daemon/daemon.h"

namespace
{

using namchain::daemon::apply_cost_table;
using namchain::daemon::CostOutcome;
using namchain::daemon::CostTable;
using namchain::daemon::UnitCache;
using namchain::graph::Chain;
using namchain::graph::Document;
using namchain::graph::NodeSpec;

std::string generated(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

std::string example(const char* name)
{
  return std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/" + name;
}

struct TempDir
{
  TempDir()
  {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("namchain-cost-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::filesystem::path path;
};

/// A `Linear` .nam with `taps` taps: an impulse response the cost walk can charge for. Only its tap
/// count matters here -- nothing in this test compiles or renders it.
std::string linear_nam(const TempDir& dir, const std::string& name, int taps)
{
  const std::filesystem::path path = dir.path / name;
  std::ofstream out(path);
  out << R"({"architecture":"Linear","config":{"receptive_field":)" << taps << R"(},"weights":[)";
  for (int i = 0; i < taps; ++i)
    out << (i == 0 ? "0.0" : ",0.0");
  out << R"(],"sample_rate":48000})";
  out.close();
  return path.string();
}

/// A table with the rows these tests charge against, in the shape the schema defines.
std::string test_table_json(double headroom = 0.8)
{
  return std::string(R"({
    "host": "test", "measured": "2026-10-07T00:00:00Z", "engine_commit": "c0e3f37", "kernel": "test",
    "rate": 48000, "quantum": 64, "deadline_us": 1333.333, "headroom": )") + std::to_string(headroom) +
         R"(, "bar": "p99 under the deadline and zero overruns",
    "core_percent": {"NAM": {"WaveNet": {"full": 0.4, "lite": 0.1}},
                     "IR": {"2048": 0.2, "8192": 0.05},
                     "node": {"nc.gain": 0.01, "nc.input": 0.01, "nc.output": 0.01}},
    "unknown": {"NAM": 0.25, "IR": 0.1, "node": 0.05},
    "sources": {"NAM.WaveNet.full": {"n": 3, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.2},
                "NAM.WaveNet.lite": {"n": 10, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "IR.2048": {"n": 5, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "IR.8192": {"n": 20, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "node.nc.gain": {"n": 100, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "node.nc.input": {"n": 100, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1},
                "node.nc.output": {"n": 100, "p99_at_n": 0.9, "p99_at_n_plus_1": 1.1}}})";
}

CostTable test_table(double headroom = 0.8)
{
  return CostTable::from_json(test_table_json(headroom), "/etc/namchain/cost.json");
}

/// One chain of one document, with the nodes named in `nodes`.
Document document_of(const std::vector<NodeSpec>& nodes)
{
  Chain chain;
  chain.id = "main";
  chain.graph.nodes.push_back(NodeSpec{"in", "nc.input"});
  for (const NodeSpec& node : nodes)
    chain.graph.nodes.push_back(node);
  chain.graph.nodes.push_back(NodeSpec{"out", "nc.output"});
  Document document;
  document.chains.push_back(chain);
  return document;
}

NodeSpec amp(const std::string& id, const std::string& path)
{
  return NodeSpec{id, "nc.nam", {}, {{"model", path}}};
}

NodeSpec cab(const std::string& id, const std::string& path)
{
  return NodeSpec{id, "nc.cab.ir", {}, {{"ir", path}}};
}

NodeSpec gain(const std::string& id)
{
  return NodeSpec{id, "nc.gain", {{"gain_db", 0.0}}, {}};
}

bool slimmed(const Document& document, const std::string& id)
{
  for (const NodeSpec& node : document.chains[0].graph.nodes)
  {
    if (node.id == id)
      return node.slim;
  }
  return false;
}

double cost_of(const CostOutcome& outcome, const std::string& id)
{
  for (const namchain::daemon::CostedNode& node : outcome.nodes)
  {
    if (node.node == id)
      return node.cost;
  }
  return -1.0;
}

} // namespace

TEST_CASE("cost table: a table that cannot be read is every unit unmeasured, not an error")
{
  const CostTable missing = CostTable::load("/nonexistent/cost.json");
  CHECK_FALSE(missing.ok);
  CHECK(missing.detail.find("cannot open") != std::string::npos);
  // D8's values are still there, so a rig is charged the expensive direction rather than nothing.
  CHECK(missing.charge_nam("WaveNet", false).cost == doctest::Approx(0.25));
  CHECK_FALSE(missing.charge_nam("WaveNet", false).measured);
  CHECK(missing.unknown_ir == doctest::Approx(0.1));
  CHECK(missing.headroom == doctest::Approx(0.8)); // D3's value survives a table that is not there

  const CostTable broken = CostTable::from_json("not json", "broken.json");
  CHECK_FALSE(broken.ok);
  CHECK(broken.detail.find("JSON parse error") != std::string::npos);
}

TEST_CASE("cost table: rows are what the device measured, and a family that has none is unknown")
{
  const CostTable table = test_table();
  REQUIRE_MESSAGE(table.ok, table.detail);

  const CostTable::Charge full = table.charge_nam("WaveNet", false);
  CHECK(full.row == "NAM.WaveNet.full");
  CHECK(full.cost == doctest::Approx(0.4));
  CHECK(full.measured);
  CHECK(table.charge_nam("WaveNet", true).row == "NAM.WaveNet.lite");
  CHECK(table.charge_nam("LSTM", false).row == "unknown.NAM");
  CHECK_FALSE(table.charge_nam("LSTM", false).measured);

  CHECK(table.charge_node("nc.gain").cost == doctest::Approx(0.01));
  CHECK(table.charge_node("nc.delay").row == "unknown.node");
  CHECK(table.rows().size() == 7);
}

TEST_CASE("cost table: an IR is charged the cheapest row that is not shorter than it")
{
  const CostTable table = test_table();
  CHECK(table.charge_ir(2048).row == "IR.2048");
  CHECK(table.charge_ir(1).row == "IR.2048");     // the shortest row pays for anything below it
  CHECK(table.charge_ir(2049).row == "IR.8192");  // and a longer IR pays the next row up
  CHECK(table.charge_ir(8192).row == "IR.8192");
  CHECK(table.charge_ir(8193).row == "unknown.IR"); // past every row: D8, and the engine truncates first
}

TEST_CASE("cost table: a rig that fits is admitted as written")
{
  // T4's first rig: 0.4 + 0.2 + 3 x 0.01 + the two structural nodes = 0.65, under the 0.8 headroom.
  Document document = document_of({amp("amp1", example("A2.nam")), cab("cab1", generated("CabIR.nam")),
                                   gain("g1"), gain("g2"), gain("g3")});
  const CostTable table = test_table();
  UnitCache units;
  const CostOutcome outcome = apply_cost_table(document, table, units, 48000.0, 64);

  CHECK(outcome.cost == doctest::Approx(0.65));
  CHECK(outcome.full_cost == doctest::Approx(0.65));
  CHECK_FALSE(outcome.over);
  CHECK_FALSE(outcome.trimmed);
  CHECK(outcome.verdict == "fits");
  CHECK_FALSE(slimmed(document, "amp1"));
  CHECK(cost_of(outcome, "cab1") == doctest::Approx(0.2));
  CHECK(cost_of(outcome, "g3") == doctest::Approx(0.01));
}

TEST_CASE("cost table: the coexistence charge is what the warm set costs the audible rig")
{
  // The shape T5 measured on vip (2026-10-09): with one warm document rendering beside it a rig's cost
  // is multiplied by 1.25, with two by 1.5. The rule this encodes is that a rig is admitted for the way
  // it will actually run, which is not the way the rows were measured -- alone on a quiet core.
  std::string json = test_table_json();
  const auto at = json.find("\"bar\"");
  REQUIRE(at != std::string::npos);
  json.insert(at, "\"coexistence\": {\"factors\": {\"1\": 1.25, \"2\": 1.5}, \"measured\": \"2026-10-09\"}, ");

  const CostTable table = CostTable::from_json(json, "/etc/namchain/cost.json");
  REQUIRE(table.ok);
  CHECK(table.coexistence_factor(0) == doctest::Approx(1.0));
  CHECK(table.coexistence_factor(1) == doctest::Approx(1.25));
  CHECK(table.coexistence_factor(2) == doctest::Approx(1.5));
  // Holding more than anyone measured is charged the worst measured case, never less.
  CHECK(table.coexistence_factor(3) == doctest::Approx(1.5));

  UnitCache units;
  Document alone = document_of({amp("amp1", example("A2.nam")), cab("cab1", generated("CabIR.nam")), gain("g1")});
  const CostOutcome unwarmed = apply_cost_table(alone, table, units, 48000.0, 64, 0);
  CHECK(unwarmed.coexistence == doctest::Approx(1.0));
  CHECK(unwarmed.effective == doctest::Approx(unwarmed.cost));
  const double raw = unwarmed.cost;

  // The same rig with one warm document beside it: its cost is the same, what it is charged is not.
  Document warmed = document_of({amp("amp1", example("A2.nam")), cab("cab1", generated("CabIR.nam")), gain("g1")});
  const CostOutcome one = apply_cost_table(warmed, table, units, 48000.0, 64, 1);
  CHECK(one.coexistence == doctest::Approx(1.25));
  CHECK(one.warm_documents == 1);
  CHECK(one.effective == doctest::Approx(one.cost * 1.25));
  CHECK(one.over == (one.effective > table.headroom));
  // Trimming is against the charged budget, so a rig that fits alone is either trimmed further or
  // refused -- never admitted at the cost it would have had alone.
  if (!one.trimmed)
    CHECK(one.cost == doctest::Approx(raw));
  else
    CHECK(one.cost < raw);

  // A rig of three models with no Lite row to fall back on is over when charged, and says so.
  Document heavy = document_of({amp("amp1", generated("A2-Full.nam")), amp("amp2", generated("A2-Full.nam")),
                               amp("amp3", generated("A2-Full.nam"))});
  const CostOutcome two = apply_cost_table(heavy, table, units, 48000.0, 64, 2);
  CHECK(two.coexistence == doctest::Approx(1.5));
  CHECK(two.effective == doctest::Approx(two.cost * 1.5));
  CHECK(two.over);
  CHECK(two.verdict == "over");
}

TEST_CASE("cost table: lite_first trims everything slimmable, and still refuses what stays over")
{
  // P8's first step: a rig that can start playing now. A Lite compiles in about a third of the time,
  // and E2's listening verdict is that a Lite standing in for a Full is not audible on the models it
  // tested -- so the fast document is the one published first, with the Full compiled behind it.
  // This is where that trimming happens: the same loop, with nothing left to reach.
  const CostTable table = test_table();
  UnitCache units;
  Document as_written = document_of({amp("amp1", example("A2.nam")), cab("cab1", generated("CabIR.nam")),
                                     gain("g1"), gain("g2"), gain("g3")});
  const CostOutcome normal = apply_cost_table(as_written, table, units, 48000.0, 64, 0, false);
  CHECK(normal.cost == doctest::Approx(0.65));   // 0.4 + 0.2 + three gains + in/out
  CHECK_FALSE(normal.trimmed);
  CHECK_FALSE(normal.lite_first);
  CHECK_FALSE(slimmed(as_written, "amp1"));

  Document fast = document_of({amp("amp1", example("A2.nam")), cab("cab1", generated("CabIR.nam")),
                               gain("g1"), gain("g2"), gain("g3")});
  const CostOutcome lite = apply_cost_table(fast, table, units, 48000.0, 64, 0, true);
  CHECK(lite.lite_first);
  CHECK(lite.trimmed);
  CHECK(lite.cost < normal.cost);
  CHECK(slimmed(fast, "amp1"));                  // the one slimmable model
  CHECK_FALSE(lite.over);                        // lighter than what it was, so certainly admitted

  // A rig with nothing to trim is what it is, and one that is over stays over: the fast path is a
  // compile-time trick, not an exemption from D3.
  Document nothing = document_of({amp("amp1", generated("A2-Full.nam")), amp("amp2", generated("A2-Full.nam")),
                                  amp("amp3", generated("A2-Full.nam"))});
  const CostOutcome stuck = apply_cost_table(nothing, table, units, 48000.0, 64, 0, true);
  CHECK(stuck.lite_first);
  CHECK_FALSE(stuck.trimmed);
  CHECK(stuck.over);
  CHECK(stuck.verdict == "over");
}

TEST_CASE("cost table: a table with no coexistence charge admits exactly as it did before")
{
  // Every table measured before 2026-10-09, and every device that has not run T5.
  Document document = document_of({amp("amp1", example("A2.nam")), cab("cab1", generated("CabIR.nam")), gain("g1")});
  const CostTable table = test_table();
  UnitCache units;
  const CostOutcome outcome = apply_cost_table(document, table, units, 48000.0, 64, 2);
  CHECK(outcome.coexistence == doctest::Approx(1.0));
  CHECK(outcome.effective == doctest::Approx(outcome.cost));
  CHECK_FALSE(outcome.over);
  CHECK(outcome.verdict != "over");
}

TEST_CASE("cost table: over the headroom with nothing to trim is over, and the verdict says so")
{
  // T4's second rig: two models that are already their own only size, 2 x 0.4 + 0.01 + in + out = 0.83.
  Document document = document_of({amp("amp1", generated("A2-Full.nam")), amp("amp2", generated("A2-Full.nam")),
                                   gain("g1")});
  const CostTable table = test_table();
  UnitCache units;
  const CostOutcome outcome = apply_cost_table(document, table, units, 48000.0, 64);

  CHECK(outcome.cost == doctest::Approx(0.83));
  CHECK(outcome.over);
  CHECK_FALSE(outcome.trimmed);
  CHECK(outcome.verdict == "over");
  // A2-Full.nam is a bare submodel, so there is no smaller one to select: nothing is trimmed for show.
  CHECK_FALSE(slimmed(document, "amp1"));
  CHECK_FALSE(slimmed(document, "amp2"));
}

TEST_CASE("cost table: a rig over the headroom is trimmed to Lite, dearest first, as little as it takes")
{
  // T4's third rig: 2 x 0.4 + 0.2 + in + out = 1.02. One model trimmed brings it to 0.72, so only one is.
  Document document = document_of({amp("amp1", example("A2.nam")), amp("amp2", example("A2.nam")),
                                   cab("cab1", generated("CabIR.nam"))});
  const CostTable table = test_table();
  UnitCache units;
  const CostOutcome outcome = apply_cost_table(document, table, units, 48000.0, 64);

  CHECK(outcome.full_cost == doctest::Approx(1.02));
  CHECK(outcome.cost == doctest::Approx(0.72));
  CHECK_FALSE(outcome.over);
  CHECK(outcome.trimmed);
  CHECK(outcome.verdict == "trimmed");
  CHECK(slimmed(document, "amp1") != slimmed(document, "amp2")); // exactly one of the two
  // Both are equally dear, so the tie goes to the later-declared node: that is what the model budget
  // did, and it leaves the first amp of a chain as the rig wrote it.
  CHECK(slimmed(document, "amp2"));
  CHECK(cost_of(outcome, "cab1") == doctest::Approx(0.2));       // the cab is never trimmed

  // The row a trimmed node is reported under is the Lite one: the status and the issue both name what
  // is playing, because a Lite and a Full are two separately trained models (D6).
  for (const namchain::daemon::CostedNode& node : outcome.nodes)
  {
    if (node.slimmed)
      CHECK(node.row == "NAM.WaveNet.lite");
  }
}

TEST_CASE("cost table: an unmeasured unit costs unknown, and a rig of them is refused")
{
  Document document = document_of({amp("amp1", example("lstm.nam")), amp("amp2", example("lstm.nam")),
                                   amp("amp3", example("lstm.nam")), amp("amp4", example("lstm.nam"))});
  const CostTable table = test_table();
  UnitCache units;
  const CostOutcome outcome = apply_cost_table(document, table, units, 48000.0, 64);

  // Four unmeasured models at D8's 0.25: over the headroom, and not a measured row in sight.
  CHECK(outcome.cost == doctest::Approx(1.02));
  CHECK(outcome.over);
  for (const namchain::daemon::CostedNode& node : outcome.nodes)
  {
    if (node.row == "unknown.NAM")
      CHECK_FALSE(node.measured);
  }
}

TEST_CASE("cost table: one file is read once, however many nodes name it")
{
  Document document = document_of({amp("amp1", example("A2.nam")), amp("amp2", example("A2.nam")),
                                   amp("amp3", example("A2.nam"))});
  const CostTable table = test_table();
  UnitCache units;
  const CostOutcome outcome = apply_cost_table(document, table, units, 48000.0, 64);
  CHECK(outcome.nodes.size() == 5); // in, three amps, out: every node is charged for
  CHECK(units.reads() == 1);

  // And a second publish reuses it: the same files, the same answers, no reparse.
  Document again = document_of({amp("amp1", example("A2.nam"))});
  apply_cost_table(again, table, units, 48000.0, 64);
  CHECK(units.reads() == 1);
}

TEST_CASE("cost table: the headroom is the table's, not a constant")
{
  Document document = document_of({amp("amp1", example("A2.nam")), amp("amp2", example("A2.nam"))});
  UnitCache units;
  const CostOutcome with_half = apply_cost_table(document, test_table(0.5), units, 48000.0, 64);
  CHECK(with_half.headroom == doctest::Approx(0.5));
  CHECK(with_half.full_cost == doctest::Approx(0.82));
  // 0.82 > 0.5, so one model goes Lite (0.4 -> 0.1) and then the other does too: 0.1 + 0.1 + 0.02.
  CHECK(with_half.trimmed);
  CHECK(with_half.cost == doctest::Approx(0.22));
  CHECK_FALSE(with_half.over);

  // With nothing above it, a rig is admitted as written -- which is what D3's 0.8 is: a policy the
  // table states, applied by the engine.
  Document same = document_of({amp("amp1", example("A2.nam")), amp("amp2", example("A2.nam"))});
  UnitCache fresh;
  const CostOutcome with_one = apply_cost_table(same, test_table(1.0), fresh, 48000.0, 64);
  CHECK(with_one.cost == doctest::Approx(0.82));
  CHECK_FALSE(with_one.over);
  CHECK_FALSE(with_one.trimmed);
}

TEST_CASE("cost table: an IR's own length decides its row, not its name")
{
  const TempDir dir;
  const std::string long_ir = linear_nam(dir, "long.nam", 3000);
  const std::string short_ir = linear_nam(dir, "short.nam", 1024);
  Document document = document_of({cab("cab1", long_ir), cab("cab2", short_ir)});
  const CostTable table = test_table();
  UnitCache units;
  const CostOutcome outcome = apply_cost_table(document, table, units, 48000.0, 64);

  // 3000 taps is between rows, so it pays the next row up; 1024 taps is below the shortest row, so it
  // pays the shortest -- both are the same row here, 0.05 against 0.2.
  CHECK(cost_of(outcome, "cab1") == doctest::Approx(0.05));
  CHECK(cost_of(outcome, "cab2") == doctest::Approx(0.2));
  CHECK(outcome.cost == doctest::Approx(0.27));
}

TEST_CASE("the warm failsafe reads a window of the render load, not the whole run")
{
  // D3's failsafe (2026-10-09): the coexistence charge is a measurement, and this is what notices
  // when it was wrong. `before` and `now` are two readings of the load histogram -- one bucket per
  // percent of the deadline, the last one "over it" -- and only the window between them counts, so a
  // spike an hour ago cannot keep the warm set cooled now.
  std::vector<uint64_t> before(namchain::daemon::kLoadBuckets, 0), now(namchain::daemon::kLoadBuckets, 0);
  // A comfortable window: everything around a third of the deadline, as T5 measured for a light rig.
  before[30] = 100;
  now[30] = 200;
  now[32] = 10;
  CHECK_FALSE(namchain::daemon::warm_pressure(before.data(), now.data()));

  // The same window, with one block over the deadline: D7's bar is zero overruns, so one is a fault.
  now[namchain::daemon::kLoadBuckets - 1] = 1;
  CHECK(namchain::daemon::warm_pressure(before.data(), now.data()));
  now[namchain::daemon::kLoadBuckets - 1] = 0;

  // A growing tail: a hundredth of the window past nine tenths of the deadline, which a rig with room
  // does not do. One block in a hundred is the threshold.
  before.assign(namchain::daemon::kLoadBuckets, 0);
  now.assign(namchain::daemon::kLoadBuckets, 0);
  before[30] = 1000;
  now[30] = 2000;   // 1000 blocks in the window at a comfortable 30%
  now[93] = 11;     // 11 more past 90%: 11 of 1011, just over one percent
  CHECK(namchain::daemon::warm_pressure(before.data(), now.data()));
  now[93] = 10;     // 10 of 1010: just under, and not yet pressure
  CHECK_FALSE(namchain::daemon::warm_pressure(before.data(), now.data()));

  // No blocks at all in the window is not pressure: a stopped engine must not cool anything.
  std::vector<uint64_t> same(namchain::daemon::kLoadBuckets, 5);
  CHECK_FALSE(namchain::daemon::warm_pressure(same.data(), same.data()));
}
