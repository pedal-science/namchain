// Tests for nc.eq.flow: flowEQ's decoder and its five-band EQ, as the engine runs them.
//
// The claims, in the order they matter:
//
//   * the decoder in the engine *is* flowEQ's trained model -- checked against the forward pass in
//     double precision that tools/floweq/generate_model.py prints (`--golden`), so a transposed
//     weight matrix, a wrong normalisation or a stale generated header cannot pass;
//   * the five sections are the plugin's own design -- checked against the plugin's formulas
//     evaluated independently, at four frequencies, to a hundredth of a dB;
//   * strength scales every gain and nothing else, so strength 0 is the identity filter exactly;
//   * a latent change ramps to the filter a fresh build would have had, rather than stepping;
//   * Auto Gain hands back the loudness the filters took away;
//   * the report a client can read (`rig.state`) describes what the render thread is running.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <vector>

#include <json.hpp>

#include "namchain/floweq.h"
#include "namchain/graph/plan.h"
#include "namchain/rt_check.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"

namespace
{

using namchain::floweq::kParamCount;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::render::CompileOptions;
using namchain::graph::RenderPlan;

constexpr double kRate = 48000.0;
constexpr int kQuantum = 64;
constexpr double kPi = 3.14159265358979323846;

NodeSpec flow_node()
{
  NodeSpec spec;
  spec.id = "n";
  spec.type = "nc.eq.flow";
  return spec;
}

Graph one_node(NodeSpec middle)
{
  Graph graph;
  graph.nodes = {NodeSpec{"in", "nc.input"}, std::move(middle), NodeSpec{"out", "nc.output"}};
  graph.connections = {{"in", 0, "n", 0}, {"n", 0, "out", 0}};
  return graph;
}

std::unique_ptr<RenderPlan> compile_plan(const Graph& graph, namchain::rt::Arena& arena, int quantum = kQuantum,
                                         double rate = kRate)
{
  CompileOptions options;
  options.max_quantum = quantum;
  options.sample_rate = rate;
  std::string error;
  auto plan = RenderPlan::compile(graph, options, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

std::vector<float> render(const Graph& graph, const std::vector<float>& input, int quantum = kQuantum)
{
  namchain::rt::Arena arena(1u << 22);
  auto plan = compile_plan(graph, arena, quantum);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += static_cast<std::size_t>(quantum))
  {
    const int frames = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(quantum), input.size() - offset));
    plan->execute(input.data() + offset, output.data() + offset, frames);
  }
  return output;
}

std::vector<float> sine(double frequency, std::size_t length, float amplitude = 0.25f, double rate = kRate)
{
  std::vector<float> samples(length);
  for (std::size_t i = 0; i < length; ++i)
    samples[i] = amplitude * static_cast<float>(std::sin(2.0 * kPi * frequency * static_cast<double>(i) / rate));
  return samples;
}

double rms(const std::vector<float>& samples, std::size_t from, std::size_t to)
{
  double sum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = from; i < std::min(to, samples.size()); ++i)
  {
    sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    ++count;
  }
  return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
}

/// The node's state as the plan holds it, for the `describe_state` tests.
void* state_of(RenderPlan& plan)
{
  const int index = plan.index_of("n");
  REQUIRE(index >= 0);
  return plan.nodes()[static_cast<std::size_t>(index)].state;
}

/// The decoder's output for a latent point, from tools/floweq/generate_model.py --golden (a double
/// precision forward pass over the same .mat flowEQ's MATLAB Decoder.m loads). Kept as literals
/// rather than recomputed here: recomputing them with the engine's own decoder would compare the
/// decoder with itself.
struct Golden
{
  float latent[3];
  double params[kParamCount];
};

const Golden kGoldens[] = {
  {{0.0f, 0.0f, 0.0f},
   {-0.891720, 149.445212, 1.569535, 424.115355, 0.735716, 2.348638, 1048.832944, 0.703947, 0.741894,
    3317.018503, 0.718770, 0.082720, 8230.599228}},
  {{1.0f, 0.0f, 0.0f},
   {-0.508041, 154.257409, -0.955981, 386.590952, 0.702983, 3.120252, 833.753924, 0.663316, 1.139568,
    3083.763466, 0.660900, -6.484797, 8535.484403}},
  {{-2.0f, -2.0f, -2.0f},
   {11.999999, 98.242990, 11.999981, 590.187313, 0.786999, 10.788155, 2651.982873, 0.654894, 8.767696,
    4500.746332, 0.826668, 9.392640, 6871.881113}},
};

