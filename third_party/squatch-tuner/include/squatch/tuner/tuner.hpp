// SPDX-License-Identifier: MIT
// Squatch Tuner: monophonic tuner = coarse MPM note finder + strobe bank + stiffness-aware fit.
//
//   input (any rate) -> decimate to ~12 kHz -> history -> MPM every hop (5.3 ms): which note?
//                                        \-> strobe bank (every sample): how far off (0.01 cent on a steady tone)
//
// Usage: configure() once (not real-time safe: it builds tables), then process() from the audio
// callback with any block size, and read reading() after a block. No allocation, no locks, no
// system calls after configure(). The object is ~128 kB: make it static or a member, not a local.
#pragma once
#include <algorithm>

#include "decimator.hpp"
#include "history.hpp"
#include "level.hpp"
#include "mains.hpp"
#include "partial_fit.hpp"
#include "period.hpp"
#include "stiffness.hpp"
#include "strobe.hpp"
#include "successor.hpp"
#include "weak_start.hpp"

namespace squatch::tuner {

struct TunerConfig {
  float sampleRate = 48000.0f;
  float a4 = 440.0f;
  float minHz = 38.0f;            // 38 covers bass E1 (41.2) and 8-string F#1 (46.2); 27 for B0
  float maxHz = 1400.0f;          // E6, 24th fret of the high E, is 1318.5
  float workRate = 12000.0f;      // decimate towards this rate
  float gate = 3e-4f;             // RMS level below which nothing is reported (-70 dBFS)
  float clarity = 0.8f;           // MPM peak needed to start a note
  float memorySeconds = 0.5f;     // phase-fit memory: longer = steadier, slower to follow
  float minBoxcarSeconds = 0.0f;  // demodulator lowpass length floor (see StrobeBank::start)
  float maxUncertainty = 1.0f;    // cents; a noisier estimate is withheld
  int partials = 6;
  FitOptions fit{};
};

struct TunerReading {
  bool valid = false;
  float hz = 0.0f;             // fundamental (partial 1) now: follows glides (drives the strobe)
  float steadyHz = 0.0f;       // fundamental averaged over the fit window (drives the number)
  int midi = 0;                // nearest note to hz
  float cents = 0.0f;          // hz's offset from that note
  float inharmonicity = 0.0f;  // B of this string, as measured on this note
  float uncertainty = 0.0f;    // standard error of `cents`, from the fit's own residuals
  float clarity = 0.0f;
  float level = 0.0f;          // RMS at the work rate
  float noteSeconds = 0.0f;    // time since this note was picked up
  int partials = 0;            // partials the fit used
  std::array<float, Limits::kMaxPartials> strobeTurns{};  // per-partial phase, for the display
};

class Tuner {
 public:
  static constexpr int kHop = 64;
  static constexpr float kSettleOctaveSeconds = 0.5f;    // octave re-naming only before this
  static constexpr float kMaxSpreadCents = 3.0f;         // partials further apart: no number
  static constexpr float kSteadyLead = 0.25f;            // the steady fit's point in its window

  void configure(const TunerConfig& c) {
    cfg_ = c;
    dec_.configure(static_cast<int>(c.sampleRate / c.workRate + 0.5f));
    rate_ = c.sampleRate / static_cast<float>(dec_.factor());
    PeriodRange r;
    r.maxLag = std::min(static_cast<int>(rate_ / c.minHz) + 1, Limits::kMaxLag - 2);
    r.minLag = std::max(static_cast<int>(rate_ / c.maxHz), 2);
    r.window = 2 * r.maxLag + 4;
    window_ = r.window;
    mpm_.configure(r);
    (void)CosTable::instance();
    reset();
  }

  void reset() {
    dec_.reset();
    hist_.clear();
    levels_.reset();
    reading_ = TunerReading{};
    tracking_ = weakStart_ = false;
    hopFill_ = hopsInNote_ = quietHops_ = disagree_ = confirmed_ = lowOctave_ = pendingOnset_ = octaveUp_ = 0;
    weak_.reset();
    successor_.reset();
    energy_ = notePeak_ = 0.0f;
  }

