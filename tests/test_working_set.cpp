#include <doctest.h>

#include <string>

#include "namchain/working_set.h"

namespace
{

std::string generated(const char* name)
{
  return std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
}

std::string example(const char* name)
{
  return std::string(NAMCHAIN_EXAMPLE_MODELS_DIR) + "/" + name;
}

} // namespace

// ---------------------------------------------------------------------------
// Golden arithmetic at q64, from the formula in working_set.h.
//
// A2-Full: one layer array, 8 channels, 23 layers whose dilation * (kernel - 1) add up to 6331
// (test_lookback.cpp), and a kernel-16 head across the bottleneck of 8.
//   weights   12,146 floats                         =  48,584
//   layers    8 ch * (6331 + 23 * 64) frames * 4 B  = 249,696
//   head      8 ch * (15 + 64) frames * 4 B         =   2,528
//                                                     300,808
// A2-Lite is the same shape at 3 channels:
//   7,484 + 3 * 7803 * 4 + 3 * 79 * 4 = 7,484 + 93,636 + 948 = 102,068
// ---------------------------------------------------------------------------
TEST_CASE("working set: A2-Full and A2-Lite match the hand-computed bytes")
{
  const auto full = namchain::working_set_from_file(generated("A2-Full.nam"), 64);
  REQUIRE_MESSAGE(full.ok, full.detail);
  CHECK(full.detail == "WaveNet");
  CHECK(full.full_bytes == 300808);
  CHECK(full.slim_bytes == 300808);
  CHECK_FALSE(full.slimmable);

  const auto lite = namchain::working_set_from_file(generated("A2-Lite.nam"), 64);
  REQUIRE_MESSAGE(lite.ok, lite.detail);
  CHECK(lite.full_bytes == 102068);
  CHECK(lite.slim_bytes == 102068);
  CHECK_FALSE(lite.slimmable);
}

TEST_CASE("working set: the A2 container loads Full and slims to Lite")
{
  const auto a2 = namchain::working_set_from_file(example("A2.nam"), 64);
  REQUIRE_MESSAGE(a2.ok, a2.detail);
  CHECK(a2.detail == "SlimmableContainer");
  CHECK(a2.full_bytes == 300808);
  CHECK(a2.slim_bytes == 102068);
  CHECK(a2.slimmable);
}

TEST_CASE("working set: the block size counts once per channel of every convolution")
{
  // A2-Full reads 8 channels across 23 layers and the head's 8: 192 columns of block. Doubling the
  // block adds 64 frames to each.
  const auto q64 = namchain::working_set_from_file(generated("A2-Full.nam"), 64);
  const auto q128 = namchain::working_set_from_file(generated("A2-Full.nam"), 128);
  REQUIRE(q64.ok);
  REQUIRE(q128.ok);
  CHECK(q128.full_bytes - q64.full_bytes == (23 + 1) * 8 * 64 * 4);
}

TEST_CASE("working set: the small example models")
{
  // wavenet.nam: 131 weights; layer array 1 is 3 channels, kernel 3, dilations {1, 2}, legacy
  // head_size; layer array 2 is 2 channels, kernel 3, dilation 8, legacy head_size.
  //   524 + 3*(2+64)*4 + 3*(4+64)*4 + 3*64*4 + 2*(16+64)*4 + 2*64*4
  //   = 524 + 792 + 816 + 768 + 640 + 512 = 4,052
  const auto wn = namchain::working_set_from_file(example("wavenet.nam"), 64);
  REQUIRE_MESSAGE(wn.ok, wn.detail);
  CHECK(wn.full_bytes == 4052);
  CHECK_FALSE(wn.slimmable);

  // lstm.nam: weights plus per layer six vectors of hidden_size.
  const auto lstm = namchain::working_set_from_file(example("lstm.nam"), 64);
  REQUIRE_MESSAGE(lstm.ok, lstm.detail);
  CHECK(lstm.detail == "LSTM");
  CHECK(lstm.full_bytes == 352);

  // slimmable_container.nam: LSTM (max 0.33), WaveNet (0.66), WaveNet (1.0). It loads the last and
  // SetSlimmableSize(0.0) picks the first.
  const auto sc = namchain::working_set_from_file(example("slimmable_container.nam"), 64);
  REQUIRE_MESSAGE(sc.ok, sc.detail);
  CHECK(sc.slimmable);
  CHECK(sc.slim_bytes == 352);
  CHECK(sc.full_bytes > sc.slim_bytes);
}