/// The plugin's own response, evaluated in double precision with the formulas from flowEQ_lite.m
/// ("Coefficient Cooking"), for the three latent points above at 100, 1000, 5000 and 10000 Hz. Same
/// provenance as the goldens: a second implementation, not the engine's.
struct ResponseGolden
{
  float latent[3];
  double db[4];
};

const ResponseGolden kResponses[] = {
  {{0.0f, 0.0f, 0.0f}, {-0.5371, 2.9827, 0.7496, 0.2215}},
  {{1.0f, 0.0f, 0.0f}, {-0.4555, 2.9188, 0.3963, -4.2476}},
  {{0.0f, 0.0f, 1.0f}, {-5.1088, 2.7636, 3.8445, 4.1385}},
};

const double kProbeHz[4] = {100.0, 1000.0, 5000.0, 10000.0};

/// The response of the designed sections at one frequency, from the coefficients, independently of
/// `floweq::response_db` (which is what the report uses): a biquad's transfer function.
double response_at(const namchain::floweq::Coeffs& section, double frequency, double rate)
{
  const double omega = 2.0 * kPi * frequency / rate;
  const std::complex<double> z(std::cos(omega), -std::sin(omega));
  const std::complex<double> numerator = double(section.b0) + double(section.b1) * z + double(section.b2) * z * z;
  const std::complex<double> denominator = 1.0 + double(section.a1) * z + double(section.a2) * z * z;
  return std::abs(numerator / denominator);
}

} // namespace

TEST_CASE("the engine's decoder is flowEQ's trained model")
{
  for (const Golden& golden : kGoldens)
  {
    float params[kParamCount];
    namchain::floweq::decode(golden.latent, params);
    for (int k = 0; k < kParamCount; ++k)
    {
      CAPTURE(k);
      CAPTURE(namchain::floweq::kParamNames[k]);
      // Relative tolerance: the C++ pass accumulates in float (as Keras does), the reference in
      // double. A layout or normalisation error is orders of magnitude larger than this.
      CHECK(params[k] == doctest::Approx(golden.params[k]).epsilon(1e-4));
    }
  }

  // The model the lite plugin hardcodes, not some other one: the header records which .mat it came
  // from, so a table regenerated from the wrong file is visible here.
  CHECK(std::string(namchain::floweq::kModelName) == "vae3d_beta_0.0010");
}

TEST_CASE("the five sections are flowEQ's own response")
{
  for (const ResponseGolden& golden : kResponses)
  {
    float params[kParamCount];
    namchain::floweq::decode(golden.latent, params);
    namchain::floweq::Coeffs sections[namchain::floweq::kSectionCount];
    namchain::floweq::design(params, 1.0, kRate, sections);

    for (int probe = 0; probe < 4; ++probe)
    {
      CAPTURE(probe);
      CAPTURE(kProbeHz[probe]);
      double db = 0.0;
      for (const auto& section : sections)
        db += 20.0 * std::log10(response_at(section, kProbeHz[probe], kRate));
      CHECK(db == doctest::Approx(golden.db[probe]).epsilon(2e-3));
    }
  }

  // And `response_db`, which the report serves, agrees with the same numbers: it is the one the
  // curve a client draws comes from.
  float params[kParamCount];
  namchain::floweq::decode(kGoldens[0].latent, params);
  float points[4];
  namchain::floweq::response_db(params, 1.0, kRate, 100.0, 10000.0, 4, points);
  // Those four points are log-spaced over the range rather than at the probe frequencies, so check
  // the endpoints: 100 Hz and 10 kHz are the first and last.
  CHECK(points[0] == doctest::Approx(static_cast<float>(kResponses[0].db[0])).epsilon(2e-3));
  CHECK(points[3] == doctest::Approx(static_cast<float>(kResponses[0].db[3])).epsilon(2e-3));
}

TEST_CASE("strength scales the gains and zero is the identity filter")
{
  // Every gain multiplied by zero is 0 dB, which is A = 1: the numerator and denominator
  // coefficients are the same numbers, so the five sections reduce to y = x exactly, not merely
  // closely. The buffer is untouched, gains included.
  NodeSpec node = flow_node();
  node.params["strength"] = 0.0;
  const std::vector<float> input = sine(440.0, 4096, 0.3f);
  const std::vector<float> output = render(one_node(node), input);
  REQUIRE(output.size() == input.size());
  for (std::size_t i = 0; i < input.size(); ++i)
  {
    if (output[i] != input[i])
    {
      CAPTURE(i);
      CHECK(output[i] == doctest::Approx(input[i]).epsilon(1e-6));
    }
  }
}

