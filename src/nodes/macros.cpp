// The first macros: the blocks ljoss offers that are not primitives (docs/ui-integration-plan.md
// slice E, docs/macros.md).
//
// Each one is a declaration and, where the declarative shape is not enough, a small `apply` function.
// The division of labour is deliberate:
//
//   * the **shape** (roles, node types, edges, port links) is a table, so a reader can see the
//     lowered graph without reading code;
//   * the **param map** maps each macro parameter to the node parameters it drives, so a macro-level
//     `set_param` and a compile derive their values from one declaration;
//   * `apply` fills in what neither can express: a mode, an arity, a line length, a bypass that a
//     selection parameter implies.
//
// No DSP lives here. Everything below lowers to biquads, gains, mixes and the existing delay line --
// which is the point of the milestone: the macros are vocabulary, not a second DSP library.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "namchain/graph/macro.h"

namespace namchain
{
namespace graph
{
namespace
{

void set_biquad(NodeSpec& node, const char* mode, double freq, double q, double gain_db)
{
  node.strings["mode"] = mode;
  node.params["freq"] = freq;
  node.params["q"] = q;
  node.params["gain_db"] = gain_db;
}

/// A linear gain, which is the parameter a crossfade wants: `gain` is the linear alias of `nc.gain`'s
/// dB parameter, and a dry/wet pair has to sum to one in the linear domain, not the dB one.
void set_gain(NodeSpec& node, double gain)
{
  node.params["gain"] = gain;
}

/// \brief The Butterworth stage Qs for an order of 2n, lowest first.
///
/// A 24 dB/oct filter is not one biquad at Q=0.707 but two at 0.541 and 1.307: cascading identical
/// stages gives a bump at the corner and a response that is not maximally flat. Encoding the real
/// tables is the difference between "a slope picker" and "four filters in a row", and it is exactly
/// the kind of thing that belongs in the engine rather than in every client.
const double kButterworthQ[4][4] = {
  {0.7071067811865476, 0.0, 0.0, 0.0},
  {0.5411961001461971, 1.3065629648763766, 0.0, 0.0},
  {0.5176380902050415, 0.7071067811865476, 1.9318516525781366, 0.0},
  {0.5097955791041592, 0.6013448869350453, 0.8999762231364156, 2.5629154477415055},
};

int stages_for_slope(double slope)
{
  const int stages = static_cast<int>(std::lround(slope / 12.0));
  return stages < 1 ? 1 : (stages > 4 ? 4 : stages);
}

// ---------------------------------------------------------------------------
// fx.eq.bands: a four-band EQ with shelving ends
// ---------------------------------------------------------------------------

struct BandDecl
{
  const char* role;
  const char* freq;
  const char* gain;
  const char* q;
  const char* mode;
};

const BandDecl kEqBands[] = {
  {"band1", "freq1", "gain1", "q1", "lowshelf"},
  {"band2", "freq2", "gain2", "q2", "peaking"},
  {"band3", "freq3", "gain3", "q3", "peaking"},
  {"band4", "freq4", "gain4", "q4", "highshelf"},
};

bool apply_eq_bands(Lowering& lowering, std::string& error)
{
  (void)error;
  const int bands = static_cast<int>(std::lround(lowering.number("bands")));
  for (int i = 0; i < 4; ++i)
  {
    const BandDecl& band = kEqBands[i];
    NodeSpec& node = lowering.node(band.role);
    set_biquad(node, band.mode, lowering.number(band.freq), lowering.number(band.q), lowering.number(band.gain));
    // A band beyond the count is *bypassed*, not removed: the shape of the lowering does not depend on
    // the value of a parameter, so `gain3` keeps meaning something when `bands` moves back to 4.
    node.bypass = node.bypass || i >= bands;
  }
  return true;
}

const MacroTarget kEqBandsFreq1[] = {{"band1", "freq", Transform::Direct}};
const MacroTarget kEqBandsFreq2[] = {{"band2", "freq", Transform::Direct}};
const MacroTarget kEqBandsFreq3[] = {{"band3", "freq", Transform::Direct}};
const MacroTarget kEqBandsFreq4[] = {{"band4", "freq", Transform::Direct}};
const MacroTarget kEqBandsGain1[] = {{"band1", "gain_db", Transform::Direct}};
const MacroTarget kEqBandsGain2[] = {{"band2", "gain_db", Transform::Direct}};
const MacroTarget kEqBandsGain3[] = {{"band3", "gain_db", Transform::Direct}};
const MacroTarget kEqBandsGain4[] = {{"band4", "gain_db", Transform::Direct}};
const MacroTarget kEqBandsQ1[] = {{"band1", "q", Transform::Direct}};
const MacroTarget kEqBandsQ2[] = {{"band2", "q", Transform::Direct}};
const MacroTarget kEqBandsQ3[] = {{"band3", "q", Transform::Direct}};
const MacroTarget kEqBandsQ4[] = {{"band4", "q", Transform::Direct}};

const MacroParamDecl kEqBandsParams[] = {
  // No targets: the band count is what the lowering reads to decide which bands are live, so editing
  // it is a relowering rather than a value written to a node.
  {.desc = {.id = "bands", .name = "Bands", .kind = render::ParamKind::Number, .default_value = 4.0,
            .has_min = true, .min = 1.0, .has_max = true, .max = 4.0, .step = 1.0, .snap = true}},
  {.desc = {.id = "freq1", .name = "Low frequency", .unit = "Hz", .default_value = 100.0, .has_min = true,
            .min = 20.0, .has_max = true, .max = 20000.0, .scale = render::ParamScale::Log,
            .rampable = true},
   .targets = kEqBandsFreq1, .target_count = 1},
  {.desc = {.id = "gain1", .name = "Low gain", .unit = "dB", .default_value = 0.0, .has_min = true,
            .min = -24.0, .has_max = true, .max = 24.0, .rampable = true},
   .targets = kEqBandsGain1, .target_count = 1},
  {.desc = {.id = "q1", .name = "Low slope", .default_value = 0.707, .has_min = true, .min = 0.1,
            .has_max = true, .max = 10.0, .scale = render::ParamScale::Log, .rampable = true},
   .targets = kEqBandsQ1, .target_count = 1},
  {.desc = {.id = "freq2", .name = "Low-mid frequency", .unit = "Hz", .default_value = 400.0,
            .has_min = true, .min = 20.0, .has_max = true, .max = 20000.0,
            .scale = render::ParamScale::Log, .rampable = true},
   .targets = kEqBandsFreq2, .target_count = 1},
  {.desc = {.id = "gain2", .name = "Low-mid gain", .unit = "dB", .default_value = 0.0, .has_min = true,
            .min = -24.0, .has_max = true, .max = 24.0, .rampable = true},
   .targets = kEqBandsGain2, .target_count = 1},
  {.desc = {.id = "q2", .name = "Low-mid Q", .default_value = 0.707, .has_min = true, .min = 0.1,
            .has_max = true, .max = 10.0, .scale = render::ParamScale::Log, .rampable = true},
   .targets = kEqBandsQ2, .target_count = 1},
  {.desc = {.id = "freq3", .name = "High-mid frequency", .unit = "Hz", .default_value = 1600.0,
            .has_min = true, .min = 20.0, .has_max = true, .max = 20000.0,
            .scale = render::ParamScale::Log, .rampable = true},
   .targets = kEqBandsFreq3, .target_count = 1},
  {.desc = {.id = "gain3", .name = "High-mid gain", .unit = "dB", .default_value = 0.0, .has_min = true,
            .min = -24.0, .has_max = true, .max = 24.0, .rampable = true},
   .targets = kEqBandsGain3, .target_count = 1},
  {.desc = {.id = "q3", .name = "High-mid Q", .default_value = 0.707, .has_min = true, .min = 0.1,
            .has_max = true, .max = 10.0, .scale = render::ParamScale::Log, .rampable = true},
   .targets = kEqBandsQ3, .target_count = 1},
  {.desc = {.id = "freq4", .name = "High frequency", .unit = "Hz", .default_value = 6400.0,
            .has_min = true, .min = 20.0, .has_max = true, .max = 20000.0,
            .scale = render::ParamScale::Log, .rampable = true},
   .targets = kEqBandsFreq4, .target_count = 1},
  {.desc = {.id = "gain4", .name = "High gain", .unit = "dB", .default_value = 0.0, .has_min = true,
            .min = -24.0, .has_max = true, .max = 24.0, .rampable = true},
   .targets = kEqBandsGain4, .target_count = 1},
  {.desc = {.id = "q4", .name = "High slope", .default_value = 0.707, .has_min = true, .min = 0.1,
            .has_max = true, .max = 10.0, .scale = render::ParamScale::Log, .rampable = true},
   .targets = kEqBandsQ4, .target_count = 1},
};

const MacroNodeDecl kEqBandsNodes[] = {
  {"band1", "nc.eq.biquad"}, {"band2", "nc.eq.biquad"}, {"band3", "nc.eq.biquad"}, {"band4", "nc.eq.biquad"}};
const MacroEdge kEqBandsEdges[] = {{"band1", 0, "band2", 0}, {"band2", 0, "band3", 0}, {"band3", 0, "band4", 0}};
const MacroPortLink kEqBandsIn[] = {{0, "band1", 0}};
const MacroPortLink kEqBandsOut[] = {{0, "band4", 0}};

// ---------------------------------------------------------------------------
// fx.eq.tilt: one knob, two shelves
// ---------------------------------------------------------------------------

bool apply_eq_tilt(Lowering& lowering, std::string& error)
{
  (void)error;
  const double freq = lowering.number("freq");
  const double tilt = lowering.number("tilt");
  set_biquad(lowering.node("low"), "lowshelf", freq, 0.707, -tilt / 2.0);
  set_biquad(lowering.node("high"), "highshelf", freq, 0.707, tilt / 2.0);
  return true;
}

// Half the tilt each way: the knob is the *difference* between the two ends, which is what makes it
// feel like one control rather than two.
const MacroTarget kEqTiltTargets[] = {{"low", "gain_db", Transform::ScaleBy, -0.5},
                                      {"high", "gain_db", Transform::ScaleBy, 0.5}};
const MacroTarget kEqTiltFreq[] = {{"low", "freq", Transform::Direct}, {"high", "freq", Transform::Direct}};

const MacroParamDecl kEqTiltParams[] = {
  {.desc = {.id = "tilt", .name = "Tilt", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -12.0,
            .has_max = true, .max = 12.0, .rampable = true},
   .targets = kEqTiltTargets, .target_count = 2},
  {.desc = {.id = "freq", .name = "Centre", .unit = "Hz", .default_value = 700.0, .has_min = true,
            .min = 100.0, .has_max = true, .max = 8000.0, .scale = render::ParamScale::Log,
            .rampable = true},
   .targets = kEqTiltFreq, .target_count = 2},
};

const MacroNodeDecl kEqTiltNodes[] = {{"low", "nc.eq.biquad"}, {"high", "nc.eq.biquad"}};
const MacroEdge kEqTiltEdges[] = {{"low", 0, "high", 0}};
const MacroPortLink kEqTiltIn[] = {{0, "low", 0}};
const MacroPortLink kEqTiltOut[] = {{0, "high", 0}};

// ---------------------------------------------------------------------------
// fx.filter.cut: a low cut and a high cut, each with a slope in stages
// ---------------------------------------------------------------------------

const char* const kLowCutRoles[] = {"low1", "low2", "low3", "low4"};
const char* const kHighCutRoles[] = {"high1", "high2", "high3", "high4"};

bool apply_filter_cut(Lowering& lowering, std::string& error)
{
  (void)error;
  const int low_stages = stages_for_slope(lowering.number("low_slope"));
  const int high_stages = stages_for_slope(lowering.number("high_slope"));
  for (int i = 0; i < 4; ++i)
  {
    const bool low_active = i < low_stages;
    const bool high_active = i < high_stages;
    NodeSpec& low = lowering.node(kLowCutRoles[i]);
    set_biquad(low, "highpass", lowering.number("low_cut"),
               low_active ? kButterworthQ[low_stages - 1][i] : 0.707, 0.0);
    low.bypass = low.bypass || !low_active;

    NodeSpec& high = lowering.node(kHighCutRoles[i]);
    set_biquad(high, "lowpass", lowering.number("high_cut"),
               high_active ? kButterworthQ[high_stages - 1][i] : 0.707, 0.0);
    high.bypass = high.bypass || !high_active;
  }
  return true;
}

const render::OptionDesc kCutSlopes[] = {{"12", "12 dB/oct"}, {"24", "24 dB/oct"}, {"36", "36 dB/oct"},
                                 {"48", "48 dB/oct"}};

/// Every stage of a cut carries the same cutoff, so one parameter has four targets. The bypassed
/// stages take the value too -- they are running, they are simply not in the signal path -- which is
/// what keeps a slope change from invalidating the value the client is holding.
const MacroTarget kLowCutTargets[] = {{"low1", "freq", Transform::Direct}, {"low2", "freq", Transform::Direct},
                                      {"low3", "freq", Transform::Direct}, {"low4", "freq", Transform::Direct}};
const MacroTarget kHighCutTargets[] = {{"high1", "freq", Transform::Direct},
                                       {"high2", "freq", Transform::Direct},
                                       {"high3", "freq", Transform::Direct},
                                       {"high4", "freq", Transform::Direct}};

const MacroParamDecl kFilterCutParams[] = {
  {.desc = {.id = "low_cut", .name = "Low cut", .unit = "Hz", .default_value = 20.0, .has_min = true,
            .min = 10.0, .has_max = true, .max = 2000.0, .scale = render::ParamScale::Log,
            .rampable = true},
   .targets = kLowCutTargets, .target_count = 4},
  {.desc = {.id = "low_slope", .name = "Low cut slope", .kind = render::ParamKind::Option,
            .default_value = 0.0, .options = kCutSlopes, .option_count = 4},
   .targets = nullptr, .target_count = 0},
  {.desc = {.id = "high_cut", .name = "High cut", .unit = "Hz", .default_value = 20000.0, .has_min = true,
            .min = 1000.0, .has_max = true, .max = 24000.0, .scale = render::ParamScale::Log,
            .rampable = true},
   .targets = kHighCutTargets, .target_count = 4},
  {.desc = {.id = "high_slope", .name = "High cut slope", .kind = render::ParamKind::Option,
            .default_value = 0.0, .options = kCutSlopes, .option_count = 4},
   .targets = nullptr, .target_count = 0},
};

const MacroNodeDecl kFilterCutNodes[] = {{"low1", "nc.eq.biquad"}, {"low2", "nc.eq.biquad"},
                                         {"low3", "nc.eq.biquad"}, {"low4", "nc.eq.biquad"},
                                         {"high1", "nc.eq.biquad"}, {"high2", "nc.eq.biquad"},
                                         {"high3", "nc.eq.biquad"}, {"high4", "nc.eq.biquad"}};
const MacroEdge kFilterCutEdges[] = {{"low1", 0, "low2", 0}, {"low2", 0, "low3", 0}, {"low3", 0, "low4", 0},
                                     {"low4", 0, "high1", 0}, {"high1", 0, "high2", 0},
                                     {"high2", 0, "high3", 0}, {"high3", 0, "high4", 0}};
const MacroPortLink kFilterCutIn[] = {{0, "low1", 0}};
const MacroPortLink kFilterCutOut[] = {{0, "high4", 0}};

// ---------------------------------------------------------------------------
// fx.mix.drywet: two lanes, one knob
// ---------------------------------------------------------------------------

bool apply_mix_drywet(Lowering& lowering, std::string& error)
{
  (void)error;
  const double mix = lowering.number("mix");
  set_gain(lowering.node("dry"), 1.0 - mix);
  set_gain(lowering.node("wet"), mix);
  lowering.node("sum").params["inputs"] = 2.0;
  lowering.node("level").params["gain_db"] = lowering.number("level_db");
  return true;
}

const MacroTarget kDryWetMix[] = {{"dry", "gain", Transform::OneMinus}, {"wet", "gain", Transform::Direct}};
const MacroTarget kDryWetLevel[] = {{"level", "gain_db", Transform::Direct}};

const MacroParamDecl kMixDryWetParams[] = {
  // One knob, two gains, and the map says both: a crossfade whose map named only the wet leg would
  // compile to a crossfade and patch to a level change, which is the kind of disagreement the
  // parameter-map test exists to catch.
  {.desc = {.id = "mix", .name = "Mix", .unit = "%", .default_value = 1.0, .has_min = true, .min = 0.0,
            .has_max = true, .max = 1.0, .rampable = true},
   .targets = kDryWetMix, .target_count = 2},
  {.desc = {.id = "level_db", .name = "Level", .unit = "dB", .default_value = 0.0, .has_min = true,
            .min = -24.0, .has_max = true, .max = 12.0, .rampable = true},
   .targets = kDryWetLevel, .target_count = 1},
};

const MacroNodeDecl kMixDryWetNodes[] = {
  {"dry", "nc.gain"}, {"wet", "nc.gain"}, {"sum", "nc.mix"}, {"level", "nc.gain"}};
const MacroEdge kMixDryWetEdges[] = {{"dry", 0, "sum", 0}, {"wet", 0, "sum", 1}, {"sum", 0, "level", 0}};
const MacroPortLink kMixDryWetIn[] = {{0, "dry", 0}, {1, "wet", 0}};
const MacroPortLink kMixDryWetOut[] = {{0, "level", 0}};

// ---------------------------------------------------------------------------
// fx.delay.feedback: a delay line with feedback and a dry/wet pair
// ---------------------------------------------------------------------------

bool apply_delay_feedback(Lowering& lowering, std::string& error)
{
  (void)error;
  const double mix = lowering.number("mix");
  set_gain(lowering.node("dry"), 1.0 - mix);
  set_gain(lowering.node("wet"), mix);
  set_gain(lowering.node("fb"), lowering.number("feedback"));

  NodeSpec& line = lowering.node("line");
  // The macro's own range is what the line has to be able to hold: `ms` is the delay time and
  // `max_ms` the length of the buffer behind it, and the delay node refuses a time beyond its
  // declared capacity rather than wrapping the read pointer into recent audio (slice B3).
  const double longest = 1000.0;
  line.params["ms"] = lowering.number("time");
  line.params["max_ms"] = longest;
  lowering.node("sum").params["inputs"] = 2.0;
  lowering.node("out").params["inputs"] = 2.0;
  return true;
}

const MacroTarget kDelayTime[] = {{"line", "ms", Transform::Direct}};
const MacroTarget kDelayFeedback[] = {{"fb", "gain", Transform::Direct}};
const MacroTarget kDelayMix[] = {{"dry", "gain", Transform::OneMinus}, {"wet", "gain", Transform::Direct}};

const MacroParamDecl kDelayFeedbackParams[] = {
  {.desc = {.id = "time", .name = "Time", .unit = "ms", .default_value = 300.0, .has_min = true, .min = 1.0,
            .has_max = true, .max = 1000.0, .scale = render::ParamScale::Log, .rampable = true},
   .targets = kDelayTime, .target_count = 1},
  {.desc = {.id = "feedback", .name = "Feedback", .default_value = 0.35, .has_min = true, .min = 0.0,
            .has_max = true, .max = 0.95, .rampable = true},
   .targets = kDelayFeedback, .target_count = 1},
  {.desc = {.id = "mix", .name = "Mix", .unit = "%", .default_value = 0.3, .has_min = true, .min = 0.0,
            .has_max = true, .max = 1.0, .rampable = true},
   .targets = kDelayMix, .target_count = 2},
};

const MacroNodeDecl kDelayFeedbackNodes[] = {{"dry", "nc.gain"},  {"sum", "nc.mix"}, {"line", "nc.delay"},
                                             {"fb", "nc.gain"},   {"tap", "nc.unit_delay"},
                                             {"wet", "nc.gain"},  {"out", "nc.mix"}};
// The cycle is `sum -> line -> fb -> tap -> sum`, and `tap` is the block-delay node that makes it a
// DAG: without it the compiler refuses the graph, which is the engine's rule for feedback (§5.1).
const MacroEdge kDelayFeedbackEdges[] = {
  {"sum", 0, "line", 0}, {"line", 0, "fb", 0},  {"fb", 0, "tap", 0},   {"tap", 0, "sum", 1},
  {"line", 0, "wet", 0}, {"dry", 0, "out", 0},  {"wet", 0, "out", 1}};
// The input goes two ways: into the dry leg and into the delay's summing node.
const MacroPortLink kDelayFeedbackIn[] = {{0, "dry", 0}, {0, "sum", 0}};
const MacroPortLink kDelayFeedbackOut[] = {{0, "out", 0}};

// ---------------------------------------------------------------------------
// fx.amp.tonestack: bass, mid and treble
// ---------------------------------------------------------------------------

bool apply_amp_tonestack(Lowering& lowering, std::string& error)
{
  (void)error;
  // The shelf corners are part of the voicing rather than knobs: what a player turns is bass, mid and
  // treble, and the mid frequency. Handing a client the shelf corners as well would be four more
  // controls on a block that is supposed to be three.
  set_biquad(lowering.node("bass"), "lowshelf", 120.0, 0.707, lowering.number("bass"));
  set_biquad(lowering.node("mid"), "peaking", lowering.number("mid_freq"), 0.7, lowering.number("mid"));
  set_biquad(lowering.node("treble"), "highshelf", 3500.0, 0.707, lowering.number("treble"));
  return true;
}

const MacroTarget kToneBass[] = {{"bass", "gain_db", Transform::Direct}};
const MacroTarget kToneMid[] = {{"mid", "gain_db", Transform::Direct}};
const MacroTarget kToneTreble[] = {{"treble", "gain_db", Transform::Direct}};
const MacroTarget kToneMidFreq[] = {{"mid", "freq", Transform::Direct}};

const MacroParamDecl kAmpTonestackParams[] = {
  {.desc = {.id = "bass", .name = "Bass", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -18.0,
            .has_max = true, .max = 18.0, .rampable = true},
   .targets = kToneBass, .target_count = 1},
  {.desc = {.id = "mid", .name = "Mid", .unit = "dB", .default_value = 0.0, .has_min = true, .min = -18.0,
            .has_max = true, .max = 18.0, .rampable = true},
   .targets = kToneMid, .target_count = 1},
  {.desc = {.id = "treble", .name = "Treble", .unit = "dB", .default_value = 0.0, .has_min = true,
            .min = -18.0, .has_max = true, .max = 18.0, .rampable = true},
   .targets = kToneTreble, .target_count = 1},
  {.desc = {.id = "mid_freq", .name = "Mid frequency", .unit = "Hz", .default_value = 500.0,
            .has_min = true, .min = 200.0, .has_max = true, .max = 2000.0,
            .scale = render::ParamScale::Log, .rampable = true},
   .targets = kToneMidFreq, .target_count = 1},
};

const MacroNodeDecl kAmpTonestackNodes[] = {
  {"bass", "nc.eq.biquad"}, {"mid", "nc.eq.biquad"}, {"treble", "nc.eq.biquad"}};
const MacroEdge kAmpTonestackEdges[] = {{"bass", 0, "mid", 0}, {"mid", 0, "treble", 0}};
const MacroPortLink kAmpTonestackIn[] = {{0, "bass", 0}};
const MacroPortLink kAmpTonestackOut[] = {{0, "treble", 0}};

// ---------------------------------------------------------------------------
// fx.cab.ir: a cabinet IR with the cuts a cab block needs
// ---------------------------------------------------------------------------

bool apply_cab_ir(Lowering& lowering, std::string& error)
{
  const std::string ir = lowering.text("ir");
  if (ir.empty())
  {
    error = "'ir' must name an impulse response from the device's library";
    return false;
  }
  lowering.node("ir").strings["ir"] = ir;
  set_biquad(lowering.node("low"), "highpass", lowering.number("low_cut"), 0.707, 0.0);
  set_biquad(lowering.node("high"), "lowpass", lowering.number("high_cut"), 0.707, 0.0);
  lowering.node("level").params["gain_db"] = lowering.number("level_db");
  return true;
}

const MacroTarget kCabIrLowCut[] = {{"low", "freq", Transform::Direct}};
const MacroTarget kCabIrHighCut[] = {{"high", "freq", Transform::Direct}};
const MacroTarget kCabIrLevel[] = {{"level", "gain_db", Transform::Direct}};
// A string target: the IR's id, resolved to a path by the asset registry before lowering. Not
// rampable, so an IR change is a structural edit -- which is also the honest answer, since swapping a
// cabinet is not a knob.
const MacroTarget kCabIrIr[] = {{"ir", "ir", Transform::Direct}};

const MacroParamDecl kCabIrParams[] = {
  {.desc = {.id = "ir", .name = "Impulse response", .kind = render::ParamKind::Option,
            .required = true, .options_from = "irs"},
   .targets = kCabIrIr, .target_count = 1},
  {.desc = {.id = "low_cut", .name = "Low cut", .unit = "Hz", .default_value = 20.0, .has_min = true,
            .min = 10.0, .has_max = true, .max = 2000.0, .scale = render::ParamScale::Log,
            .rampable = true},
   .targets = kCabIrLowCut, .target_count = 1},
  {.desc = {.id = "high_cut", .name = "High cut", .unit = "Hz", .default_value = 20000.0, .has_min = true,
            .min = 1000.0, .has_max = true, .max = 24000.0, .scale = render::ParamScale::Log,
            .rampable = true},
   .targets = kCabIrHighCut, .target_count = 1},
  {.desc = {.id = "level_db", .name = "Level", .unit = "dB", .default_value = 0.0, .has_min = true,
            .min = -24.0, .has_max = true, .max = 12.0, .rampable = true},
   .targets = kCabIrLevel, .target_count = 1},
};

const MacroNodeDecl kCabIrNodes[] = {
  {"low", "nc.eq.biquad"}, {"ir", "nc.cab.ir"}, {"high", "nc.eq.biquad"}, {"level", "nc.gain"}};
const MacroEdge kCabIrEdges[] = {
  {"low", 0, "ir", 0}, {"ir", 0, "high", 0}, {"high", 0, "level", 0}};
const MacroPortLink kCabIrIn[] = {{0, "low", 0}};
const MacroPortLink kCabIrOut[] = {{0, "level", 0}};

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

const MacroType kMacroTypes[] = {
  {.id = "fx.eq.bands",
   .name = "EQ",
   .category = "eq",
   .inputs = 1,
   .outputs = 1,
   .input_links = kEqBandsIn,
   .input_link_count = 1,
   .output_links = kEqBandsOut,
   .output_link_count = 1,
   .params = kEqBandsParams,
   .param_count = 13,
   .nodes = kEqBandsNodes,
   .node_count = 4,
   .edges = kEqBandsEdges,
   .edge_count = 3,
   .bypass = MacroBypass::AllRolesCarry,
   .apply = apply_eq_bands},
  {.id = "fx.eq.tilt",
   .name = "Tilt EQ",
   .category = "eq",
   .inputs = 1,
   .outputs = 1,
   .input_links = kEqTiltIn,
   .input_link_count = 1,
   .output_links = kEqTiltOut,
   .output_link_count = 1,
   .params = kEqTiltParams,
   .param_count = 2,
   .nodes = kEqTiltNodes,
   .node_count = 2,
   .edges = kEqTiltEdges,
   .edge_count = 1,
   .bypass = MacroBypass::AllRolesCarry,
   .apply = apply_eq_tilt},
  {.id = "fx.filter.cut",
   .name = "Cut",
   .category = "filter",
   .inputs = 1,
   .outputs = 1,
   .input_links = kFilterCutIn,
   .input_link_count = 1,
   .output_links = kFilterCutOut,
   .output_link_count = 1,
   .params = kFilterCutParams,
   .param_count = 4,
   .nodes = kFilterCutNodes,
   .node_count = 8,
   .edges = kFilterCutEdges,
   .edge_count = 7,
   .bypass = MacroBypass::AllRolesCarry,
   .apply = apply_filter_cut},
  {.id = "fx.mix.drywet",
   .name = "Dry/Wet",
   .category = "utility",
   .inputs = 2,
   .outputs = 1,
   .input_links = kMixDryWetIn,
   .input_link_count = 2,
   .output_links = kMixDryWetOut,
   .output_link_count = 1,
   .params = kMixDryWetParams,
   .param_count = 2,
   .nodes = kMixDryWetNodes,
   .node_count = 4,
   .edges = kMixDryWetEdges,
   .edge_count = 3,
   .bypass = MacroBypass::Crossfade,
   .dry_role = "dry",
   .wet_role = "wet",
   .apply = apply_mix_drywet},
  {.id = "fx.delay.feedback",
   .name = "Delay",
   .category = "delay",
   .inputs = 1,
   .outputs = 1,
   .input_links = kDelayFeedbackIn,
   .input_link_count = 2,
   .output_links = kDelayFeedbackOut,
   .output_link_count = 1,
   .params = kDelayFeedbackParams,
   .param_count = 3,
   .nodes = kDelayFeedbackNodes,
   .node_count = 7,
   .edges = kDelayFeedbackEdges,
   .edge_count = 7,
   .bypass = MacroBypass::Crossfade,
   .dry_role = "dry",
   .wet_role = "wet",
   .apply = apply_delay_feedback},
  {.id = "fx.amp.tonestack",
   .name = "Tone stack",
   .category = "amp",
   .inputs = 1,
   .outputs = 1,
   .input_links = kAmpTonestackIn,
   .input_link_count = 1,
   .output_links = kAmpTonestackOut,
   .output_link_count = 1,
   .params = kAmpTonestackParams,
   .param_count = 4,
   .nodes = kAmpTonestackNodes,
   .node_count = 3,
   .edges = kAmpTonestackEdges,
   .edge_count = 2,
   .bypass = MacroBypass::AllRolesCarry,
   .apply = apply_amp_tonestack},
  {.id = "fx.cab.ir",
   .name = "Cabinet",
   .category = "cab",
   .inputs = 1,
   .outputs = 1,
   .input_links = kCabIrIn,
   .input_link_count = 1,
   .output_links = kCabIrOut,
   .output_link_count = 1,
   .params = kCabIrParams,
   .param_count = 4,
   .nodes = kCabIrNodes,
   .node_count = 4,
   .edges = kCabIrEdges,
   .edge_count = 3,
   .bypass = MacroBypass::AllRolesCarry,
   .apply = apply_cab_ir},
};

std::vector<const MacroType*> g_macro_types;

} // namespace

const std::vector<const MacroType*>& all_macro_types()
{
  if (g_macro_types.empty())
  {
    for (const MacroType& type : kMacroTypes)
      g_macro_types.push_back(&type);
  }
  return g_macro_types;
}

const MacroType* find_macro_type(const std::string& id)
{
  for (const MacroType* type : all_macro_types())
  {
    if (id == type->id)
      return type;
  }
  return nullptr;
}

const MacroParamDecl* find_macro_param(const MacroType& type, const std::string& name)
{
  for (int i = 0; i < type.param_count; ++i)
  {
    if (name == type.params[i].desc.id)
      return &type.params[i];
  }
  return nullptr;
}

} // namespace graph
} // namespace namchain
