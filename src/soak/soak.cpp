#include "namchain/soak.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <thread>
#include <vector>

#include "namchain/io/backend.h"
#include "namchain/offline.h"
#include "namchain/pcm.h"
#include "namchain/rt_check.h"
#include "namchain/telemetry.h"
#include "namchain/wav.h"

#include "namchain/graph/plan.h"
#include "namchain/graph/reference.h"
#include "namchain/graph/rig.h"
#include "namchain/rt/arena.h"


namespace namchain
{
namespace
{

using Clock = std::chrono::steady_clock;

/// Oracle comparison is quadratic-ish in memory (the whole rendered output is retained), so a
/// multi-hour soak must not ask for it. Timing-only runs are unaffected.
constexpr double kMaxOracleSeconds = 300.0;

SoakResult fail(std::string message)
{
  SoakResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

/// A scratch WAV path for the oracle comparison. Unique per call so two soaks in one process (the
/// unit tests do exactly that) cannot compare against each other's output.
std::string temp_wav_path(const std::string& tag)
{
  static int counter = 0;
  const auto dir = std::filesystem::temp_directory_path() / "namchain-soak";
  std::filesystem::create_directories(dir);
  return (dir / (tag + "-" + std::to_string(++counter) + ".wav")).string();
}

struct TempFileGuard
{
  std::string path;
  bool keep = false;
  ~TempFileGuard()
  {
    if (!keep && !path.empty())
      std::filesystem::remove(path);
  }
};

/// Lock the pages the audio path touches, once everything it touches has been allocated.
///
/// Deliberately `MCL_CURRENT` only, and deliberately last. With `MCL_FUTURE` every later mapping is
/// charged against `RLIMIT_MEMLOCK`, so a limit generous enough for the pages already mapped is not
/// necessarily generous enough for the next large one -- and the failure is a `std::bad_alloc` inside
/// a process that has just been told its memory is locked. CI found exactly that, on the `null`
/// backend with no device in the picture: `mlockall` succeeded at ~6 MB of RSS and the very next
/// allocation, a 44 MB block-timer reserve, came back `EAGAIN`. Both halves of that are fixed now --
/// the reserve is sized from the run (telemetry.h) and the lock no longer charges the future -- and
/// this comment is the half that would otherwise come back.
///
/// The soft limit is raised to the hard one first: on a host whose administrator has already granted
/// a large `LimitMEMLOCK`, the soft limit is the only thing in the way.
bool lock_process_memory()
{
  rlimit limit{};
  if (::getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_cur != limit.rlim_max)
  {
    limit.rlim_cur = limit.rlim_max;
    ::setrlimit(RLIMIT_MEMLOCK, &limit); // best effort: the hard limit is the administrator's call
  }
  return ::mlockall(MCL_CURRENT) == 0;
}

/// Stream soak: drive a full-duplex backend a period at a time, run the chain in the middle, and
/// count what the backend reports. Everything after this point is written once and runs against a
/// WAV file, a paced clock with no device, or raw ALSA ioctls alike (plan.md §9.1).
SoakResult run_backend_soak(const SoakOptions& options, const std::string& input_wav)
{
  BackendConfig config;
  config.device = options.device;
  config.sample_rate = options.sample_rate;
  config.capture_channels = options.capture_channels == 0 ? options.device_channels : options.capture_channels;
  config.playback_channels = options.playback_channels == 0 ? options.device_channels : options.playback_channels;
  config.period_size = options.quantum;
  config.periods = options.device_periods;
  config.input_path = input_wav;
  config.output_path = options.output_wav;
  config.realtime_paced = options.realtime_paced;
  config.require_exact_period = true; // §9.5: a silently rounded quantum is not acceptable

  std::string error;
  std::unique_ptr<AudioBackend> device = open_backend(config, options.backend, error);
  if (device == nullptr)
    return fail(error);

  const BackendInfo info = device->info();

  // How many blocks the run will record, computed before the timers exist: each one reserves its
  // whole expected series up front so that `record()` never allocates, and a fixed "one hour at 32
  // frames" reserve is 220 MB across the five of them whatever the run actually asks for. That was
  // not merely wasteful -- it was the allocation that turned a finite `RLIMIT_MEMLOCK` into a
  // `std::bad_alloc` on CI (§14.10).
  const uint64_t total_blocks =
    std::max<uint64_t>(1, static_cast<uint64_t>(options.duration_seconds * info.sample_rate / info.period_size));

  const uint64_t deadline_ns =
    static_cast<uint64_t>(std::llround(1e9 * static_cast<double>(info.period_size) / static_cast<double>(info.sample_rate)));
  BlockTimer timer(deadline_ns, total_blocks);
  // Time blocked on the device, kept separate from the work we do. If wait + work exceeds the
  // deadline the run cannot keep real time; which of the two dominates says where the problem is.
  BlockTimer wait_timer(deadline_ns, total_blocks);
  // Phase breakdown. A single "block time" conflates our work with time spent blocked inside the
  // backend's own read/write, and those two call for completely different responses.
  BlockTimer read_timer(deadline_ns, total_blocks);
  BlockTimer chain_timer(deadline_ns, total_blocks);
  BlockTimer write_timer(deadline_ns, total_blocks);

  // Bytes, not samples: period_size frames x bytes_per_frame. See AlsaRawDuplex::read_capture.
  // Bytes, not samples, and per direction: the SSL 2+ Mk II is 6 out / 8 in, so one count for
  // both would size the capture buffer 25 % short.
  const size_t capture_frame_bytes = static_cast<size_t>(info.period_size) * info.capture_bytes_per_frame;
  const size_t playback_frame_bytes = static_cast<size_t>(info.period_size) * info.playback_bytes_per_frame;
  std::vector<unsigned char> capture_buffer(capture_frame_bytes);
  std::vector<unsigned char> playback_buffer(playback_frame_bytes);

  // The chain is now a compiled plan rather than a hand-rolled loop over models. That is the point
  // of M1: the thing under test on the device is the compiler and the render path together, with
  // the same code the offline renders and the unit tests use.
  graph::Graph graph;
  std::string graph_source;
  bool have_chain = false;
  if (!options.graph_path.empty())
  {
    std::string graph_error;
    if (!graph::load_graph_json(options.graph_path, graph, graph_error))
      return fail(graph_error);
    graph_source = options.graph_path;
    have_chain = true;
  }
  else if (!options.model.empty())
  {
    graph = graph::chain_graph(options.model, options.instances);
    graph_source = "single model chain";
    have_chain = true;
  }

  const PcmFormat pcm_format = info.format;
  std::vector<float> mono_in(static_cast<size_t>(info.period_size), 0.0f);
  std::vector<float> mono_out(static_cast<size_t>(info.period_size), 0.0f);

  render::CompileOptions compile_options;
  compile_options.max_quantum = static_cast<int>(info.period_size);
  compile_options.sample_rate = static_cast<double>(info.sample_rate);

  // Big enough for any sane rig at a 64-frame quantum; the report states what it actually used, so
  // the number in the log is measured rather than assumed.
  rt::Arena arena(1u << 20);
  std::unique_ptr<graph::RenderPlan> plan;
  if (have_chain)
  {
    if (pcm_format == PcmFormat::Unknown)
      return fail("device sample format is not one the chain can convert");
    std::string compile_error;
    plan = graph::RenderPlan::compile(graph, compile_options, arena, compile_error);
    if (plan == nullptr)
      return fail(compile_error);

    // Warm the chain before the stream starts. The first process() call is cold -- measured at
    // ~2.8 ms against an 80 us steady state -- and a stall that long can drain the ring on its
    // own, so this is a control-thread job and not the first block's problem (§3.3).
    plan->prewarm();
  }

  // Device-mode self-oracle: the engine's output during a real-time run, against the same graph
  // rendered offline from the very input that run consumed. This is the only form the comparison
  // can take on a device -- the input is live capture, so "the same input" means recording it --
  // and it is what makes criterion 3's last clause a measurement instead of an assumption.
  //
  // Bounded to a window, in whole blocks so that the offline re-render sees exactly the block sizes
  // the real-time run did. Every buffer is allocated here, before the stream starts, because the
  // recording happens on the audio thread.
  if (options.self_oracle && !have_chain)
    return fail("--self-oracle in device mode needs a chain (--model or --graph) to render");
  std::vector<float> oracle_in;
  std::vector<float> oracle_out;
  size_t oracle_recorded = 0;
  if (options.self_oracle && have_chain)
  {
    const uint64_t window_blocks = std::min<uint64_t>(
      total_blocks, std::max<uint64_t>(1, static_cast<uint64_t>(options.oracle_window_seconds *
                                                               info.sample_rate / info.period_size)));
    const size_t window_frames = static_cast<size_t>(window_blocks) * info.period_size;
    oracle_in.assign(window_frames, 0.0f);
    oracle_out.assign(window_frames, 0.0f);
  }

  // What the engine does after start (§3.3): lock the address space so a page fault cannot stall a
  // period. Without it a soak measures the page cache as much as the engine. Skipped for the file
  // backend, which is offline by definition and would only end up locking a growing WAV buffer.
  // Last, so that the model, the plan and every stream buffer are covered by MCL_CURRENT -- see
  // lock_process_memory.
  const bool memory_locked = options.backend != BackendKind::File && lock_process_memory();

  // Start the stream only now that everything it needs is in memory. `start` primes the playback
  // ring and lets the device run, so the time between it and the first wait_capture is time the
  // ring spends draining: with the model loaded afterwards instead, the device was already starving
  // while the plan was built, which is where the single bring-up overrun every configuration
  // showed came from.
  if (!device->start(error))
    return fail(error);

  // Bring-up iteration, deliberately untimed. The first period after START fills an empty capture
  // ring, so its cost is an artefact of starting rather than a real-time property -- every
  // configuration, passthrough included, showed exactly one overrun and it was always block 0.
  // Run one cycle outside the measurement and report xruns seen during bring-up separately from
  // those seen while running, rather than discarding either.
  if (info.needs_bringup_cycle && device->wait_capture(2000, error))
  {
    std::string ignored;
    std::fill(playback_buffer.begin(), playback_buffer.end(), 0);
    device->read_capture(capture_buffer.data(), info.period_size, 1000, ignored);
    device->write_playback(playback_buffer.data(), info.period_size, 1000, ignored);
  }
  const uint64_t startup_xruns = device->driver_xruns();

  rt_reset_counters();
  uint64_t first_overrun_block = std::numeric_limits<uint64_t>::max();
  bool occupancy_seen = false;
  long long capture_occupancy_min = 0;
  long long capture_occupancy_max = 0;
  long long capture_occupancy_last = 0;
  long long playback_occupancy_min = 0;
  long long playback_occupancy_max = 0;
  long long playback_occupancy_last = 0;
  uint64_t playback_full_blocks = 0;
  const auto start = Clock::now();
  for (uint64_t block = 0; block < total_blocks; ++block)
  {
    // A file backend runs out of input; stop there rather than reading silence for an hour labelled
    // as if it were a real measurement.
    if (device->info().capture_exhausted)
      break;

    // Wait untimed; time only the work. Measuring from the start of the wait would just report the
    // period length, which is not what the deadline is about.
    const auto tw0 = Clock::now();
    if (!device->wait_capture(2000, error))
    {
      device->stop();
      return fail(error);
    }
    const auto tw1 = Clock::now();
    wait_timer.record(
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(tw1 - tw0).count()));

    const auto t0 = Clock::now();
    if (device->read_capture(capture_buffer.data(), info.period_size, 1000, error) < 0)
    {
      device->stop();
      return fail(error);
    }
    const auto t1 = Clock::now();
    read_timer.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    {
      // The engine's render path lives here. The read and write either side of it are the
      // backend's own syscalls and are deliberately outside the scope (namchain/rt_check.h).
      RtScope scope;
      if (plan != nullptr)
      {
        const size_t width = info.bytes_per_sample;
        const unsigned in_ch = std::min(options.input_channel, info.capture_channels - 1);
        const unsigned out_ch = std::min(options.output_channel, info.playback_channels - 1);

        for (unsigned i = 0; i < info.period_size; ++i)
          mono_in[i] = pcm_decode(
            capture_buffer.data() + (static_cast<size_t>(i) * info.capture_channels + in_ch) * width, pcm_format);

        plan->execute(mono_in.data(), mono_out.data(), static_cast<int>(info.period_size));

        // Start from silence every block: the passthrough copy only fills one channel, and a device
        // with 6 playback channels would otherwise be sent whatever was left in the buffer.
        std::fill(playback_buffer.begin(), playback_buffer.end(), 0);
        for (unsigned i = 0; i < info.period_size; ++i)
          pcm_encode(
            playback_buffer.data() + (static_cast<size_t>(i) * info.playback_channels + out_ch) * width, pcm_format,
            mono_out[i]);
      }
    }
    const auto t2 = Clock::now();
    chain_timer.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()));

