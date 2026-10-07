// rt::Crossfade and the structural transition it serves.
//
// The primitive's own properties are checked directly (equal power, endpoints, a fade that closes
// mid-block). Then the transition is judged the same way the daemon's is: two compiled plans run
// together for the fade, their outputs are crossfaded, and the glitch detector says whether the
// result is continuous and converges to the right chain on either side. The hard-switch case is the
// negative control -- without it, a detector that passes everything would look like success.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "namchain/glitch.h"
#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"
#include "namchain/rt/crossfade.h"
#include "namchain/rt_check.h"

namespace
{

using namchain::graph::Graph;
using namchain::graph::NodeSpec;

constexpr int kRate = 48000;
constexpr int kQuantum = 64;
constexpr std::size_t kFrames = 4096;
constexpr std::size_t kEditAt = 2048;
constexpr int kFade = 240; // 5 ms at 48 kHz

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

/// One gain, or two in series: the transition tests want two plans that differ structurally, not
/// just in a parameter, so the second has a node the first does not.
Graph chain(double gain, bool two_gains)
{
  Graph graph;
  NodeSpec gn = node("g", "nc.gain");
  gn.params["gain"] = gain;
  graph.nodes = {node("in", "nc.input"), gn};
  if (two_gains)
  {
    NodeSpec g2 = node("g2", "nc.gain");
    g2.params["gain"] = 1.0;
    graph.nodes.push_back(g2);
  }
  graph.nodes.push_back(node("out", "nc.output"));

  std::string previous = "g";
  if (two_gains)
  {
    graph.connections.push_back({"g", 0, "g2", 0});
    previous = "g2";
  }
  graph.connections.push_back({"in", 0, "g", 0});
  graph.connections.push_back({previous, 0, "out", 0});
  return graph;
}

std::vector<float> tone(std::size_t frames)
{
  std::vector<float> samples(frames);
  for (std::size_t i = 0; i < frames; ++i)
    samples[i] = 0.4f * std::sin(2.0f * 3.14159265358979f * 1000.0f * static_cast<float>(i) /
                                 static_cast<float>(kRate));
  return samples;
}

std::unique_ptr<namchain::graph::RenderPlan> compile(const Graph& graph, namchain::rt::Arena& arena)
{
  std::string error;
  auto plan = namchain::graph::RenderPlan::compile(graph, kQuantum, arena, error);
  REQUIRE_MESSAGE(plan != nullptr, error);
  return plan;
}

std::vector<float> render(const Graph& graph, const std::vector<float>& input)
{
  namchain::rt::Arena arena(1 << 16);
  auto plan = compile(graph, arena);
  plan->prewarm();
  std::vector<float> output(input.size(), 0.0f);
  for (std::size_t at = 0; at < input.size(); at += kQuantum)
  {
    const int frames = static_cast<int>(std::min<std::size_t>(kQuantum, input.size() - at));
    plan->execute(input.data() + at, output.data() + at, frames);
  }
  return output;
}

/// Run plan A until \p kEditAt, then either crossfade into plan B over \p kFade frames or switch to
/// it in one sample. This is the daemon's transition, reduced to the two things it does.
std::vector<float> render_transition(const Graph& a_graph, const Graph& b_graph, const std::vector<float>& input,
                                     bool crossfade)
{
  namchain::rt::Arena arena_a(1 << 16);
  namchain::rt::Arena arena_b(1 << 16);
  auto a = compile(a_graph, arena_a);
  auto b = compile(b_graph, arena_b);
  a->prewarm();
  b->prewarm();

  namchain::rt::Crossfade fade(kFade);
  std::vector<float> output(input.size(), 0.0f);
  std::vector<float> block_a(kQuantum, 0.0f);
  std::vector<float> block_b(kQuantum, 0.0f);
  bool switched = false;

  for (std::size_t at = 0; at < input.size(); at += kQuantum)
  {
    const int frames = static_cast<int>(std::min<std::size_t>(kQuantum, input.size() - at));
    a->execute(input.data() + at, block_a.data(), frames);
    b->execute(input.data() + at, block_b.data(), frames);

    if (!switched && at == kEditAt)
    {
      switched = true;
      if (crossfade)
        fade.start(kFade);
    }

    if (!switched)
      std::copy(block_a.begin(), block_a.begin() + frames, output.begin() + static_cast<std::ptrdiff_t>(at));
    else if (crossfade && fade.active())
      fade.mix(block_a.data(), block_b.data(), output.data() + at, frames);
    else
      std::copy(block_b.begin(), block_b.begin() + frames, output.begin() + static_cast<std::ptrdiff_t>(at));
  }
  return output;
}


/// The worst peak inside a fade against the louder of the two signals it is mixing, in dB: what the
/// fade *adds*. The glitch detector cannot see this. A bump is continuous (rule 1 looks for a step),
/// it is the opposite of a dropout (rule 2), it puts no excess energy above 5 kHz (rule 3) and the
/// fade does converge (rule 4) -- so a transition can pass every rule the detector has and still
/// thump, which is how this went unnoticed until someone listened.
double fade_excess(const std::vector<float>& y, const std::vector<float>& a, const std::vector<float>& b,
                   std::size_t from, std::size_t to)
{
  double peak = 0.0, louder = 0.0;
  for (std::size_t i = from; i < to && i < y.size(); ++i)
  {
    peak = std::max(peak, std::abs(static_cast<double>(y[i])));
    louder = std::max(louder, std::max(std::abs(static_cast<double>(a[i])), std::abs(static_cast<double>(b[i]))));
  }
  return louder > 0.0 ? 20.0 * std::log10(peak / louder) : 0.0;
}

/// A second tone, unrelated to `tone`: what a fade between two *different* rigs puts through it.
std::vector<float> other_tone(std::size_t frames)
{
  std::vector<float> samples(frames);
  for (std::size_t i = 0; i < frames; ++i)
    samples[i] = 0.4f * std::sin(2.0f * 3.14159265358979f * 1310.0f * static_cast<float>(i) /
                                 static_cast<float>(kRate));
  return samples;
}

/// A graph whose one gain can be bypassed: with gain 1 the node passes the input through, so a
/// bypass flip crossfades two signals that are the same audio -- the case that thumps.
Graph pass_through(bool bypassed)
{
  Graph graph;
  NodeSpec gn = node("g", "nc.gain");
  gn.params["gain"] = 1.0;
  gn.bypass = bypassed;
  graph.nodes = {node("in", "nc.input"), gn, node("out", "nc.output")};
  graph.connections = {{"in", 0, "g", 0}, {"g", 0, "out", 0}};
  return graph;
}

/// The same input into both sides of an `nc.xfade`, which is the node the engine inserts for a local
/// transition (a bypass flip, a rewire). Both of its inputs are the same audio here, which is the
/// worst case and the one a bypass of a linear node is.
Graph xfade_graph()
{
  Graph graph;
  NodeSpec fade = node("f", "nc.xfade");
  fade.params["frames"] = static_cast<double>(kFade);
  graph.nodes = {node("in", "nc.input"), node("s", "nc.split"), fade, node("out", "nc.output")};
  graph.connections = {{"in", 0, "s", 0},   {"s", 0, "f", 0}, {"s", 1, "f", 1},
                       {"f", 0, "out", 0}};
  return graph;
}

namchain::glitch::Report analyse(const std::vector<float>& y, const std::vector<float>& before,
                                 const std::vector<float>& after)
{
  namchain::glitch::Options options;
  options.sample_rate = static_cast<double>(kRate);
  const std::vector<namchain::glitch::Edit> edits = {{kEditAt, kEditAt + kFade}};
  std::string error;
  const namchain::glitch::Report report =
    namchain::glitch::analyse(y.data(), before.data(), after.data(), y.size(), edits, options, error);
  REQUIRE_MESSAGE(error.empty(), error);
  return report;
}

} // namespace

