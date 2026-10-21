// nc.tuner: the Squatch Tuner (third_party/squatch-tuner) as an engine node.
//
// The core is header-only C++17 with fixed buffers: configure() builds its tables (control thread,
// at build), process() is real-time safe and takes any block size. docs/tuner.md is the design and
// the vendored copy's provenance; this file is the node around it.
//
// Three things happen here, and the first is the one that matters:
//
//  * **The output is silence.** A pedal tuner takes the rig out of the signal path, and this node
//    does it by writing zeros, so everything downstream is inaudible while the player tunes. The
//    input is still read -- that is the signal the tuner listens to.
//  * **The tuner runs on the render thread**, one call per block, over the block's first channel.
//    Its state is ~128 kB of fixed buffers, so it lives in the node's state (allocated at build)
//    rather than on a stack, and process() touches no heap.
//  * **What it reads is published once per block** through an epoch-guarded snapshot, which the
//    control thread reads for `rig.state`. The render thread never hands a pointer to anything it
//    is still writing: nc.eq.flow's publish/read_report is the pattern, and this is the same one.
//
// The node has no parameters on purpose. A4 is the fixed 440 Hz a guitar pedal tunes to; the note
// range covers a 5-string bass on the way up and a 24th-fret high E on the way down. A reference
// pitch control is a parameter and a UI, not an engine default, and when someone wants one it
// arrives as `a4` with the schema's range around it (docs/tuner.md).

#include "namchain/tuner.h"

#include "namchain/graph/plan.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include <json.hpp>

// The vendored core. Its include root is added as a SYSTEM directory, so its warnings are not ours
// (CMakeLists.txt), and nothing outside this file sees it.
#include <squatch/tuner/tuner.hpp>

namespace namchain
{
namespace render
{
namespace
{

/// The reference pitch, and the note range the coarse stage searches.
///
/// 38 Hz is below a 5-string bass's B0 (30.9) less a semitone of tolerance and comfortably under
/// E1 (41.2), which is the lowest note a guitar-shaped instrument in this rig is likely to play;
/// 1400 Hz is above the 24th fret of the high E (1318.5). Both are well inside what the core's
/// decimated analysis window can find, which is why it takes them rather than the core's own wider
/// defaults.
constexpr float kA4 = 440.0f;
constexpr float kMinHz = 38.0f;
constexpr float kMaxHz = 1400.0f;
/// The level published when the tuner has heard nothing, and the floor a level readout shows.
constexpr float kFloorDb = -120.0f;

/// \brief What the render thread has heard, as the control thread may read it.
///
/// Plain values, copied out of the atomics in one epoch-guarded read (see read_report), so the
/// reporting code works on a consistent set rather than on a snapshot that changed underneath it.
struct Report
{
  float hz = 0.0f;
  float steady_hz = 0.0f;
  float cents = 0.0f;
  float clarity = 0.0f;
  float uncertainty_cents = 0.0f;
  float level_db = kFloorDb;
  int midi = 0;
  int partials = 0;
  uint32_t blocks = 0;
  bool valid = false;
};

struct TunerState
{
  static_assert(std::atomic<float>::is_always_lock_free, "the tuner report must be lock-free");
  static_assert(std::atomic<uint32_t>::is_always_lock_free, "the tuner report's counters must be lock-free");
  static_assert(std::atomic<int32_t>::is_always_lock_free, "the tuner report's integers must be lock-free");

  squatch::tuner::Tuner tuner;
  /// Blocks processed since the node was built. Only the render thread writes it; the published
  /// copy is what a client sees, and it is what lets a client tell a held reading from a live one.
  uint32_t blocks = 0;

