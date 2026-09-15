// rt::SpliceFader -- make a period-sized hole in the output inaudible.
//
// A device loop loses audio in two ways, and both are the same artefact (plan.md §9.5 item 9, §10.3):
//
//   * a DROPPED period, when the queue cap (or the ring itself) refuses a write. The ring is a FIFO of
//     period-sized buffers, so skipping an append means the device plays out(k-1) and then out(k+1) --
//     a 64-frame jump in the waveform, which at 48 kHz is 1.33 ms of missing signal and a step big
//     enough to hear as a click for anything above a few hundred hertz.
//   * an INSERTED period, when the floor controller writes one to keep the queue off the bottom: the
//     step is at its entry, where a payload arrives mid-signal without continuing it.
//
// Neither can be repaired where it happens -- the previous period is already in the ring, and queued
// audio cannot be rewritten -- so the repair happens on the way out, on the periods that follow. The
// bridge is a cubic Hermite that matches the signal's **value and slope** at the join and reaches zero
// offset with zero slope at the end of the window:
//
//     d  = a1 - a0                    the slope the device was last travelling at
//     xs = x1 - x0                    the slope the new signal starts with
//     e0 = (a1 + d) - x0              value mismatch, against the *continuation* rather than a hold
//     m0 = d - xs                     slope mismatch
//     y[i] = x[i] + e0*h00(t) + m0*(K-1)*h10(t),   t = i/(K-1),  K = fade_frames
//
// h00 and h10 are the Hermite bases (h00(0)=1, h00(1)=0, h10(0)=h10(1)=0, h10'(0)=1). Three earlier
// shapes were tried and the project's own glitch detector rejected all three, which is why the shape is
// what it is rather than what looked reasonable:
//
//   * a **hold** at a1 makes the first difference zero at the join, and the continuity rule measures a
//     *second* difference, so a hold scores |d| -- the signal's own first difference, ~1/omega above the
//     reference it is judged against. Measured at 7.6x the allowance on a 997 Hz tone.
//   * a **decaying extrapolation** of the hold (`a1 + d*(1-w)`) zeroes the second difference at the
//     join and then puts it all back at the next sample, because the slope collapses from d to zero in
//     one step: measured at 0.0521 against a 0.0136 limit, no better than the hold.
//   * a **long crossfade** (w over 5 ms) reaches neither: it holds the value for the first samples and
//     attenuates the signal, which is a swell, not a repair.
//
// Matching value *and* slope is what makes both differences small: e0 and m0 are the whole mismatch, and
// the Hermite bases take them to zero smoothly, so the residual second difference is ~|e0|/K^2 + |m0|/K.
// At K = 64 that is ~0.002 against a reference of ~0.007, and the window is one period rather than five
// milliseconds -- the signal is bridged, not ducked.
//
// Which makes this a **loop policy and not a model property**: it is applied to the emitted signal
// between the render plan and the write, it is armed by the backend's own decisions (a refusal, an
// insertion), and it must leave the oracle's copy of the engine's output untouched. The soak encodes
// the bridged signal into the device buffer while the self-oracle keeps comparing the pristine one, so
// "the engine is sample-exact" and "the emitted signal is continuous" stay two independent claims.
#ifndef NAMCHAIN_RT_SPLICE_FADER_H
#define NAMCHAIN_RT_SPLICE_FADER_H

#include <cstdint>
#include <vector>

namespace namchain
{
namespace rt
{

/// \brief One sample of the payload an inserted period is filled with.
///
/// The other half of the same repair, and the fix for the last audible step in it. The loop's bridge
/// repairs the *exit* of an inserted period and cannot reach its entry, because the loop does not write
/// that period -- the backend does. A payload that simply holds the last sample removes the step in
/// *value* but leaves one in *slope*: the signal's slope goes to zero in a single sample, which the
/// continuity rule sees as a second difference of |slope| (measured on a 313 Hz tone: 0.4 step for
/// silence, under 0.05 for a hold, still over the allowance).
///
/// This continues the signal from where it was and arrives flat at the same level:
///
///     p(t) = h00(t)*(last + slope) + h10(t)*slope*(frames-1) + h01(t)*last
///
/// so the entry matches the signal's value *and* slope -- p[0] = last + slope makes the second
/// difference at the join exactly zero against the two real samples before it -- and the exit is flat,
/// which is what the loop's bridge expects when it arms with Cause::Inserted. Both joints are then
/// continuous, and neither depends on the other layer knowing this function exists.
///
/// \param last the last sample the device heard, \param slope the slope it was travelling at,
/// \param index the sample within the inserted period, \param frames the period's length (>= 2).
float splice_insert_sample(float last, float slope, unsigned index, unsigned frames);

class SpliceFader
{
public:
  /// \param channels interleaved channels in the buffers apply() is given.
  /// \param fade_frames length of the bridge in frames: 0 disables the fader, otherwise at least 3.
  ///
  /// Allocates, so it is constructed on a control thread before the loop starts -- like the rest of the
  /// render path's state. apply() itself does not allocate.
  SpliceFader(unsigned channels, unsigned fade_frames);

