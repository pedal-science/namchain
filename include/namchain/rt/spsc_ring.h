#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace namchain
{
namespace rt
{

/// \brief Lock-free single-producer / single-consumer ring.
///
/// The shape the design needs in two places (§3.2, §8.3): the control thread posts POD commands and
/// the audio thread drains them, and the audio thread posts telemetry that the control thread
/// drains. Both directions are one producer and one consumer, which is the case that can be done
/// wait-free with two indices.
///
/// Constraints this class exists to enforce:
///   * `push`/`pop` never allocate, lock, or block -- storage is sized at construction;
///   * the producer only ever writes `head_`, the consumer only ever writes `tail_`, so neither
///     needs a compare-exchange, and the padding keeps them off each other's cache line;
///   * capacity is rounded up to a power of two so indexing is a mask, not a division.
///
/// It is not a queue for arbitrary concurrent access: exactly one thread may call push and exactly
/// one (different) thread may call pop. The threaded test in tests/test_rt_primitives.cpp exercises
/// it under ThreadSanitizer.
template <typename T>
class SpscRing
{
public:
  explicit SpscRing(std::size_t capacity)
  {
    std::size_t rounded = 1;
    while (rounded < capacity)
      rounded <<= 1;
    capacity_ = rounded;
    mask_ = rounded - 1;
    buffer_.resize(capacity_);
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  /// Producer side.
  ///
  /// \return false if the ring is full. Nothing is stored and nothing is overwritten. That is a
  /// *refusal*, not a loss: a producer that retries (as the threaded test does) loses nothing,
  /// while a command ring that refuses has hit a design bug and must report it. The counter is
  /// named for what actually happened rather than for what it might imply.
  bool push(const T& value) noexcept
  {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    const uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= capacity_)
    {
      refused_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    buffer_[head & mask_] = value;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Consumer side. \return false if the ring is empty.
  bool pop(T& out) noexcept
  {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    const uint64_t head = head_.load(std::memory_order_acquire);
    if (tail == head)
      return false;
    out = buffer_[tail & mask_];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const noexcept { return capacity_; }

  std::size_t size() const noexcept
  {
    return static_cast<std::size_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
  }

  bool empty() const noexcept { return size() == 0; }
  bool full() const noexcept { return size() >= capacity_; }

  /// How many pushes were refused because the ring was full.
  uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }

private:
  std::vector<T> buffer_;
  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  // One cache line each: the producer and consumer indices are written by different threads, and
  // sharing a line between them turns every push into a coherence stall on the consumer's core.
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) std::atomic<uint64_t> refused_{0};
};

} // namespace rt
} // namespace namchain
