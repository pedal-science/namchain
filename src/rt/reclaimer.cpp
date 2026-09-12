#include "namchain/rt/reclaimer.h"

namespace namchain
{
namespace rt
{

Reclaimer::Reclaimer(uint64_t grace_epochs, std::size_t capacity)
  : capacity_(capacity == 0 ? 1 : capacity), grace_epochs_(grace_epochs == 0 ? 1 : grace_epochs)
{
  entries_.resize(capacity_);
}

bool Reclaimer::retire(void* object, Deleter deleter) noexcept
{
  const uint64_t head = head_.load(std::memory_order_relaxed);
  const uint64_t tail = tail_.load(std::memory_order_acquire);
  if (head - tail >= capacity_)
  {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  Entry& entry = entries_[head % capacity_];
  entry.object = object;
  entry.deleter = deleter;
  entry.epoch = epoch_.load(std::memory_order_relaxed);

  head_.store(head + 1, std::memory_order_release);
  retired_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void Reclaimer::advance_epoch() noexcept
{
  epoch_.fetch_add(1, std::memory_order_release);
}

uint64_t Reclaimer::collect() noexcept
{
  const uint64_t current = epoch_.load(std::memory_order_acquire);
  uint64_t tail = tail_.load(std::memory_order_relaxed);
  const uint64_t head = head_.load(std::memory_order_acquire);

  uint64_t freed = 0;
  while (tail != head)
  {
    Entry& entry = entries_[tail % capacity_];
    // Entries are appended in epoch order, so the first ineligible one ends the scan.
    if (entry.epoch + grace_epochs_ > current)
      break;
    if (entry.deleter != nullptr)
      entry.deleter(entry.object);
    entry.object = nullptr;
    entry.deleter = nullptr;
    ++tail;
    ++freed;
  }

  tail_.store(tail, std::memory_order_release);
  collected_ += freed;
  return freed;
}

uint64_t Reclaimer::epoch() const noexcept
{
  return epoch_.load(std::memory_order_acquire);
}

std::size_t Reclaimer::pending() const noexcept
{
  return static_cast<std::size_t>(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
}

bool Reclaimer::has_pending() const noexcept
{
  return pending() > 0;
}

} // namespace rt
} // namespace namchain