  void process(const float* in, int n) {
    for (int i = 0; i < n; ++i) {
      float y;
      if (dec_.push(in[i], y)) pushWork(y);
    }
  }

  const TunerReading& reading() const { return reading_; }
  float workRate() const { return rate_; }

 private:
  void pushWork(float y) {
    hist_.push(y);
    energy_ += y * y;
    if (tracking_) bank_.push(y);
    if (++hopFill_ < kHop) return;
    hopFill_ = 0;
    onHop();
  }

  void onHop() {
    levels_.hop(energy_, kHop, tracking_ || hist_.count() < window_);  // unanalysed = not idle
    energy_ = 0.0f;
    const float level = levels_.level();
    reading_.level = level;
    // While a note rings, only a jump to within 8 dB of its peak counts as a new pluck; smaller
    // jumps are beating between the dying note and hum or noise.
    const bool onset = levels_.rising() && level > cfg_.gate && (!tracking_ || level > 0.4f * notePeak_);
    if (belowGate(level) || hist_.count() < window_) return;
    followNote(onset, level);
    if (!tracking_) return;
    checkOctave();
    updateReading();
  }

  bool belowGate(float level) {
    successor_.heard(level >= cfg_.gate);
    if (level >= cfg_.gate) {
      quietHops_ = 0;
      return false;
    }
    if (tracking_ && ++quietHops_ > 10) stopNote();
    return true;
  }

  // The coarse stage runs every hop until a note is found and for the first 16 hops of a note,
  // then every 4th hop to check the note has not changed.
  void followNote(bool onset, float level) {
    ++hopsInNote_;
    noteBookkeeping(onset);
    const bool due = !tracking_ || onset || pendingOnset_ > 0 || hopsInNote_ < 16 || (hopsInNote_ & 3) == 0;
    if (!due) return;
    coarse_ = mpm_.analyze(hist_.latest(window_));
    if (!coarseIsGood()) return unclearCoarse();
    unclear_ = 0;
    if (!tracking_ || pendingOnset_ > 0) {
      if (refusedStart(rate_ / coarse_.period)) return;
      startNote(rate_ / coarse_.period);
      pendingOnset_ = 0;
    } else {
      checkAgreement(level);
    }
  }

  // A pluck stays pending for 8 hops (43 ms): on the onset hop itself the coarse window is still
  // mostly what came before (hum, the last note's tail).
  void noteBookkeeping(bool onset) {
    notePeak_ = std::max(notePeak_, levels_.level43());
    pendingOnset_ = onset ? 8 : std::max(pendingOnset_ - 1, 0);
    successor_.hop(onset, tracking_, notePeak_);
  }

  // A note starting with no pluck at another's tail (successor.hpp), or at mains (mains.hpp).
  bool quietSuccessor(float hz) const { return successor_.refuses(hz, levels_.level43(), pendingOnset_ > 0); }

  // A start the coarse stage already places at mains frequency is not worth fitting.
  bool humStart() const { return atMains(rate_ / coarse_.period, 8.0f); }

  bool refusedStart(float hz) const { return humStart() || quietSuccessor(hz); }

  bool humNote(const PartialFit& f) {
    const bool hum = f.valid && atMains(f.f0, 8.0f);
    mainsHops_ = hum ? mainsHops_ + 1 : 0;
    return hum;
  }

  // Nothing may start within 10 dB of the floor once it is known (hum and hiss are periodic
  // enough). A steady tone from power-on (a test oscillator) still reads: it is tracked from
  // the first hop, so no idle hop ever comes and the floor stays unknown. The rule used to wait
  // for a loud note, and until then a Hi-Z input's hum wandering at 55 to 59 Hz, 3 dB over
  // the floor, read as A1/A#1 (23% of an idle take).
  bool coarseIsGood() const {
    const bool clear = coarse_.valid && coarse_.clarity >= cfg_.clarity;
    return clear && (!levels_.floorKnown() || levels_.level43() > 3.16f * levels_.floor());
  }

