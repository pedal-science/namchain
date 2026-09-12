#include "namchain/rt_check.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace namchain
{
namespace
{

thread_local int g_scope_depth = 0;
std::atomic<uint64_t> g_allocations{0};
std::atomic<uint64_t> g_deallocations{0};

void note_allocation()
{
  if (g_scope_depth > 0)
    g_allocations.fetch_add(1, std::memory_order_relaxed);
}

void note_deallocation()
{
  if (g_scope_depth > 0)
    g_deallocations.fetch_add(1, std::memory_order_relaxed);
}

std::size_t non_zero(std::size_t bytes)
{
  return bytes == 0 ? 1 : bytes;
}

std::size_t round_up(std::size_t bytes, std::size_t alignment)
{
  return (non_zero(bytes) + alignment - 1) / alignment * alignment;
}

void* allocate_bytes(std::size_t bytes)
{
  void* p = std::malloc(non_zero(bytes));
  if (p == nullptr)
    throw std::bad_alloc();
  return p;
}

void* allocate_aligned(std::size_t bytes, std::size_t alignment)
{
  void* p = std::aligned_alloc(alignment, round_up(bytes, alignment));
  if (p == nullptr)
    throw std::bad_alloc();
  return p;
}

} // namespace

RtScope::RtScope()
{
  ++g_scope_depth;
}

RtScope::~RtScope()
{
  --g_scope_depth;
}

bool rt_in_scope()
{
  return g_scope_depth > 0;
}

bool rt_check_available()
{
#if defined(NAMCHAIN_RT_CHECK_ENABLED)
  return true;
#else
  return false;
#endif
}

uint64_t rt_allocation_count()
{
  return g_allocations.load(std::memory_order_relaxed);
}

uint64_t rt_deallocation_count()
{
  return g_deallocations.load(std::memory_order_relaxed);
}

void rt_reset_counters()
{
  g_allocations.store(0, std::memory_order_relaxed);
  g_deallocations.store(0, std::memory_order_relaxed);
}

} // namespace namchain

#if defined(NAMCHAIN_RT_CHECK_ENABLED)

// ---------------------------------------------------------------------------
// Global allocation interposer.
//
// Replacing operator new/delete is the standard-sanctioned way to observe every C++ allocation,
// and it works on the target (GCC on aarch64) unlike RealtimeSanitizer, which is clang-only. The
// two are complementary rather than redundant: this proves "zero allocations" in a Release soak on
// the Pi, while RTSan in CI additionally covers locks and syscalls, which cannot be interposed
// this way without preempting pthread/libc symbols for the whole process.
//
// The cost is one predictable branch per allocation. This is a test instrument: a production
// daemon builds with -DNAMCHAIN_RT_CHECK=OFF.
// ---------------------------------------------------------------------------

void* operator new(std::size_t bytes)
{
  namchain::note_allocation();
  return namchain::allocate_bytes(bytes);
}

void* operator new[](std::size_t bytes)
{
  namchain::note_allocation();
  return namchain::allocate_bytes(bytes);
}

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
  namchain::note_allocation();
  return std::malloc(namchain::non_zero(bytes));
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept
{
  namchain::note_allocation();
  return std::malloc(namchain::non_zero(bytes));
}

void* operator new(std::size_t bytes, std::align_val_t alignment)
{
  namchain::note_allocation();
  return namchain::allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment)
{
  namchain::note_allocation();
  return namchain::allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  namchain::note_allocation();
  return std::aligned_alloc(static_cast<std::size_t>(alignment),
                            namchain::round_up(bytes, static_cast<std::size_t>(alignment)));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  namchain::note_allocation();
  return std::aligned_alloc(static_cast<std::size_t>(alignment),
                            namchain::round_up(bytes, static_cast<std::size_t>(alignment)));
}

void operator delete(void* p) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete[](void* p) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete(void* p, std::align_val_t) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete[](void* p, std::align_val_t) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
  namchain::note_deallocation();
  std::free(p);
}

#endif // NAMCHAIN_RT_CHECK_ENABLED