  /// \brief The epoch: odd while the render thread is writing the fields below, even when settled.
  std::atomic<uint32_t> published{0};
  std::atomic<float> report_hz{0.0f};
  std::atomic<float> report_steady_hz{0.0f};
  std::atomic<float> report_cents{0.0f};
  std::atomic<float> report_clarity{0.0f};
  std::atomic<float> report_uncertainty{0.0f};
  std::atomic<float> report_level_db{kFloorDb};
  std::atomic<int32_t> report_midi{0};
  std::atomic<int32_t> report_partials{0};
  std::atomic<uint32_t> report_blocks{0};
  /// Bit 0: the reading is one a player may trust. A word rather than a bool atomic, as nc.eq.flow
  /// packs its flags, so the whole report is one epoch.
  std::atomic<uint32_t> report_flags{0};
};

int octave_of(int midi) noexcept
{
  // Scientific pitch: MIDI 60 is C4. Integer division truncates toward zero, which is the wrong
  // direction below MIDI 0 -- a range the tuner cannot reach (its floor is 38 Hz, MIDI 23).
  return midi / 12 - 1;
}

const char* note_name(int midi) noexcept
{
  static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int pitch_class = ((midi % 12) + 12) % 12;
  return names[pitch_class];
}

float level_db_of(float level) noexcept
{
  if (!(level > 0.0f))
    return kFloorDb;
  return std::max(kFloorDb, static_cast<float>(20.0 * std::log10(level)));
}

// --- the report, render thread -> control thread ----------------------------

void publish(TunerState& state) noexcept
{
  const squatch::tuner::TunerReading& reading = state.tuner.reading();
  const uint32_t epoch = state.published.load(std::memory_order_relaxed);
  state.published.store(epoch + 1, std::memory_order_relaxed); // odd: a write is in progress
  std::atomic_thread_fence(std::memory_order_release);
  state.report_hz.store(reading.hz, std::memory_order_relaxed);
  state.report_steady_hz.store(reading.steadyHz, std::memory_order_relaxed);
  state.report_cents.store(reading.cents, std::memory_order_relaxed);
  state.report_clarity.store(reading.clarity, std::memory_order_relaxed);
  state.report_uncertainty.store(reading.uncertainty, std::memory_order_relaxed);
  state.report_level_db.store(level_db_of(reading.level), std::memory_order_relaxed);
  state.report_midi.store(reading.midi, std::memory_order_relaxed);
  state.report_partials.store(reading.partials, std::memory_order_relaxed);
  state.report_blocks.store(state.blocks, std::memory_order_relaxed);
  state.report_flags.store(reading.valid ? 1u : 0u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  state.published.store(epoch + 2, std::memory_order_relaxed); // even: settled
}

/// \return false when the render thread wrote throughout, which is a `rig.state` read to skip
///         rather than one to report from.
bool read_report(const TunerState& state, Report& out) noexcept
{
  for (int attempt = 0; attempt < 8; ++attempt)
  {
    const uint32_t before = state.published.load(std::memory_order_acquire);
    if (before & 1u)
      continue; // a write is in progress
    out.hz = state.report_hz.load(std::memory_order_relaxed);
    out.steady_hz = state.report_steady_hz.load(std::memory_order_relaxed);
    out.cents = state.report_cents.load(std::memory_order_relaxed);
    out.clarity = state.report_clarity.load(std::memory_order_relaxed);
    out.uncertainty_cents = state.report_uncertainty.load(std::memory_order_relaxed);
    out.level_db = state.report_level_db.load(std::memory_order_relaxed);
    out.midi = state.report_midi.load(std::memory_order_relaxed);
    out.partials = state.report_partials.load(std::memory_order_relaxed);
    out.blocks = state.report_blocks.load(std::memory_order_relaxed);
    out.valid = (state.report_flags.load(std::memory_order_relaxed) & 1u) != 0u;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (before == state.published.load(std::memory_order_relaxed))
      return true;
  }
  return false;
}

// --- the node ---------------------------------------------------------------

double round_to(double value, double places)
{
  return std::round(value * places) / places;
}

void* create_tuner(const graph::NodeSpec&, const CompileOptions& options, std::string& error)
{
  if (!(options.sample_rate > 0.0))
  {
    error = "nc.tuner: the plan has no sample rate";
    return nullptr;
  }
  auto state = std::make_unique<TunerState>();
  squatch::tuner::TunerConfig config;
  config.sampleRate = static_cast<float>(options.sample_rate);
  config.a4 = kA4;
  config.minHz = kMinHz;
  config.maxHz = kMaxHz;
  // configure() is where the core builds its tables and its decimation filter; it is not real-time
  // safe, which is why it happens here, on the control thread, and never again.
  state->tuner.configure(config);
  publish(*state);
  return state.release();
}

void destroy_tuner(void* raw)
{
  delete static_cast<TunerState*>(raw);
}

void process_tuner(void* raw, const float* const* in, float* const* out, int frames) noexcept
{
  auto* state = static_cast<TunerState*>(raw);
  if (state == nullptr)
    return;
  // The compiler binds an unconnected input port to the plan's silence buffer, so in[0] is a real
  // buffer in every legal graph; the null check is for the frames == 0 path a plan may take.
  const float* input = (frames > 0 && in != nullptr) ? in[0] : nullptr;
  if (input != nullptr)
    state->tuner.process(input, frames);
  // Silence, whatever the input was. This is the whole of the node's audio behaviour.
  if (frames > 0 && out != nullptr)
    std::fill_n(out[0], frames, 0.0f);
  ++state->blocks;
  publish(*state);
}

/// \brief What the running node has heard, for a client that wants to draw it (`rig.state`).
///
/// Control thread. The values come from the render thread's snapshot, so a client sees what the
/// node heard at the last block rather than a value recomputed here.
std::string describe_tuner(void* raw)
{
  auto* state = static_cast<TunerState*>(raw);
  if (state == nullptr)
    return {};
  Report report;
  if (!read_report(*state, report))
    return {};

  nlohmann::json json;
  json["a4"] = kA4;
  // The counter wraps (a uint32 at 750 blocks a second is 66 days); a client compares two reads of
  // it to tell a live reading from a frozen one, so a wrap is not a problem, a stall is.
  json["blocks"] = report.blocks;
  json["valid"] = report.valid;
  json["level_db"] = round_to(report.level_db, 10.0);
  if (report.valid)
  {
    // Everything below is about a note. When there is none these keys are *absent* rather than
    // stale, so a client cannot draw the last note as if it were the current one.
    json["midi"] = report.midi;
    json["note"] = note_name(report.midi);
    json["octave"] = octave_of(report.midi);
    json["cents"] = round_to(report.cents, 100.0);
    json["hz"] = round_to(report.hz, 100.0);
    json["steady_hz"] = round_to(report.steady_hz, 100.0);
    json["clarity"] = round_to(report.clarity, 1000.0);
    json["uncertainty_cents"] = round_to(report.uncertainty_cents, 1000.0);
    json["partials"] = report.partials;
  }
  return json.dump();
}

} // namespace

// Field order matches NodeType (plan.h). `in_palette = false` is the schema saying a palette should
// not offer this type: it is a view's node, mounted by a client while its tuner is on screen
// (docs/tuner.md), not a block a player adds to a chain. A document may still name it, because that
// is how it is mounted -- unlike an `internal` type, which only the engine may place.
const NodeType kTuner{.id = "nc.tuner",
                      .name = "Tuner",
                      .category = "utility",
                      .inputs = 1,
                      .outputs = 1,
                      .create = create_tuner,
                      .destroy = destroy_tuner,
                      .process = process_tuner,
                      .in_palette = false,
                      .describe_state = describe_tuner};

} // namespace render
} // namespace namchain
