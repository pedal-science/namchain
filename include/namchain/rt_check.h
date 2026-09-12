#pragma once

#include <cstdint>

namespace namchain
{

/// \brief RAII marker meaning "the current thread is now executing the render path".
///
/// **The rule is about a scope, not a thread.** A device loop must issue read/write syscalls to
/// move audio at all, so §3.3's "no allocation, lock, syscall on the audio thread" cannot literally
/// mean the whole audio thread -- if it did, the backend could not do its job. It means the
/// *engine's render function*. Everything between the backend's read and its write is inside this
/// scope; everything the backend does is outside it. Drawing that line in code now is the point:
/// it is much easier to define the boundary before the engine is written around it than after.
///
/// Scopes nest, and the guard is thread-local, so a control thread allocating concurrently is
/// neither counted nor able to hide an allocation on the render thread.
class RtScope
{
public:
  RtScope();
  ~RtScope();

  RtScope(const RtScope&) = delete;
  RtScope& operator=(const RtScope&) = delete;
};

/// True when this thread is inside at least one RtScope.
bool rt_in_scope();

/// \brief Whether the allocation interposer is compiled in.
///
/// When false, the counters below are meaningless and a report must say "not instrumented" rather
/// than "zero allocations" -- the difference between a clean measurement and no measurement.
bool rt_check_available();

/// Allocations performed inside an RtScope. Must be zero on the render path.
uint64_t rt_allocation_count();

/// Deallocations performed inside an RtScope. Must be zero too: freeing on the render thread is
/// the same class of mistake as allocating.
uint64_t rt_deallocation_count();

void rt_reset_counters();

} // namespace namchain
