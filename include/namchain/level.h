// Level probes: what is actually coming out of each place in the signal path.
//
// The engine measures no audio level anywhere else. `glitch.h` and the soak's oracle compute RMS
// offline, over files, on a control thread; nothing reports what a node, a chain or the device is
// doing *now*. A level meter in ljoss needs that, and so does a diagnostic that has to answer "is
// the input dead, or is the level just low".
//
// The design, and why this shape:
//
//   - **Latest value, not history.** A meter wants the newest number, not a log of old ones, so a
//     probe is a small set of accumulators that are reset as they are read rather than a ring that
//     grows. Nothing allocates on the render path, and nothing goes wrong when a reader is slow.
//   - **Relaxed atomics, one writer.** The render thread accumulates for a whole period and
//     publishes at the end; the control thread reads. A reader that catches a period mid-publish
//     gets values from two adjacent periods, which is a meter flickering by a few milliseconds --
//     not a fault worth a lock on the audio thread. Two *writers* would be a lie, so there is one.
//   - **Fixed point for the sums.** `sum_abs` accumulates `|x| * kScale` as an integer: exact, and
//     independent of the order blocks arrive in, which a float accumulator in a relaxed atomic is
//     not. RMS is then one division on the control thread.
//   - **No dB here.** The wire's unit is dBFS (`db_of_peak`, `db_of_rms`), but the accumulators
//     stay in linear amplitude: the conversion needs a logarithm, and the audio thread is no place
//     for one.
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

namespace namchain::level
{

/// Amplitude is accumulated as `|x| * kScale`, so a 32-bit sample keeps about 0.0002 of a unit:
/// far below the 0.1 dB the wire reports, and nowhere near a uint64 for a period's worth of samples.
inline constexpr double kScale = 4096.0;

/// The dBFS a peak is quoted in when it is silence. A floor rather than `-inf`, because the wire is
/// JSON and a client that does arithmetic on a sentinel is a client that works.
inline constexpr double kFloorDb = -200.0;

/// Linear amplitude to dBFS, floored.
inline double db_of_peak(float amplitude) noexcept
{
  if (!(amplitude > 0.0f))
    return kFloorDb;
  const double db = 20.0 * std::log10(static_cast<double>(amplitude));
  return db < kFloorDb ? kFloorDb : db;
}

/// RMS (already a mean of magnitudes) to dBFS, floored.
inline double db_of_rms(double rms) noexcept
{
  if (!(rms > 0.0))
    return kFloorDb;
  const double db = 20.0 * std::log10(rms);
  return db < kFloorDb ? kFloorDb : db;
}

/// \brief One place in the signal path, measured.
///
/// Every field is an atomic, which is why this is not copyable: a probe lives in a vector that the
/// control thread may rebuild while a reader is looking at an older plan, and an atomic is not a
/// value that can be copied.
struct Probe
{
  /// \brief The largest |sample| since the last read, as a float's bit pattern in the low 32 bits.
  ///
  /// A bit pattern rather than a `std::atomic<float>`, which is not guaranteed lock-free; and a
  /// *wide* atomic (`uint64_t`, which this daemon already relies on for its counters) rather than
  /// a `uint32_t`, because `std::atomic<unsigned int>` has no `fetch_max` on every standard
  /// library — libc++ does not have it. The upper half is always zero, so the integer order is the
  /// float order: `fabs` is applied before the store, so every value is non-negative and its bit
  /// pattern sorts as an unsigned integer.
  std::atomic<uint64_t> peak_bits{0};
  /// `sum(|x| * kScale)` since the last read.
  std::atomic<uint64_t> sum_abs{0};
  /// Samples accumulated since the last read.
  std::atomic<uint64_t> frames{0};
  /// \brief Samples at or past full scale since the last read.
  ///
  /// Only meaningful where full scale is the device's limit -- the ADC's input, the DAC's output,
  /// a chain's contribution to the bus. A node's float output is *allowed* past 1.0 and routinely
  /// is, so the wire does not report this per node.
  std::atomic<uint64_t> clipped{0};

