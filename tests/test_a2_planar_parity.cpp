// Bit-identity gate for the planar NEON A2 kernels (vendor/patches/0001, see plan.md §7.8).
//
// The kernels claim something stronger than "close enough": for the A2 nano (3ch) and A2
// standard (8ch) shapes they produce the *same float32 bits* as the portable A2FastModel.
// This asserts exactly that with memcmp, against real .nam model data, across block sizes
// that exercise the partial-tile and single-frame tails.
//
// WHY THIS FILE IS NOT PART OF run_tests UPSTREAM
// ----------------------------------------------
// The property being asserted is an optimisation-level property. Where the planar kernels
// are active (NAM_A2_PLANAR), bit-identity rests on the compiler contracting `a*b+c` into an
// FMA inside the reference's own branches, because the planar kernel uses explicit
// vfmaq_f32 intrinsics. At -O0 no contraction happens, the reference computes different bits,
// and the comparison fails on the very first sample -- through no fault of either
// implementation. Upstream's copy of this test lives in tools/run_tests, which the core's
// CMakeLists deliberately compiles at -O0 (so its allocation tracking works), so upstream's
// copy cannot pass as wired. Verified on this host 2026-09-11: fails at -O0 at block size 1,
// passes all 14 block sizes when the same test is rebuilt at -O3.
//
// Hence the __OPTIMIZE__ guard below: in an unoptimised build we report a skip instead of a
// false failure, and CI is expected to run this at Release. Do not "fix" a failure here by
// relaxing the comparison to a tolerance without first checking the build type.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// These must be included BEFORE the guard below: a2_planar.h is what defines NAM_A2_PLANAR,
// and it only does so where the kernels exist (AArch64, or ARMv7 with NEON + FMA). Both
// headers are empty no-ops when NAM_ENABLE_A2_FAST is undefined.
#include "NAM/wavenet/a2_fast.h"
#include "NAM/wavenet/a2_planar.h"

#if defined(NAM_ENABLE_A2_FAST) && defined(NAM_A2_PLANAR)

  #include <json.hpp>

  #include "NAM/dsp.h"

namespace
{

std::string read_text(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open ", path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::vector<NAM_SAMPLE> render(nam::DSP& dsp, const std::vector<NAM_SAMPLE>& input, int block_size)
{
  dsp.Reset(48000.0, block_size); // also prewarms
  std::vector<NAM_SAMPLE> out(input.size(), static_cast<NAM_SAMPLE>(0));
  size_t pos = 0;
  while (pos < input.size())
  {
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block_size), input.size() - pos));
    const NAM_SAMPLE* in_ptr = input.data() + pos;
    NAM_SAMPLE* out_ptr = out.data() + pos;
    const NAM_SAMPLE* in_arr[] = {in_ptr};
    NAM_SAMPLE* out_arr[] = {out_ptr};
    dsp.process(const_cast<NAM_SAMPLE**>(in_arr), out_arr, n);
    pos += static_cast<size_t>(n);
  }
  return out;
}

} // namespace

TEST_CASE("planar A2 kernels are bit-identical to the reference A2FastModel")
{
#if !defined(NAMCHAIN_A2_PARITY_O3)
  // Bit-identity is specifically an -O3 property, not merely "an optimised build": at -O2 the
  // reference A2FastModel contracts its FMAs differently from the explicit-intrinsic planar
  // kernels, so the comparison fails without either side being wrong (measured on aarch64/GCC 13;
  // -O0 behaves the same way). CMake defines NAMCHAIN_A2_PARITY_O3 only when the release flags
  // really are -O3. CI sets NAMCHAIN_REQUIRE_A2_PARITY so a mis-configured build fails loudly
  // instead of quietly retiring the strongest correctness test we have.
  #if defined(NAMCHAIN_REQUIRE_A2_PARITY)
  FAIL("the A2 planar parity gate is being required on a build that is not -O3, where the "
       "comparison is not meaningful; configure CMAKE_BUILD_TYPE=Release (see this file's header)");
  #else
  MESSAGE("skipped: the planar bit-identity gate requires -O3; see this file's header");
  #endif
  return;
#else
  // 20000 frames is long enough that every layer's ring wraps and rewinds several times.
  std::vector<NAM_SAMPLE> input(20000);
  for (size_t i = 0; i < input.size(); ++i)
  {
    const double t = static_cast<double>(i) / 48000.0;
    const double pi = 3.14159265358979323846;
    input[i] = static_cast<NAM_SAMPLE>(0.25 * std::sin(2.0 * pi * 220.0 * t) + 0.10 * std::sin(2.0 * pi * 1230.0 * t));
  }

  for (const char* name : {"A2-Full.nam", "A2-Lite.nam"})
  {
    const std::string path = std::string(NAMCHAIN_TEST_DATA_DIR) + "/" + name;
    const auto doc = nlohmann::json::parse(read_text(path));
    const auto& config = doc.at("config");
    const auto weights = doc.at("weights").get<std::vector<float>>();

    int channels = 0;
    REQUIRE(nam::wavenet::a2_fast::is_a2_shape(config, &channels));
    REQUIRE((channels == 3 || channels == 8));

    // Block sizes below one vector, not a multiple of four, exactly one vector, between one
    // vector and one tile, the tile widths, and well past them.
    for (const int block_size : {1, 3, 4, 7, 8, 15, 16, 31, 32, 33, 64, 65, 128, 512})
    {
      auto reference = nam::wavenet::a2_fast::create_a2_fast_reference_model(channels, weights, 48000.0);
      auto planar = nam::wavenet::a2_fast::create_a2_planar_model(channels, weights, 48000.0);
      REQUIRE(reference != nullptr);
      REQUIRE(planar != nullptr);
      CHECK(reference->GetPrewarmSamples() == planar->GetPrewarmSamples());

      const auto expected = render(*reference, input, block_size);
      const auto actual = render(*planar, input, block_size);
      REQUIRE(expected.size() == actual.size());

      const bool identical = std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(NAM_SAMPLE)) == 0;
      if (!identical)
      {
        size_t first = 0;
        while (first < expected.size() && expected[first] == actual[first])
          ++first;
        MESSAGE("channels=", channels, " block=", block_size, " first difference at sample ", first);
      }
      CHECK(identical);
    }
  }
#endif
}

#endif // NAM_ENABLE_A2_FAST && NAM_A2_PLANAR
