#include "namchain/io/loop_source.h"

#include <utility>

#include "namchain/wav.h"

namespace namchain
{

bool LoopSource::load(const std::string& path, unsigned expected_rate, LoopSource& out, std::string& error)
{
  std::vector<float> interleaved;
  unsigned channels = 0;
  double rate = 0.0;
  // load_wav's own message names the file and the reason it could not be read; there is nothing to
  // add to it, so it is passed through unchanged.
  if (!load_wav(path, interleaved, channels, rate, error))
    return false;

  if (static_cast<unsigned>(rate + 0.5) != expected_rate)
  {
    error = "input '" + path + "' is " + std::to_string(static_cast<long long>(rate)) +
            " Hz but the engine runs at " + std::to_string(expected_rate) + " Hz";
    return false;
  }
  if (interleaved.empty() || channels == 0)
  {
    error = "input '" + path + "' has no frames to loop";
    return false;
  }

  out = from_samples(std::move(interleaved), channels);
  return true;
}

LoopSource LoopSource::from_samples(std::vector<float> samples, unsigned channels)
{
  LoopSource source;
  source.channels_ = channels == 0 ? 1 : channels;
  source.samples_ = std::move(samples);
  source.frames_ = static_cast<unsigned>(source.samples_.size() / source.channels_);
  source.position_ = 0;
  return source;
}

void LoopSource::read(float* dst, unsigned frames)
{
  if (frames_ == 0 || channels_ == 0)
  {
    // A source with nothing in it is a caller error -- load() refuses one -- but a silent period is
    // the safe reading of it, and the alternative is a read past the end of an empty vector.
    for (unsigned i = 0; i < frames * channels_; ++i)
      dst[i] = 0.0f;
    return;
  }

  for (unsigned i = 0; i < frames; ++i)
  {
    if (position_ >= frames_)
    {
      position_ = 0;
      ++wraps_;
    }
    const float* frame = samples_.data() + static_cast<std::size_t>(position_) * channels_;
    for (unsigned c = 0; c < channels_; ++c)
      dst[static_cast<std::size_t>(i) * channels_ + c] = frame[c];
    ++position_;
  }
}

} // namespace namchain