TEST_CASE("the crossfade weights are equal-power and reach both endpoints")
{
  namchain::rt::Crossfade fade(1024);
  CHECK(fade.length() == 0);
  CHECK_FALSE(fade.active());

  const int length = 240;
  std::vector<float> ones(length, 1.0f);
  std::vector<float> zeros(length, 0.0f);
  std::vector<float> out(length, 0.0f);

  // rho = 0 explicitly: this is the shape the equal-power identity belongs to.
  fade.start(length, 0.0);
  REQUIRE(fade.active());
  CHECK(fade.length() == length);
  fade.mix(ones.data(), zeros.data(), out.data(), length);
  const std::vector<float> from = out;

  fade.start(length, 0.0);
  fade.mix(zeros.data(), ones.data(), out.data(), length);
  const std::vector<float> to = out;

  CHECK(from.front() == doctest::Approx(1.0f));
  CHECK(from.back() == doctest::Approx(0.0f));
  CHECK(to.front() == doctest::Approx(0.0f));
  CHECK(to.back() == doctest::Approx(1.0f));
  for (int i = 0; i < length; ++i)
  {
    // The property that makes it equal-power rather than a 3 dB dip in the middle, for two signals
    // that are unrelated. For two that are the same audio the same identity is a 3 dB *bump*: see
    // the constant-amplitude test below.
    CHECK(from[static_cast<std::size_t>(i)] * from[static_cast<std::size_t>(i)] +
            to[static_cast<std::size_t>(i)] * to[static_cast<std::size_t>(i)] ==
          doctest::Approx(1.0).epsilon(1e-4));
    CHECK(to[static_cast<std::size_t>(i)] >= 0.0f);
    CHECK(to[static_cast<std::size_t>(i)] <= 1.0f);
  }
  CHECK_FALSE(fade.active());
}

