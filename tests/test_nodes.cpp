// Tests for the built-in node library (plan.md §6.2).
//
// Two kinds of claim are worth testing here. The first is arithmetic: a biquad at 0 dB must be
// transparent, a lowpass must pass DC and block Nyquist, a delay must delay by exactly the number
// of samples it says. The second is that the node is honest about what it cannot do -- a 30 kHz EQ
// at 48 kHz or a delay of half a sample is rejected with a reason rather than approximated.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include "namchain/graph/plan.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"
#include "namchain/wav.h"

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::graph::RenderPlan;

constexpr double kRate = 48000.0;
constexpr int kQuantum = 64;

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

/// input -> the one node under test -> output.
Graph one_node(NodeSpec middle)
{
  Graph graph;
  graph.nodes = {node("in", "nc.input"), std::move(middle), node("out", "nc.output")};
  graph.connections = {{"in", 0, "n", 0}, {"n", 0, "out", 0}};
  return graph;
}

std::unique_ptr<RenderPlan> compile_plan(const Graph& graph, namchain::rt::Arena& arena, int quantum = kQuantum,
                                         double rate = kRate)
{
  namchain::render::CompileOptions options;
  options.max_quantum = quantum;
  options.sample_rate = rate;
  std::string error;
  auto plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

/// Run `input` through a freshly compiled plan in quantum-sized blocks.
std::vector<float> render(const Graph& graph, const std::vector<float>& input, int quantum = kQuantum,
                          double rate = kRate)
{
  namchain::rt::Arena arena(1u << 22);
  auto plan = compile_plan(graph, arena, quantum, rate);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += static_cast<std::size_t>(quantum))
  {
    const int frames = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(quantum),
                                                              input.size() - offset));
    plan->execute(input.data() + offset, output.data() + offset, frames);
  }
  return output;
}

std::vector<float> impulse_response(const Graph& graph, std::size_t length, int quantum = kQuantum,
                                    double rate = kRate)
{
  std::vector<float> input(length, 0.0f);
  input[0] = 1.0f;
  return render(graph, input, quantum, rate);
}

std::vector<float> sine(double frequency, std::size_t length, float amplitude, double rate = kRate)
{
  std::vector<float> samples(length);
  for (std::size_t i = 0; i < length; ++i)
    samples[i] = amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * frequency *
                                                         static_cast<double>(i) / rate));
  return samples;
}

/// RMS over the second half, so the filter's transient is excluded.
double settled_rms(const std::vector<float>& samples)
{
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = samples.size() / 2; i < samples.size(); ++i)
  {
    sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    ++count;
  }
  return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
}

std::string temp_path(const std::string& tag)
{
  static int counter = 0;
  const auto dir = std::filesystem::temp_directory_path() / "namchain-node-tests";
  std::filesystem::create_directories(dir);
  return (dir / (tag + "-" + std::to_string(++counter) + ".wav")).string();
}

} // namespace

// ---------------------------------------------------------------------------
// nc.eq.biquad
// ---------------------------------------------------------------------------

TEST_CASE("a 0 dB peaking EQ is transparent")
{
  // Not "close to transparent": with A = 1 the numerator and denominator coefficients are the same
  // numbers, so the difference equation reduces to y = x and any deviation is a coding error rather
  // than a rounding artefact worth tolerating.
  NodeSpec eq = node("n", "nc.eq.biquad");
  eq.params["freq"] = 1000.0;
  eq.params["q"] = 1.0;
  eq.params["gain_db"] = 0.0;

  const std::vector<float> input = sine(440.0, 4096, 0.5f);
  const std::vector<float> output = render(one_node(eq), input);
  for (std::size_t i = 0; i < input.size(); ++i)
    CHECK(output[i] == doctest::Approx(input[i]).epsilon(1e-6));
}

