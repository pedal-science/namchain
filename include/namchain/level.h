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
//   - **Relaxed atomics, one writer.** The render thread folds every sample into the accumulators as
//     it goes and the control thread reads them; there is no publish step and no second writer. A
//     reader that catches a block mid-accumulation gets values from two adjacent windows, which is a
//     meter flickering by a few milliseconds -- not a fault worth a lock on the audio thread. Two
//     *writers* would be a lie, so there is one.
//   - **Fixed point for the sum.** `sum_sq` accumulates `x * x * kScaleSq` as an integer: exact, and
//     independent of the order blocks arrive in, which a float accumulator in a relaxed atomic is
//     not. The mean square is one division on the control thread and the RMS one square root there.
//   - **No dB and no square root here.** The wire's unit is dBFS (`db_of_peak`, `db_of_rms`), but the
//     accumulators stay in linear amplitude squared: the conversion needs a logarithm and a root, and
//     the audio thread is no place for either.
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>

namespace namchain::level
{

/// The square is accumulated as `x * x * kScaleSq`, so one sample's square keeps about 1.5e-8 of a
/// unit: a lone sample quieter than -81 dBFS rounds to nothing, which is a little below the -78 dBFS
/// that accumulating `|x|` could resolve. A whole window of quiet samples still reads correctly --
/// thousands of terms of a fraction of a unit sum to a number -- so only a window of a few samples
/// that are all below the floor is lost, and that is not a level anybody meters.
///
/// Bigger would resolve quieter samples and overflow sooner, because the sum grows with the *square*:
/// at 2^26 a `uint64_t` holds about 66 days of unbroken full-scale accumulation in a window nobody
/// reads, and the clamp on each term (\ref kMaxSquare) holds the worst case at about six hours.
inline constexpr double kScaleSq = 67108864.0; // 2^26

/// The largest square that is accumulated as itself: `|x| <= 16`, 24 dB past full scale.
///
/// Without it the sum is a cliff rather than a meter. A sample past this contributes the clamp, so
/// the worst case is bounded no matter what a node, a model or a device hands over: 6.2 hours at
/// 48 kHz of a *continuous* +24 dBFS in a window nobody has read. The peak is untouched by this and
/// still reports the sample itself.
inline constexpr float kMaxSquare = 256.0f;

/// What one sample past \ref kMaxSquare contributes. Checking a running sum for overflow would cost
/// another atomic read per sample, so the sum is bounded by construction instead.
inline constexpr uint64_t kMaxTerm = static_cast<uint64_t>(kMaxSquare * kScaleSq);

/// The dBFS a peak is quoted in when it is silence. A floor rather than `-inf`, because the wire is
/// JSON and a client that does arithmetic on a sentinel is a client that works.
inline constexpr double kFloorDb = -200.0;

/// \brief Linear amplitude -- a peak, or an RMS -- to dBFS, floored.
///
/// Silence, a NaN and an infinity all read as the floor. NaN is not greater than zero, so the first
/// test catches it; an infinity has to be caught by name, because JSON cannot carry one and a meter
/// has nothing to say about it. A device that hands over an inf sample therefore reports a level at
/// the floor and a nonzero `clipped` count rather than an `engine.levels` reply that fails to
/// serialize.
inline double db_of_amplitude(double amplitude) noexcept
{
  if (!(amplitude > 0.0) || !std::isfinite(amplitude))
    return kFloorDb;
  const double db = 20.0 * std::log10(amplitude);
  return db < kFloorDb ? kFloorDb : db;
}

/// A sample peak to dBFS, floored.
inline double db_of_peak(float amplitude) noexcept
{
  return db_of_amplitude(static_cast<double>(amplitude));
}

/// An RMS (the root of a mean square) to dBFS, floored.
inline double db_of_rms(double rms) noexcept
{
  return db_of_amplitude(rms);
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
  /// `sum(x * x * kScaleSq)` since the last read, each `x * x` clamped to \ref kMaxSquare.
  std::atomic<uint64_t> sum_sq{0};
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
    // One square and a compare, where the amplitude accumulator had an `fabs` and a multiply: the same
    // order of work on the same buffers. A non-finite sample contributes nothing (its peak already
    // reads as the floor) rather than making the cast to `uint64_t` undefined, and one past the clamp
    // contributes the clamp.
    const float square = x * x;
    uint64_t term = 0;
    if (std::isfinite(square))
      term = square < kMaxSquare ? static_cast<uint64_t>(square * kScaleSq + 0.5) : kMaxTerm;
    sum_sq.fetch_add(term, std::memory_order_relaxed);
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
    double rms = 0.0;       ///< root of the mean square in the window
    uint64_t frames = 0;    ///< samples in the window, 0 when nothing was measured
    uint64_t clipped = 0;   ///< samples at or past full scale
  };

  /// Take another probe's numbers and leave it empty. Move only, before publication.
  void adopt(Probe& other) noexcept
  {
    peak_bits.store(other.peak_bits.load(std::memory_order_relaxed), std::memory_order_relaxed);
    sum_sq.store(other.sum_sq.load(std::memory_order_relaxed), std::memory_order_relaxed);
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
    const uint64_t sum = sum_sq.exchange(0, std::memory_order_relaxed);
    out.frames = frames.exchange(0, std::memory_order_relaxed);
    out.clipped = clipped.exchange(0, std::memory_order_relaxed);
    out.rms = root_mean_square(sum, out.frames);
    return out;
  }

  /// The reading without emptying it, for a report that wants the last window again. Control thread.
  Reading peek() noexcept
  {
    Reading out;
    out.peak = std::bit_cast<float>(static_cast<uint32_t>(peak_bits.load(std::memory_order_relaxed)));
    const uint64_t sum = sum_sq.load(std::memory_order_relaxed);
    out.frames = frames.load(std::memory_order_relaxed);
    out.clipped = clipped.load(std::memory_order_relaxed);
    out.rms = root_mean_square(sum, out.frames);
    return out;
  }

  /// \brief The fixed-point sum of squares as an amplitude: one division and one square root.
  ///
  /// Control thread only -- this is the part that is deliberately kept off the render path. Zero
  /// frames is silence rather than a division by nothing, and an empty window and a silent one are
  /// told apart by \ref Reading::frames, not by this.
  static double root_mean_square(uint64_t sum, uint64_t frames) noexcept
  {
    if (frames == 0)
      return 0.0;
    return std::sqrt((static_cast<double>(sum) / kScaleSq) / static_cast<double>(frames));
  }
};

} // namespace namchain::level
