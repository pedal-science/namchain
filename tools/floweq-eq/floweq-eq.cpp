// floweq-eq: run the engine's nc.eq.flow block over a WAV file, offline.
//
// This is a test instrument, not a second implementation of flowEQ. It builds the same one-node
// graph the flowEQ tests use (input -> nc.eq.flow -> output), compiles it into a RenderPlan and
// executes it a block at a time, so the audio comes from the block's own create/process path: the
// decoder, the five biquads, the gain smoothing, the coefficient crossfade and Auto Gain's loudness
// meter are all the engine's. The report it writes is the block's own `describe_state`, the same
// JSON `rig.state` serves to rigpa's panel, so the curves and bands in the report are the ones the
// audio was made with.
//
// The block is mono and so is the engine; a multichannel file is run one channel at a time, each
// through a fresh node, and written back with its channel count. (That is N mono runs, not a
// channel-linked EQ: Auto Gain meters each channel separately, as two mono nodes would.)
//
// Usage is in `usage()` below. It lives in a git worktree so it can be built without touching the
// working checkout; keep `tools/floweq-eq/` with the tree for the next isolation test.

#include "namchain/graph/graph.h"
#include "namchain/graph/plan.h"
#include "namchain/rt/arena.h"
#include "namchain/wav.h"

#include <json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{

using namchain::graph::Connection;
using namchain::graph::Graph;
using namchain::graph::NodeSpec;
using namchain::graph::RenderPlan;
using namchain::render::CompileOptions;

constexpr const char* kTypeId = "nc.eq.flow";
constexpr double kLatentLimit = 2.0;
constexpr double kGainMinDb = -60.0;
constexpr double kGainMaxDb = 12.0;
constexpr double kRampMaxMs = 1000.0;
constexpr int kMaxBlock = 65536;

struct Options
{
  std::string input;
  std::string output;
  std::string report;
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  double strength = 1.0;
  double in_db = 0.0;
  double out_db = 0.0;
  double ramp_ms = 5.0;
  double rate = 48000.0;
  int block = 64;
  bool extend = false;
  bool auto_gain = false;
  bool analyze = false;
  bool json = false;
  bool quiet = false;
};

void usage(FILE* out)
{
  std::fprintf(out,
               "floweq-eq -- run the engine's nc.eq.flow (flowEQ-lite) block over a WAV file\n"
               "\n"
               "Usage:\n"
               "  floweq-eq --input IN.wav --output OUT.wav [EQ options]\n"
               "  floweq-eq --input IN.wav --analyze [EQ options]     (print the block's response only)\n"
               "  floweq-eq --analyze [--rate 48000] [EQ options]     (no file; inspect a setting)\n"
               "\n"
               "EQ options (the block's own parameters):\n"
               "  --x <v>            latent X, -2..2                  (default 0)\n"
               "  --y <v>            latent Y, -2..2                  (default 0)\n"
               "  --z <v>            latent Z, -2..2                  (default 0)\n"
               "  --strength <v>     scales every band's gain, 0..1   (default 1; 0 is the identity EQ)\n"
               "  --extend           double the latent values before the decoder\n"
               "  --auto-gain        match output loudness to the input (BS.1770 short-term), +/-12 dB\n"
               "  --in-db <dB>       input gain, -60..12              (default 0)\n"
               "  --out-db <dB>      output gain, -60..12             (default 0)\n"
               "  --ramp-ms <ms>     smoothing time, 0..1000          (default 5)\n"
               "\n"
               "Other options:\n"
               "  --block <frames>   render block size                (default 64, the engine's quantum)\n"
               "  --rate <Hz>        rate for --analyze with no input (default 48000)\n"
               "  --report <path>    write the block's describe_state JSON (plus run stats) here\n"
               "  --json             print that JSON to stdout instead of the human summary\n"
               "  --quiet            print nothing but errors\n"
               "  -h, --help         this text\n"
               "\n"
               "Output WAVs are 32-bit IEEE float at the input's rate and channel count. The report's\n"
               "bands, curve and gain_db come from the running node, not from a second model of it.\n");
}