TEST_CASE("a fade that closes mid-block leaves the new signal for the rest of the block")
{
  namchain::rt::Crossfade fade(1024);
  std::vector<float> a(64, 1.0f);
  std::vector<float> b(64, 0.5f);
  std::vector<float> out(64, -1.0f);

  fade.start(10);
  fade.mix(a.data(), b.data(), out.data(), 64);
  CHECK_FALSE(fade.active());
  for (int i = 10; i < 64; ++i)
    CHECK(out[static_cast<std::size_t>(i)] == doctest::Approx(0.5f));
}

TEST_CASE("the crossfade render path allocates nothing")
{
  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped allocation check: interposer not compiled in");
    return;
  }
  namchain::rt::Crossfade fade(4096);
  std::vector<float> a(64, 1.0f);
  std::vector<float> b(64, 0.5f);
  std::vector<float> out(64, 0.0f);

  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int i = 0; i < 100; ++i)
    {
      fade.start(240);
      fade.mix(a.data(), b.data(), out.data(), 64);
    }
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
}

TEST_CASE("a structural change crossfaded between two plans passes the detector")
{
  const std::vector<float> input = tone(kFrames);
  const Graph before_graph = chain(1.0, false);
  const Graph after_graph = chain(0.5, true);

  const std::vector<float> y = render_transition(before_graph, after_graph, input, true);
  const std::vector<float> before = render(before_graph, input);
  const std::vector<float> after = render(after_graph, input);

  const namchain::glitch::Report report = analyse(y, before, after);
  if (!report.ok)
    for (const namchain::glitch::Finding& finding : report.findings)
      MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  CHECK(report.ok);
  CHECK(report.continuity_evaluated);
  // The transition did something: somewhere in the fade window y is neither chain.
  float worst = 0.0f;
  for (std::size_t i = kEditAt; i < kEditAt + kFade; ++i)
    worst = std::max(worst, std::abs(y[i] - before[i]));
  CHECK(worst > 0.01f);
  CHECK(std::abs(y.back() - after.back()) < 1e-6f);
}