  /// Why a repair is being armed, which decides the slope it leaves with.
  enum class Cause
  {
    /// A period was refused, so the device's last two samples are the tail of a real period and the
    /// ramp leaves with their slope.
    Dropped,
    /// The backend inserted a period, so the device's last samples were a held value: the slope it was
    /// travelling at is zero and the ramp must leave flat. Using the pre-insert slope here would put a
    /// second-difference spike at the join that the backend's payload never had.
    Inserted
  };

  bool enabled() const { return fade_frames_ > 0; }
  unsigned fade_frames() const { return fade_frames_; }
  unsigned channels() const { return channels_; }

  /// \brief Arm a repair, anchored on the last sample emitted and its slope.
  ///
  /// Called when a period was refused (a drop) or when the backend inserted one. Callable while a fade
  /// is still running, and then it re-anchors: the sample the device last heard is the one the ramp has
  /// to leave from, so a second splice during a fade replaces the first rather than queueing behind it.
  void arm(Cause cause);

  /// True when the next apply() will alter samples.
  bool armed() const { return remaining_ > 0; }
  /// Frames of ramp still to emit, for a report that wants to say a fade is in progress.
  unsigned remaining() const { return remaining_; }
  /// Repairs started since construction: one per drop and one per insertion.
  uint64_t fades() const { return fades_; }

  /// \brief Emit `in` into `out`, repairing the head if armed.
  ///
  /// `out` may alias `in`. Every sample is written either way, so a caller can encode from `out`
  /// unconditionally; when the fader is not armed the copy is exact and the path is bit-for-bit what it
  /// would have been without the fader.
  ///
  /// **Computing a period and emitting one are different events, and this does not record that it
  /// happened.** The anchor the next repair leaves from is what the *device* last heard, and a period
  /// that was refused was never heard: a loop that let apply() move the anchor would bridge from the
  /// last thing it *computed* rather than the last thing it *played*. Measured on the device, one
  /// evening of confusing results and a debug trace: the bridge left from 0.1331 when the device's last
  /// sample was -0.0513, which is a step the repair was supposed to remove. The unit tests could not
  /// see it because they never refuse a period -- which is exactly why the on-device verifier exists.
  /// Call commit() once the period is known to have been taken.
  ///
  /// RT-safe: no allocation, no locks, no syscalls.
  void apply(const float* in, float* out, unsigned frames);

  /// Record `emitted` as the audio the device has heard, for the next repair to leave from.
  void commit(const float* emitted, unsigned frames);

  /// The slope the last committed period was travelling at, per channel. The loop needs it to rebuild
  /// the payload an inserted period was filled with, which is how an insertion can be judged on device.
  float last_slope(unsigned channel) const
  {
    return channel < anchor_.size() ? anchor_[channel] - previous_[channel] : 0.0f;
  }

  /// The last sample emitted, per channel. What the next arm(Dropped) will anchor on.
  float last_emitted(unsigned channel) const
  {
    return channel < anchor_.size() ? anchor_[channel] : 0.0f;
  }

private:
  unsigned channels_ = 1;
  unsigned fade_frames_ = 0;
  /// Samples of ramp still to emit; 0 when idle.
  unsigned remaining_ = 0;
  /// The last two samples emitted, per channel, tracked through every apply(): the bridge's anchor
  /// and the slope it leaves with.
  std::vector<float> anchor_;
  std::vector<float> previous_;
  /// The anchor and slope captured when the bridge was armed. Separate from the tracked samples because
  /// a bridge can span several periods, and letting its starting point drift with the signal would
  /// quietly shorten the repair into something that no longer leaves from the splice.
  std::vector<float> ramp_from_;
  std::vector<float> ramp_slope_;
  /// The value and slope mismatch, captured on the bridge's *first* period and held for its whole
  /// length. They depend on the signal the bridge is joining, which is only visible then: recomputing
  /// them per period would re-inject the mismatch every period and leave the offset never reaching
  /// zero. That bug is why the window-length test exists, and it failed at 240 frames before this.
  std::vector<float> bridge_value_;
  std::vector<float> bridge_slope_;
  uint64_t fades_ = 0;
};

} // namespace rt
} // namespace namchain

#endif // NAMCHAIN_RT_SPLICE_FADER_H