bool parse_number(const std::string& text, double& out)
{
  try
  {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value))
      return false;
    out = value;
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool parse_int(const std::string& text, int& out)
{
  try
  {
    std::size_t used = 0;
    const long value = std::stol(text, &used);
    if (used != text.size() || value <= 0 || value > kMaxBlock)
      return false;
    out = static_cast<int>(value);
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool parse_args(int argc, char** argv, Options& options, std::string& error)
{
  const auto need = [&](int& index, const char* name, std::string& value) {
    if (index + 1 >= argc)
    {
      error = std::string("missing value for ") + name;
      return false;
    }
    value = argv[++index];
    return true;
  };

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    std::string text;
    if (arg == "-h" || arg == "--help")
    {
      usage(stdout);
      std::exit(0);
    }
    else if (arg == "--input" || arg == "-i")
    {
      if (!need(i, "--input", options.input))
        return false;
    }
    else if (arg == "--output" || arg == "-o")
    {
      if (!need(i, "--output", options.output))
        return false;
    }
    else if (arg == "--report")
    {
      if (!need(i, "--report", options.report))
        return false;
    }
    else if (arg == "--x" || arg == "--y" || arg == "--z")
    {
      if (!need(i, arg.c_str(), text))
        return false;
      double* target = arg == "--x" ? &options.x : arg == "--y" ? &options.y : &options.z;
      if (!parse_number(text, *target))
      {
        error = arg + " expects a number, got '" + text + "'";
        return false;
      }
    }
    else if (arg == "--strength" || arg == "--in-db" || arg == "--out-db" || arg == "--ramp-ms" ||
             arg == "--rate")
    {
      if (!need(i, arg.c_str(), text))
        return false;
      double value = 0.0;
      if (!parse_number(text, value))
      {
        error = arg + " expects a number, got '" + text + "'";
        return false;
      }
      if (arg == "--strength")
        options.strength = value;
      else if (arg == "--in-db")
        options.in_db = value;
      else if (arg == "--out-db")
        options.out_db = value;
      else if (arg == "--ramp-ms")
        options.ramp_ms = value;
      else
        options.rate = value;
    }
    else if (arg == "--block")
    {
      if (!need(i, "--block", text))
        return false;
      if (!parse_int(text, options.block))
      {
        error = "--block expects an integer in [1, " + std::to_string(kMaxBlock) + "], got '" + text + "'";
        return false;
      }
    }
    else if (arg == "--extend")
      options.extend = true;
    else if (arg == "--auto-gain")
      options.auto_gain = true;
    else if (arg == "--analyze")
      options.analyze = true;
    else if (arg == "--json")
      options.json = true;
    else if (arg == "--quiet")
      options.quiet = true;
    else
    {
      error = "unknown option '" + arg + "' (try --help)";
      return false;
    }
  }

  if (!options.analyze && options.output.empty())
  {
    error = "--output is required unless --analyze is given";
    return false;
  }
  if (!options.analyze && options.input.empty())
  {
    error = "--input is required unless --analyze is given";
    return false;
  }
  if (!options.input.empty() && options.analyze == false && options.output == options.input)
  {
    error = "--output must differ from --input";
    return false;
  }
  return true;
}

bool validate(const Options& options, std::string& error)
{
  const auto in_range = [&](double value, double low, double high, const char* name) {
    if (!std::isfinite(value) || value < low || value > high)
    {
      error = std::string("--") + name + " must be in [" + std::to_string(low) + ", " +
              std::to_string(high) + "]";
      return false;
    }
    return true;
  };
  if (!in_range(options.x, -kLatentLimit, kLatentLimit, "x") ||
      !in_range(options.y, -kLatentLimit, kLatentLimit, "y") ||
      !in_range(options.z, -kLatentLimit, kLatentLimit, "z") ||
      !in_range(options.strength, 0.0, 1.0, "strength") ||
      !in_range(options.in_db, kGainMinDb, kGainMaxDb, "in-db") ||
      !in_range(options.out_db, kGainMinDb, kGainMaxDb, "out-db") ||
      !in_range(options.ramp_ms, 0.0, kRampMaxMs, "ramp-ms") ||
      !in_range(options.rate, 8000.0, 384000.0, "rate"))
    return false;
  return true;
}

Graph make_graph(const Options& options)
{
  NodeSpec eq;
  eq.id = "eq";
  eq.type = kTypeId;
  eq.element = "eq";
  eq.params = {{"x", options.x},
               {"y", options.y},
               {"z", options.z},
               {"strength", options.strength},
               {"in_db", options.in_db},
               {"out_db", options.out_db},
               {"ramp_ms", options.ramp_ms}};
  if (options.extend)
    eq.params["extend"] = 1.0;
  if (options.auto_gain)
    eq.params["auto_gain"] = 1.0;

  Graph graph;
  graph.nodes = {NodeSpec{"in", "nc.input"}, std::move(eq), NodeSpec{"out", "nc.output"}};
  graph.connections = {Connection{"in", 0, "eq", 0}, Connection{"eq", 0, "out", 0}};
  return graph;
}

double rms_of(const std::vector<float>& samples)
{
  if (samples.empty())
    return 0.0;
  double sum = 0.0;
  for (const float sample : samples)
    sum += static_cast<double>(sample) * static_cast<double>(sample);
  return std::sqrt(sum / static_cast<double>(samples.size()));
}

double peak_of(const std::vector<float>& samples)
{
  double peak = 0.0;
  for (const float sample : samples)
    peak = std::max(peak, std::abs(static_cast<double>(sample)));
  return peak;
}

struct ChannelResult
{
  std::vector<float> samples;
  nlohmann::json state;
  double peak = 0.0;
  double rms_in = 0.0;
  double rms_out = 0.0;
};

bool render_channel(const Options& options, double rate, const std::vector<float>& input,
                    ChannelResult& result, std::string& error)
{
  namchain::rt::Arena arena(1u << 22); // the same arena size the flowEQ tests use
  CompileOptions compile;
  compile.max_quantum = options.block;
  compile.sample_rate = rate;
  compile.levels = false; // no meters: this is an offline render, not a device plan

  auto plan = RenderPlan::compile(make_graph(options), compile, arena, error);
  if (!plan)
    return false;

  result.samples.assign(input.size(), 0.0f);
  for (std::size_t offset = 0; offset < input.size(); offset += static_cast<std::size_t>(options.block))
  {
    const int frames = static_cast<int>(
        std::min<std::size_t>(static_cast<std::size_t>(options.block), input.size() - offset));
    plan->execute(input.data() + offset, result.samples.data() + offset, frames);
  }

  const int index = plan->index_of("eq");
  if (index < 0)
  {
    error = "the compiled plan has no 'eq' node";
    return false;
  }
  const namchain::graph::PlanNode& node = plan->nodes()[static_cast<std::size_t>(index)];
  if (node.type == nullptr || node.type->describe_state == nullptr)
  {
    error = "the compiled node has no describe_state";
    return false;
  }

  const std::string text = node.type->describe_state(node.state);
  if (text.empty())
  {
    error = "the node's describe_state returned nothing";
    return false;
  }
  try
  {
    result.state = nlohmann::json::parse(text);
  }
  catch (const std::exception& exception)
  {
    error = std::string("could not parse the node's state: ") + exception.what();
    return false;
  }

  result.rms_in = rms_of(input);
  result.rms_out = rms_of(result.samples);
  result.peak = peak_of(result.samples);
  return true;
}

nlohmann::json settings_json(const Options& options)
{
  return {{"x", options.x},
          {"y", options.y},
          {"z", options.z},
          {"strength", options.strength},
          {"extend", options.extend},
          {"auto_gain", options.auto_gain},
          {"in_db", options.in_db},
          {"out_db", options.out_db},
          {"ramp_ms", options.ramp_ms}};
}

void print_summary(const Options& options, double rate, unsigned channels, std::size_t frames,
                   const std::vector<ChannelResult>& results)
{
  std::printf("floweq-eq: %s", options.input.empty() ? "(no input)" : options.input.c_str());
  if (!options.output.empty())
    std::printf(" -> %s", options.output.c_str());
  std::printf("\n");
  std::printf("  %.0f Hz, %u channel%s, %zu frames (%.3f s), block %d\n", rate, channels,
              channels == 1 ? "" : "s", frames, rate > 0.0 ? static_cast<double>(frames) / rate : 0.0,
              options.block);
  std::printf(
      "  x=%.3f y=%.3f z=%.3f strength=%.3f extend=%s auto_gain=%s in=%.2f dB out=%.2f dB ramp=%.1f ms\n",
      options.x, options.y, options.z, options.strength, options.extend ? "on" : "off",
      options.auto_gain ? "on" : "off", options.in_db, options.out_db, options.ramp_ms);

  for (std::size_t channel = 0; channel < results.size(); ++channel)
  {
    const ChannelResult& result = results[channel];
    if (results.size() > 1)
      std::printf("  channel %zu:\n", channel);

    const auto& state = result.state;
    std::printf("    model %s, output gain %.2f dB\n", state.value("model", "?").c_str(),
                state.value("gain_db", 0.0));
    std::printf("    bands:\n");
    if (state.contains("bands") && state["bands"].is_array())
    {
      for (const auto& band : state["bands"])
      {
        std::printf("      %-10s %+7.2f dB @ %8.2f Hz  Q %.3f\n", band.value("type", "?").c_str(),
                    band.value("gain_db", 0.0), band.value("freq", 0.0), band.value("q", 0.0));
      }
    }
    if (state.contains("curve") && state["curve"].contains("points") &&
        state["curve"]["points"].is_array() && !state["curve"]["points"].empty())
    {
      double low = std::numeric_limits<double>::infinity();
      double high = -std::numeric_limits<double>::infinity();
      for (const auto& point : state["curve"]["points"])
      {
        const double db = point.get<double>();
        low = std::min(low, db);
        high = std::max(high, db);
      }
      std::printf("    response: %+.2f .. %+.2f dB over %.0f .. %.0f Hz\n", low, high,
                  state["curve"].value("f_min", 20.0), state["curve"].value("f_max", 20000.0));
    }
    if (!result.samples.empty())
    {
      const double peak_db = result.peak > 0.0 ? 20.0 * std::log10(result.peak) : -std::numeric_limits<double>::infinity();
      const double change_db = result.rms_in > 0.0 && result.rms_out > 0.0
                                   ? 20.0 * std::log10(result.rms_out / result.rms_in)
                                   : 0.0;
      std::printf("    peak %.2f dBFS, RMS change %+.2f dB\n", peak_db, change_db);
      if (result.peak > 1.0)
        std::printf("    warning: peak exceeds 0 dBFS; some players will clip this file\n");
    }
  }
}

bool ensure_parent(const std::string& path, std::string& error)
{
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty())
    return true;
  std::error_code code;
  std::filesystem::create_directories(parent, code);
  if (code)
  {
    error = "cannot create '" + parent.string() + "': " + code.message();
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv)
{
  Options options;
  std::string error;
  if (!parse_args(argc, argv, options, error) || !validate(options, error))
  {
    std::fprintf(stderr, "floweq-eq: %s\n", error.c_str());
    return 1;
  }

  std::vector<float> interleaved;
  unsigned channels = 1;
  double rate = options.rate;
  std::size_t frames = 0;

  if (!options.input.empty())
  {
    if (!namchain::load_wav(options.input, interleaved, channels, rate, error))
    {
      std::fprintf(stderr, "floweq-eq: %s\n", error.c_str());
      return 1;
    }
    if (rate <= 0.0 || channels == 0)
    {
      std::fprintf(stderr, "floweq-eq: '%s' has no usable audio\n", options.input.c_str());
      return 1;
    }
    frames = interleaved.size() / channels;
  }

  std::vector<ChannelResult> results;
  results.reserve(channels);
  for (unsigned channel = 0; channel < channels; ++channel)
  {
    std::vector<float> samples(frames, 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame)
      samples[frame] = interleaved[frame * channels + channel];
    ChannelResult result;
    if (!render_channel(options, rate, samples, result, error))
    {
      std::fprintf(stderr, "floweq-eq: %s\n", error.c_str());
      return 1;
    }
    results.push_back(std::move(result));
  }

  nlohmann::json report;
  report["input"] = options.input.empty() ? nlohmann::json(nullptr) : nlohmann::json(options.input);
  report["output"] = options.output.empty() ? nlohmann::json(nullptr) : nlohmann::json(options.output);
  report["sample_rate"] = rate;
  report["channels"] = channels;
  report["frames"] = frames;
  report["block"] = options.block;
  report["settings"] = settings_json(options);
  report["channel_states"] = nlohmann::json::array();
  for (std::size_t channel = 0; channel < results.size(); ++channel)
  {
    const ChannelResult& result = results[channel];
    nlohmann::json entry;
    entry["channel"] = channel;
    entry["state"] = result.state;
    if (frames > 0)
    {
      entry["peak"] = result.peak;
      entry["peak_dbfs"] = result.peak > 0.0 ? 20.0 * std::log10(result.peak) : -120.0;
      entry["rms_in"] = result.rms_in;
      entry["rms_out"] = result.rms_out;
      entry["rms_change_db"] =
          result.rms_in > 0.0 && result.rms_out > 0.0 ? 20.0 * std::log10(result.rms_out / result.rms_in) : 0.0;
    }
    report["channel_states"].push_back(std::move(entry));
  }

  if (options.analyze == false)
  {
    if (!ensure_parent(options.output, error))
    {
      std::fprintf(stderr, "floweq-eq: %s\n", error.c_str());
      return 1;
    }
    for (unsigned channel = 0; channel < channels; ++channel)
    {
      for (std::size_t frame = 0; frame < frames; ++frame)
        interleaved[frame * channels + channel] = results[channel].samples[frame];
    }
    if (!namchain::save_wav_float32(options.output, interleaved.data(), frames,
                                    static_cast<int>(channels), rate, error))
    {
      std::fprintf(stderr, "floweq-eq: %s\n", error.c_str());
      return 1;
    }
  }

  if (!options.report.empty())
  {
    if (!ensure_parent(options.report, error))
    {
      std::fprintf(stderr, "floweq-eq: %s\n", error.c_str());
      return 1;
    }
    std::ofstream out(options.report);
    if (!out)
    {
      std::fprintf(stderr, "floweq-eq: cannot write '%s'\n", options.report.c_str());
      return 1;
    }
    out << report.dump(2) << "\n";
    if (!out.good())
    {
      std::fprintf(stderr, "floweq-eq: write failed for '%s'\n", options.report.c_str());
      return 1;
    }
  }

  if (options.json)
    std::printf("%s\n", report.dump(2).c_str());
  else if (!options.quiet)
    print_summary(options, rate, channels, frames, results);

  return 0;
}