TEST_CASE("working set: every vendor example model is estimated")
{
  for (const char* name : {"A2.nam", "lstm.nam", "my_model.nam", "slimmable_container.nam",
                           "slimmable_wavenet.nam", "wavenet.nam", "wavenet_a1_standard.nam",
                           "wavenet_a2_feature_test.nam", "wavenet_condition_dsp.nam"})
  {
    const auto ws = namchain::working_set_from_file(example(name), 64);
    CHECK_MESSAGE(ws.ok, name, ": ", ws.detail);
    CHECK_MESSAGE(ws.full_bytes > 0, name);
    CHECK_MESSAGE(ws.slim_bytes <= ws.full_bytes, name);
  }
}

TEST_CASE("working set: an IR as a Linear model counts its taps twice, as weights and as history")
{
  const auto ir = namchain::working_set_from_file(generated("CabIR.nam"), 64);
  REQUIRE_MESSAGE(ir.ok, ir.detail);
  CHECK(ir.detail == "Linear");
  CHECK_FALSE(ir.slimmable);
  CHECK(ir.full_bytes > 0);
}

TEST_CASE("working set: what it can't read comes back not ok, with a reason")
{
  using namchain::working_set_from_json;

  CHECK_FALSE(working_set_from_json("{", 64).ok);
  CHECK(working_set_from_json("{", 64).detail.rfind("JSON parse error", 0) == 0);
  CHECK_FALSE(working_set_from_json("[]", 64).ok);
  CHECK_FALSE(working_set_from_json(R"({"config": {}})", 64).ok);

  const auto unknown = working_set_from_json(R"({"architecture": "Transformer", "config": {}})", 64);
  CHECK_FALSE(unknown.ok);
  CHECK(unknown.detail == "unknown architecture 'Transformer'");

  // A layer array with no channels, and one with a kernel size and kernel sizes.
  CHECK_FALSE(working_set_from_json(
                R"({"architecture": "WaveNet", "config": {"layers": [{"dilations": [1], "kernel_size": 3,
                    "head_size": 1}]}, "weights": []})",
                64)
                .ok);
  CHECK_FALSE(working_set_from_json(
                R"({"architecture": "WaveNet", "config": {"layers": [{"channels": 2, "dilations": [1],
                    "kernel_size": 3, "kernel_sizes": [3], "head_size": 1}]}, "weights": []})",
                64)
                .ok);

  // A dilation that would overflow is refused, not wrapped.
  CHECK_FALSE(working_set_from_json(
                R"({"architecture": "WaveNet", "config": {"layers": [{"channels": 2,
                    "dilations": [4611686018427387904], "kernel_size": 3, "head_size": 1}]}, "weights": []})",
                64)
                .ok);

  // A container nested in a container, and a container submodel without a max_value.
  CHECK_FALSE(working_set_from_json(
                R"({"architecture": "SlimmableContainer", "config": {"submodels": [{"max_value": 1.0,
                    "model": {"architecture": "SlimmableContainer", "config": {}}}]}})",
                64)
                .ok);
  CHECK_FALSE(working_set_from_json(
                R"({"architecture": "SlimmableContainer", "config": {"submodels": [{"model": {}}]}})", 64)
                .ok);

  CHECK_FALSE(namchain::working_set_from_file(generated("no-such-model.nam"), 64).ok);
  CHECK_FALSE(namchain::working_set_from_file(generated("A2-Full.nam"), 0).ok);
}
