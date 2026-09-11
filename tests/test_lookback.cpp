#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "namchain/lookback.h"

namespace
{

std::string read_file(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open ", path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// The number the plan claims for A2 (G7): lookback 6346, prewarm count 6347.
constexpr int kA2PrewarmSamples = 6347;

} // namespace

// ---------------------------------------------------------------------------
// Golden arithmetic: A2 receptive field, hand-computed from the documented formula.
//
//   23 layers, kernel sizes {6 x14, 15, 15, 6 x7}, dilations
//   {1,3,7,17,41,101,239, 1,3,7,17,41,101,239, 1,13, 1,3,7,17,41,101,239}
//   RF = sum(dilation_i * (kernel_i - 1)) + head_kernel(16) - 1
//      = 6331 + 15 = 6346
//   prewarm count = RF + 1 = 6347
// ---------------------------------------------------------------------------
TEST_CASE("A2 receptive field matches the hand-computed value")
{
  const auto lb = namchain::compute_lookback_from_file(std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Full.nam");
  REQUIRE(lb.ok());
  CHECK(lb.samples == kA2PrewarmSamples);

  const auto lite = namchain::compute_lookback_from_file(std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Lite.nam");
  REQUIRE(lite.ok());
  // A2-Full and A2-Lite share the architecture shape; only channels differ.
  CHECK(lite.samples == kA2PrewarmSamples);
}

TEST_CASE("lookback matches the core's own GetPrewarmSamples for plain A2 WaveNets")
{
  for (const char* name : {"A2-Full.nam", "A2-Lite.nam"})
  {
    const std::string path = std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
    const auto lb = namchain::compute_lookback_from_file(path);
    REQUIRE(lb.ok());

    auto dsp = nam::get_dsp(std::filesystem::path(path));
    REQUIRE(dsp != nullptr);
    CHECK(lb.samples == dsp->GetPrewarmSamples());
  }
}

TEST_CASE("SlimmableContainer reports the worst case over submodels, not the active one")
{
  const std::string path = std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/A2.nam";
  const auto lb = namchain::compute_lookback_from_file(path);
  REQUIRE(lb.ok());
  CHECK(lb.detail.find("SlimmableContainer") != std::string::npos);
  // Both submodels are A2-shaped, so the worst case equals the single-model number.
  CHECK(lb.samples == kA2PrewarmSamples);

  // The core returns the *active* submodel's value; it agrees here because both
  // submodels share the same receptive field. The distinction matters for containers
  // whose submodels differ, which is why the engine keeps its own calculator.
  auto dsp = nam::get_dsp(std::filesystem::path(path));
  REQUIRE(dsp != nullptr);
  CHECK(dsp->GetPrewarmSamples() == kA2PrewarmSamples);
}

TEST_CASE("SlimmableWaveNet is computed even though the core hardcodes zero")
{
  const std::string path = std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/slimmable_wavenet.nam";
  const auto lb = namchain::compute_lookback_from_file(path);

  auto dsp = nam::get_dsp(std::filesystem::path(path));
  REQUIRE(dsp != nullptr);

  // Documented divergence: NAM/wavenet/slimmable.h returns 0 from GetPrewarmSamples(),
  // which would leave warm-start replay with no idea how much history to replay.
  CHECK(dsp->GetPrewarmSamples() == 0);
  REQUIRE(lb.ok());
  CHECK(lb.samples > 0);
}

TEST_CASE("LSTM is unbounded, not silently approximate")
{
  const std::string path = std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/lstm.nam";
  const auto lb = namchain::compute_lookback_from_file(path);
  CHECK(lb.status == namchain::LookbackStatus::Unbounded);
  CHECK_FALSE(lb.ok());
}

TEST_CASE("condition_dsp is recursed into")
{
  const std::string path = std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/wavenet_condition_dsp.nam";
  const auto lb = namchain::compute_lookback_from_file(path);

  auto dsp = nam::get_dsp(std::filesystem::path(path));
  REQUIRE(dsp != nullptr);

  REQUIRE(lb.ok());
  CHECK(lb.samples == dsp->GetPrewarmSamples());
}

TEST_CASE("malformed input is reported, never thrown")
{
  using namchain::LookbackStatus;

  CHECK(namchain::compute_lookback_from_json("").status == LookbackStatus::Malformed);
  CHECK(namchain::compute_lookback_from_json("{").status == LookbackStatus::Malformed);
  CHECK(namchain::compute_lookback_from_json("[]").status == LookbackStatus::Malformed);
  CHECK(namchain::compute_lookback_from_json(R"({"architecture":"WaveNet"})").status == LookbackStatus::Malformed);
  CHECK(namchain::compute_lookback_from_json(R"({"architecture":"WaveNet","config":{"layers":[]}})")
            .status == LookbackStatus::Malformed);
  CHECK(namchain::compute_lookback_from_json(R"({"architecture":"NoSuchArch","config":{}})")
            .status == LookbackStatus::UnknownArchitecture);
  // kernel/dilation length mismatch must not be trusted
  CHECK(namchain::compute_lookback_from_json(
            R"({"architecture":"WaveNet","config":{"layers":[{"kernel_sizes":[6,6],"dilations":[1]}]}})")
            .status == LookbackStatus::Malformed);
  CHECK(namchain::compute_lookback_from_file("/nonexistent/model.nam").status == LookbackStatus::Malformed);
}

TEST_CASE("a hand-built WaveNet computes the documented sum")
{
  // One layer array: 3 layers with k=2, d=1,2,4 => 1+2+4 = 7; head k=3 => +2; base 1.
  const std::string doc = R"({
    "architecture": "WaveNet",
    "config": {
      "layers": [{
        "kernel_sizes": [2, 2, 2],
        "dilations": [1, 2, 4],
        "head": {"kernel_size": 3}
      }]
    }
  })";
  const auto lb = namchain::compute_lookback_from_json(doc);
  REQUIRE(lb.ok());
  CHECK(lb.samples == 1 + 7 + 2);
}

TEST_CASE("legacy scalar kernel_size is replicated across layers")
{
  // model.cpp:1030 -- legacy .nam uses one kernel_size for every dilated layer.
  // sum(dilation * (kernel-1)) = (1+2+4) * (4-1) = 21; legacy head_size implies kernel 1
  // => +0; base 1 => 22.
  const std::string doc = R"({
    "architecture": "WaveNet",
    "config": {
      "layers": [{
        "kernel_size": 4,
        "dilations": [1, 2, 4],
        "head_size": 8,
        "head_bias": true
      }]
    }
  })";
  const auto lb = namchain::compute_lookback_from_json(doc);
  REQUIRE(lb.ok());
  CHECK(lb.samples == 1 + 21);

  // Supplying both forms is an error, as in the core.
  const std::string both = R"({
    "architecture": "WaveNet",
    "config": {
      "layers": [{
        "kernel_size": 4,
        "kernel_sizes": [4, 4, 4],
        "dilations": [1, 2, 4],
        "head_size": 8,
        "head_bias": true
      }]
    }
  })";
  CHECK(namchain::compute_lookback_from_json(both).status == namchain::LookbackStatus::Malformed);
}

TEST_CASE("IR (Linear) models get a lookback even though the core reports zero")
{
  const std::string path = std::string(NAMCHAIN_TEST_DATA_DIR) + "/CabIR.nam";
  const auto lb = namchain::compute_lookback_from_file(path);
  REQUIRE(lb.ok());
  CHECK(lb.samples == 2048); // = config.receptive_field

  auto dsp = nam::get_dsp(std::filesystem::path(path));
  REQUIRE(dsp != nullptr);
  CHECK(dsp->GetPrewarmSamples() == 0); // documented core gap: FIR history would be skipped
}

TEST_CASE("a container is unbounded if any submodel is")
{
  // slimmable_container.nam mixes LSTM and WaveNet submodels, so selecting the LSTM
  // makes exact state replay impossible. The calculator must not average that away.
  const std::string path = std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/slimmable_container.nam";
  const auto lb = namchain::compute_lookback_from_file(path);
  CHECK(lb.status == namchain::LookbackStatus::Unbounded);
}