    // Sampled before the write, and after the timed section is closed: what decides whether the write
    // can proceed immediately is how much room the playback ring has *now*, not after the period we
    // are about to add to it. Reading it after the write reports the ring one period fuller than the
    // state the write actually met, which is the difference between "the ring is full" and "the ring
    // is full because I just filled it".
    {
      long long capture_frames = 0;
      long long playback_frames = 0;
      if (device->ring_occupancy(capture_frames, playback_frames))
      {
        if (!occupancy_seen)
        {
          occupancy_seen = true;
          capture_occupancy_min = capture_occupancy_max = capture_frames;
          playback_occupancy_min = playback_occupancy_max = playback_frames;
        }
        capture_occupancy_min = std::min(capture_occupancy_min, capture_frames);
        capture_occupancy_max = std::max(capture_occupancy_max, capture_frames);
        playback_occupancy_min = std::min(playback_occupancy_min, playback_frames);
        playback_occupancy_max = std::max(playback_occupancy_max, playback_frames);
        capture_occupancy_last = capture_frames;
        playback_occupancy_last = playback_frames;
        if (playback_frames + static_cast<long long>(info.period_size) > static_cast<long long>(info.buffer_size))
          ++playback_full_blocks;
      }
    }

    if (device->write_playback(playback_buffer.data(), info.period_size, 1000, error) < 0)
    {
      device->stop();
      return fail(error);
    }
    const auto t3 = Clock::now();
    write_timer.record(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count()));
    const uint64_t block_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t0).count());
    timer.record(block_ns);
    if (block_ns > deadline_ns && first_overrun_block == std::numeric_limits<uint64_t>::max())
      first_overrun_block = block;

    // After the timed section on purpose. The copy is real work, but it is work the next
    // wait_capture absorbs, and charging it to the block would make the oracle change the number it
    // is meant to be validating. It is still reported, because a run that passes only because the
    // oracle is off is not the same claim as one that passes with it on.
    if (!oracle_in.empty() && oracle_recorded < oracle_in.size())
    {
      const size_t room = oracle_in.size() - oracle_recorded;
      const size_t count = std::min<size_t>(info.period_size, room);
      std::copy(mono_in.begin(), mono_in.begin() + count, oracle_in.begin() + oracle_recorded);
      std::copy(mono_out.begin(), mono_out.begin() + count, oracle_out.begin() + oracle_recorded);
      oracle_recorded += count;
    }
  }
  const auto end = Clock::now();
  const uint64_t steady_xruns = device->driver_xruns() - startup_xruns;
  device->stop();
  {
    // `file` writes its WAV here; every other backend has nothing buffered.
    std::string finish_error;
    if (!device->finish(finish_error))
      return fail(finish_error);
  }

  SoakResult result;
  result.ok = true;
  result.rt_instrumented = rt_check_available();
  result.rt_allocations = rt_allocation_count();
  result.rt_deallocations = rt_deallocation_count();
  result.stream_mode = true;
  result.backend = backend_kind_name(options.backend);
  result.first_overrun_block = first_overrun_block;
  result.memory_locked = memory_locked;
  result.device_period_size = info.period_size;
  result.device_buffer_size = info.buffer_size;
  result.chain_instances = options.model.empty() ? 0u : std::max(1u, options.instances);
  result.chain_model = options.model;
  result.graph_mode = !options.graph_path.empty();
  result.graph_source = graph_source;
  result.plan_nodes = plan == nullptr ? 0u : static_cast<unsigned>(plan->nodes().size());
  result.plan_buffers = plan == nullptr ? 0u : static_cast<unsigned>(plan->buffer_count());
  result.plan_buffer_bytes = plan == nullptr ? 0u : static_cast<unsigned>(plan->buffer_bytes());
  if (plan != nullptr)
  {
    // Every distinct implementation in the plan, not just the first: a rig can run an A2-Full and
    // an IR convolution side by side, and a timing number has to name all of what it timed.
    std::vector<std::string> described;
    for (const graph::PlanNode& node : plan->nodes())
    {
      if (node.type == nullptr || node.type->describe == nullptr)
        continue;
      const std::string name = node.type->describe(node.state);
      if (!name.empty() && std::find(described.begin(), described.end(), name) == described.end())
        described.push_back(name);
    }
    for (std::size_t i = 0; i < described.size(); ++i)
      result.model_type += (i == 0 ? "" : ", ") + described[i];
  }
  result.device_sample_format = pcm_format_name(info.format);
  result.device_bytes_per_sample = info.bytes_per_sample;
  result.device_period_exact = info.period_exact;
  result.device_capture_node = info.capture_name;
  result.device_playback_node = info.playback_name;
  result.occupancy_measured = occupancy_seen;
  result.capture_occupancy_min = capture_occupancy_min;
  result.capture_occupancy_max = capture_occupancy_max;
  result.capture_occupancy_last = capture_occupancy_last;
  result.playback_occupancy_min = playback_occupancy_min;
  result.playback_occupancy_max = playback_occupancy_max;
  result.playback_occupancy_last = playback_occupancy_last;
  result.playback_full_blocks = playback_full_blocks;

  result.blocks = timer.blocks();
  result.frames = timer.blocks() * info.period_size;
  result.deadline_overruns = timer.deadline_overruns();
  result.driver_xruns = steady_xruns;
  result.startup_driver_xruns = startup_xruns;
  result.deadline_ns = deadline_ns;
  result.read_p50_ns = read_timer.p50_ns();
  result.chain_p50_ns = chain_timer.p50_ns();
  result.chain_p99_ns = chain_timer.p99_ns();
  result.write_p50_ns = write_timer.p50_ns();
  result.wait_p50_ns = wait_timer.p50_ns();
  result.wait_p99_ns = wait_timer.p99_ns();
  result.wait_max_ns = wait_timer.max_ns();
  result.p50_ns = timer.p50_ns();
  result.p99_ns = timer.p99_ns();
  result.max_ns = timer.max_ns();
  result.p99_percent_of_deadline = timer.p99_percent_of_deadline();
  result.audio_seconds = static_cast<double>(result.frames) / info.sample_rate;
  result.wall_seconds = std::chrono::duration<double>(end - start).count();
  result.rtf = result.audio_seconds > 0.0 ? result.wall_seconds / result.audio_seconds : 0.0;

  // The oracle itself, after the stream has stopped: the very input the run consumed, pushed
  // through a freshly compiled plan for the same graph, compared frame by frame against what the
  // real-time run wrote. A second plan and a second arena, because the first plan's state is the
  // thing under suspicion and its arena is full of it.
  if (!oracle_in.empty() && oracle_recorded > 0)
  {
    rt::Arena oracle_arena(1u << 20);
    std::string compile_error;
    std::unique_ptr<graph::RenderPlan> oracle_plan =
      graph::RenderPlan::compile(graph, compile_options, oracle_arena, compile_error);
    if (oracle_plan == nullptr)
      return fail("oracle re-compile failed: " + compile_error);
    // Prewarmed exactly as the real-time plan was, so the two start from the same state.
    oracle_plan->prewarm();

    std::vector<float> block_in(static_cast<size_t>(info.period_size), 0.0f);
    std::vector<float> block_out(static_cast<size_t>(info.period_size), 0.0f);
    double max_abs = 0.0;
    double sum_squares = 0.0;
    size_t first_mismatch = oracle_recorded;
    for (size_t offset = 0; offset < oracle_recorded; offset += info.period_size)
    {
      const unsigned count =
        static_cast<unsigned>(std::min<size_t>(info.period_size, oracle_recorded - offset));
      std::copy(oracle_in.begin() + static_cast<std::ptrdiff_t>(offset),
                oracle_in.begin() + static_cast<std::ptrdiff_t>(offset + count), block_in.begin());
      oracle_plan->execute(block_in.data(), block_out.data(), static_cast<int>(count));
      for (unsigned i = 0; i < count; ++i)
      {
        const double difference =
          std::abs(static_cast<double>(block_out[i]) - static_cast<double>(oracle_out[offset + i]));
        if (difference > options.oracle_tolerance && first_mismatch == oracle_recorded)
          first_mismatch = offset + i;
        max_abs = std::max(max_abs, difference);
        sum_squares += difference * difference;
      }
    }

    result.oracle.checked = true;
    result.oracle.frames = oracle_recorded;
    result.oracle.max_abs = max_abs;
    result.oracle.rms =
      oracle_recorded > 0 ? std::sqrt(sum_squares / static_cast<double>(oracle_recorded)) : 0.0;
    result.oracle.match = max_abs <= options.oracle_tolerance;
    result.oracle.source =
      "offline render of the captured input, first " +
      std::to_string(static_cast<double>(oracle_recorded) / info.sample_rate) + " s";
    if (!result.oracle.match)
      result.oracle.detail = "first difference at frame " + std::to_string(first_mismatch) +
                             " of " + std::to_string(oracle_recorded);
  }

  return result;
}

} // namespace

