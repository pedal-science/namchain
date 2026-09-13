// namchain-soak — run a bounded soak and judge it against the plan's acceptance criterion.
//
// This is the M1 acceptance harness (plan.md §1 criterion 3, §10.5). It reports the evidence
// rather than a verdict: block-time percentiles, deadline overruns, driver-reported xruns, and
// — when asked — whether the audio a real-time-shaped run produced is sample-identical to the
// deterministic offline render of the same input.
//
// Usage: namchain-soak [options] <model.nam> <input.wav>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <json.hpp>

#include "namchain/soak.h"
#include "namchain/version.h"

namespace
{

void usage()
{
  std::fprintf(stderr,
               "namchain-soak %s\n"
               "usage: namchain-soak [options] <model.nam> <input.wav>\n"
               "       namchain-soak --graph rig.json [options] <input.wav>\n"
               "options:\n"
               "  --quantum N          block size, 1..4096 (default 64)\n"
               "  --seconds S          audio seconds to process (default 60)\n"
               "  --input-offset S     skip S seconds of input and loop back to there; the example\n"
               "                       fixture is 1 s silence then 1 s signal, so looping from 0\n"
               "                       spends half the run on silence\n"
               "  --no-pacing          run flat out; measures throughput, not lateness\n"
               "  --self-oracle        compare output against an independent render of the same input\n"
               "                       (a rig compares against the reference interpreter in-process)\n"
               "  --oracle FILE        compare output against this WAV instead\n"
               "  --output FILE        write the rendered audio here\n"
               "  --tolerance X        oracle tolerance (default 1e-5)\n"
               "  --oracle-window S    device mode: seconds of the run the self-oracle compares\n"
               "                       (default 60; the input has to be kept to re-render it)\n"
               "  --sample-rate N      engine rate in Hz (default 48000)\n"
               "  --max-p99-percent X  gate on p99 block time, %% of deadline (default 50)\n"
               "  --json               print a JSON summary instead of text\n"
               "stream mode (a backend drives the loop a period at a time):\n"
               "  --backend KIND       alsa_raw (default), null, or file. With null or file no\n"
               "                       device is needed; file reads <input.wav> and writes --output\n"
               "  --device hw:C,D      capture and play back through a real ALSA device\n"
               "  --channels N         channel count for both directions (default 1)\n"
               "  --capture-channels N device capture channels, when it differs (SSL 2+ Mk II: 8)\n"
               "  --playback-channels N device playback channels (SSL 2+ Mk II: 6)\n"
               "  --periods N          buffer length in periods (default 4)\n"
               "  --model FILE         run this .nam between capture and playback\n"
               "  --instances N        chain N copies of the model (default 1)\n"
               "  --graph FILE         run this rig instead: a JSON graph of nodes and connections\n"
               "  --input-channel N    device channel feeding the chain (default 0)\n"
               "  --output-channel N   device channel the chain writes to (default 0)\n"
               "exit: 0 pass, 1 failed a gate, 2 usage/IO error\n",
               NAMCHAIN_VERSION_STRING);
}

} // namespace