TEST_CASE("a lowpass passes DC and blocks Nyquist")
{
  NodeSpec eq = node("n", "nc.eq.biquad");
  eq.strings["mode"] = "lowpass";
  eq.params["freq"] = 1000.0;
  eq.params["q"] = 0.707;
  const Graph graph = one_node(eq);

  // DC: the steady-state output must reach the input level.
  const std::vector<float> dc = render(graph, std::vector<float>(4800, 1.0f));
  CHECK(dc.back() == doctest::Approx(1.0f).epsilon(1e-3));

  // Nyquist: alternating +-1 sits at fs/2, 24x above the cutoff, so it must be pushed to nothing.
  std::vector<float> nyquist(4800);
  for (std::size_t i = 0; i < nyquist.size(); ++i)
    nyquist[i] = (i % 2 == 0) ? 1.0f : -1.0f;
  const std::vector<float> blocked = render(graph, nyquist);
  CHECK(std::abs(blocked.back()) < 1e-3f);
}

TEST_CASE("a peaking EQ boosts by the gain it was given at its centre frequency")
{
  const double boost_db = 12.0;
  NodeSpec eq = node("n", "nc.eq.biquad");
  eq.params["freq"] = 1000.0;
  eq.params["q"] = 1.0;
  eq.params["gain_db"] = boost_db;

  const std::vector<float> input = sine(1000.0, 16384, 0.1f);
  const std::vector<float> output = render(one_node(eq), input);

  const double expected = std::pow(10.0, boost_db / 20.0);
  const double measured = settled_rms(output) / settled_rms(input);
  CHECK(measured == doctest::Approx(expected).epsilon(0.05));
}

TEST_CASE("a shelf EQ changes the level at the end of the spectrum it names")
{
  // A high shelf cut must leave a low tone alone and attenuate a high one. Checking both directions
  // is what stops "the filter does nothing" from passing as a shelf.
  NodeSpec eq = node("n", "nc.eq.biquad");
  eq.strings["mode"] = "highshelf";
  eq.params["freq"] = 3000.0;
  eq.params["gain_db"] = -12.0;
  const Graph graph = one_node(eq);

  const std::vector<float> low = sine(200.0, 16384, 0.1f);
  const std::vector<float> high = sine(12000.0, 16384, 0.1f);
  const double low_ratio = settled_rms(render(graph, low)) / settled_rms(low);
  const double high_ratio = settled_rms(render(graph, high)) / settled_rms(high);

  CHECK(low_ratio == doctest::Approx(1.0).epsilon(0.05));
  CHECK(high_ratio == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.05));
  CHECK(high_ratio < low_ratio);
}

TEST_CASE("nc.eq.biquad rejects settings it cannot honour")
{
  namchain::rt::Arena arena(1u << 16);

  SUBCASE("frequency above Nyquist")
  {
    NodeSpec eq = node("n", "nc.eq.biquad");
    eq.params["freq"] = 30000.0; // > 24 kHz
    std::string error;
    CHECK(RenderPlan::compile(one_node(eq), kQuantum, arena, error) == nullptr);
    CHECK(error.find("freq") != std::string::npos);
  }

  SUBCASE("zero frequency")
  {
    NodeSpec eq = node("n", "nc.eq.biquad");
    eq.params["freq"] = 0.0;
    std::string error;
    CHECK(RenderPlan::compile(one_node(eq), kQuantum, arena, error) == nullptr);
  }

  SUBCASE("non-positive Q")
  {
    NodeSpec eq = node("n", "nc.eq.biquad");
    eq.params["q"] = 0.0;
    std::string error;
    CHECK(RenderPlan::compile(one_node(eq), kQuantum, arena, error) == nullptr);
    CHECK(error.find("q") != std::string::npos);
  }

  SUBCASE("unknown mode")
  {
    NodeSpec eq = node("n", "nc.eq.biquad");
    eq.strings["mode"] = "squiggle";
    std::string error;
    CHECK(RenderPlan::compile(one_node(eq), kQuantum, arena, error) == nullptr);
    CHECK(error.find("squiggle") != std::string::npos);
  }

  SUBCASE("the same settings at 96 kHz are in range")
  {
    // The guard is against the *rate*, not against a hardcoded 24 kHz ceiling.
    NodeSpec eq = node("n", "nc.eq.biquad");
    eq.params["freq"] = 30000.0;
    CHECK(compile_plan(one_node(eq), arena, kQuantum, 96000.0) != nullptr);
  }
}

