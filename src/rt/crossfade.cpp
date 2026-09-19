#include "namchain/rt/crossfade.h"

#include <cmath>

namespace namchain
{
namespace rt
{

namespace
{
constexpr float kHalfPi = 1.57079632679489661923f;
}

Crossfade::Crossfade(int max_frames) : max_frames_(max_frames > 0 ? max_frames : 1)
{
  from_.resize(static_cast<std::size_t>(max_frames_) + 1);
  to_.resize(static_cast<std::size_t>(max_frames_) + 1);
  // cos/sin of a quarter turn: at t = 0 the old signal is at full weight and the new at zero; at
  // t = 1 the reverse; and from^2 + to^2 == 1 everywhere between.
  for (int i = 0; i <= max_frames_; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(max_frames_);
    const float theta = kHalfPi * t;
    from_[static_cast<std::size_t>(i)] = std::cos(theta);
    to_[static_cast<std::size_t>(i)] = std::sin(theta);
  }
}

void Crossfade::start(int frames) noexcept
{
  length_ = frames < 1 ? 1 : (frames > max_frames_ ? max_frames_ : frames);
  position_ = 0;
  remaining_ = length_;
}

void Crossfade::mix(const float* a, const float* b, float* out, int frames) noexcept
{
  int i = 0;
  while (i < frames && remaining_ > 0)
  {
    // Map the fade's samples onto the table so the *last* one lands on the endpoint: with `length`
    // samples there are `length - 1` steps between them, not `length`. Reading `position * max /
    // length` instead leaves a residual `cos(pi/2 * (length-1)/length)` of the old signal at the
    // handoff -- 0.008 for a 240-frame fade, which is a step the detector measures. A one-frame fade
    // is the endpoint by definition.
    int index = max_frames_;
    if (length_ > 1)
      index = static_cast<int>((static_cast<long long>(position_) * max_frames_) / (length_ - 1));
    out[i] = a[i] * from_[static_cast<std::size_t>(index)] + b[i] * to_[static_cast<std::size_t>(index)];
    ++i;
    ++position_;
    --remaining_;
  }
  for (; i < frames; ++i)
    out[i] = b[i];
}

} // namespace rt
} // namespace namchain
