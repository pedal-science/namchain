#include "namchain/rt/arena.h"

#include <cstdint>

namespace namchain
{
namespace rt
{
namespace
{

std::size_t align_up(std::size_t value, std::size_t alignment)
{
  if (alignment == 0)
    return value;
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

} // namespace

Arena::Arena(std::size_t bytes, std::size_t alignment)
{
  // Round the capacity up so an object of the requested alignment can always fit at offset 0.
  const std::size_t base_alignment = alignment == 0 ? 1 : alignment;
  capacity_ = align_up(bytes, base_alignment);
  storage_ = std::unique_ptr<std::byte, AlignedDelete>(
    static_cast<std::byte*>(::operator new(capacity_, std::align_val_t{base_alignment})), AlignedDelete{base_alignment});

  // Touch every page now. The plan is built on the control thread precisely so that the render
  // path never takes a page fault, and a fault in a callback is indistinguishable from a stall.
  volatile std::byte* touch = storage_.get();
  for (std::size_t i = 0; i < capacity_; i += 4096)
    touch[i] = std::byte{0};
  if (capacity_ > 0)
    touch[capacity_ - 1] = std::byte{0};
}

void* Arena::allocate(std::size_t bytes, std::size_t alignment) noexcept
{
  const std::size_t start = align_up(offset_, alignment == 0 ? 1 : alignment);
  if (bytes > capacity_ || start > capacity_ - bytes)
    return nullptr; // exhausted: a plan-build error, reported by the caller

  void* result = storage_.get() + start;
  offset_ = start + bytes;
  ++allocations_;
  if (offset_ > high_water_)
    high_water_ = offset_;
  return result;
}

void Arena::reset() noexcept
{
  offset_ = 0;
}

} // namespace rt
} // namespace namchain