  // Sub-octave lock: if the odd partials of the reference stay 20 dB below the even ones for
  // three hops, the string is an octave higher than the coarse stage said.
  void checkOctave() {
    if (bank_.partials() < 4 || hopsInNote_ < 4 || established()) return;
    lowOctave_ = bank_.oddEvenRatio() < 0.01f ? lowOctave_ + 1 : 0;
    if (lowOctave_ >= 3 && bank_.refHz() * 2.0f <= cfg_.maxHz) startNote(bank_.refHz() * 2.0f);
  }

  // The coarse stage re-checks the note. Agreement confirms it; an octave away is ignored (MPM
  // slips an octave on decaying notes) unless it keeps saying "octave up" while our odd partials
  // are weak (a sympathetic string an octave down can hide an empty odd series from
  // checkOctave); a persistent, strong disagreement means the player moved on without a
  // detectable onset; a weak one means the note has sunk into noise or hum.
  void checkAgreement(float level) {
    const float hz = rate_ / coarse_.period;
    const float c = centsBetween(hz, reading_.hz > 0.0f ? reading_.hz : bank_.refHz());
    octaveUp_ = std::fabs(c - 1200.0f) <= 60.0f ? octaveUp_ + 1 : 0;
    if (std::fabs(c) <= 60.0f) return confirm();
    if (octaveUp_ >= 3 && oddPartialsWeak() && !established()) return startNote(hz);
    if (std::fabs(std::fabs(c) - 1200.0f) <= 60.0f || ++disagree_ < 2) return;
    disagreed(hz, level);
  }

  // A persistent disagreement: a strong one is a new note (unless it is hum), a weak one ends it.
  void disagreed(float hz, float level) {
    if (strongCoarse(level) && !refusedStart(hz)) startNote(hz);
    else if (disagree_ >= 4) stopNote();
  }

  // A lock an octave low shows at once (an empty odd series, or a sympathetic string an octave
  // down that the coarse stage keeps contradicting). Half a second in, the note is what it is:
  // late in a note the fundamental of a wound string dies before its 2nd partial, the coarse
  // stage slips to the octave, and both octave tests fire. They used to re-name 35 EGFxSet notes
  // an octave up 1 to 3 s in (1.4% of its frames) with no new pluck.
  bool established() const { return reading_.noteSeconds >= kSettleOctaveSeconds; }

  bool strongCoarse(float level) const { return coarse_.clarity >= 0.9f && level >= 0.1f * notePeak_; }

  void confirm() {
    ++confirmed_;
    disagree_ = 0;
  }

  // Weak odd partials that also disagree with the even ones (> 1 c) come from another source,
  // such as a sympathetic string an octave down. A weak fundamental on the same string does not
  // disagree: its odd and even partials share one f0.
  bool oddPartialsWeak() const {
    return bank_.partials() >= 4 && bank_.oddEvenRatio() < 0.1f && std::fabs(oddEvenCents()) > 1.0f;
  }

  // Fundamental implied by the odd partials minus that implied by the even ones, in cents.
  float oddEvenCents() const {
    double w[2] = {0.0, 0.0}, wy[2] = {0.0, 0.0};
    const double c = 0.5 * stiffness_.b() * bank_.refHz();
    for (int k = 1; k <= bank_.partials(); ++k) {
      const PartialMeasure m = bank_.measure(k - 1, false);
      const double y = bank_.refHz() + m.offsetHz / k - c * k * k, wk = static_cast<double>(m.information) * k * k;
      w[k & 1] += wk;
      wy[k & 1] += wk * y;
    }
    if (w[0] <= 0.0 || w[1] <= 0.0) return 0.0f;
    return static_cast<float>(1200.0 * std::log2((wy[1] / w[1]) / (wy[0] / w[0])));
  }

