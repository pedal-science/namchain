#pragma once

// Pure manipulation of the ALSA kernel parameter space (the `snd_pcm_hw_params` struct from
// <sound/asound.h>). No device and no libasound: this is the arithmetic behind "give me exactly
// this, or tell me what you can actually do", which is the part that has to be right.
//
// Kept separate from the device class so it can be unit-tested without hardware. The specific
// lesson it exists to encode (plan.md §9.5): requesting a 32-frame period on a full-speed USB
// device silently yields 48, because PERIOD_TIME has a 1000 us floor. The engine's quantum must
// equal the device period, so the negotiation result is verified rather than assumed.
#if defined(__linux__)

  #include <sound/asound.h>

  #include "namchain/pcm.h"

namespace namchain
{
namespace alsa
{

/// Reset every parameter to "any": the widest space the kernel will accept as a starting point.
void params_set_any(struct snd_pcm_hw_params& params);

/// Narrow to the properties we actually depend on, leaving period/buffer sizes unconstrained so
/// that a subsequent SNDRV_PCM_IOCTL_HW_REFINE reports the device's real ranges.
void params_narrow_basics(struct snd_pcm_hw_params& params, unsigned channels, unsigned sample_rate);

/// Bounds of an interval parameter, e.g. SNDRV_PCM_HW_PARAM_PERIOD_SIZE. Returns false if the
/// parameter is not an interval parameter or the interval is empty.
bool params_interval_bounds(const struct snd_pcm_hw_params& params, unsigned param, unsigned& min,
                            unsigned& max);

/// Pin an interval parameter to a single value.
void params_set_interval(struct snd_pcm_hw_params& params, unsigned param, unsigned value);

/// Pin the format mask to exactly one value.
void params_set_format(struct snd_pcm_hw_params& params, unsigned format);

/// True when exactly `format` is permitted (and nothing else).
bool params_format_is_exactly(const struct snd_pcm_hw_params& params, unsigned format);

/// True when `format` is permitted at all.
bool params_format_available(const struct snd_pcm_hw_params& params, unsigned format);

/// Pick the first of `preferred` (in order) that the device offers.
/// \return false when none of them is available, leaving \p chosen untouched.
bool params_pick_format(const struct snd_pcm_hw_params& params, const unsigned* preferred, unsigned count,
                        unsigned& chosen);

/// Bytes per sample for a SNDRV_PCM_FORMAT_* value we support; 0 if unknown.
unsigned bytes_per_sample(unsigned format);

/// The portable counterpart of a SNDRV_PCM_FORMAT_* value: what pcm_decode/pcm_encode take.
/// PcmFormat::Unknown when the format is not one this project converts.
PcmFormat pcm_format(unsigned format);

/// Read back a single-valued interval parameter. Returns false if it is not exactly one value.
bool params_single_value(const struct snd_pcm_hw_params& params, unsigned param, unsigned& value);

/// Clamp `requested` into [min, max], which is what the kernel does to a too-small period.
unsigned clamp_to_bounds(unsigned requested, unsigned min, unsigned max);

/// \brief Frames actually sitting in a PCM ring, derived from the pointer pair ALSA reports.
///
/// This is the way to ask how full a ring is, and `status.delay` is not. On the SSL 2+ Mk II the
/// playback delay counts the frames already handed to the USB host controller as well as the ring
/// contents -- a constant +72 frames in a sample taken three times via /proc/asound, and larger when
/// the ring is busy -- and the capture delay carries an intermittent +48. That is how a 256-frame
/// ring came to report 544 frames of "occupancy" (plan.md §9.5). `status.avail` agrees with this
/// figure in both directions at every sample taken, and is what poll() readiness is computed from.
///
/// The kernel's \p hw_ptr and \p appl_ptr run up to an internal boundary rather than wrapping at the
/// buffer length, so the difference is taken modulo \p buffer_frames. Getting that wrong is easy and
/// silent -- reducing each pointer first and then subtracting is not the same as subtracting the raw
/// values when they sit in different buffer epochs -- which is why this is a tested function rather
/// than an expression inline in the ioctl wrapper.
///
/// \param capture selects the direction the pointers describe: capture counts `hw - appl`, playback
/// `appl - hw`. \return the ring contents in frames, in `[0, buffer_frames)`.
unsigned long long pcm_ring_frames(unsigned long long hw_ptr, unsigned long long appl_ptr,
                                   unsigned long long buffer_frames, bool capture);

/// \brief Frames of silence to prime the playback ring with so the loop starts at its queue target.
///
/// At least one period, because the kernel's start threshold is one period (sw.start_threshold in
/// `configure`): a prime of zero leaves the stream started-but-empty, which underruns immediately.
/// \param queue_periods the target occupancy in periods; 0 or 1 both mean "one period", which is
/// what every run before the target existed did.
unsigned long long queue_prime_frames(unsigned long long period_frames, unsigned queue_periods);

/// \brief Whether the loop may write a period now, given how full the playback ring already is.
///
/// This is the output-queue target's entire policy (§9.5 item 7), and it is a separate, pure function
/// because the off-by-one in it decides whether the loop holds its queue or oscillates around it.
///
/// \param ring_frames the ring's contents *before* this write, as derived from the pointer pair.
/// \param target_frames the occupancy to hold, in frames; 0 means no target, and every write is
///        allowed (which is the behaviour every measurement before this knob existed was taken with).
///
/// The comparison is `<=`, not `<`, and that is the difference between a target the loop holds and
/// one it half-drops into. The device consumes one period while the loop produces one, so a write
/// made when the ring holds exactly the target leaves the ring holding the target again by the next
/// iteration: the occupancy is conserved, and `== target` is a fixed point rather than a condition to
/// refuse at. Refusing there instead would drop every other period for ever -- the ring falls one
/// period, the next write refills it, and the one after refuses again -- which is 50 % of the audio
/// gone to hold a queue that a `<=` holds for free. Above the target a refusal is the *correction*:
/// each one lowers the ring by a period, so a queue that a burst pushed over the target comes back
/// down in a bounded number of drops rather than staying there.
bool queue_allows_write(unsigned long long ring_frames, unsigned long long target_frames);

/// \brief Whether the output queue is short enough that a period of silence should be inserted.
///
/// The floor controller's entire policy (§9.5 item 9). A cap bounds the queue from above; this is the
/// other end, and it exists because the queue is not renewable: the loop produces one period per period,
/// so once the queue is short only *something other than the signal* can refill it. Measured on this
/// host, the alternative is worse than it looks -- a playback underrun makes the driver stop the
/// stream, and recovery re-`PREPARE`s it, which discards **the whole queued ring** rather than one
/// period. So the choice is between inserting 1.33 ms of silence now and losing the entire queue plus
/// a timeline slip at an arbitrary later moment.
///
/// \param ring_frames the ring's contents, from the pointer pair, at the moment of the decision.
/// \param floor_frames the level to hold, in frames; 0 means no controller, and the decision is false.
/// \param period_frames the period, in frames.
/// \param buffer_frames the ring's capacity, in frames.
///
/// The room condition is what keeps the insertion from blocking: a write is only safe when the ring can
/// take a whole period, and the check is here rather than at the write so the arithmetic is testable.
/// Given the configuration validation -- a floor below the ring's length -- a short queue always leaves
/// room for a period, so this is a guard rather than a live path. That is worth knowing in itself: it is
/// why the controller can never block in the write, which is the one thing the render path may not do.
/// Insertion continues while the queue is short, so a single late burst of stalls can be covered by more
/// than one period -- but only as many as there were blocks missed, which is what "catching up" means
/// here: the timeline is preserved and the audio is delayed, rather than the audio being skipped.
bool queue_needs_top_up(unsigned long long ring_frames, unsigned long long floor_frames,
                        unsigned long long period_frames, unsigned long long buffer_frames);

} // namespace alsa
} // namespace namchain

#endif // __linux__
