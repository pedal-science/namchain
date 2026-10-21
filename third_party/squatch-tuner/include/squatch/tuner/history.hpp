// SPDX-License-Identifier: MIT
// Fixed-capacity history of the most recent samples, readable oldest-to-newest.
#pragma once
#include <array>

namespace squatch::tuner {

/// Keeps the last N samples. Storage is doubled so that the newest `len` samples are always
/// one contiguous span (no wrap handling in the inner loops that read it).
template <int N>
class History {
 public:
  void clear() {
    data_.fill(0.0f);
    head_ = 0;
    count_ = 0;
  }

  void push(float x) {
    data_[head_] = x;
    data_[head_ + N] = x;
    head_ = (head_ + 1) % N;
    if (count_ < N) ++count_;
  }

  /// Pointer to the newest `len` samples (len <= N), oldest first.
  const float* latest(int len) const { return &data_[head_ + N - len]; }

  int count() const { return count_; }
  static constexpr int capacity() { return N; }

 private:
  std::array<float, 2 * N> data_{};
  int head_ = 0;
  int count_ = 0;
};

}  // namespace squatch::tuner
