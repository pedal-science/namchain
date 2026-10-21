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
// The node has one parameter, `mute`, and it is what a pedal tuner's switch does: on (the default),
// the output is silence; off, the input passes through unchanged, and the tuner keeps listening so
// its display carries on. It is a *rampable* flag, as nc.mix's lane mutes are, so a client's toggle
// is one `rig.patch` -- a smooth op that ramps the pass-through inside the node over a few
// milliseconds, with no rebuild, no crossfade and no gap in the reading. A rebuild there would be
// visible as the strobe restarting, which is the one thing a player is watching.
//
// A4 is the fixed 440 Hz a guitar pedal tunes to; the note range covers a 5-string bass on the way
// up and a 24th-fret high E on the way down. A reference pitch control would be another parameter
// and a UI, not an engine default, and when someone wants one it arrives as `a4` with the schema's
// range around it (docs/tuner.md).

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

/// The parameters, in id order: `params[i]` has id `i + 1` (plan.h).
constexpr uint32_t kTunerParamMute = 1;

const ParamDesc kTunerParams[] = {
  // A flag with a rampable change is a switch, not a structural edit: `set_param_tuner` writes the
  // target and `process_tuner` ramps to it, so unmuting the rig is a fade rather than a rebuild
  // (nc.mix's lane mutes are the same shape).
  {.id = "mute", .name = "Mute", .kind = ParamKind::Flag, .default_value = 1.0, .rampable = true},
};

/// How long the pass-through takes to open or close. Long enough to be a fade rather than a click,
/// short enough that a player hears the switch act.
constexpr double kMuteRampSeconds = 0.005;

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
  /// The switch as the client set it, not the value a ramp is passing through: a toggle that
  /// reported the in-flight fade would flicker back on the way.
  bool muted = true;
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

  /// The pass-through, as a gain of `1 - mute` applied to the output. Written by `set_param_tuner`
  /// on the control thread and read here per block, which is the same contract nc.gain's ramp has:
  /// one writer, one reader, and a ramp that restarts from wherever the last one had got to.
  float mute = 1.0f;
  float mute_from = 1.0f;
  float mute_to = 1.0f;
  int mute_pos = 0;
  int mute_len = 0;
  /// The length `set_param_tuner` gives a new ramp, from the plan's sample rate.
  int mute_ramp_samples = 0;

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
  // Bit 0: the reading is trustworthy. Bit 1: the output is silent, which is the switch's target
  // rather than the ramp's current value.
  state.report_flags.store((reading.valid ? 1u : 0u) | (state.mute_to != 0.0f ? 2u : 0u),
                           std::memory_order_relaxed);
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
    const uint32_t flags = state.report_flags.load(std::memory_order_relaxed);
    out.valid = (flags & 1u) != 0u;
    out.muted = (flags & 2u) != 0u;
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

double number_param(const graph::NodeSpec& spec, const std::string& name, double fallback)
{
  const auto it = spec.params.find(name);
  return it == spec.params.end() ? fallback : it->second;
}

void* create_tuner(const graph::NodeSpec& spec, const CompileOptions& options, std::string& error)
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
  state->mute_ramp_samples =
    std::max(1, static_cast<int>(std::lround(kMuteRampSeconds * options.sample_rate)));
  // A document may mount the node already open — `"mute": 0` — which is how a client that remembers
  // "tuning with sound on" mounts it, and what the schema's honesty test renders against the
  // default. It starts settled rather than ramping: there is nothing to fade from at build time.
  const bool muted = number_param(spec, "mute", 1.0) >= 0.5;
  state->mute = state->mute_from = state->mute_to = muted ? 1.0f : 0.0f;
  publish(*state);
  return state.release();
}

void destroy_tuner(void* raw)
{
  delete static_cast<TunerState*>(raw);
}

/// The switch. Called on the control thread against the live plan's state, so it only ever writes a
/// target and a ramp: nothing here allocates, locks or throws, and a second toggle mid-ramp starts
/// from wherever the first had got to rather than from its own endpoint (nc.gain's rule).
bool set_param_tuner(void* raw, uint32_t param_id, float value) noexcept
{
  auto* state = static_cast<TunerState*>(raw);
  if (state == nullptr || !std::isfinite(value))
    return false;
  if (param_id != kTunerParamMute)
    return false;
  // A flag is 0 or 1, and a value in between is a client bug rather than a half-muted rig. The
  // ramp is what makes the change a fade, not the number.
  state->mute_from = state->mute;
  state->mute_to = value >= 0.5f ? 1.0f : 0.0f;
  state->mute_pos = 0;
  state->mute_len = state->mute_ramp_samples;
  return true;
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
  // The output: silence while the switch is on, the input unchanged while it is off, and a fade
  // between the two for as long as a ramp lasts. The tuner above ran either way -- that is what
  // makes "unmute to hear yourself, keep reading" one switch rather than two.
  if (frames > 0 && out != nullptr)
  {
    float* dst = out[0];
    if (state->mute_pos >= state->mute_len)
    {
      // Settled, which is the usual block: exact, and the cheap path for both ends.
      if (state->mute_to != 0.0f)
        std::fill_n(dst, frames, 0.0f);
      else if (input != nullptr)
        std::copy_n(input, frames, dst);
      else
        std::fill_n(dst, frames, 0.0f);
      state->mute = state->mute_to;
    }
    else
    {
      const float from = state->mute_from;
      const float to = state->mute_to;
      const float span = static_cast<float>(state->mute_len);
      int pos = state->mute_pos;
      float mute = state->mute;
      for (int i = 0; i < frames; ++i)
      {
        if (pos < state->mute_len)
          mute = from + (to - from) * (static_cast<float>(++pos) / span);
        else
          mute = to;
        const float gain = 1.0f - mute;
        dst[i] = (input != nullptr) ? input[i] * gain : 0.0f;
      }
      state->mute = mute;
      state->mute_pos = pos;
    }
  }
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
  // The switch as it was set, which is what a toggle binds to. A client that mounts the node gets
  // the default (muted) from the schema and reads the truth from here, so the two cannot drift.
  json["mute"] = report.muted;
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
                      .params = kTunerParams,
                      .param_count = 1,
                      .set_param = set_param_tuner,
                      .in_palette = false,
                      .describe_state = describe_tuner};

} // namespace render
} // namespace namchain
