#pragma once

#include <cstddef>
#include <memory>
#include <new>

namespace namchain
{
namespace rt
{

/// \brief Bump allocator over one preallocated block.
///
/// The engine allocates nothing on the audio thread (§3.3): every buffer a plan needs is carved out
/// of an arena while the plan is being built, on the control thread. `allocate` is therefore
/// noexcept and returns nullptr rather than throwing -- a render-path caller cannot deal with an
/// exception, and an allocation failure here is a plan-build error that must be reported as such.
///
/// There is no `free`: releasing happens by `reset()` when a plan is retired, which is why the
/// memory planner in M2 is a liveness/interval-colouring pass rather than a heap.
class Arena
{
public:
  /// \param bytes Total size. Allocated (and touched) here, so the first render never page-faults.
  explicit Arena(std::size_t bytes, std::size_t alignment = 64);

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  /// \return aligned storage of \p bytes, or nullptr if the arena is exhausted.
  void* allocate(std::size_t bytes, std::size_t alignment = 64) noexcept;

  /// Typed convenience: `count` objects of \p T, aligned for \p T.
  template <typename T>
  T* allocate_array(std::size_t count) noexcept
  {
    return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
  }

  /// Forget everything allocated so far. Does not free or clear storage.
  void reset() noexcept;

  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t used() const noexcept { return offset_; }
  /// Largest `used()` ever reached, i.e. the arena size this plan actually needed.
  std::size_t high_water() const noexcept { return high_water_; }
  std::size_t allocations_served() const noexcept { return allocations_; }

private:
  /// Deleter that matches the aligned allocation. `std::make_unique<std::byte[]>` would give only
  /// 16-byte alignment, so a "64-byte aligned" pointer came back at 32 mod 64 -- aligning the
  /// offset is not enough, the base has to be aligned too.
  struct AlignedDelete
  {
    std::size_t alignment;
    void operator()(std::byte* pointer) const noexcept
    {
      ::operator delete(static_cast<void*>(pointer), std::align_val_t{alignment});
    }
  };

  std::unique_ptr<std::byte, AlignedDelete> storage_;
  std::size_t capacity_ = 0;
  std::size_t offset_ = 0;
  std::size_t high_water_ = 0;
  std::size_t allocations_ = 0;
};

} // namespace rt
} // namespace namchain
