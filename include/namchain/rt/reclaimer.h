#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace namchain
{
namespace rt
{

/// \brief Epoch-based reclamation for objects the render path retires.
///
/// The audio thread may not `delete` (§3.3): freeing can take a lock, and the object may still be
/// referenced by a plan the audio thread is part-way through. So the render thread only *retires* --
/// it records the pointer and the current epoch -- and the control thread frees later, once
/// `grace_epochs` have passed. That delay is what makes it safe to have swapped a plan while a block
/// in flight is still reading the old one.
///
/// `retire` and `advance_epoch` are render-path calls and allocate nothing: storage is sized at
/// construction. `collect` is a control-thread call.
class Reclaimer
{
public:
  /// Type-erased delete, so the reclaimer need not know what it holds. A plain function pointer,
  /// not std::function, which would allocate on the render path.
  using Deleter = void (*)(void*);

  /// \param grace_epochs How many epoch advances must pass before an object may be freed; the
  ///        engine uses `fade + 2` blocks (§3.2).
  /// \param capacity Maximum objects in flight. Exceeding it is an engine bug rather than a load
  ///        condition, so it is counted and reported rather than silently growing.
  Reclaimer(uint64_t grace_epochs, std::size_t capacity);

  Reclaimer(const Reclaimer&) = delete;
  Reclaimer& operator=(const Reclaimer&) = delete;

  /// Render thread: hand an object over for eventual deletion.
  /// \return false if the queue was full (counted in dropped()).
  bool retire(void* object, Deleter deleter) noexcept;

  /// Render thread: called once per block.
  void advance_epoch() noexcept;

  /// Control thread: free everything whose grace window has closed. FIFO by epoch, so the scan
  /// stops at the first object that is not yet eligible.
  /// \return how many objects were freed.
  uint64_t collect() noexcept;

  uint64_t epoch() const noexcept;
  bool has_pending() const noexcept;
  std::size_t pending() const noexcept;

  uint64_t retired() const noexcept { return retired_.load(std::memory_order_relaxed); }
  uint64_t collected() const noexcept { return collected_; }
  uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
  struct Entry
  {
    void* object = nullptr;
    Deleter deleter = nullptr;
    uint64_t epoch = 0;
  };

  std::vector<Entry> entries_;
  std::size_t capacity_ = 0;
  uint64_t grace_epochs_ = 1;
  alignas(64) std::atomic<uint64_t> head_{0}; // producer: render thread
  alignas(64) std::atomic<uint64_t> tail_{0}; // consumer: control thread
  alignas(64) std::atomic<uint64_t> epoch_{0};
  alignas(64) std::atomic<uint64_t> retired_{0};
  std::atomic<uint64_t> dropped_{0};
  uint64_t collected_ = 0; // control thread only
};

} // namespace rt
} // namespace namchain
