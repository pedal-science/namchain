#include <doctest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "namchain/daemon/model_budget.h"

namespace
{

using namchain::daemon::apply_model_budget;
using namchain::daemon::BudgetOutcome;
using namchain::daemon::WorkingSetCache;
using namchain::graph::Chain;
using namchain::graph::Document;
using namchain::graph::NodeSpec;

// From test_working_set.cpp's golden arithmetic, at q64.
constexpr std::size_t kFull = 300808;
constexpr std::size_t kLite = 102068;
// pedal.project's figure for vip: one Full and one Lite fit, two Fulls do not.
constexpr std::size_t kVipBudget = 448 * 1024;

std::string generated(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

std::string example(const char* name)
{
  return std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/" + name;
}

NodeSpec amp(const std::string& id, const std::string& path)
{
  return NodeSpec{id, "nc.nam", {}, {{"model", path}}};
}

/// One chain, in -> each model in turn -> out. The connections do not matter to the budget, which
/// reads nodes, so there are none.
Chain chain_of(const std::string& id, const std::vector<std::string>& models)
{
  Chain chain;
  chain.id = id;
  chain.graph.nodes.push_back(NodeSpec{"in", "nc.input"});
  for (std::size_t i = 0; i < models.size(); ++i)
    chain.graph.nodes.push_back(amp("amp" + std::to_string(i + 1), models[i]));
  chain.graph.nodes.push_back(NodeSpec{"out", "nc.output"});
  return chain;
}

Document document_of(const std::vector<std::string>& models)
{
  Document document;
  document.chains.push_back(chain_of("main", models));
  return document;
}

const NodeSpec& node(const Document& document, std::size_t chain, const std::string& id)
{
  for (const NodeSpec& spec : document.chains[chain].graph.nodes)
  {
    if (spec.id == id)
      return spec;
  }
  FAIL("no node " << id);
  return document.chains[chain].graph.nodes.front();
}

struct TempDir
{
  TempDir()
  {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("namchain-budget-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
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

} // namespace

TEST_CASE("model budget: off is off")
{
  Document document = document_of({example("A2.nam"), example("A2.nam")});
  WorkingSetCache cache;
  const BudgetOutcome outcome = apply_model_budget(document, 0, 64, cache);
  CHECK(outcome.budget_bytes == 0);
  CHECK(outcome.models.empty());
  CHECK_FALSE(outcome.over);
  CHECK(cache.parses() == 0);
  CHECK_FALSE(node(document, 0, "amp2").slim);
}

TEST_CASE("model budget: of two A2 containers, the second is slimmed to fit")
{
  Document document = document_of({example("A2.nam"), example("A2.nam")});
  WorkingSetCache cache;
  const BudgetOutcome outcome = apply_model_budget(document, kVipBudget, 64, cache);
  REQUIRE(outcome.models.size() == 2);
  CHECK(outcome.full_bytes == 2 * kFull);
  CHECK(outcome.used_bytes == kFull + kLite);
  CHECK_FALSE(outcome.over);
  CHECK_FALSE(outcome.models[0].slimmed);
  CHECK(outcome.models[1].slimmed);
  CHECK(outcome.models[1].node == "amp2");
  CHECK(outcome.models[1].element == "amp2");
  CHECK(outcome.models[1].bytes() == kLite);
  CHECK_FALSE(node(document, 0, "amp1").slim);
  CHECK(node(document, 0, "amp2").slim);

  // One file, parsed once.
  CHECK(cache.parses() == 1);
}

TEST_CASE("model budget: a document that fits is left alone, and a stale choice is undone")
{
  Document document = document_of({example("A2.nam"), example("A2.nam")});
  // As if a previous publish had slimmed it: the budget decides afresh every time.
  document.chains[0].graph.nodes[2].slim = true;
  WorkingSetCache cache;
  const BudgetOutcome outcome = apply_model_budget(document, 2 * kFull, 64, cache);
  CHECK(outcome.used_bytes == 2 * kFull);
  CHECK_FALSE(outcome.over);
  CHECK_FALSE(node(document, 0, "amp1").slim);
  CHECK_FALSE(node(document, 0, "amp2").slim);
}

TEST_CASE("model budget: what cannot be slimmed is passed over, and what still does not fit is over")
{
  WorkingSetCache cache;

  // Two standalone Fulls: nothing to slim.
  Document fulls = document_of({generated("A2-Full.nam"), generated("A2-Full.nam")});
  const BudgetOutcome over = apply_model_budget(fulls, kVipBudget, 64, cache);
  CHECK(over.over);
  CHECK(over.used_bytes == 2 * kFull);
  CHECK_FALSE(over.models[0].slimmed);
  CHECK_FALSE(over.models[1].slimmed);
  CHECK_FALSE(over.models[1].slimmable);

  // A container before a standalone Full: the Full is last but cannot be slimmed, so the container is.
  Document mixed = document_of({example("A2.nam"), generated("A2-Full.nam")});
  const BudgetOutcome fits = apply_model_budget(mixed, kVipBudget, 64, cache);
  CHECK_FALSE(fits.over);
  CHECK(fits.used_bytes == kLite + kFull);
  CHECK(node(mixed, 0, "amp1").slim);
  CHECK_FALSE(node(mixed, 0, "amp2").slim);
}

TEST_CASE("model budget: slims from the last until it fits, and no further")
{
  // Three containers: the third slimmed is still over (Full + Full + Lite), the second makes it fit.
  Document three = document_of({example("A2.nam"), example("A2.nam"), example("A2.nam")});
  WorkingSetCache cache;
  const BudgetOutcome outcome = apply_model_budget(three, 2 * kFull, 64, cache);
  CHECK_FALSE(outcome.over);
  CHECK(outcome.used_bytes == kFull + 2 * kLite);
  CHECK_FALSE(node(three, 0, "amp1").slim);
  CHECK(node(three, 0, "amp2").slim);
  CHECK(node(three, 0, "amp3").slim);
}

TEST_CASE("model budget: counts the whole document, every chain, bypassed models too")
{
  // Each chain on its own fits; the document does not, because every chain renders on one thread.
  Document document;
  document.chains.push_back(chain_of("guitar", {example("A2.nam")}));
  document.chains.push_back(chain_of("bass", {example("A2.nam")}));
  // A bypassed model still renders, so it still counts.
  document.chains[1].graph.nodes[1].bypass = true;
  // Something that is not a model is not counted.
  document.chains[1].graph.nodes.push_back(NodeSpec{"g", "nc.gain", {{"gain", 0.5}}});

  WorkingSetCache cache;
  const BudgetOutcome outcome = apply_model_budget(document, kVipBudget, 64, cache);
  REQUIRE(outcome.models.size() == 2);
  CHECK(outcome.models[1].chain == "bass");
  CHECK(outcome.models[1].chain_index == 1);
  CHECK(outcome.models[1].node_index == 1);
  CHECK(outcome.models[1].slimmed);
  CHECK(outcome.used_bytes == kFull + kLite);
  CHECK(node(document, 1, "amp1").slim);
}

TEST_CASE("model budget: a model it cannot read counts as nothing, and says why")
{
  TempDir dir;
  const auto unknown = dir.path / "unknown.nam";
  {
    std::ofstream file(unknown);
    file << R"({"version": "0.5.4", "architecture": "Transformer", "config": {}, "weights": []})";
  }
  Document document = document_of({unknown.string(), example("A2.nam")});
  WorkingSetCache cache;
  const BudgetOutcome outcome = apply_model_budget(document, kVipBudget, 64, cache);
  REQUIRE(outcome.models.size() == 2);
  CHECK_FALSE(outcome.models[0].estimated);
  CHECK(outcome.models[0].why == "unknown architecture 'Transformer'");
  CHECK(outcome.models[0].bytes() == 0);
  CHECK(outcome.used_bytes == kFull);
  CHECK_FALSE(outcome.over);
}

TEST_CASE("model budget: the cache rereads a file that has changed")
{
  TempDir dir;
  const auto model = dir.path / "amp.nam";
  std::filesystem::copy_file(generated("A2-Lite.nam"), model);

  WorkingSetCache cache;
  CHECK(cache.get(model.string(), 64).full_bytes == kLite);
  CHECK(cache.get(model.string(), 64).full_bytes == kLite);
  CHECK(cache.parses() == 1);

  // Another block size is another answer.
  CHECK(cache.get(model.string(), 128).full_bytes > kLite);
  CHECK(cache.parses() == 2);

  // The same name, another model: a library entry replaced in place.
  std::filesystem::copy_file(generated("A2-Full.nam"), model, std::filesystem::copy_options::overwrite_existing);
  CHECK(cache.get(model.string(), 64).full_bytes == kFull);
  CHECK(cache.parses() == 3);
}

TEST_CASE("model budget: a chain is capped on its own, whatever the document is allowed")
{
  // A chain runs on one core, so its models have to fit one core's L2 even when the document as a
  // whole is allowed more than that (docs/daemon.md, "Render threads"). One Full and one Lite fit a
  // core; two Fulls do not.
  const auto two_chains = [] {
    Document document;
    document.chains.push_back(chain_of("a", {example("A2.nam"), example("A2.nam")}));
    document.chains.push_back(chain_of("b", {example("A2.nam")}));
    return document;
  };

  // With one ceiling for everything -- the single-core rule -- three Fulls inside 3 * kFull are left
  // exactly as written, because nothing is over the only budget there is.
  {
    Document document = two_chains();
    WorkingSetCache cache;
    const BudgetOutcome flat = apply_model_budget(document, 3 * kFull, 64, cache);
    CHECK_FALSE(flat.over);
    CHECK(flat.budget_bytes == flat.per_chain_bytes);
    CHECK_FALSE(node(document, 0, "amp1").slim);
    CHECK_FALSE(node(document, 0, "amp2").slim);
    CHECK_FALSE(node(document, 1, "amp1").slim);
  }

  // With one core's ceiling for a chain and two cores' worth for the document, chain "a" is over its
  // own ceiling and is slimmed until it fits -- and no further -- while the document stays inside its.
  {
    Document document = two_chains();
    WorkingSetCache cache;
    const BudgetOutcome outcome = apply_model_budget(document, kVipBudget, 2 * kVipBudget, 64, cache);
    CHECK_FALSE(outcome.over);
    CHECK(outcome.per_chain_bytes == kVipBudget);
    CHECK(outcome.budget_bytes == 2 * kVipBudget);
    CHECK(outcome.heaviest_chain == 0u);
    CHECK(outcome.heaviest_chain_bytes == kFull + kLite);
    // Last declared first within the chain that is over, and only that chain.
    CHECK_FALSE(node(document, 0, "amp1").slim);
    CHECK(node(document, 0, "amp2").slim);
    CHECK_FALSE(node(document, 1, "amp1").slim);
    CHECK(outcome.used_bytes == kFull + kLite + kFull);
  }

  // A single chain that cannot be brought under one core's ceiling is over, however much room the
  // document has: the second engine cannot lend its L2 to a chain it is not running.
  {
    Document document;
    document.chains.push_back(chain_of("a", {generated("A2-Full.nam"), generated("A2-Full.nam")}));
    WorkingSetCache cache;
    const BudgetOutcome outcome = apply_model_budget(document, kVipBudget, 8 * kVipBudget, 64, cache);
    CHECK(outcome.over);
    CHECK(outcome.heaviest_chain_bytes == 2 * kFull);
    CHECK(outcome.heaviest_chain == 0u);
  }
}
