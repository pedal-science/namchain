// The scripted edit timeline: parsing, generation, and the offline glitch check that makes the
// `smooth` op class a measurement rather than a claim.
#include <doctest.h>

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "namchain/render/timeline.h"
#include "namchain/wav.h"

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::render::EditOp;
using namchain::render::EditTimeline;

constexpr double kRate = 48000.0;
constexpr std::size_t kFrames = 48000; // one second

struct TempDir
{
  TempDir()
  {
    static std::atomic<unsigned> counter{0};
    path = std::filesystem::temp_directory_path() /
           ("namchain-timeline-" + std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1)));
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

NodeSpec node(const std::string& id, const std::string& type)
{
  NodeSpec spec;
  spec.id = id;
  spec.type = type;
  return spec;
}

Graph gain_graph(double ramp_ms)
{
  Graph graph;
  NodeSpec gn = node("g", "nc.gain");
  gn.params["gain"] = 1.0;
  gn.params["ramp_ms"] = ramp_ms;
  graph.nodes = {node("in", "nc.input"), gn, node("out", "nc.output")};
  graph.connections = {{"in", 0, "g", 0}, {"g", 0, "out", 0}};
  return graph;
}

std::string write_input(const std::filesystem::path& path)
{
  std::vector<float> samples(kFrames);
  for (std::size_t i = 0; i < kFrames; ++i)
    samples[i] = 0.4f * std::sin(2.0f * 3.14159265358979f * 1000.0f * static_cast<float>(i) / 48000.0f);
  std::string error;
  REQUIRE_MESSAGE(namchain::save_wav_float32(path.string(), samples.data(), samples.size(), 1, kRate, error),
                  error);
  return path.string();
}

} // namespace

TEST_CASE("a timeline parses and names what it cannot honour")
{
  const Graph graph = gain_graph(5.0);

  SUBCASE("valid")
  {
    EditTimeline timeline;
    std::string error;
    REQUIRE_MESSAGE(namchain::render::parse_timeline_json(
                      R"([{"at_sample":1000,"node":"g","param":"gain","value":0.5}])", graph, timeline, error),
                    error);
    REQUIRE(timeline.ops.size() == 1u);
    CHECK(timeline.ops[0].at_sample == 1000u);
    CHECK(timeline.ops[0].node == "g");
    CHECK(timeline.ops[0].param == "gain");
    CHECK(timeline.ops[0].value == doctest::Approx(0.5));
  }

  SUBCASE("unknown node")
  {
    EditTimeline timeline;
    std::string error;
    CHECK_FALSE(namchain::render::parse_timeline_json(
      R"([{"at_sample":0,"node":"ghost","param":"gain","value":1}])", graph, timeline, error));
    CHECK(error.find("ghost") != std::string::npos);
  }

  SUBCASE("param that is not settable")
  {
    EditTimeline timeline;
    std::string error;
    // `ramp_ms` is declared and patchable, but not rampable: it is a smoothing time, so changing it is
    // a rebuild rather than an edit inside a running plan.
    CHECK_FALSE(namchain::render::parse_timeline_json(
      R"([{"at_sample":0,"node":"g","param":"ramp_ms","value":10}])", graph, timeline, error));
    CHECK(error.find("ramp_ms") != std::string::npos);
  }

  SUBCASE("the canonical gain param is settable too")
  {
    EditTimeline timeline;
    std::string error;
    CHECK(namchain::render::parse_timeline_json(
      R"([{"at_sample":0,"node":"g","param":"gain_db","value":-6}])", graph, timeline, error));
  }

  SUBCASE("unimplemented op kind")
  {
    EditTimeline timeline;
    std::string error;
    CHECK_FALSE(namchain::render::parse_timeline_json(
      R"([{"op":"rewire","at_sample":0,"node":"g","param":"gain","value":1}])", graph, timeline, error));
    CHECK(error.find("not implemented") != std::string::npos);
  }

  SUBCASE("out of order")
  {
    EditTimeline timeline;
    std::string error;
    CHECK_FALSE(namchain::render::parse_timeline_json(
      R"([{"at_sample":500,"node":"g","param":"gain","value":1},
          {"at_sample":100,"node":"g","param":"gain","value":1}])", graph, timeline, error));
    CHECK(error.find("order") != std::string::npos);
  }
}

