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
               "  --max-output-drops N  gate on periods dropped for want of output room (default 0; with\n"
               "                       --output-queue-periods the drop gate is off unless this is given,\n"
               "                       because a target trades a dropped period for latency by design)\n"
               "  --rt-priority N  request SCHED_FIFO at this priority, applied by the program (default 80,\n"
               "                   0 to leave the policy alone); a refusal is reported, not fatal\n"
               "  --cpu N          confine the loop thread to one CPU (default: leave the affinity alone)\n"
               "  --json               print a JSON summary instead of text\n"
               "stream mode (a backend drives the loop a period at a time):\n"
               "  --backend KIND       alsa_raw (default), null, or file. With null or file no\n"
               "                       device is needed; file reads <input.wav> and writes --output\n"
               "  --device hw:C,D      capture and play back through a real ALSA device\n"
               "  --channels N         channel count for both directions (default 1)\n"
               "  --capture-channels N device capture channels, when it differs (SSL 2+ Mk II: 8)\n"
               "  --playback-channels N device playback channels (SSL 2+ Mk II: 6)\n"
               "  --periods N          buffer length in periods (default 4)\n"
               "  --output-queue-periods N  cap the output queue at N periods of processed audio: the\n"
               "                       latency this run is willing to carry (default 0: no cap, write\n"
               "                       whenever the kernel has room). A cap, not a cushion -- what a run\n"
               "                       actually holds is measured and printed. Must be less than --periods,\n"
               "                       and wants a ring with headroom above it (plan.md 9.5 item 8)\n"
               "  --output-queue-floor-periods N  insert a period of silence when the queue falls below\n"
               "                       N periods, so it never reaches the driver's underrun. Costs 1.33 ms\n"
               "                       of inserted audio each time and no timeline slip, where an underrun\n"
               "                       discards the whole queued ring. Default 0 (off); must be <= the cap\n"
               "  --splice-fade-ms X   bridge a dropped or inserted period over X ms, leaving from the\n"
               "                       last emitted sample and its slope so the splice is not a click\n"
               "                       (default 0: off). Applied to the emitted signal only, so the oracle\n"
               "                       still checks the engine's pristine output\n"
               "  --inject-splice-every S  DIAGNOSTIC: lose one output period every S seconds, so the\n"
               "                       repair can be judged on isolated splices in a session that\n"
               "                       produces none. Reported separately from real drops\n"
               "  --splice-verify      judge each splice on the emitted signal with the glitch detector,\n"
               "                       against the raw splice as its oracle (needs no bridge; with the\n"
               "                       bridge off this measures the click itself)\n"
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
  unsigned long long max_output_drops = 0;
  bool drops_gate_given = false;
  int rt_priority = 80;
  int cpu = -1;
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
    else if (arg == "--max-output-drops" && i + 1 < argc)
    {
      max_output_drops = std::strtoull(argv[++i], nullptr, 10);
      drops_gate_given = true;
    }
    else if (arg == "--rt-priority" && i + 1 < argc)
      rt_priority = std::atoi(argv[++i]);
    else if (arg == "--cpu" && i + 1 < argc)
      cpu = std::atoi(argv[++i]);
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
    else if (arg == "--output-queue-periods" && i + 1 < argc)
      options.output_queue_periods = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--output-queue-floor-periods" && i + 1 < argc)
      options.output_queue_floor_periods = static_cast<unsigned>(std::atoi(argv[++i]));
    else if (arg == "--splice-fade-ms" && i + 1 < argc)
      options.splice_fade_ms = std::atof(argv[++i]);
    else if (arg == "--splice-verify")
      options.splice_verify = true;
    else if (arg == "--inject-splice-every" && i + 1 < argc)
      options.inject_splice_seconds = std::atof(argv[++i]);
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
      input_path = positionals[0];    // The offline path builds its graph from the positional path rather than from the option, so
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

  // The scheduling requests are applied by the program itself, so they travel in the options rather
  // than being a property of how the process was launched.
  options.rt_priority = rt_priority;
  options.cpu = cpu;

  // Refused by name rather than ignored. Only the ALSA backend has an output ring to hold a queue in,
  // and a run that reports a latency it is not actually carrying is worse than one that will not
  // start -- the whole point of the knob is that the number is a stated trade (plan.md §9.5 item 7).
  if ((options.output_queue_periods != 0 || options.output_queue_floor_periods != 0) &&
      options.backend != namchain::BackendKind::AlsaRaw)
  {
    std::fprintf(stderr,
                 "error: the output-queue settings only apply to --backend alsa_raw; the %s backend has no\n"
                 "       output ring to hold a queue in, so they would be reported and not applied\n",
                 namchain::backend_kind_name(options.backend));
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
  // A dropped period is 1.33 ms of audio removed from the output, chosen by us rather than by the
  // driver. It is strictly better than the blocking write it replaces -- the engine keeps its
  // deadline -- but it is still an audible discontinuity, so the default gate is zero.
  //
  // With a queue target set, a drop is not an accident, it is the trade: the target's whole purpose is
  // to refuse the period that would have raised the queue, so every refusal is the policy working and
  // a zero-drop gate would make a targeted run unable to pass. The gate is therefore off by default in
  // target mode -- stated in the report rather than left implicit, and restored the moment an explicit
  // --max-output-drops is given. What a target buys is the absence of *playback underruns*, and that
  // is what the criterion should be read against.
  const bool drop_gate =
    drops_gate_given ? result.output_drops <= max_output_drops
                     : (options.output_queue_periods == 0 ? result.output_drops == 0 : true);
  const bool oracle_gate = !result.oracle.checked || result.oracle.match;
  const bool pass = p99_gate && overrun_gate && drop_gate && oracle_gate;

  if (json)
  {
    nlohmann::json j;
    j["model"] = model_path;
    j["input"] = input_path;
    j["quantum"] = options.quantum;
    j["realtime_paced"] = options.realtime_paced;
    j["scheduling"] = {{"description", result.scheduling},
                       {"rt_priority_held", result.rt_priority_held},
                       {"cpu_pinned", result.cpu_pinned},
                       {"running_cpu", result.running_cpu},
                       {"requested_priority", options.rt_priority},
                       {"requested_cpu", options.cpu}};
    j["blocks"] = result.blocks;
    j["frames"] = result.frames;
    j["audio_seconds"] = result.audio_seconds;
    j["wall_seconds"] = result.wall_seconds;
    j["rtf"] = result.rtf;
    j["deadline_ns"] = result.deadline_ns;
    j["block_ns"] = {{"p50", result.p50_ns}, {"p99", result.p99_ns}, {"max", result.max_ns}};
    j["phase_ns"] = {{"read_p50", result.read_p50_ns},
                     {"read_p99", result.read_p99_ns},
                     {"read_max", result.read_max_ns},
                     {"chain_p50", result.chain_p50_ns},
                     {"chain_p99", result.chain_p99_ns},
                     {"chain_max", result.chain_max_ns},
                     {"write_p50", result.write_p50_ns},
                     {"write_p99", result.write_p99_ns},
                     {"write_max", result.write_max_ns}};
    j["render_path_cpu_ns"] = {{"max", result.chain_cpu_max_ns},
                               {"total", result.chain_cpu_ns},
                               {"wall_total", result.chain_wall_ns},
                               {"stall_blocks", result.chain_stall_blocks},
                               {"stall_threshold_ns", namchain::SoakResult::stall_threshold_ns}};
    {
      nlohmann::json slow = nlohmann::json::array();
      for (const namchain::SoakResult::SlowBlock& s : result.slow_blocks)
      {
        if (s.chain_cpu_ns == 0)
          continue;
        slow.push_back({{"block", s.block}, {"chain_cpu_ns", s.chain_cpu_ns}, {"chain_wall_ns", s.chain_wall_ns}});
      }
      j["slow_blocks"] = slow;
    }
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
      j["backend"] = result.backend;
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
      if (result.occupancy_measured)
        j["device"]["ring_occupancy_frames"] = {
          {"capture_min", result.capture_occupancy_min},
          {"capture_max", result.capture_occupancy_max},
          {"capture_last", result.capture_occupancy_last},
          {"playback_min", result.playback_occupancy_min},
          {"playback_max", result.playback_occupancy_max},
          {"playback_last", result.playback_occupancy_last},
          {"playback_p1", result.playback_occupancy_p1},
          {"playback_p50", result.playback_occupancy_p50},
          {"capture_p1", result.capture_occupancy_p1},
          {"capture_p50", result.capture_occupancy_p50},
          {"playback_below_period_blocks", result.playback_below_period_blocks},
          {"playback_full_blocks", result.playback_full_blocks}};
      j["device"]["queue"] = {{"output_drops", result.output_drops},
                              {"output_target_drops", result.output_target_drops},
                              {"output_queue_periods", result.output_queue_periods},
                              {"output_queue_floor_periods", result.output_queue_floor_periods},
                              {"floor_insertions", result.floor_insertions},
                              {"floor_inserted_frames", result.floor_inserted_frames},
                              {"capture_bursts", result.capture_bursts},
                              {"settle_discarded_frames", result.settle_discarded_frames},
                              {"prefill_frames", result.prefill_frames},
                              {"gate_capture_xruns", result.gate_capture_xruns},
                              {"occupancy_mismatches", result.occupancy_mismatches}};
      j["device"]["xruns_by_direction"] = {{"capture_overruns", result.capture_overruns},
                                           {"playback_underruns", result.playback_underruns}};
      j["device"]["splice_injected"] = {{"periods", result.injected_splices},
                                        {"every_seconds", options.inject_splice_seconds}};
      j["device"]["splice_measured"] = {{"judged", result.splice_verified},
                                        {"emitted_d2", result.splice_worst_d2},
                                        {"reference_d2", result.splice_reference_d2},
                                        {"raw_d2", result.splice_raw_d2},
                                        {"raw_reference_d2", result.splice_raw_reference_d2}};
      j["device"]["splice_repair"] = {{"bridge_frames", result.splice_bridge_frames},
                                      {"bridges", result.splice_bridges},
                                      {"for_drops", result.splice_bridges_for_drops},
                                      {"for_insertions", result.splice_bridges_for_insertions}};
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
    j["gates"] = {{"max_p99_percent", max_p99_percent},
                  {"max_output_drops", drops_gate_given ? nlohmann::json(max_output_drops) : nlohmann::json(nullptr)},
                  {"drop_gate_applied", drops_gate_given || options.output_queue_periods == 0},
                  {"deadline_overruns_zero", result.deadline_overruns == 0},
                  {"driver_xruns_zero", result.driver_xruns == 0},
                  {"oracle", oracle_gate}};
    std::printf("%s\n", j.dump(2).c_str());
  }
  else
  {
    if (result.stream_mode)
    {
      std::printf("backend:          %s\n", result.backend.c_str());
      std::printf("device:           %s -> %s\n", result.device_capture_node.c_str(),
                  result.device_playback_node.c_str());
      std::printf("period/buffer:    %u / %u frames%s\n", result.device_period_size, result.device_buffer_size,
                  result.device_period_exact ? "" : " (rounded by the device)");
      std::printf("sample format:    %s (%u bytes/sample)\n", result.device_sample_format.c_str(),
                  result.device_bytes_per_sample);
      std::printf("mlockall:         %s\n", result.memory_locked ? "locked" : "FAILED (measurement includes page faults)");
      std::printf("scheduling:       %s%s\n", result.scheduling.c_str(),
                  result.rt_priority_held ? "" : "  <- NOT real-time: this run says so rather than assuming");
      if (result.chain_instances > 0)
        std::printf("chain:            %u x %s\n", result.chain_instances, result.chain_model.c_str());
      if (!result.model_type.empty())
        std::printf("model type:       %s\n", result.model_type.c_str());
      if (result.occupancy_measured)
      {
        // Printed next to the device because it is a statement about how the loop was paced, not
        // about the engine: a capture minimum that never comes back to zero, or a playback maximum
        // at the buffer length, means the block percentiles below are describing a queue.
        //
        // `last` is the one that says which way a run was going. The ring's contents are conserved --
        // one period produced per period played -- so the cushion a queue target describes is spent by
        // lateness and restored only by a burst, and a max/min pair cannot tell a queue that held from
        // one that decayed to its final value. The last sample can.
        std::printf("ring occupancy:   capture min/max %lld/%lld, playback min/max %lld/%lld (last %lld) frames",
                    result.capture_occupancy_min, result.capture_occupancy_max, result.playback_occupancy_min,
                    result.playback_occupancy_max, result.playback_occupancy_last);
        if (result.playback_full_blocks > 0)
          std::printf(" (%llu blocks wrote into a full playback ring)",
                      static_cast<unsigned long long>(result.playback_full_blocks));
        std::printf("\n");
        // The same figure as the thing it actually buys: how long the loop can be absent before the
        // device runs out. The *minimum* is the number, not the target: the ring drains in steps
        // between the write and the next one, so the worst instant of a run is well below its mean.
        if (result.device_period_size > 0 && options.sample_rate > 0)
        {
          const double frames_per_period = static_cast<double>(result.device_period_size);
          const double ms_per_frame = 1e3 / static_cast<double>(options.sample_rate);
          std::printf("                  playback queue %.2f..%.2f periods (last %.2f); the floor is the "
                      "stall the device\n"
                      "                  absorbs without starving: %.2f periods = %.3f ms\n",
                      static_cast<double>(result.playback_occupancy_min) / frames_per_period,
                      static_cast<double>(result.playback_occupancy_max) / frames_per_period,
                      static_cast<double>(result.playback_occupancy_last) / frames_per_period,
                      static_cast<double>(result.playback_occupancy_min) / frames_per_period,
                      static_cast<double>(result.playback_occupancy_min) * ms_per_frame);
          // The distribution, which is the instrument the min/max pair cannot be: `min` is 0 in any run
          // that underran once, so it says nothing about a run that did not, and nothing about where the
          // stalls arrived. p1 is the floor asked properly; the below-one-period count is the exposure.
          std::printf("                  p1 %.2f, p50 %.2f periods; %llu of %llu blocks held less than one\n"
                      "                  period (%.2f %%), which is the window a stall has to miss\n",
                      static_cast<double>(result.playback_occupancy_p1) / frames_per_period,
                      static_cast<double>(result.playback_occupancy_p50) / frames_per_period,
                      static_cast<unsigned long long>(result.playback_below_period_blocks),
                      static_cast<unsigned long long>(result.blocks),
                      result.blocks == 0
                        ? 0.0
                        : 100.0 * static_cast<double>(result.playback_below_period_blocks) /
                            static_cast<double>(result.blocks));
        }
        // The cap, in the unit a listener cares about, and the line that says where the numbers that
        // matter are. The trade is stated before the figures that trade produced.
        if (result.output_queue_periods > 0)
        {
          // The cap in the unit a listener cares about, and explicitly *not* a promise of cushion.
          // Saying "3 periods = 4 ms of tolerance" would be the easy lie here: the ring's contents are
          // conserved, so the target only binds a burst that tries to grow the queue, while the level a
          // run actually holds is established before block 0 and spent by lateness. The floor is the
          // line above, and it is the number to read.
          const double cap_ms = options.sample_rate == 0
                                  ? 0.0
                                  : 1e3 * static_cast<double>(result.output_queue_periods) *
                                      static_cast<double>(result.device_period_size) /
                                      static_cast<double>(options.sample_rate);
          std::printf("queue target:     %u periods = %.3f ms: a cap on the ring's contents, so a burst\n"
                      "                  cannot buy latency beyond it. Not a promise of cushion -- the\n"
                      "                  floor held above is the number to read\n",
                      result.output_queue_periods, cap_ms);
        }
        else
        {
          std::printf("queue target:     none: the loop writes whenever the kernel has room, so how much audio\n"
                      "                  is queued -- and so the stall it can absorb -- is whatever the run holds\n");
        }
        if (result.output_queue_floor_periods > 0)
        {
          const double inserted_ms = options.sample_rate == 0
                                       ? 0.0
                                       : 1e3 * static_cast<double>(result.floor_inserted_frames) /
                                           static_cast<double>(options.sample_rate);
          // The floor is enforced *after* a write, which is the whole meaning of the number: the queue
          // drains a period between the write and the next one, and a stall finds the instant before a
          // write, so asking for N buys N-1 periods of tolerance. Printed as both so the arithmetic
          // cannot be misread for a guarantee it is not.
          std::printf("queue floor:      %u periods after each write, so %u periods = %.3f ms at the worst\n"
                      "                  instant; held by inserting silence %llu times (%llu frames =\n"
                      "                  %.3f ms of audio delayed rather than skipped, %s)\n",
                      result.output_queue_floor_periods,
                      result.output_queue_floor_periods == 0 ? 0 : result.output_queue_floor_periods - 1,
                      result.output_queue_floor_periods == 0 || options.sample_rate == 0
                        ? 0.0
                        : 1e3 * static_cast<double>(result.output_queue_floor_periods - 1) *
                            static_cast<double>(result.device_period_size) /
                            static_cast<double>(options.sample_rate),
                      static_cast<unsigned long long>(result.floor_insertions),
                      static_cast<unsigned long long>(result.floor_inserted_frames), inserted_ms,
                      result.floor_insertions == 0 ? "never needed" : "each one audible without a splice fade");
        }
        if (result.splice_bridge_frames > 0)
        {
          // The repair, and the two facts that keep it honest: it acts on the emitted signal, and the
          // engine's output is still compared sample for sample against the offline render.
          std::printf("splice repair:    %u-frame bridge (%.3f ms) armed %llu times: %llu dropped periods,\n"
                      "                  %llu inserted ones. Applied to the emitted signal only -- the oracle\n"
                      "                  below still compares the engine's pristine output\n",
                      result.splice_bridge_frames,
                      options.sample_rate == 0
                        ? 0.0
                        : 1e3 * static_cast<double>(result.splice_bridge_frames) /
                            static_cast<double>(options.sample_rate),
                      static_cast<unsigned long long>(result.splice_bridges),
                      static_cast<unsigned long long>(result.splice_bridges_for_drops),
                      static_cast<unsigned long long>(result.splice_bridges_for_insertions));
        }
        if (!result.splice_verify_error.empty())
          std::printf("splice measured:  the detector could not judge the splices: %s\n",
                      result.splice_verify_error.c_str());
        else if (options.splice_verify && result.splice_bridges + result.output_drops > 0 &&
                 result.splice_verified == 0)
          std::printf("splice measured:  %llu splices happened and none was judged\n",
                      static_cast<unsigned long long>(result.splice_bridges + result.output_drops));
        if (result.injected_splices > 0)
          // Said first, and in the same block as the measured numbers, because a reader who takes an
          // injected rate for a real one has been misled by the report rather than by the run.
          std::printf("splice inject:    %llu periods lost deliberately (one every %.3f s), beside %llu\n"
                      "                  refused by the queue: the measured numbers below are of the\n"
                      "                  injected ones, which is the point -- they are isolated\n",
                      static_cast<unsigned long long>(result.injected_splices),
                      options.inject_splice_seconds, static_cast<unsigned long long>(result.output_drops));
        // A bridge that fires on a large fraction of blocks is not repairing isolated splices, it is
        // part of a loop that is thrashing -- and then the repair is meaningless, because its whole
        // design assumes clean audio either side of one splice. Measured: at cap 2 with the floor at the
        // same level, the bridge was armed 236 841 times in 480 s (one per two blocks) and the emitted
        // signal was *worse* than the raw one, a chain of overlapping bridges. Said out loud rather than
        // left for the reader to notice in the ratios.
        if (result.splice_bridges > 0 && result.blocks > 0 &&
            result.splice_bridges * 20 > result.blocks)
          std::printf("splice repair:    WARNING: armed %llu times in %llu blocks -- above 5 %%, so this is\n"
                      "                  not isolated repair but a thrashing configuration; the measured\n"
                      "                  numbers below describe a chain of bridges, not a splice\n",
                      static_cast<unsigned long long>(result.splice_bridges),
                      static_cast<unsigned long long>(result.blocks));
        if (result.splice_verify_evaluated)
        {
          // Measured on the audio this run produced, not on a test tone: the largest second difference
          // inside the splice window against the largest the surrounding signal produces. Both the raw
          // and the emitted number come from the same splices, which is what makes them a pair.
          const double ratio = result.splice_reference_d2 > 0.0
                                 ? result.splice_worst_d2 / result.splice_reference_d2
                                 : 0.0;
          const double raw_ratio = result.splice_raw_reference_d2 > 0.0
                                     ? result.splice_raw_d2 / result.splice_raw_reference_d2
                                     : 0.0;
          std::printf("splice measured:  %llu splices judged on the emitted signal (reference %.6f):\n"
                      "                  raw %.6f (%.1fx) -> emitted %.6f (%.1fx)%s\n",
                      static_cast<unsigned long long>(result.splice_verified), result.splice_reference_d2,
                      result.splice_raw_d2, raw_ratio, result.splice_worst_d2, ratio,
                      result.splice_bridge_frames == 0 ? "   [no bridge: both are the raw splice]" : "");
        }
        // The loop's own decisions, next to the occupancy that provoked them. A drop is audio the
        // run chose to lose rather than block for, so it is the one number here that a listener
        // would hear; the burst count is the mechanism that leads to one. The gate is spelled out
        // because in target mode it is deliberately not zero (see drop_gate).
        const std::string drop_gate_text =
          drops_gate_given ? "gate <= " + std::to_string(max_output_drops)
                           : (options.output_queue_periods == 0
                                ? std::string("gate <= 0")
                                : std::string("not gated: each refusal is the target's trade"));
        std::printf("queue control:    %llu output drops (%s), %llu capture bursts\n",
                    static_cast<unsigned long long>(result.output_drops), drop_gate_text.c_str(),
                    static_cast<unsigned long long>(result.capture_bursts));
        if (result.output_target_drops > 0 || result.output_queue_periods > 0)
          std::printf("                  %llu of those drops were the queue target refusing a period that\n"
                      "                  would have raised the ring past %u periods; %llu were the ring itself\n"
                      "                  being full\n",
                      static_cast<unsigned long long>(result.output_target_drops), result.output_queue_periods,
                      static_cast<unsigned long long>(result.output_drops - result.output_target_drops));
        if (result.settle_discarded_frames > 0)
          std::printf("settle:           discarded %llu frames of bring-up capture (before block 0)\n",
                      static_cast<unsigned long long>(result.settle_discarded_frames));
        if (result.prefill_frames > 0)
          std::printf("queue prefill:    %llu frames of silence written before block 0 to bring the ring\n"
                      "                  to its target (%.3f ms of output latency, paid once)\n",
                      static_cast<unsigned long long>(result.prefill_frames),
                      options.sample_rate == 0 ? 0.0
                                               : 1e3 * static_cast<double>(result.prefill_frames) /
                                                   static_cast<double>(options.sample_rate));
        if (result.gate_capture_xruns > 0)
          std::printf("capture xruns:    %llu found by the gate (a subset of the driver xruns below: an\n"
                      "                  xrun freezes hw_ptr, so without this the loop stalls instead)\n",
                      static_cast<unsigned long long>(result.gate_capture_xruns));
        if (result.occupancy_mismatches > 0)
          std::printf("occupancy check:  %llu samples where delay disagreed with the pointer pair\n",
                      static_cast<unsigned long long>(result.occupancy_mismatches));
      }
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
      // Where the tail lives. A chain max under the deadline with a read or write max over it says
      // the engine finished every block and the outliers were syscalls or the host descheduling it
      // -- which is the difference between "make the DSP faster" and "get an RT kernel".
      std::printf("split (tail):     read p99 %llu max %llu, chain p99 %llu max %llu, write p99 %llu max %llu ns\n",
                  static_cast<unsigned long long>(result.read_p99_ns),
                  static_cast<unsigned long long>(result.read_max_ns),
                  static_cast<unsigned long long>(result.chain_p99_ns),
                  static_cast<unsigned long long>(result.chain_max_ns),
                  static_cast<unsigned long long>(result.write_p99_ns),
                  static_cast<unsigned long long>(result.write_max_ns));
    if (result.stream_mode)
      // The engine's own p99, separately from the block time. The block time includes whatever the
      // write waited for, and on this device the playback ring is close to full nearly always, so the
      // write's wait for room is charged to the "block" -- which turns a statement about the engine
      // into a statement about the ring. This is the number that answers "did the engine finish its
      // work inside the deadline", and it is printed next to the one that does not.
      std::printf("chain p99:        %llu ns (%.2f%% of deadline)\n",
                  static_cast<unsigned long long>(result.chain_p99_ns),
                  result.deadline_ns == 0
                    ? 0.0
                    : 100.0 * static_cast<double>(result.chain_p99_ns) / static_cast<double>(result.deadline_ns));
    if (result.stream_mode)
      std::printf("device wait:      p50 %llu ns, p99 %llu ns, max %llu ns\n",
                  static_cast<unsigned long long>(result.wait_p50_ns),
                  static_cast<unsigned long long>(result.wait_p99_ns),
                  static_cast<unsigned long long>(result.wait_max_ns));
    if (result.stream_mode)
    {
      const double cpu_pct =
        result.chain_wall_ns == 0
          ? 0.0
          : 100.0 * static_cast<double>(result.chain_cpu_ns) / static_cast<double>(result.chain_wall_ns);
      std::printf("render path cpu:  p99 wall vs cpu is the deadline question: cpu max %llu ns, "
                  "wall max %llu ns\n",
                  static_cast<unsigned long long>(result.chain_cpu_max_ns),
                  static_cast<unsigned long long>(result.chain_max_ns));
      std::printf("                  %.2f s of CPU over %.2f s of wall in the render path (%.1f%%), "
                  "%llu blocks lost >= %.1f ms to not being on CPU\n",
                  static_cast<double>(result.chain_cpu_ns) / 1e9,
                  static_cast<double>(result.chain_wall_ns) / 1e9, cpu_pct,
                  static_cast<unsigned long long>(result.chain_stall_blocks),
                  static_cast<double>(namchain::SoakResult::stall_threshold_ns) / 1e6);
      {
        std::printf("slowest blocks:   ");
        bool any = false;
        for (const namchain::SoakResult::SlowBlock& s : result.slow_blocks)
        {
          if (s.chain_cpu_ns == 0)
            continue;
          std::printf("%sblock %llu cpu %llu ns (wall %llu ns)", any ? ", " : "", 
                      static_cast<unsigned long long>(s.block),
                      static_cast<unsigned long long>(s.chain_cpu_ns),
                      static_cast<unsigned long long>(s.chain_wall_ns));
          any = true;
        }
        std::printf("%s\n", any ? "" : "none recorded");
      }
      // Group the over-half-a-period blocks into runs. Isolated spikes and short bursts of them have
      // different causes, and the run length plus the spacing is what identifies which one this is.
      if (!result.chain_spikes.empty())
      {
        std::printf("cpu spike bursts: %zu blocks over half a period of CPU%s, in ",
                    result.chain_spikes.size(),
                    result.chain_spikes.size() >= namchain::SoakResult::max_spikes ? " (capped: there were more)" : "");
        struct Burst
        {
          uint64_t first = 0;
          uint64_t last = 0;
          uint64_t max_cpu = 0;
        };
        std::vector<Burst> bursts;
        for (const namchain::SoakResult::SlowBlock& s : result.chain_spikes)
        {
          if (!bursts.empty() && s.block <= bursts.back().last + 2)
          {
            bursts.back().last = s.block;
            bursts.back().max_cpu = std::max(bursts.back().max_cpu, s.chain_cpu_ns);
          }
          else
          {
            bursts.push_back(Burst{s.block, s.block, s.chain_cpu_ns});
          }
        }
        std::printf("%zu runs", bursts.size());
        if (bursts.size() > 1)
        {
          uint64_t min_gap = UINT64_MAX;
          uint64_t max_gap = 0;
          for (size_t i = 1; i < bursts.size(); ++i)
          {
            const uint64_t gap = bursts[i].first - bursts[i - 1].first;
            min_gap = std::min(min_gap, gap);
            max_gap = std::max(max_gap, gap);
          }
          std::printf("; runs start every %llu..%llu blocks (%.1f..%.1f s at 48 k/64)",
                      static_cast<unsigned long long>(min_gap), static_cast<unsigned long long>(max_gap),
                      static_cast<double>(min_gap) * 64.0 / 48000.0, static_cast<double>(max_gap) * 64.0 / 48000.0);
        }
        std::printf("\n");
        for (size_t i = 0; i < bursts.size() && i < 8; ++i)
          std::printf("                  run at block %llu..%llu (%llu long), worst %llu ns of CPU\n",
                      static_cast<unsigned long long>(bursts[i].first),
                      static_cast<unsigned long long>(bursts[i].last),
                      static_cast<unsigned long long>(bursts[i].last - bursts[i].first + 1),
                      static_cast<unsigned long long>(bursts[i].max_cpu));
      }
    }
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
    // Only the ALSA backend has a driver to report anything, so a zero from any other backend is a
    // statement about the backend and not about the host. Say which, rather than letting a bare 0
    // read as "the device never underran".
    const bool driver_backed = result.backend == "alsa_raw";
    std::printf("driver xruns:     %llu%s\n", static_cast<unsigned long long>(result.driver_xruns),
                (!driver_backed && result.driver_xruns == 0) ? " (this backend has no driver to report)" : "");
    if (driver_backed)
      // The split decides what a queue target can be chosen against: a capture overrun is the loop not
      // reading promptly enough and no amount of output queue helps, while a playback underrun is the
      // ring running dry, which is the event the target exists to prevent.
      std::printf("  by direction:   %llu capture overruns, %llu playback underruns\n",
                  static_cast<unsigned long long>(result.capture_overruns),
                  static_cast<unsigned long long>(result.playback_underruns));
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