TEST_CASE("an uncrossfaded structural switch fails the same detector")
{
  const std::vector<float> input = tone(kFrames);
  const Graph before_graph = chain(1.0, false);
  const Graph after_graph = chain(0.5, true);

  const std::vector<float> y = render_transition(before_graph, after_graph, input, false);
  const std::vector<float> before = render(before_graph, input);
  const std::vector<float> after = render(after_graph, input);

  const namchain::glitch::Report report = analyse(y, before, after);
  CHECK_FALSE(report.ok);
  CHECK(report.max_edit_d2 > report.reference_d2);
}

// ---------------------------------------------------------------------------
// What the fade adds. Invisible to the detector, audible as a thud.
// ---------------------------------------------------------------------------

TEST_CASE("a fade between the same audio on both sides adds no energy")
{
  // g1^2 + g2^2 == 1 is the power of the sum only when the two signals are uncorrelated. With
  // rho = 1 it is g1^2 + g2^2 + 2*g1*g2, which is 2 at the midpoint: +3 dB of thump.
  const std::vector<float> x = tone(kFrames);
  namchain::rt::Crossfade fade(kFade);
  std::vector<float> y(kFrames, 0.0f);
  fade.start(kFade);
  fade.mix(x.data(), x.data(), y.data(), kFade);
  const double excess = fade_excess(y, x, x, 0, kFade);
  MESSAGE("excess " << excess << " dB");
  CHECK(excess < 0.1);
}

TEST_CASE("a fade between unrelated signals does not dip")
{
  // The case the equal-power law was chosen for, and the reason the fix is not simply equal gain:
  // (1-k, k) is 3 dB down at the midpoint here.
  const std::vector<float> a = tone(kFrames);
  const std::vector<float> b = other_tone(kFrames);
  namchain::rt::Crossfade fade(kFade);
  std::vector<float> y(kFrames, 0.0f);
  fade.start(kFade);
  fade.mix(a.data(), b.data(), y.data(), kFade);
  // Peak is the wrong measure for two unrelated tones (they peak at different places); the rms of
  // the output against the louder input is what a dip shows up in.
  double energy = 0.0;
  for (int i = 0; i < kFade; ++i)
    energy += static_cast<double>(y[static_cast<std::size_t>(i)]) * y[static_cast<std::size_t>(i)];
  const double rms = std::sqrt(energy / kFade);
  const double louder = std::max(0.4 / std::sqrt(2.0), 0.4 / std::sqrt(2.0));
  const double dip = 20.0 * std::log10(rms / louder);
  MESSAGE("rms against the inputs " << dip << " dB");
  CHECK(dip > -3.5);
}

TEST_CASE("a bypass transition does not add energy")
{
  // The daemon's own path: two plans run together and their outputs are crossfaded. Here both
  // produce the same audio, which is what a bypass flip of a linear node is.
  const std::vector<float> input = tone(kFrames);
  const std::vector<float> y = render_transition(pass_through(false), pass_through(true), input, true);
  const std::vector<float> before = render(pass_through(false), input);
  const std::vector<float> after = render(pass_through(true), input);
  const double excess = fade_excess(y, before, after, kEditAt, kEditAt + kFade);
  MESSAGE("excess " << excess << " dB");
  CHECK(excess < 0.2);
}

TEST_CASE("an nc.xfade between the same audio on both sides adds no energy")
{
  // The node carry.cpp inserts for a local transition, which starts its fade when it is built.
  const std::vector<float> input = tone(kFrames);
  const std::vector<float> y = render(xfade_graph(), input);
  const double excess = fade_excess(y, input, input, 0, kFade);
  MESSAGE("excess " << excess << " dB");
  CHECK(excess < 0.2);
}

