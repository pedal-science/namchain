// SPDX-License-Identifier: MIT
// A note's stiffness B, learned from the free fits and then held.
//
// B is the string's own, so it is learned while the note is young and loud and then held:
//  - not in the first 50 ms (the attack gives wild estimates), so B starts at 0;
//  - fast (0.2 per hop) to 0.3 s, then slowly (0.03 per hop) to 2 s, then held;
//  - only while the note is within 30 dB of its peak, and at most kMaxStep per hop, so one wild
//    free fit cannot jump it.
// Learned all along, it followed whatever else rang: as partials faded, or other strings'
// partials sat beside the note's, the free fit's B ran to its 2e-3 limit and f0 moved with it by
// c k^2. That flashed the number ~45 c on quiet EGFxSet tails, swung it 35 c when open strings
// rang together, and stepped our open-E take 2.4 c at 1.5 s.
#pragma once
#include <algorithm>

#include "partial_fit.hpp"

namespace squatch::tuner {

class StiffnessLearner {
 public:
  static constexpr float kStartSeconds = 0.05f, kFastSeconds = 0.3f, kHoldSeconds = 2.0f;
  static constexpr float kMaxStep = 1e-4f;  // B per hop

  void reset() { b_ = 0.0f; }

  /// Whether a free fit now would be learned from (so it need not be made otherwise).
  bool wants(float noteSeconds, bool loud) const { return loud && noteSeconds >= kStartSeconds && noteSeconds <= kHoldSeconds; }

  /// One hop's free fit, `noteSeconds` into the note, `loud` while within 30 dB of its peak.
  void learn(const PartialFit& free, float noteSeconds, bool loud) {
    if (!wants(noteSeconds, loud) || !free.valid || free.used < 3) return;
    const float rate = noteSeconds < kFastSeconds ? 0.2f : 0.03f;
    b_ += std::clamp(rate * (free.inharmonicity - b_), -kMaxStep, kMaxStep);
  }

  float b() const { return b_; }

 private:
  float b_ = 0.0f;
};

}  // namespace squatch::tuner
