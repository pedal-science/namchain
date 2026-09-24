#pragma once

// A WAV held in memory and read round-robin: the "signal" for the two backends that audition a rig
// with no instrument in the picture (`pipe`, and `coreaudio`'s file capture).
//
// It is its own type rather than forty lines inside each backend because **the wrap rule is the
// interesting part and it is easy to get wrong**: a reader that wraps once per call loses the tail of
// a file whose length is not a whole number of periods, and a fixture is exactly the kind of file
// that is not. One implementation means the two backends cannot disagree about what "looped" means,
// and it can be tested on every platform -- CI has no CoreAudio and no FIFO.
#include <string>
#include <vector>

namespace namchain
{

/// \brief A WAV loaded once and read round-robin, one frame at a time.
///
/// Not thread-safe, deliberately: the reader is the engine thread, or the device's IO thread in the
/// CoreAudio backend, and never both at once. Making it atomic would hide a design mistake rather
/// than prevent one.
class LoopSource
{
public:
  LoopSource() = default;

  /// \brief Load \p path as interleaved float samples, refusing anything that cannot be looped.
  ///
  /// Refuses a file at another rate -- the engine has no resampler (§9.2, G11) -- and a file with no
  /// frames, because a loop over nothing is an infinite silence that looks like a working device.
  /// \return false with \p error set, naming the file and the number that did not fit.
  static bool load(const std::string& path, unsigned expected_rate, LoopSource& out, std::string& error);

  /// \brief Take ownership of already-loaded interleaved samples.
  ///
  /// \param samples interleaved, \p channels wide. Must not be empty, and must be a whole number of
  ///        frames; the caller has already decided both are true.
  static LoopSource from_samples(std::vector<float> samples, unsigned channels);

  unsigned channels() const { return channels_; }
  /// Frames in one pass of the file.
  unsigned frames() const { return frames_; }
  /// Where the next frame will come from, as a frame index. Only for tests and reports.
  unsigned position() const { return position_; }

  /// \brief Fill \p frames frames of interleaved output from the loop and advance.
  ///
  /// Wraps **per frame**, not per call: a request that straddles the end of the file returns the
  /// file's last frames followed by its first ones, so nothing is dropped and no partial period is
  /// ever silent.
  void read(float* dst, unsigned frames);

  /// Rewind to the top. `start()` calls this so a reopen begins in the same place every time.
  void reset() { position_ = 0; }

  /// How many times the read position has wrapped since construction. Only for diagnostics: a
  /// counter here is how "this file is being consumed at the right rate" becomes checkable.
  unsigned wraps() const { return wraps_; }

private:
  std::vector<float> samples_; ///< interleaved
  unsigned channels_ = 0;
  unsigned frames_ = 0;
  unsigned position_ = 0;
  unsigned wraps_ = 0;
};

} // namespace namchain