  // Strings ringing together (weak_start.hpp): a pitch the coarse stage holds at lower clarity
  // starts a note if it is 20 dB over the floor, not hum, never below 70 Hz (two
  // strings' common subharmonic lives there) and not a quiet successor. Such a note learns no
  // stiffness (other strings' partials sit beside its own) and shows its number only while that
  // agrees with the coarse pitch (weakPitchAgrees); it passes every other test a clear one does.
  void weakCandidate() {
    if (!coarse_.valid || coarse_.clarity < WeakStart::kClarity || !loudOverFloor()) return weak_.miss();
    if (!weak_.offer(rate_ / coarse_.period)) return;
    const float hz = weak_.hz();
    if (hz < SuccessorGuard::kLowestHz || atMains(hz, 8.0f) || quietSuccessor(hz)) return;
    startNote(hz);
    weakStart_ = true;
  }

  // A coarse check too unclear to start or confirm a note: a weak note's confirmation, a weak
  // start's candidate, or a note sinking into the noise.
  void unclearCoarse() {
    if (weakAgreement()) return confirm();
    if (!tracking_) weakCandidate();
    noteSinking();
  }

  // A note started that way is confirmed by the same evidence: the coarse stage at
  // WeakStart::kClarity or more on its pitch or its sub-octave. Its number must also agree with
  // the coarse stage's own pitch.
  bool weakAgreement() const {
    if (!tracking_ || !weakStart_ || !coarse_.valid || coarse_.clarity < WeakStart::kClarity) return false;
    return WeakStart::near(rate_ / coarse_.period, reading_.hz > 0.0f ? reading_.hz : bank_.refHz(), 60.0f);
  }
  bool weakPitchAgrees(float f0) const { return !weakStart_ || (coarse_.valid && WeakStart::near(rate_ / coarse_.period, f0, 35.0f)); }

  bool loudOverFloor() const { return !levels_.floorKnown() || levels_.level43() > 10.0f * levels_.floor(); }

  // Noise has low clarity and hum is refused within 10 dB of the floor, so neither ever
  // reaches checkAgreement: without this, a note would never end in a room louder than the
  // -70 dB gate. Eight unclear checks in a row, with the level down at the floor or 20 dB below
  // the note's peak, end it.
  void noteSinking() {
    if (tracking_ && ++unclear_ >= 8 && (!aboveFloor() || levels_.level43() < 0.1f * notePeak_)) stopNote();
  }

  void startNote(float hz) {
    unclear_ = mainsHops_ = 0;
    weak_.reset();
    weakStart_ = false;
    stiffness_.reset();
    bank_.start(hz, rate_, cfg_.partials, cfg_.memorySeconds, cfg_.minBoxcarSeconds);
    // Replay the last two periods so the fit starts from what the coarse stage already saw.
    const int replay = std::min(static_cast<int>(2.0f * rate_ / hz), window_);
    const float* p = hist_.latest(replay);
    for (int i = 0; i < replay; ++i) bank_.push(p[i]);
    tracking_ = true;
    hopsInNote_ = disagree_ = confirmed_ = lowOctave_ = octaveUp_ = 0;
    notePeak_ = 0.0f;
    infoNow_.fill(0.0f);
    infoSteady_.fill(0.0f);
    reading_.valid = false;
    reading_.hz = 0.0f;
    reading_.noteSeconds = static_cast<float>(replay) / rate_;
  }

  void stopNote() {
    tracking_ = false;
    reading_.valid = false;
  }

  // B is learned per note from the steady fit (stiffness.hpp) and f0 is fitted with it held, so
  // a partial fading in or out changes the weights, never the model. Each partial's weight is
  // smoothed too (~25 ms): when two partials disagree (a sympathetic string, a beating pair) the
  // blend must not flip from hop to hop. The steady fit reads each partial kSteadyLead of the way
  // from its window's centre to the newest point (where a significant curvature says): at the
  // centre it trailed a real pluck's glide by a quarter second, 3 c on our open-E take.
  using Measures = std::array<PartialMeasure, Limits::kMaxPartials>;
  PartialFit fitNote(Measures m, bool lagCompensation, std::array<float, Limits::kMaxPartials>& info) {
    FitOptions o = cfg_.fit;
    o.lagCompensation = lagCompensation;
    const int n = bank_.partials();
    for (int k = 0; k < n; ++k) {
      info[k] = info[k] <= 0.0f ? m[k].information : info[k] + 0.2f * (m[k].information - info[k]);
      m[k].information = info[k];
    }
    const bool loud = levels_.level43() >= 0.0316f * notePeak_;
    if (o.fitInharmonicity && !lagCompensation && !weakStart_ && stiffness_.wants(reading_.noteSeconds, loud))
      stiffness_.learn(fitPartials(m.data(), n, bank_.refHz(), o), reading_.noteSeconds, loud);
    if (o.fitInharmonicity) o.fixedInharmonicity = stiffness_.b();
    return fitPartials(m.data(), n, bank_.refHz(), o);
  }