TEST_CASE("a rendered sine sees the curve the report draws")
{
  // The decoder's own default (latent 0,0,0) boosts 1 kHz by 2.98 dB, so a sine at 1 kHz comes out
  // 2.98 dB louder. Comparing the audio with the design is what catches a render path that builds
  // one set of coefficients and uses another.
  const std::vector<float> input = sine(1000.0, 24000, 0.25f);
  const std::vector<float> output = render(one_node(flow_node()), input);
  const double in_rms = rms(input, input.size() / 2, input.size());
  const double out_rms = rms(output, output.size() / 2, output.size());
  const double measured_db = 20.0 * std::log10(out_rms / in_rms);

  float params[kParamCount];
  namchain::floweq::decode(kGoldens[0].latent, params);
  float point[1];
  namchain::floweq::response_db(params, 1.0, kRate, 1000.0, 1000.0, 1, point);

  CHECK(measured_db == doctest::Approx(double(point[0])).epsilon(1e-3));
  CHECK(measured_db == doctest::Approx(2.9827).epsilon(1e-2));
}

TEST_CASE("a latent change ramps to the filter a fresh build would have")
{
  // Build the same node twice: one takes the latent change while it plays, the other is built with
  // it. Once the crossfade has closed they must agree sample for sample -- which is what makes a
  // ramp a transition rather than a different filter from a rebuilt one.
  const std::vector<float> input = sine(440.0, kQuantum * 200, 0.25f);

  namchain::rt::Arena arena_a(1u << 22);
  auto ramped = compile_plan(one_node(flow_node()), arena_a);
  std::vector<float> ramped_out(input.size(), 0.0f);
  std::size_t offset = 0;
  for (; offset < kQuantum * 20; offset += kQuantum)
    ramped->execute(input.data() + offset, ramped_out.data() + offset, kQuantum);

  std::size_t node_index = 0;
  uint32_t param = 0;
  REQUIRE(ramped->resolve_param("n", "x", -1, node_index, param));
  REQUIRE(ramped->set_param(node_index, param, 1.0f));
  for (; offset + kQuantum <= input.size(); offset += kQuantum)
    ramped->execute(input.data() + offset, ramped_out.data() + offset, kQuantum);

  NodeSpec changed = flow_node();
  changed.params["x"] = 1.0;
  namchain::rt::Arena arena_b(1u << 22);
  auto fresh = compile_plan(one_node(changed), arena_b);
  std::vector<float> fresh_out(input.size(), 0.0f);
  for (std::size_t at = 0; at < input.size(); at += kQuantum)
    fresh->execute(input.data() + at, fresh_out.data() + at, kQuantum);

  // The tail: the filters share a steady sine, so their transients have long died away.
  const std::size_t tail = input.size() - kQuantum * 20;
  double worst = 0.0;
  for (std::size_t i = tail; i < input.size(); ++i)
    worst = std::max(worst, std::abs(double(ramped_out[i]) - double(fresh_out[i])));
  REQUIRE(fresh_out[tail] != 0.0f); // the node did something
  CHECK(worst < 1e-3);
}

TEST_CASE("auto gain hands back the loudness the filters took away")
{
  // 1 kHz through the default curve gains 2.98 dB. With Auto Gain on, the compensation is the
  // opposite, so a steady sine comes out at the level it went in at. The meter's window is the
  // standard's 3 s, so the run is long enough for it to converge and the tail is what is measured.
  NodeSpec node = flow_node();
  node.params["auto_gain"] = 1.0;
  const std::vector<float> input = sine(1000.0, static_cast<std::size_t>(kRate * 12), 0.25f);
  const std::vector<float> output = render(one_node(node), input);

  const std::size_t tail_from = input.size() - static_cast<std::size_t>(kRate * 2);
  const double in_rms = rms(input, tail_from, input.size());
  const double out_rms = rms(output, tail_from, output.size());
  CHECK(20.0 * std::log10(out_rms / in_rms) == doctest::Approx(0.0).epsilon(2e-2));

  // And with Auto Gain off, Out is what applies: the same run at -6 dB comes out 6 dB lower once
  // the EQ is out of the way (strength 0), so the number is the gain and nothing else.
  NodeSpec manual = flow_node();
  manual.params["out_db"] = -6.0;
  manual.params["strength"] = 0.0;
  const std::vector<float> quiet = render(one_node(manual), input);
  CHECK(20.0 * std::log10(rms(quiet, tail_from, quiet.size()) / in_rms) == doctest::Approx(-6.0).epsilon(1e-2));
}