TEST_CASE("the weights are constant-amplitude where the two signals are the same audio")
{
  // rho = 1: g1 + g2 == 1, so passing the same signal through both sides leaves it alone rather
  // than adding 3 dB in the middle. This is a bypass of a linear node, an EQ ramp, a Lite/Full
  // handover -- every fade whose two sides carry the same audio.
  namchain::rt::Crossfade fade(1024);
  const int length = 240;
  std::vector<float> ones(length, 1.0f);
  std::vector<float> out(length, 0.0f);

  fade.start(length, 1.0);
  CHECK(fade.correlation() == doctest::Approx(1.0));
  fade.mix(ones.data(), ones.data(), out.data(), length);
  for (int i = 0; i < length; ++i)
    CHECK(out[static_cast<std::size_t>(i)] == doctest::Approx(1.0f).epsilon(1e-4));
}

TEST_CASE("a fade given no rho measures it from the block it is handed")
{
  namchain::rt::Crossfade fade(1024);
  std::vector<float> a(64, 0.0f), b(64, 0.0f), out(64, 0.0f);
  const std::vector<float> x = tone(64);
  const std::vector<float> other = other_tone(64);

  // The same audio on both sides: what a bypass of a gain, an EQ ramp or a model handover does.
  std::copy(x.begin(), x.end(), a.begin());
  std::copy(x.begin(), x.end(), b.begin());
  fade.start(240);
  CHECK(fade.correlation() == 0.0); // until the block arrives, the law is the safe one
  fade.mix(a.data(), b.data(), out.data(), 64);
  CHECK(fade.correlation() > 0.99);

  // Two unrelated signals: the measurement must not invent a correlation and start compensating,
  // because that would put the 3 dB dip back in the case equal power exists for.
  std::copy(other.begin(), other.end(), b.begin());
  fade.start(240);
  fade.mix(a.data(), b.data(), out.data(), 64);
  CHECK(fade.correlation() < 0.5);
}

TEST_CASE("the correlation is clamped, so a fade can never be amplified")
{
  // A negative rho would divide the weights by less than one. Over a short window of near-silence
  // an estimate comes out negative readily, and unclamped it made the thump it was fixing worse.
  namchain::rt::Crossfade fade(1024);
  std::vector<float> a(64, 0.0f), b(64, 0.0f), out(64, 0.0f);
  const std::vector<float> x = tone(64);
  for (std::size_t i = 0; i < x.size(); ++i)
  {
    a[i] = x[i];
    b[i] = -x[i]; // perfectly anti-correlated: rho would be -1
  }
  fade.start(240);
  fade.mix(a.data(), b.data(), out.data(), 64);
  CHECK(fade.correlation() >= 0.0);
  CHECK(fade.correlation() <= 1.0);
}

TEST_CASE("prewarm does not spend part of a local transition's fade")
{
  // `prewarm` runs one silent block through the whole plan, and a `skip_prewarm` node counts the
  // blocks it has played -- so running it there spent 64 of a 240-frame fade before any audio
  // arrived: the transition was 3.7 ms of crossfade rather than the 5 ms it asked for, and the
  // fade's correlation measurement compared silence with silence.
  //
  // The probe is a constant input with its inverse on the other side of the fade: the output is
  // g1 - g2, which is exactly zero at the midpoint, so *where* the midpoint falls says where the
  // fade started.
  Graph graph;
  NodeSpec fade = node("f", "nc.xfade");
  fade.params["frames"] = static_cast<double>(kFade);
  NodeSpec invert = node("n", "nc.gain");
  invert.params["gain"] = -1.0;
  graph.nodes = {node("in", "nc.input"), node("s", "nc.split"), invert, fade, node("out", "nc.output")};
  graph.connections = {{"in", 0, "s", 0}, {"s", 0, "f", 0}, {"s", 1, "n", 0},
                       {"n", 0, "f", 1},  {"f", 0, "out", 0}};

  const std::vector<float> input(kFrames, 1.0f);
  const std::vector<float> y = render(graph, input);
  const std::size_t middle = kFade / 2;
  MESSAGE("at the midpoint " << y[middle] << ", 64 frames early " << y[middle - 64]);
  CHECK(std::abs(y[middle]) < 0.05f);
  CHECK(std::abs(y[middle - 64]) > 0.3f);
}