SoakResult run_soak(const std::string& model_path, const std::string& input_wav_path, const SoakOptions& options)
{
  if (options.quantum < 1 || options.quantum > 4096)
    return fail("quantum " + std::to_string(options.quantum) + " is out of range [1, 4096]");
  if (options.duration_seconds <= 0.0)
    return fail("duration must be > 0");

  // A device was named, or a backend other than the device one was asked for. Either way this is a
  // stream run rather than a flat-out batch render.
  if (!options.device.empty() || options.backend != BackendKind::AlsaRaw)
    return run_backend_soak(options, input_wav_path);

  const bool want_oracle = options.self_oracle || !options.oracle_wav.empty();
  if (want_oracle && options.duration_seconds > kMaxOracleSeconds)
    return fail("oracle comparison is intended for bounded runs (<= " + std::to_string(static_cast<int>(kMaxOracleSeconds)) +
                " s); a multi-hour soak should check timing only");

  // The engine runs at a fixed rate; the input file and every model must agree with it. Deriving
  // the rate from the model was only possible while a soak meant exactly one model, and it silently
  // picked a different rate depending on which file happened to be named first.
  const double rate = static_cast<double>(options.sample_rate);

  graph::Graph graph;
  std::string graph_source = "single model chain";
  bool graph_mode = false;
  if (!options.graph_path.empty())
  {
    std::string graph_error;
    if (!graph::load_graph_json(options.graph_path, graph, graph_error))
      return fail(graph_error);
    graph_source = options.graph_path;
    graph_mode = true;
  }
  else
  {
    graph = graph::chain_graph(model_path, options.instances);
  }

  render::CompileOptions compile_options;
  compile_options.max_quantum = options.quantum;
  compile_options.sample_rate = rate;

  // Sized for any sane rig at a 64-frame quantum, and the report states what was actually used.
  rt::Arena arena(1u << 20);
  std::string compile_error;
  std::unique_ptr<graph::RenderPlan> plan = graph::RenderPlan::compile(graph, compile_options, arena, compile_error);
  if (plan == nullptr)
    return fail(compile_error);

  std::vector<float> input;
  double input_rate = 0.0;
  std::string wav_error;
  if (!load_wav_mono(input_wav_path, input, input_rate, wav_error))
    return fail(wav_error);
  if (std::abs(input_rate - rate) > 0.5)
    return fail("the input WAV is " + std::to_string(input_rate) + " Hz but the engine runs at " +
                std::to_string(rate) + " Hz");

  // A self-oracle compare must be a like-for-like render. If the run is longer than the input
  // file the input is looped, and there is no bounded offline render to compare against, so
  // say so rather than produce a comparison that means nothing.
  const double input_seconds = static_cast<double>(input.size()) / input_rate;
  if (options.input_offset_seconds < 0.0)
    return fail("input offset must be >= 0");
  if (options.self_oracle && options.input_offset_seconds + options.duration_seconds > input_seconds + 1e-9)
  {
    return fail("self-oracle requires offset + duration (" + std::to_string(options.input_offset_seconds) + " + " +
                std::to_string(options.duration_seconds) + " s) to stay within the input file (" +
                std::to_string(input_seconds) +
                " s); use --oracle FILE to compare a looped run against a fixed reference");
  }
  const size_t input_start = std::min<size_t>(static_cast<size_t>(options.input_offset_seconds * input_rate),
                                              input.size() - 1);

  // A rig cannot be checked against a single model's offline render, so its self-oracle is the
  // naive interpreter instead: an independent evaluation of the same IR, run block for block
  // alongside the plan. That checks the compiler -- schedule, buffer colouring, pointer binding --
  // rather than merely re-running it.
  std::unique_ptr<graph::ReferenceRender> reference;
  if (graph_mode && options.self_oracle)
  {
    std::string reference_error;
    reference = graph::ReferenceRender::compile(graph, compile_options, reference_error);
    if (reference == nullptr)
      return fail("the reference oracle could not be built: " + reference_error);
  }

  std::vector<float> block_in(static_cast<size_t>(options.quantum), 0.0f);
  std::vector<float> block_out(static_cast<size_t>(options.quantum), 0.0f);
  std::vector<float> block_reference(static_cast<size_t>(options.quantum), 0.0f);

  // Warm both engines on the control thread before the timed loop. The first `process` call on a
  // freshly built node does one-time work -- notably the FFT plan a partitioned convolution builds
  // lazily on first use -- and that must not land inside the render path, where the interposer is
  // watching, nor inside a block that is being timed. Both sides get the same warm-up, so the
  // oracle comparison stays like for like.
  plan->prewarm();
  if (reference != nullptr)
    reference->prewarm();

  const uint64_t deadline_ns = static_cast<uint64_t>(std::llround(1e9 * static_cast<double>(options.quantum) / rate));
  const uint64_t total_frames = static_cast<uint64_t>(options.duration_seconds * rate);
  const uint64_t total_blocks = std::max<uint64_t>(1, total_frames / static_cast<uint64_t>(options.quantum));
  BlockTimer timer(deadline_ns, total_blocks);

  // Keep the audio only when something will consume it: a one-hour run is ~690 MB as float32.
  const bool keep_audio = want_oracle || !options.output_wav.empty();
  std::vector<float> rendered;
  std::vector<float> reference_rendered;
  if (keep_audio)
    rendered.reserve(static_cast<size_t>(total_blocks * static_cast<uint64_t>(options.quantum)));
  if (reference != nullptr)
    reference_rendered.reserve(static_cast<size_t>(total_blocks * static_cast<uint64_t>(options.quantum)));

  size_t input_pos = input_start;
  double max_lateness_ns = 0.0;
  rt_reset_counters();
  const auto start = Clock::now();
  uint64_t expected_elapsed_ns = 0;

  for (uint64_t block = 0; block < total_blocks; ++block)
  {
    const int frames = options.quantum;
    for (int i = 0; i < frames; ++i)
    {
      block_in[static_cast<size_t>(i)] = input[input_pos];
      // Loop back to the offset, not to the file start: an xrun is a timing failure and content
      // does not matter to it, but CPU load does, and the start of this fixture is silence.
      if (++input_pos >= input.size())
        input_pos = input_start;
    }

    const auto t0 = Clock::now();
    {
      // The render-path boundary, expressed in code: everything the engine does happens here and
      // nothing here may allocate (plan.md §3.3, §10.4).
      RtScope scope;
      plan->execute(block_in.data(), block_out.data(), frames);
    }
    const auto t1 = Clock::now();

    const uint64_t duration_ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    timer.record(duration_ns);

    if (keep_audio)
      rendered.insert(rendered.end(), block_out.begin(), block_out.begin() + frames);
    if (reference != nullptr)
    {
      // Outside the RtScope on purpose: the oracle is not the render path, and timing it would
      // halve the measured throughput of the thing under test.
      reference->execute(block_in.data(), block_reference.data(), frames);
      reference_rendered.insert(reference_rendered.end(), block_reference.begin(), block_reference.begin() + frames);
    }

    if (options.realtime_paced)
    {
      expected_elapsed_ns += deadline_ns;
      const auto target = start + std::chrono::nanoseconds(expected_elapsed_ns);
      const auto now = Clock::now();
      if (now < target)
      {
        std::this_thread::sleep_until(target);
      }
      else
      {
        const double late_ns = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now - target).count());
        max_lateness_ns = std::max(max_lateness_ns, late_ns);
      }
    }
  }

  const auto end = Clock::now();
  const double wall_seconds = std::chrono::duration<double>(end - start).count();
  const double audio_seconds = static_cast<double>(total_blocks * static_cast<uint64_t>(options.quantum)) / rate;

  SoakResult result;
  result.ok = true;
  result.rt_instrumented = rt_check_available();
  result.rt_allocations = rt_allocation_count();
  result.rt_deallocations = rt_deallocation_count();
  result.blocks = timer.blocks();
  result.frames = total_blocks * static_cast<uint64_t>(options.quantum);
  result.deadline_overruns = timer.deadline_overruns();
  result.driver_xruns = 0; // no device backend yet; alsa_raw/pipewire will drive this
  result.deadline_ns = deadline_ns;
  result.p50_ns = timer.p50_ns();
  result.p99_ns = timer.p99_ns();
  result.max_ns = timer.max_ns();
  result.p99_percent_of_deadline = timer.p99_percent_of_deadline();
  result.audio_seconds = audio_seconds;
  result.wall_seconds = wall_seconds;
  result.rtf = audio_seconds > 0.0 ? wall_seconds / audio_seconds : 0.0;
  result.max_lateness_ms = max_lateness_ns / 1e6;
  result.graph_mode = graph_mode;
  result.graph_source = graph_source;
  result.plan_nodes = static_cast<unsigned>(plan->nodes().size());
  result.plan_buffers = static_cast<unsigned>(plan->buffer_count());
  result.plan_buffer_bytes = static_cast<unsigned>(plan->buffer_bytes());
  result.reference_buffers = reference == nullptr ? 0u : static_cast<unsigned>(reference->buffer_count());
  for (const graph::PlanNode& node : plan->nodes())
    if (node.type != nullptr && node.type->describe != nullptr)
    {
      const std::string described = node.type->describe(node.state);
      if (!described.empty())
      {
        result.model_type = described;
        break;
      }
    }

  // ---- oracle: does a paced real-time-shaped run produce the same samples as a second, naive
  // render of the same rig? ----
  if (reference != nullptr)
  {
    result.oracle.checked = true;
    result.oracle.source = "reference interpreter (in-process)";
    result.oracle.frames = std::min(rendered.size(), reference_rendered.size());
    double max_abs = 0.0;
    double sum_squares = 0.0;
    for (std::size_t i = 0; i < result.oracle.frames; ++i)
    {
      const double difference = static_cast<double>(rendered[i]) - static_cast<double>(reference_rendered[i]);
      max_abs = std::max(max_abs, std::abs(difference));
      sum_squares += difference * difference;
    }
    result.oracle.max_abs = max_abs;
    result.oracle.rms = result.oracle.frames > 0
                          ? std::sqrt(sum_squares / static_cast<double>(result.oracle.frames))
                          : 0.0;
    result.oracle.match = rendered.size() == reference_rendered.size() && max_abs <= options.oracle_tolerance;
    if (rendered.size() != reference_rendered.size())
      result.oracle.detail = "the plan produced " + std::to_string(rendered.size()) +
                             " frames and the reference " + std::to_string(reference_rendered.size());
    else if (!result.oracle.match)
      result.oracle.detail = "the plan differs from the reference interpreter by more than the tolerance";
  }
  else if (want_oracle)
  {
    TempFileGuard paced_file;
    paced_file.path = options.output_wav.empty() ? temp_wav_path("paced") : options.output_wav;
    paced_file.keep = !options.output_wav.empty();

    std::string save_error;
    if (!save_wav_float32(paced_file.path, rendered.data(), rendered.size(), 1, rate, save_error))
      return fail(save_error);

    std::string reference_path;
    TempFileGuard reference_file;
    if (options.self_oracle)
    {
      reference_file.path = temp_wav_path("offline");
      OfflineRenderOptions offline;
      offline.quantum = options.quantum;
      // Bound the offline render to exactly the frames this run produced, starting where it
      // started. Without this the comparison would fail on length alone and quietly be
      // measuring nothing.
      offline.max_frames = result.frames;
      offline.start_frame = input_start;
      const OfflineRenderResult offline_result =
        render_model_to_wav(model_path, input_wav_path, reference_file.path, offline);
      if (!offline_result.ok)
        return fail("offline oracle render failed: " + offline_result.error);
      reference_path = reference_file.path;
      result.oracle.source = "offline self-render";
    }
    else
    {
      reference_path = options.oracle_wav;
      result.oracle.source = options.oracle_wav;
    }

    WavDiff diff;
    std::string compare_error;
    const WavCompareStatus status =
      compare_wav_mono(reference_path, paced_file.path, options.oracle_tolerance, diff, compare_error);
    result.oracle.checked = true;
    result.oracle.frames = diff.frames;
    result.oracle.max_abs = diff.max_abs;
    result.oracle.rms = diff.rms;
    result.oracle.match = (status == WavCompareStatus::Match);
    if (status == WavCompareStatus::Error)
      return fail(compare_error);
    if (status == WavCompareStatus::Differs)
      result.oracle.detail = compare_error.empty()
                               ? "output differs from the oracle by more than the tolerance"
                               : compare_error;
  }
  else if (!options.output_wav.empty())
  {
    std::string save_error;
    if (!save_wav_float32(options.output_wav, rendered.data(), rendered.size(), 1, rate, save_error))
      return fail(save_error);
  }

  return result;
}

} // namespace namchain