// ---------------------------------------------------------------------------
// nc.delay
// ---------------------------------------------------------------------------

TEST_CASE("nc.delay delays by exactly the number of samples it was given")
{
  for (const int delay : {1, 5, 64, 65, 200})
  {
    NodeSpec spec = node("n", "nc.delay");
    spec.params["samples"] = static_cast<double>(delay);

    const std::vector<float> response = impulse_response(one_node(spec), 512);
    for (int i = 0; i < 512; ++i)
    {
      const float expected = i == delay ? 1.0f : 0.0f;
      CAPTURE(delay);
      CAPTURE(i);
      CHECK(response[static_cast<std::size_t>(i)] == doctest::Approx(expected));
    }
  }
}

TEST_CASE("nc.delay converts milliseconds at the engine rate")
{
  // 1 ms at 48 kHz is 48 samples; the same parameter at 96 kHz is 96. Getting this wrong by
  // rounding to whole milliseconds is a classic way for a delay to be silently wrong.
  NodeSpec spec = node("n", "nc.delay");
  spec.params["ms"] = 1.0;

  const std::vector<float> at_48k = impulse_response(one_node(spec), 256, kQuantum, 48000.0);
  CHECK(at_48k[48] == doctest::Approx(1.0f));
  CHECK(at_48k[47] == doctest::Approx(0.0f));

  const std::vector<float> at_96k = impulse_response(one_node(spec), 256, kQuantum, 96000.0);
  CHECK(at_96k[96] == doctest::Approx(1.0f));
  CHECK(at_96k[48] == doctest::Approx(0.0f));
}

TEST_CASE("nc.delay carries its ring across block boundaries")
{
  // The impulse starts near the end of the first block and lands in the second. A delay line that
  // resets its write pointer per block, or that indexes relative to the block, passes a
  // single-block test and fails this one.
  NodeSpec spec = node("n", "nc.delay");
  spec.params["samples"] = 40.0;

  std::vector<float> input(192, 0.0f);
  input[30] = 1.0f;
  const std::vector<float> output = render(one_node(spec), input);

  for (std::size_t i = 0; i < output.size(); ++i)
  {
    CAPTURE(i);
    CHECK(output[i] == doctest::Approx(i == 70 ? 1.0f : 0.0f));
  }
}