TEST_CASE("a generated timeline is deterministic, ordered and only touches settable params")
{
  const Graph graph = gain_graph(5.0);
  const EditTimeline first = namchain::render::random_timeline(graph, 16, kFrames, 1234);
  const EditTimeline second = namchain::render::random_timeline(graph, 16, kFrames, 1234);
  REQUIRE(first.ops.size() == second.ops.size());
  for (std::size_t i = 0; i < first.ops.size(); ++i)
  {
    CHECK(first.ops[i].at_sample == second.ops[i].at_sample);
    CHECK(first.ops[i].value == doctest::Approx(second.ops[i].value));
    CHECK(first.ops[i].node == "g");
    // `gain_db` is the visible rampable one; the hidden linear alias is not offered to a generator,
    // because two names for one value is a choice for a client rather than for a fuzzer.
    CHECK(first.ops[i].param == "gain_db");
    CHECK(first.ops[i].at_sample < kFrames);
    if (i > 0)
      CHECK(first.ops[i].at_sample > first.ops[i - 1].at_sample);
  }
  // Two: `gain_db` and its hidden linear alias `gain`. A node with nothing rampable counts zero,
  // which is what tells a client that every edit to it is structural.
  CHECK(namchain::render::settable_param_count(graph) == 2u);
}

TEST_CASE("the timeline renders a ramped edit and the detector passes it")
{
  TempDir dir;
  const Graph graph = gain_graph(5.0);
  const std::string input = write_input(dir.path / "in.wav");

  EditTimeline timeline;
  timeline.ops = {{namchain::render::EditKind::SetParam, 12000, "g", "gain", 0.5},
                  {namchain::render::EditKind::SetParam, 24000, "g", "gain", 1.5},
                  {namchain::render::EditKind::SetParam, 36000, "g", "gain", 0.8}};

  namchain::render::TimelineRenderOptions options;
  options.quantum = 64;
  options.glitch_checks = true;
  const namchain::render::TimelineRenderResult result =
    namchain::render::render_timeline_to_wav(graph, timeline, input, (dir.path / "out.wav").string(), options);

  REQUIRE_MESSAGE(result.ok, result.error);
  CHECK(result.frames == kFrames);
  CHECK(result.edits == 3u);
  CHECK(result.judged == 3u);
  if (!result.glitch_ok)
    for (const namchain::glitch::Finding& finding : result.findings)
      MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  CHECK(result.glitch_ok);
  CHECK(result.findings.empty());
}

TEST_CASE("the timeline's own negative control fails: an instant edit is caught")
{
  TempDir dir;
  const Graph graph = gain_graph(0.0); // ramp_ms = 0: a step, the click the smooth class removes
  const std::string input = write_input(dir.path / "in.wav");

  EditTimeline timeline;
  timeline.ops = {{namchain::render::EditKind::SetParam, 24000, "g", "gain", 0.5}};

  namchain::render::TimelineRenderOptions options;
  const namchain::render::TimelineRenderResult result =
    namchain::render::render_timeline_to_wav(graph, timeline, input, (dir.path / "out.wav").string(), options);

  REQUIRE_MESSAGE(result.ok, result.error);
  CHECK(result.judged == 1u);
  CHECK_FALSE(result.glitch_ok);
  CHECK_FALSE(result.findings.empty());
  CHECK(result.findings.front().rule == "continuity");
}

TEST_CASE("a randomised parameter torture passes the detector")
{
  TempDir dir;
  const Graph graph = gain_graph(5.0);
  const std::string input = write_input(dir.path / "in.wav");

  namchain::render::TimelineRenderOptions options;
  options.quantum = 64;
  const EditTimeline timeline = namchain::render::random_timeline(graph, 12, kFrames, 99);
  REQUIRE(timeline.ops.size() == 12u);

  const namchain::render::TimelineRenderResult result =
    namchain::render::render_timeline_to_wav(graph, timeline, input, (dir.path / "out.wav").string(), options);
  REQUIRE_MESSAGE(result.ok, result.error);
  if (!result.glitch_ok)
    for (const namchain::glitch::Finding& finding : result.findings)
      MESSAGE(finding.rule << " at " << finding.frame << ": " << finding.detail);
  CHECK(result.glitch_ok);
  CHECK(result.judged >= 10u); // edge edits with no settled region are skipped, not failed
}