int main(int argc, char** argv)
{
  namchain::SoakOptions options;
  double max_p99_percent = 50.0;
  bool json = false;
  std::string model_path;
  std::string input_path;
  std::vector<std::string> positionals;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--quantum" && i + 1 < argc)
      options.quantum = std::atoi(argv[++i]);
    else if (arg == "--seconds" && i + 1 < argc)
      options.duration_seconds = std::atof(argv[++i]);
    else if (arg == "--input-offset" && i + 1 < argc)
      options.input_offset_seconds = std::atof(argv[++i]);
    else if (arg == "--no-pacing")
      options.realtime_paced = false;
    else if (arg == "--self-oracle")
      options.self_oracle = true;
    else if (arg == "--oracle" && i + 1 < argc)
      options.oracle_wav = argv[++i];
    else if (arg == "--output" && i + 1 < argc)
      options.output_wav = argv[++i];
    else if (arg == "--tolerance" && i + 1 < argc)
      options.oracle_tolerance = std::atof(argv[++i]);
    else if (arg == "--oracle-window" && i + 1 < argc)
      options.oracle_window_seconds = std::atof(argv[++i]);
    else if (arg == "--max-p99-percent" && i + 1 < argc)
      max_p99_percent = std::atof(argv[++i]);
    else if (arg == "--backend" && i + 1 < argc)
    {
      if (!namchain::parse_backend_kind(argv[++i], options.backend))
      {
        std::fprintf(stderr, "error: unknown backend '%s'; expected alsa_raw, null or file\n", argv[i]);
        return 2;
      }
    }
    else if (arg == "--device" && i + 1 < argc)
      options.device = argv[++i];
    else if (arg == "--channels" && i + 1 < argc)
      options.device_channels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--capture-channels" && i + 1 < argc)
      options.capture_channels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--playback-channels" && i + 1 < argc)
      options.playback_channels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--periods" && i + 1 < argc)
      options.device_periods = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--model" && i + 1 < argc)
      options.model = argv[++i];
    else if (arg == "--graph" && i + 1 < argc)
      options.graph_path = argv[++i];
    else if (arg == "--sample-rate" && i + 1 < argc)
      options.sample_rate = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--instances" && i + 1 < argc)
      options.instances = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--input-channel" && i + 1 < argc)
      options.input_channel = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--output-channel" && i + 1 < argc)
      options.output_channel = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--json")
      json = true;
    else if (arg == "-h" || arg == "--help")
    {
      usage();
      return 0;
    }
    else if (!arg.empty() && arg[0] == '-')
    {
      std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      usage();
      return 2;
    }
    else
      positionals.push_back(arg);
  }

  // A rig and a model are two ways of saying the same thing, so accepting both would mean silently
  // choosing one. Say so instead.
  if (!options.graph_path.empty() && !options.model.empty())
  {
    std::fprintf(stderr, "error: --graph and --model are alternatives; give one\n");
    return 2;
  }
  // What the positionals mean depends on how the graph was given, so they are collected first and
  // assigned here:
  //     --graph RIG in.wav          --model M in.wav          model.nam in.wav
  // In the first two, the single positional is the input; in the third there are two.
  //
  // This used to be three separate special cases, and they disagreed: `--model X in.wav` set
  // `options.model` but left the positional to be read as the model path, so the run was rejected
  // with a usage message while `X in.wav` worked. A flag and its positional equivalent behaving
  // differently is exactly the sort of thing that gets measured one way and quoted the other.
  const bool source_from_flag = !options.graph_path.empty() || !options.model.empty();
  if (source_from_flag)
  {
    if (positionals.size() > 1)
    {
      std::fprintf(stderr, "error: unexpected extra argument '%s'\n", positionals[1].c_str());
      return 2;
    }
    if (!positionals.empty())
      input_path = positionals[0];
    // The offline path builds its graph from the positional path rather than from the option, so
    // the two are kept equal here instead of being allowed to drift apart.
    if (!options.model.empty())
      model_path = options.model;
  }
  else
  {
    if (positionals.size() > 2)
    {
      std::fprintf(stderr, "error: unexpected extra argument '%s'\n", positionals[2].c_str());
      return 2;
    }
    if (!positionals.empty())
      model_path = positionals[0];
    if (positionals.size() > 1)
      input_path = positionals[1];
  }

  const bool have_source = !options.graph_path.empty() || !model_path.empty();
  // In offline mode a source and an input file are both required. In stream mode the chain is
  // optional (passthrough is a real measurement of the host), so only a missing device has to be
  // reported -- and the backend itself says that far better than a usage dump.
  if (options.device.empty() && options.backend == namchain::BackendKind::AlsaRaw &&
      (!have_source || input_path.empty()))
  {
    usage();
    return 2;
  }

  const namchain::SoakResult result = namchain::run_soak(model_path, input_path, options);
  if (!result.ok)
  {
    std::fprintf(stderr, "error: %s\n", result.error.c_str());
    return 2;
  }

  // The acceptance criterion, as a machine-checkable gate rather than a judgement call.
  const bool p99_gate = result.p99_percent_of_deadline <= max_p99_percent;
  const bool overrun_gate = result.deadline_overruns == 0 && result.driver_xruns == 0;
  const bool oracle_gate = !result.oracle.checked || result.oracle.match;
  const bool pass = p99_gate && overrun_gate && oracle_gate;

  if (json)
  {
    nlohmann::json j;
    j["model"] = model_path;
    j["input"] = input_path;
    j["quantum"] = options.quantum;
    j["realtime_paced"] = options.realtime_paced;
    j["blocks"] = result.blocks;
    j["frames"] = result.frames;
    j["audio_seconds"] = result.audio_seconds;
    j["wall_seconds"] = result.wall_seconds;
    j["rtf"] = result.rtf;
    j["deadline_ns"] = result.deadline_ns;
    j["block_ns"] = {{"p50", result.p50_ns}, {"p99", result.p99_ns}, {"max", result.max_ns}};
    j["p99_percent_of_deadline"] = result.p99_percent_of_deadline;
    j["max_lateness_ms"] = result.max_lateness_ms;
    j["device_wait_ns"] = {{"p50", result.wait_p50_ns}, {"p99", result.wait_p99_ns}, {"max", result.wait_max_ns}};
    j["deadline_overruns"] = result.deadline_overruns;
    j["plan"] = {{"source", result.graph_source},
                 {"graph_mode", result.graph_mode},
                 {"nodes", result.plan_nodes},
                 {"buffers", result.plan_buffers},
                 {"buffer_bytes", result.plan_buffer_bytes},
                 {"reference_buffers", result.reference_buffers}};
    j["rt_check"] = {{"instrumented", result.rt_instrumented},
                     {"allocations", result.rt_allocations},
                     {"deallocations", result.rt_deallocations}};
    j["driver_xruns"] = result.driver_xruns;
    if (result.stream_mode)
    {
      j["device"] = {{"capture_node", result.device_capture_node},
                     {"playback_node", result.device_playback_node},
                     {"period_size", result.device_period_size},
                     {"buffer_size", result.device_buffer_size},
                     {"period_exact", result.device_period_exact},
                     {"sample_format", result.device_sample_format},
                     {"bytes_per_sample", result.device_bytes_per_sample},
                     {"chain_instances", result.chain_instances},
                     {"chain_model", result.chain_model},
                     {"startup_xruns", result.startup_driver_xruns},
                     {"model_type", result.model_type}};
    }
    if (result.oracle.checked)
    {
      j["oracle"] = {{"source", result.oracle.source},
                     {"match", result.oracle.match},
                     {"frames", result.oracle.frames},
                     {"max_abs", result.oracle.max_abs},
                     {"rms", result.oracle.rms},
                     {"detail", result.oracle.detail}};
    }
    j["pass"] = pass;
    std::printf("%s\n", j.dump(2).c_str());
  }
  else
  {
    if (result.stream_mode)
    {
      std::printf("device:           %s -> %s\n", result.device_capture_node.c_str(),
                  result.device_playback_node.c_str());
      std::printf("period/buffer:    %u / %u frames%s\n", result.device_period_size, result.device_buffer_size,
                  result.device_period_exact ? "" : " (rounded by the device)");
      std::printf("sample format:    %s (%u bytes/sample)\n", result.device_sample_format.c_str(),
                  result.device_bytes_per_sample);
      std::printf("mlockall:         %s\n", result.memory_locked ? "locked" : "FAILED (measurement includes page faults)");
      if (result.chain_instances > 0)
        std::printf("chain:            %u x %s\n", result.chain_instances, result.chain_model.c_str());
      if (!result.model_type.empty())
        std::printf("model type:       %s\n", result.model_type.c_str());
    }
    else
    {
      std::printf("model:            %s\n", model_path.c_str());
    }
    std::printf("plan:             %s, %u nodes, %u buffers (%u bytes)\n", result.graph_source.c_str(),
                result.plan_nodes, result.plan_buffers, result.plan_buffer_bytes);
    if (result.reference_buffers > 0)
      std::printf("                  a naive one-buffer-per-value layout would need %u\n",
                  result.reference_buffers);
    std::printf("quantum:          %d\n", options.quantum);
    std::printf("pacing:           %s\n", options.realtime_paced ? "wall clock" : "flat out");
    std::printf("blocks:           %llu\n", static_cast<unsigned long long>(result.blocks));
    std::printf("audio:            %.3f s in %.3f s wall (RTF %.4f)\n", result.audio_seconds, result.wall_seconds,
                result.rtf);
    std::printf("deadline:         %llu ns\n", static_cast<unsigned long long>(result.deadline_ns));
    if (result.stream_mode)
      std::printf("split (p50):      read %llu ns, chain %llu ns, write %llu ns\n",
                  static_cast<unsigned long long>(result.read_p50_ns),
                  static_cast<unsigned long long>(result.chain_p50_ns),
                  static_cast<unsigned long long>(result.write_p50_ns));
    if (result.stream_mode)
      std::printf("device wait:      p50 %llu ns, p99 %llu ns, max %llu ns\n",
                  static_cast<unsigned long long>(result.wait_p50_ns),
                  static_cast<unsigned long long>(result.wait_p99_ns),
                  static_cast<unsigned long long>(result.wait_max_ns));
    std::printf("block time:       p50 %llu ns, p99 %llu ns, max %llu ns\n",
                static_cast<unsigned long long>(result.p50_ns), static_cast<unsigned long long>(result.p99_ns),
                static_cast<unsigned long long>(result.max_ns));
    std::printf("p99/deadline:     %.2f%% (gate <= %.2f%%)\n", result.p99_percent_of_deadline, max_p99_percent);
    std::printf("max lateness:     %.3f ms\n", result.max_lateness_ms);
    if (result.rt_instrumented)
      std::printf("render-path alloc:%llu allocations, %llu frees (must be 0)\n",
                  static_cast<unsigned long long>(result.rt_allocations),
                  static_cast<unsigned long long>(result.rt_deallocations));
    else
      std::printf("render-path alloc:not instrumented\n");
    if (result.stream_mode && result.startup_driver_xruns > 0)
      std::printf("bring-up xruns:   %llu (before the measured loop)\n",
                  static_cast<unsigned long long>(result.startup_driver_xruns));
    if (result.stream_mode && result.first_overrun_block != UINT64_MAX)
      std::printf("first overrun at: block %llu of %llu\n",
                  static_cast<unsigned long long>(result.first_overrun_block),
                  static_cast<unsigned long long>(result.blocks));
    std::printf("deadline overruns:%llu\n", static_cast<unsigned long long>(result.deadline_overruns));
    std::printf("driver xruns:     %llu%s\n", static_cast<unsigned long long>(result.driver_xruns),
                (!result.stream_mode && result.driver_xruns == 0) ? " (no device backend in this mode)" : "");
    if (result.oracle.checked)
    {
      std::printf("oracle:           %s vs %s\n", result.oracle.match ? "match" : "MISMATCH",
                  result.oracle.source.c_str());
      std::printf("  max|diff|:      %.9g over %zu frames\n", result.oracle.max_abs, result.oracle.frames);
      if (!result.oracle.detail.empty())
        std::printf("  detail:         %s\n", result.oracle.detail.c_str());
    }
    std::printf("result:           %s\n", pass ? "PASS" : "FAIL");
  }

  return pass ? 0 : 1;
}
