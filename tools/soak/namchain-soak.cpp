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

#include <sound/asound.h>

#include <json.hpp>

#include "namchain/soak.h"
#include "namchain/version.h"

namespace
{

const char* format_name(unsigned format)
{
  switch (format)
  {
  case SNDRV_PCM_FORMAT_S16_LE: return "S16_LE";
  case SNDRV_PCM_FORMAT_S24_3LE: return "S24_3LE";
  case SNDRV_PCM_FORMAT_S32_LE: return "S32_LE";
  default: return "unknown";
  }
}

void usage()
{
  std::fprintf(stderr,
               "namchain-soak %s\n"
               "usage: namchain-soak [options] <model.nam> <input.wav>\n"
               "options:\n"
               "  --quantum N          block size, 1..4096 (default 64)\n"
               "  --seconds S          audio seconds to process (default 60)\n"
               "  --input-offset S     skip S seconds of input and loop back to there; the example\n"
               "                       fixture is 1 s silence then 1 s signal, so looping from 0\n"
               "                       spends half the run on silence\n"
               "  --no-pacing          run flat out; measures throughput, not lateness\n"
               "  --self-oracle        compare output against an offline render of the same input\n"
               "  --oracle FILE        compare output against this WAV instead\n"
               "  --output FILE        write the rendered audio here\n"
               "  --tolerance X        oracle tolerance (default 1e-5)\n"
               "  --max-p99-percent X  gate on p99 block time, %% of deadline (default 50)\n"
               "  --json               print a JSON summary instead of text\n"
               "device mode (passthrough, no model):\n"
               "  --device hw:C,D      capture and play back through a real ALSA device\n"
               "  --channels N         1 or 2 (default 1)\n"
               "  --periods N          buffer length in periods (default 4)\n"
               "  --model FILE         run this .nam between capture and playback\n"
               "  --instances N        chain N copies of the model (default 1)\n"
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
    else if (arg == "--max-p99-percent" && i + 1 < argc)
      max_p99_percent = std::atof(argv[++i]);
    else if (arg == "--device" && i + 1 < argc)
      options.device = argv[++i];
    else if (arg == "--channels" && i + 1 < argc)
      options.device_channels = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--periods" && i + 1 < argc)
      options.device_periods = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--model" && i + 1 < argc)
      options.model = argv[++i];
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
    else if (model_path.empty())
      model_path = arg;
    else if (input_path.empty())
      input_path = arg;
    else
    {
      std::fprintf(stderr, "error: unexpected extra argument '%s'\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (options.device.empty() && (model_path.empty() || input_path.empty()))
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
    j["rt_check"] = {{"instrumented", result.rt_instrumented},
                     {"allocations", result.rt_allocations},
                     {"deallocations", result.rt_deallocations}};
    j["driver_xruns"] = result.driver_xruns;
    if (result.device_mode)
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
                     {"startup_xruns", result.startup_driver_xruns}};
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
    if (result.device_mode)
    {
      std::printf("device:           %s -> %s\n", result.device_capture_node.c_str(),
                  result.device_playback_node.c_str());
      std::printf("period/buffer:    %u / %u frames%s\n", result.device_period_size, result.device_buffer_size,
                  result.device_period_exact ? "" : " (rounded by the device)");
      std::printf("sample format:    %s (%u bytes/sample)\n", format_name(result.device_sample_format),
                  result.device_bytes_per_sample);
      std::printf("mlockall:         %s\n", result.memory_locked ? "locked" : "FAILED (measurement includes page faults)");
      if (result.chain_instances > 0)
        std::printf("chain:            %u x %s\n", result.chain_instances, result.chain_model.c_str());
    }
    else
    {
      std::printf("model:            %s\n", model_path.c_str());
    }
    std::printf("quantum:          %d\n", options.quantum);
    std::printf("pacing:           %s\n", options.realtime_paced ? "wall clock" : "flat out");
    std::printf("blocks:           %llu\n", static_cast<unsigned long long>(result.blocks));
    std::printf("audio:            %.3f s in %.3f s wall (RTF %.4f)\n", result.audio_seconds, result.wall_seconds,
                result.rtf);
    std::printf("deadline:         %llu ns\n", static_cast<unsigned long long>(result.deadline_ns));
    if (result.device_mode)
      std::printf("split (p50):      read %llu ns, chain %llu ns, write %llu ns\n",
                  static_cast<unsigned long long>(result.read_p50_ns),
                  static_cast<unsigned long long>(result.chain_p50_ns),
                  static_cast<unsigned long long>(result.write_p50_ns));
    if (result.device_mode)
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
    if (result.device_mode && result.startup_driver_xruns > 0)
      std::printf("bring-up xruns:   %llu (before the measured loop)\n",
                  static_cast<unsigned long long>(result.startup_driver_xruns));
    if (result.device_mode && result.first_overrun_block != UINT64_MAX)
      std::printf("first overrun at: block %llu of %llu\n",
                  static_cast<unsigned long long>(result.first_overrun_block),
                  static_cast<unsigned long long>(result.blocks));
    std::printf("deadline overruns:%llu\n", static_cast<unsigned long long>(result.deadline_overruns));
    std::printf("driver xruns:     %llu%s\n", static_cast<unsigned long long>(result.driver_xruns),
                (!result.device_mode && result.driver_xruns == 0) ? " (no device backend in this mode)" : "");
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