  Probe() = default;
  Probe(const Probe&) = delete;
  Probe& operator=(const Probe&) = delete;
  /// rief Movable, because a probe lives in a `std::vector` that is reserved and filled as a plan
  /// is built. Nothing moves one after publication: the vector is finished before that.
  Probe(Probe&& other) noexcept { adopt(other); }
  Probe& operator=(Probe&& other) noexcept
  {
    if (this != &other)
      adopt(other);
    return *this;
  }

  /// The largest |sample| so far, as a bit pattern. A compare-exchange loop rather than
  /// `fetch_max`, which libc++ does not have: one word, so it is lock-free, and it retries only
  /// when another thread stored a larger peak in between -- which is to say almost never.
  void raise_peak(float magnitude) noexcept
  {
    uint64_t held = peak_bits.load(std::memory_order_relaxed);
    const uint64_t candidate = std::bit_cast<uint32_t>(magnitude);
    while (candidate > held &&
           !peak_bits.compare_exchange_weak(held, candidate, std::memory_order_relaxed,
                                            std::memory_order_relaxed))
    {
    }
  }

  /// Add one sample. Render thread.
  void accumulate(float x) noexcept
  {
    const float magnitude = std::fabs(x);
    raise_peak(magnitude);
    sum_abs.fetch_add(static_cast<uint64_t>(magnitude * kScale + 0.5), std::memory_order_relaxed);
    frames.fetch_add(1, std::memory_order_relaxed);
    if (magnitude >= 1.0f)
      clipped.fetch_add(1, std::memory_order_relaxed);
  }

  /// Add \p count samples of one buffer. Render thread.
  void accumulate(const float* buffer, int count) noexcept
  {
    for (int i = 0; i < count; ++i)
      accumulate(buffer[i]);
  }

  /// What one read takes away: the window that just ended.
  struct Reading
  {
    float peak = 0.0f;      ///< largest |sample| in the window
    double rms = 0.0;       ///< mean |sample| in the window
    uint64_t frames = 0;    ///< samples in the window, 0 when nothing was measured
    uint64_t clipped = 0;   ///< samples at or past full scale
  };

  /// Take another probe's numbers and leave it empty. Move only, before publication.
  void adopt(Probe& other) noexcept
  {
    peak_bits.store(other.peak_bits.load(std::memory_order_relaxed), std::memory_order_relaxed);
    sum_abs.store(other.sum_abs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    frames.store(other.frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
    clipped.store(other.clipped.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }

  /// \brief Read and reset, so the next window starts empty. Control thread.
  ///
  /// The resets are `exchange`s rather than a compare-and-swap loop: a render thread that
  /// accumulates while this runs loses at most a period's tail, and that tail is the next read's
  /// business. Nothing is lost for longer than one period.
  Reading take() noexcept
  {
    Reading out;
    out.peak = std::bit_cast<float>(static_cast<uint32_t>(peak_bits.exchange(0, std::memory_order_relaxed)));
    const uint64_t sum = sum_abs.exchange(0, std::memory_order_relaxed);
    out.frames = frames.exchange(0, std::memory_order_relaxed);
    out.clipped = clipped.exchange(0, std::memory_order_relaxed);
    out.rms = out.frames == 0 ? 0.0 : (static_cast<double>(sum) / kScale) / static_cast<double>(out.frames);
    return out;
  }

  /// The reading without emptying it, for a report that wants the last window again. Control thread.
  Reading peek() noexcept
  {
    Reading out;
    out.peak = std::bit_cast<float>(static_cast<uint32_t>(peak_bits.load(std::memory_order_relaxed)));
    const uint64_t sum = sum_abs.load(std::memory_order_relaxed);
    out.frames = frames.load(std::memory_order_relaxed);
    out.clipped = clipped.load(std::memory_order_relaxed);
    out.rms = out.frames == 0 ? 0.0 : (static_cast<double>(sum) / kScale) / static_cast<double>(out.frames);
    return out;
  }
};

} // namespace namchain::level