TEST_CASE("nc.delay rejects a delay it cannot mean")
{
  namchain::rt::Arena arena(1u << 16);

  SUBCASE("no time given")
  {
    std::string error;
    CHECK(RenderPlan::compile(one_node(node("n", "nc.delay")), kQuantum, arena, error) == nullptr);
    CHECK(error.find("samples") != std::string::npos);
  }

  SUBCASE("sub-sample delay")
  {
    NodeSpec spec = node("n", "nc.delay");
    spec.params["samples"] = 0.2; // rounds to 0
    std::string error;
    CHECK(RenderPlan::compile(one_node(spec), kQuantum, arena, error) == nullptr);
  }

  SUBCASE("a typo'd millisecond value")
  {
    NodeSpec spec = node("n", "nc.delay");
    spec.params["ms"] = 1.0e9;
    std::string error;
    CHECK(RenderPlan::compile(one_node(spec), kQuantum, arena, error) == nullptr);
    CHECK(error.find("nc.delay") != std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// nc.unit_delay — the feedback primitive of §5.1
// ---------------------------------------------------------------------------

TEST_CASE("nc.unit_delay delays by exactly one block")
{
  NodeSpec spec = node("n", "nc.unit_delay");
  const Graph graph = one_node(spec);

  namchain::rt::Arena arena(1u << 20);
  auto plan = compile_plan(graph, arena);

  std::vector<float> block(kQuantum, 1.0f);
  std::vector<float> output(kQuantum, -1.0f);

  plan->execute(block.data(), output.data(), kQuantum);
  for (const float value : output)
    CHECK(value == doctest::Approx(0.0f)); // the first block is the silence already in the ring

  plan->execute(block.data(), output.data(), kQuantum);
  for (const float value : output)
    CHECK(value == doctest::Approx(1.0f));

  plan->execute(block.data(), output.data(), kQuantum);
  for (const float value : output)
    CHECK(value == doctest::Approx(1.0f));
}

TEST_CASE("nc.unit_delay closes a feedback loop without a cycle error")
{
  // The whole reason the node exists: without something that breaks the cycle, this graph is a
  // compile error (§5.1). With it, the loop must converge -- y[n] = x[n] + 0.5*y[n-64] -- which
  // also proves the delay really is one block and not, say, one sample.
  Graph graph;
  NodeSpec input = node("in", "nc.input");
  NodeSpec mix = node("mix", "nc.mix");
  NodeSpec delay = node("d", "nc.unit_delay");
  NodeSpec gain = node("g", "nc.gain");
  gain.params["gain"] = 0.5;
  NodeSpec output_node = node("out", "nc.output");
  graph.nodes = {input, mix, delay, gain, output_node};
  graph.connections = {{"in", 0, "mix", 0},
                       {"mix", 0, "out", 0},
                       {"mix", 0, "d", 0},
                       {"d", 0, "g", 0},
                       {"g", 0, "mix", 1}};

  namchain::rt::Arena arena(1u << 20);
  auto plan = compile_plan(graph, arena);

  // One impulse in, then silence: the energy must decay by a factor of two every block, and never
  // come back louder than it went in.
  std::vector<float> block(kQuantum, 0.0f);
  std::vector<float> output(kQuantum, 0.0f);
  block[0] = 1.0f;
  plan->execute(block.data(), output.data(), kQuantum);
  CHECK(output[0] == doctest::Approx(1.0f));

  std::fill(block.begin(), block.end(), 0.0f);
  for (int pass = 1; pass <= 4; ++pass)
  {
    plan->execute(block.data(), output.data(), kQuantum);
    CHECK(output[0] == doctest::Approx(std::pow(0.5f, static_cast<float>(pass))));
  }
}

// ---------------------------------------------------------------------------
// nc.cab.ir
// ---------------------------------------------------------------------------

TEST_CASE("nc.cab.ir convolves with a WAV impulse response, first sample first")
{
  // A two-tap IR pins the tap ordering, which is the one thing a convolution node can silently get
  // backwards: the core's `Linear` stores the kernel reversed internally, so an off-by-a-reversal
  // here would show up as a time-reversed cab rather than as an error.
  const std::string path = temp_path("two-tap-ir");
  const std::vector<float> taps{1.0f, 0.5f};
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, taps.data(), taps.size(), 1, kRate, error));

  NodeSpec spec = node("n", "nc.cab.ir");
  spec.strings["ir"] = path;

  const std::vector<float> response = impulse_response(one_node(spec), 64);
  CHECK(response[0] == doctest::Approx(1.0f).epsilon(1e-5));
  CHECK(response[1] == doctest::Approx(0.5f).epsilon(1e-5));
  for (std::size_t i = 2; i < response.size(); ++i)
    CHECK(response[i] == doctest::Approx(0.0f).epsilon(1e-5));
}

namespace
{

/// y[n] = sum_k h[k] x[n-k], in double: the definition the partitioned convolver has to agree with.
std::vector<double> direct_convolution(const std::vector<float>& taps, const std::vector<float>& input)
{
  std::vector<double> output(input.size(), 0.0);
  for (std::size_t n = 0; n < input.size(); ++n)
    for (std::size_t k = 0; k < taps.size() && k <= n; ++k)
      output[n] += static_cast<double>(taps[k]) * static_cast<double>(input[n - k]);
  return output;
}

/// Worst difference from the direct convolution, in dB relative to the output's peak.
double error_db(const std::vector<float>& output, const std::vector<double>& reference)
{
  double worst = 0.0;
  double peak = 0.0;
  for (std::size_t i = 0; i < output.size(); ++i)
  {
    worst = std::max(worst, std::abs(static_cast<double>(output[i]) - reference[i]));
    peak = std::max(peak, std::abs(reference[i]));
  }
  REQUIRE(peak > 1e-3); // a comparison against silence would pass anything
  return worst == 0.0 ? -400.0 : 20.0 * std::log10(worst / peak);
}

/// A decaying, cabinet-like IR of `count` taps.
std::vector<float> synthetic_ir(std::size_t count)
{
  std::vector<float> taps(count);
  for (std::size_t n = 0; n < count; ++n)
  {
    const double t = static_cast<double>(n) / kRate;
    taps[n] = static_cast<float>(std::exp(-t * 30.0) * std::sin(2.0 * 3.14159265358979323846 * 110.0 * t) +
                                 (n == 0 ? 1.0 : 0.0));
  }
  return taps;
}

std::vector<float> noise(std::size_t length, float amplitude)
{
  std::vector<float> samples(length);
  unsigned state = 12345u;
  for (float& sample : samples)
  {
    state = state * 1664525u + 1013904223u;
    sample = amplitude * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
  }
  return samples;
}

} // namespace

TEST_CASE("nc.cab.ir agrees with direct convolution on both the direct and the FFT path")
{
  // The partitioned FFT path reorders the arithmetic, so it is not bit-identical to a direct FIR.
  // NAMBench measured it 136-138 dB below the signal against upstream's direct path; the bar here
  // is -110 dB relative to the output's peak, loose enough for float accumulation over 8192 taps
  // and tight enough that a misplaced partition or a dropped block fails by 60 dB or more.
  // 512 taps is the last length `Auto` runs directly, 513 the first on the FFT path.
  for (const std::size_t count : {std::size_t{2}, std::size_t{512}, std::size_t{513}, std::size_t{2048},
                                  std::size_t{8192}})
  {
    CAPTURE(count);
    const std::vector<float> taps = synthetic_ir(count);
    const std::string path = temp_path("ir-" + std::to_string(count));
    std::string error;
    REQUIRE(namchain::save_wav_float32(path, taps.data(), taps.size(), 1, kRate, error));
    NodeSpec spec = node("n", "nc.cab.ir");
    spec.strings["ir"] = path;

    const std::vector<float> input = noise(12000, 0.25f);
    const std::vector<double> reference = direct_convolution(taps, input);
    // 37 is not a divisor of anything: blocks that straddle the FFT block boundary.
    for (const int quantum : {32, 64, 37})
    {
      CAPTURE(quantum);
      CHECK(error_db(render(one_node(spec), input, quantum), reference) < -110.0);
    }
  }
}

TEST_CASE("nc.cab.ir reads the taps of a Linear .nam")
{
  const std::string path = std::string(NAMCHAIN_TEST_DATA_DIR) + "/CabIR.nam";
  NodeSpec spec = node("n", "nc.cab.ir");
  spec.strings["ir"] = path;

  // The core's own Linear, run directly, is the reference for what the container means.
  std::unique_ptr<nam::DSP> model = nam::get_dsp(std::filesystem::path(path));
  REQUIRE(model != nullptr);
  model->Reset(kRate, kQuantum);

  const std::vector<float> input = sine(220.0, 4096, 0.25f);
  const std::vector<float> output = render(one_node(spec), input);
  std::vector<double> reference(input.size(), 0.0);
  for (std::size_t offset = 0; offset < input.size(); offset += kQuantum)
  {
    std::vector<NAM_SAMPLE> block_in(kQuantum), block_out(kQuantum, 0.0);
    for (int i = 0; i < kQuantum; ++i)
      block_in[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(input[offset + static_cast<std::size_t>(i)]);
    NAM_SAMPLE* in_ptrs[] = {block_in.data()};
    NAM_SAMPLE* out_ptrs[] = {block_out.data()};
    model->process(in_ptrs, out_ptrs, kQuantum);
    for (int i = 0; i < kQuantum; ++i)
      reference[offset + static_cast<std::size_t>(i)] = static_cast<double>(block_out[static_cast<std::size_t>(i)]);
  }
  CHECK(error_db(output, reference) < -110.0);
}

TEST_CASE("nc.cab.ir truncates at 8192 taps, as the plugin does")
{
  // 24000 taps is a 500 ms IR at 48 kHz, which is what nearly every file in a real library is.
  std::vector<float> taps(24000, 0.0f);
  taps[0] = 1.0f;
  taps[8191] = 0.5f;  // the last tap kept
  taps[8192] = 0.25f; // the first tap cut
  const std::string path = temp_path("long-ir");
  std::string error;
  REQUIRE(namchain::save_wav_float32(path, taps.data(), taps.size(), 1, kRate, error));
  NodeSpec spec = node("n", "nc.cab.ir");
  spec.strings["ir"] = path;

  const std::vector<float> response = impulse_response(one_node(spec), 9000);
  CHECK(response[0] == doctest::Approx(1.0f).epsilon(1e-5));
  CHECK(response[8191] == doctest::Approx(0.5f).epsilon(1e-5));
  CHECK(std::abs(response[8192]) < 1e-5f);
}

TEST_CASE("nc.cab.ir resamples an IR recorded at another rate")
{
  // 44.1 and 96 kHz files are a third each of a real library. A cubic resample to the engine rate
  // keeps the cabinet's pitch; running the taps unconverted would shift every resonance by 9%.
  for (const double source_rate : {44100.0, 96000.0})
  {
    CAPTURE(source_rate);
    const std::size_t count = static_cast<std::size_t>(source_rate / 10.0); // 100 ms
    std::vector<float> taps(count);
    for (std::size_t n = 0; n < count; ++n)
      taps[n] = static_cast<float>(std::exp(-static_cast<double>(n) / source_rate * 40.0) *
                                   std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(n) /
                                            source_rate));
    const std::string path = temp_path("ir-" + std::to_string(static_cast<int>(source_rate)));
    std::string error;
    REQUIRE(namchain::save_wav_float32(path, taps.data(), taps.size(), 1, source_rate, error));
    NodeSpec spec = node("n", "nc.cab.ir");
    spec.strings["ir"] = path;

    // The ringing is at 1 kHz whatever the file's rate: find it from the zero crossings.
    const std::vector<float> response = impulse_response(one_node(spec), 4800);
    int crossings = 0;
    for (std::size_t i = 1; i < 2400; ++i)
      if ((response[i - 1] < 0.0f) != (response[i] < 0.0f))
        ++crossings;
    const double frequency = crossings / 2.0 / (2400.0 / kRate);
    CHECK(frequency == doctest::Approx(1000.0).epsilon(0.02));
  }
}

TEST_CASE("nc.cab.ir rejects an impulse response it cannot use")
{
  namchain::rt::Arena arena(1u << 20);

  SUBCASE("no ir named")
  {
    std::string error;
    CHECK(RenderPlan::compile(one_node(node("n", "nc.cab.ir")), kQuantum, arena, error) == nullptr);
    CHECK(error.find("ir") != std::string::npos);
  }

  SUBCASE("missing file")
  {
    NodeSpec spec = node("n", "nc.cab.ir");
    spec.strings["ir"] = "/nonexistent/cab.wav";
    std::string error;
    CHECK(RenderPlan::compile(one_node(spec), kQuantum, arena, error) == nullptr);
    CHECK(error.find("nc.cab.ir") != std::string::npos);
  }

  SUBCASE("a .nam that is not a Linear model")
  {
    NodeSpec spec = node("n", "nc.cab.ir");
    spec.strings["ir"] = std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Lite.nam";
    std::string error;
    CHECK(RenderPlan::compile(one_node(spec), kQuantum, arena, error) == nullptr);
    CHECK(error.find("not a Linear") != std::string::npos);
  }
}

TEST_CASE("nc.nam refuses a model trained for a different sample rate")
{
  // The core has no rate guard of its own: a 44.1 kHz model on a 48 kHz engine runs happily and is
  // simply wrong. The node is where that becomes an error.
  namchain::rt::Arena arena(1u << 22);

  NodeSpec spec = node("n", "nc.nam");
  spec.strings["model"] = std::string(NAMCHAIN_TEST_DATA_DIR) + "/A2-Lite.nam";

  namchain::render::CompileOptions options;
  options.max_quantum = kQuantum;
  options.sample_rate = 44100.0;
  std::string error;
  CHECK(RenderPlan::compile(one_node(spec), options, arena, error) == nullptr);
  CHECK(error.find("48000") != std::string::npos);
}