  // Withhold the reading once the note has decayed (10 dB below its peak) to within 10 dB of the
  // idle floor: past that point hum and noise, not the string, set the phase. A steady tone (a
  // test oscillator, the tuner switched on mid-note) never decays, so it always reads.
  bool aboveFloor() const {
    if (!levels_.floorKnown()) return true;
    return levels_.level43() > 3.16f * levels_.floor() || levels_.level43() > 0.316f * notePeak_;
  }

  void updateReading() {
    reading_.noteSeconds += static_cast<float>(kHop) / rate_;
    reading_.clarity = coarse_.clarity;
    Measures steadyM{}, nowM{};
    for (int k = 0; k < bank_.partials(); ++k) bank_.measurePair(k, kSteadyLead, 1.0f, steadyM[k], nowM[k]);
    const PartialFit steady = fitNote(steadyM, false, infoSteady_);
    const PartialFit f = cfg_.fit.lagCompensation ? fitNote(nowM, true, infoNow_) : steady;
    // No steady fit, no number: the fast fit is no stand-in (it read +46 c on EGFxSet quiet tails).
    if (steady.valid) reading_.steadyHz = steady.f0;
    reading_.uncertainty = f.valid ? 1731.23f * f.sigmaHz / f.f0 : 0.0f;  // 1200 / ln 2
    const bool hum = humNote(f);
    reading_.valid = f.valid && steady.valid && !hum && trustworthy(steady);
    if (f.valid) publish(f);
    if (mainsHops_ >= 8) stopNote();
  }

  // Everything a valid fit must also pass before its number is shown; partials that disagree by
  // more than kMaxSpreadCents (partial_fit.hpp partialsAgree) show none.
  bool trustworthy(const PartialFit& steady) const {
    return confirmed_ > 0 && aboveFloor() && partialsAgree(steady, kMaxSpreadCents) && weakPitchAgrees(steady.f0) &&
           reading_.uncertainty <= cfg_.maxUncertainty;
  }

  void publish(const PartialFit& f) {
    reading_.hz = f.f0;
    reading_.inharmonicity = f.inharmonicity;
    reading_.partials = f.used;
    const NoteReading n = nearestNote(f.f0, cfg_.a4);
    reading_.midi = n.midi;
    reading_.cents = n.cents;
    for (int k = 0; k < bank_.partials(); ++k) reading_.strobeTurns[k] = bank_.phaseTurns(k);
  }

  TunerConfig cfg_{};
  Decimator dec_;
  History<Limits::kMaxWindow> hist_;
  Mpm mpm_;
  StrobeBank bank_;
  LevelTracker levels_;
  PeriodEstimate coarse_{};
  TunerReading reading_{};
  float rate_ = 12000.0f;
  float energy_ = 0.0f, notePeak_ = 0.0f;
  StiffnessLearner stiffness_;  // this note's B (stiffness.hpp)
  WeakStart weak_;              // a start for strings ringing together (weak_start.hpp)
  SuccessorGuard successor_;    // no start without a pluck at a note's tail (successor.hpp)
  std::array<float, Limits::kMaxPartials> infoNow_{}, infoSteady_{};  // smoothed partial weights
  int window_ = 0, hopFill_ = 0;
  int hopsInNote_ = 0, quietHops_ = 0, disagree_ = 0, confirmed_ = 0, lowOctave_ = 0, pendingOnset_ = 0;
  int octaveUp_ = 0, unclear_ = 0, mainsHops_ = 0;
  bool tracking_ = false, weakStart_ = false;
};

}  // namespace squatch::tuner
