#include "namchain/rt/crossfade.h"

#include <algorithm>
#include <cmath>

namespace namchain
{
namespace rt
{

namespace
{
constexpr float kHalfPi = 1.57079632679489661923f;
} // namespace

Crossfade::Crossfade(int max_frames) : max_frames_(max_frames > 0 ? max_frames : 1)
{
  from_.resize(static_cast<std::size_t>(max_frames_) + 1);
  to_.resize(static_cast<std::size_t>(max_frames_) + 1);
  // cos/sin of a quarter turn: at t = 0 the old signal is at full weight and the new at zero, at
  // t = 1 the reverse, and from^2 + to^2 == 1 everywhere between -- the equal-power law, which is
  // right for two signals that are unrelated. `mix` normalises these weights for the correlation the
  // two signals actually have, so the same table serves every rho.
  for (int i = 0; i <= max_frames_; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(max_frames_);
    const float theta = kHalfPi * t;
    from_[static_cast<std::size_t>(i)] = std::cos(theta);
    to_[static_cast<std::size_t>(i)] = std::sin(theta);
  }
}

void Crossfade::start(int frames, double rho) noexcept
{
  length_ = frames < 1 ? 1 : (frames > max_frames_ ? max_frames_ : frames);
  position_ = 0;
  remaining_ = length_;
  measure_ = rho < 0.0;
  rho_ = measure_ ? 0.0f : static_cast<float>(std::min(1.0, std::max(0.0, rho)));
}

void Crossfade::mix(const float* a, const float* b, float* out, int frames) noexcept
{
  if (measure_ && frames > 0)
  {
    // The correlation of the two signals as they meet, from the block that is about to be mixed.
    // None is available from before: a fade starts when an edit commits, and what matters is whether
    // these two carry the same audio *now*. Clamped to [0, 1] -- a negative rho would divide by less
    // than one and amplify the fade, which is the opposite of the point, and a 1.33 ms window over
    // near-silence produces one readily.
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int i = 0; i < frames; ++i)
    {
      ab += static_cast<double>(a[i]) * b[i];
      aa += static_cast<double>(a[i]) * a[i];
      bb += static_cast<double>(b[i]) * b[i];
    }
    const double den = std::sqrt(aa * bb);
    rho_ = static_cast<float>(den > 0.0 ? std::min(1.0, std::max(0.0, ab / den)) : 0.0);
    measure_ = false;
  }

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
    float g1 = from_[static_cast<std::size_t>(index)];
    float g2 = to_[static_cast<std::size_t>(index)];
    if (rho_ > 0.0f)
    {
      // The sum's power is g1^2 + g2^2 + 2*rho*g1*g2, which the equal-power weights take to be 1
      // whatever rho is. Divide it out. Skipped entirely at rho = 0, which is what two unrelated
      // signals measure and what the law was already right for.
      const float norm = 1.0f / std::sqrt(1.0f + 2.0f * rho_ * g1 * g2);
      g1 *= norm;
      g2 *= norm;
    }
    out[i] = a[i] * g1 + b[i] * g2;
    ++i;
    ++position_;
    --remaining_;
  }
  for (; i < frames; ++i)
    out[i] = b[i];
}

} // namespace rt
} // namespace namchain