TEST_CASE("the report says what the render thread is running")
{
  NodeSpec node = flow_node();
  node.params["x"] = 0.5;
  node.params["strength"] = 0.5;
  namchain::rt::Arena arena(1u << 22);
  auto plan = compile_plan(one_node(node), arena);
  const std::vector<float> input = sine(440.0, kQuantum * 4, 0.25f);
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t at = 0; at < input.size(); at += kQuantum)
    plan->execute(input.data() + at, output.data() + at, kQuantum);

  const namchain::render::NodeType* type = namchain::render::find_node_type("nc.eq.flow");
  REQUIRE(type != nullptr);
  REQUIRE(type->describe_state != nullptr);
  const std::string text = type->describe_state(state_of(*plan));
  REQUIRE(!text.empty());
  const nlohmann::json report = nlohmann::json::parse(text);

  CHECK(report["model"] == "vae3d_beta_0.0010");
  CHECK(report["latent"][0] == doctest::Approx(0.5));
  CHECK(report["latent"][1] == doctest::Approx(0.0));
  CHECK(report["strength"] == doctest::Approx(0.5));
  CHECK(report["extend"] == false);
  CHECK(report["auto_gain"] == false);
  CHECK(report["gain_db"] == doctest::Approx(0.0).epsilon(1e-3));

  // The bands are the decoder's, with strength applied: five sections, the shelves' Q fixed.
  REQUIRE(report["bands"].size() == 5);
  const nlohmann::json& shelf = report["bands"][0];
  CHECK(shelf["type"] == "lowshelf");
  CHECK(shelf["q"] == doctest::Approx(0.71));
  // The report rounds to a hundredth: -0.4918 dB of shelf at half strength is -0.2459, published as -0.25.
  CHECK(shelf["gain_db"] == doctest::Approx(-0.2459).epsilon(1e-2));

  // The curve covers 20 Hz to 20 kHz in 96 points, and its peak is the designed one.
  CHECK(report["curve"]["f_min"] == doctest::Approx(20.0));
  CHECK(report["curve"]["f_max"] == doctest::Approx(20000.0));
  REQUIRE(report["curve"]["points"].size() == 96);
  double peak = -1000.0;
  for (const auto& point : report["curve"]["points"])
    peak = std::max(peak, point.get<double>());
  CHECK(peak == doctest::Approx(2.3732).epsilon(2e-3)); // the curve's peak at half strength
}

TEST_CASE("the render path allocates nothing, a latent change and a crossfade included")
{
  // The decoder runs inside `process` (once per block a change arrives), designs five sections and
  // starts a crossfade -- all of it on the audio thread. The allocation interposer is the claim's
  // instrument, not a convention: with the RT_CHECK build, any allocator call inside an RtScope is
  // counted, and this test's whole interaction with the node happens inside one.
  namchain::rt::Arena arena(1u << 22);
  auto plan = compile_plan(one_node(flow_node()), arena);
  const std::vector<float> input = sine(330.0, kQuantum, 0.2f);
  std::vector<float> output(input.size(), 0.0f);

  std::size_t node_index = 0;
  uint32_t x_param = 0, auto_param = 0;
  REQUIRE(plan->resolve_param("n", "x", -1, node_index, x_param));
  REQUIRE(plan->resolve_param("n", "auto_gain", -1, node_index, auto_param));

  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped allocation check: interposer not compiled in");
    return;
  }
  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int block = 0; block < 50; ++block)
      plan->execute(input.data(), output.data(), kQuantum);
    REQUIRE(plan->set_param(node_index, x_param, 1.0f)); // decode + design + a 5-section crossfade
    for (int block = 0; block < 50; ++block)
      plan->execute(input.data(), output.data(), kQuantum);
    REQUIRE(plan->set_param(node_index, auto_param, 1.0f)); // the loudness meters run from here
    for (int block = 0; block < 50; ++block)
      plan->execute(input.data(), output.data(), kQuantum);
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
}

TEST_CASE("the latent space's bounds are the schema's, and the node is in the catalogue")
{
  const namchain::render::NodeType* type = namchain::render::find_node_type("nc.eq.flow");
  REQUIRE(type != nullptr);
  CHECK(std::string(type->name) == "flowEQ");
  CHECK(std::string(type->category) == "eq");
  CHECK(type->inputs == 1);
  CHECK(type->outputs == 1);

  NodeSpec out_of_range = flow_node();
  out_of_range.params["x"] = 3.0;
  const namchain::graph::Validation validation = namchain::graph::validate(one_node(out_of_range));
  CHECK_FALSE(validation.ok);
  CHECK(validation.message.find("nc.eq.flow") != std::string::npos);
  CHECK(validation.message.find("'x'") != std::string::npos);

  // A flag is a switch: anything nonzero means on, and the document does not have to write 1.
  NodeSpec flag = flow_node();
  flag.params["extend"] = 2.0;
  CHECK(namchain::graph::validate(one_node(flag)).ok);
}
