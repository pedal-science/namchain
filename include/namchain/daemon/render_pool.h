#pragma once

// The render pool: the worker threads behind `render_threads` (pedal.project docs/multi-core.md).
//
// The render path has one coordinator and, above one core, N-1 workers. A block is block-synchronous:
// the coordinator publishes the block, runs its own share, waits for every worker to publish
// completion, and only then does the work that must be single-owner -- the crossfade mixes, the
// transition bookkeeping and the output bus. Nothing about a *chain* changes: each chain is still
// executed by exactly one thread, in its original node order, so a document rendered with two
// threads is bit-identical to the same document rendered with one.
//
// What this file promises about the render path:
//   * no allocation, no lock and no unbounded wait on any participant;
//   * a worker that is already spinning makes no syscall at all;
//   * the coordinator's only possible syscall is the wake of a worker that has parked, which happens
//     after an idle gap rather than in the steady state, and only when that worker is actually
//     parked (the pool publishes a `parked` flag through which the coordinator decides).
//
// Everything here is deliberately a fixed-shape function-pointer callback rather than a
// `std::function`: the contract is a per-block call, and a per-block call may not allocate.
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "namchain/rt/scheduling.h"

namespace namchain
{
namespace daemon
{

/// What one participant of the pool asked the kernel for, and what it got. Read back rather than
/// assumed, so a build that could not pin a worker says so (rt/scheduling.h).
struct RenderParticipant
{
  unsigned index = 0;          ///< 0 is the coordinator, which is the caller's own thread.
  int requested_cpu = -1;      ///< The CPU asked for, or -1 for "leave the affinity alone".
  rt::SchedulingState scheduling; ///< What the thread is actually in.
};

/// \brief A block-synchronous pool of real-time worker threads.
///
/// The coordinator is participant 0 and is whatever thread calls \ref run: the pool never owns it.
/// Participants 1..N-1 are threads this class starts, pins and stops.
class RenderPool
{
public:
  /// The per-block work. Called on every participant with its own index and the participant count,
  /// so a participant can take every `participants`-th unit of work. Must not allocate, lock or
  /// throw (rt_check.h) and must not touch another participant's state.
  using Work = void (*)(void* context, unsigned participant, unsigned participants, int frames) noexcept;

  RenderPool() = default;
  ~RenderPool();
  RenderPool(const RenderPool&) = delete;
  RenderPool& operator=(const RenderPool&) = delete;

  /// \brief Start \p threads - 1 workers, participant \p i pinned to `first_cpu + i`.
  ///
  /// \param threads Total participants including the coordinator. 1 starts nothing and makes \ref run
  ///        call the work inline, which is the single-core engine and allocates nothing.
  /// \param first_cpu The coordinator's CPU, or -1 with \p threads of 1. Above 1 it must be >= 0, so
  ///        that each worker has a CPU of its own to be pinned to.
  /// \param priority SCHED_FIFO priority for the workers; 0 leaves the policy alone.
  /// \param error Set only for a request this class refuses outright (an impossible participant
  ///        count). A worker that cannot be *created* is not a refusal: the pool starts with the
  ///        workers it has and reports it in \ref degraded_reason, because an appliance that cannot
  ///        have its extra cores still has to play.
  /// \return false only when \p error says the request was refused.
  bool start(unsigned threads, int first_cpu, int priority, std::string& error);

  /// \brief Run one block on every participant and return when all have finished.
  ///
  /// With one participant this is a plain call, with no atomic traffic at all.
  void run(void* context, Work work, int frames) noexcept;

  /// Stop and join the workers. Idempotent, and called by the destructor.
  void stop() noexcept;

  unsigned threads() const noexcept { return threads_; }
  /// Empty when every requested worker started; otherwise why the pool is smaller than asked for.
  const std::string& degraded_reason() const noexcept { return degraded_reason_; }
  /// One entry per participant, with what it actually got. Valid after \ref start.
  const std::vector<RenderParticipant>& participants() const noexcept { return participants_; }
  /// Blocks dispatched since \ref start. A report, not a control.
  uint64_t blocks() const noexcept { return sequence_; }

private:
  /// One worker's mailbox: the coordinator writes `frames` and then `go`, the worker writes `done`.
  /// `alignas(64)` keeps two participants' flags off one cache line.
  struct alignas(64) Slot
  {
    std::atomic<uint64_t> go{0};
    std::atomic<uint64_t> done{0};
    std::atomic<bool> quit{false};
    std::atomic<unsigned> parked{0};
    int frames = 0;
  };

  void worker_main(unsigned index);

  std::vector<std::unique_ptr<Slot>> slots_;
  std::vector<std::thread> workers_;
  std::vector<RenderParticipant> participants_;
  /// The block's work and its context, published with `go` and stable until every participant has
  /// finished the block.
  Work work_ = nullptr;
  void* context_ = nullptr;
  std::atomic<unsigned> ready_{0};
  unsigned threads_ = 1; ///< Participants, coordinator included. 1 means "no workers".
  int first_cpu_ = -1;
  int priority_ = 0;
  uint64_t sequence_ = 0;
  std::string degraded_reason_;
};

} // namespace daemon
} // namespace namchain
