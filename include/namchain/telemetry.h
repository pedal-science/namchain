#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace namchain
{

/// \brief Per-block timing accumulator for soak runs and acceptance tests.
///
/// This is **measurement apparatus, not the engine's telemetry**. It records from the thread
/// that runs the DSP and keeps every duration, which is acceptable for a harness but is not
/// something to lift into the render path: the engine's own RT telemetry (M4) must be a
/// lock-free ring drained by the control thread, because the audio thread may not allocate,
/// lock, or sort.
///
/// The distinction that matters for the acceptance criterion (plan.md §1, §10.5) is between
/// the two failure kinds, which are counted separately:
///
///   * **deadline overrun** -- this block took longer than the period deadline. Measured here.
///     On a real device this is what turns into an xrun; catching it as a margin (p99 well
///     under the deadline) is how you prove xruns *cannot* happen rather than waiting to
///     observe one.
///   * **driver xrun** -- the kernel says the DMA ring ran dry/overflowed (`-EPIPE`, or
///     `SNDRV_PCM_STATE_XRUN` from `SNDRV_PCM_IOCTL_STATUS`). Only a real backend can report
///     it, so it is counted by the backend and merely carried here.
class BlockTimer
{
public:
  /// \param deadline_ns Period deadline: `quantum / sample_rate` in nanoseconds.
  /// \param expected_blocks How many blocks the caller expects to record. The series is reserved up
  ///        front so that `record()` never allocates -- a reallocation mid-run would show up as a
  ///        spike in exactly the measurement being made -- but the reserve is the caller's number and
  ///        not a fixed "one hour at 32 frames". That default cost 44 MB per timer and five timers
  ///        per soak, for a run of any length, and it was the allocation that turned a finite
  ///        `RLIMIT_MEMLOCK` into a `std::bad_alloc` on CI (plan.md §14.10). 0 reserves nothing.
  explicit BlockTimer(uint64_t deadline_ns, uint64_t expected_blocks = 0);

  /// Record one block's processing duration. No allocation if the estimate held.
  void record(uint64_t duration_ns);

  uint64_t blocks() const { return durations_.size(); }
  uint64_t deadline_overruns() const { return overruns_; }
  uint64_t total_ns() const { return total_ns_; }
  uint64_t deadline_ns() const { return deadline_ns_; }

  /// 0 when no blocks have been recorded.
  uint64_t p50_ns() const;
  uint64_t p99_ns() const;
  /// 0 when no blocks have been recorded.
  uint64_t max_ns() const;

  /// p99 block time as a percentage of the deadline. 0 when empty.
  double p99_percent_of_deadline() const;

  void reset();

private:
  uint64_t percentile(double quantile) const;

  uint64_t deadline_ns_ = 0;
  uint64_t overruns_ = 0;
  uint64_t total_ns_ = 0;
  uint64_t max_ns_ = 0;
  std::vector<uint64_t> durations_;
};

} // namespace namchain
