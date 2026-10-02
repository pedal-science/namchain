#include "namchain/daemon/render_pool.h"

#include <cstring>

#if defined(__linux__)
  #include <sched.h>
#endif

namespace namchain
{
namespace daemon
{
namespace
{

/// The participant cap. A document is at most eight chains (kMaxChains), and a pin beyond this is a
/// configuration mistake rather than a request, so refusing here keeps `sched_setaffinity` from
/// being handed a CPU index nobody thought about.
constexpr unsigned kMaxRenderThreads = 16;

/// Spin rounds before a worker parks. A 64-frame block at 48 kHz is 1333 us and a worker's share of
/// a two-chain document is a few hundred microseconds, so a spin of this order keeps the common case
/// syscall-free -- the worker is still spinning when the next block is published -- while an idle
/// pool (a closed device, a paused daemon) sleeps instead of burning a core.
///
/// **Measured cost of the spin, 2026-10-02 (pedal.project, bench-results/2026-10-02-multi-core-piv.md,
/// "The thermal soak").** A participant whose share of the block is small spends most of the block
/// spinning: a worker rendering one A2-Full chain (about 7% of the deadline) reads back at 27% of its
/// core busy, and a participant with *no chain at all* still reads 19%. Three participants rendering a
/// one-chain document cost 58% of three cores to do what one core does in 13%. Thermally it was free
/// on that host (40.8 C peak with throttling never set), so this was left as it is: the spin is what
/// keeps a worker's wake off the critical path, and parking sooner trades it for one futex wake per
/// block. Lower this constant when something cares about the watts or the last few degrees.
constexpr unsigned kSpinRounds = 200000;

/// The CPU's own "I am spinning" hint: aarch64 `yield`, x86 `pause`.
inline void cpu_relax() noexcept
{
#if defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

} // namespace

RenderPool::~RenderPool()
{
  stop();
}

bool RenderPool::start(unsigned threads, int first_cpu, int priority, std::string& error)
{
  stop();

  if (threads > kMaxRenderThreads)
  {
    error = "render_threads " + std::to_string(threads) + " is more than the " +
            std::to_string(kMaxRenderThreads) + " this engine will start";
    return false;
  }
  if (threads > 1 && first_cpu < 0)
  {
    error = "render_threads above 1 needs a CPU to start from: set cpu as well";
    return false;
  }
  if (threads < 1)
    threads = 1;

  threads_ = threads;
  first_cpu_ = first_cpu;
  priority_ = priority;
  sequence_ = 0;
  degraded_reason_.clear();
  participants_.clear();
  workers_.clear();

  slots_.clear();
  slots_.reserve(threads_);
  for (unsigned i = 0; i < threads_; ++i)
    slots_.push_back(std::make_unique<Slot>());

  // Participant 0 is the caller: its scheduling is read, never set, and it is the coordinator for
  // the life of the pool.
  RenderParticipant self;
  self.index = 0;
  self.requested_cpu = first_cpu_;
  self.scheduling = rt::current_scheduling();
  participants_.push_back(self);
  for (unsigned i = 1; i < threads_; ++i)
  {
    RenderParticipant participant;
    participant.index = i;
    participant.requested_cpu = first_cpu_ + static_cast<int>(i);
    participant.scheduling.detail = "not started";
    participants_.push_back(participant);
  }

  ready_.store(1, std::memory_order_relaxed); // participant 0 is ready by definition
  unsigned started = 1;
  for (unsigned i = 1; i < threads_; ++i)
  {
    const int worker_cpu = first_cpu_ + static_cast<int>(i);
    auto start = std::make_unique<WorkerStart>(WorkerStart{this, i});

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(__linux__)
    // Born on its own core, not moved there afterwards: a worker that inherits the coordinator's
    // single-core mask could never run to pin itself once the coordinator is spinning at SCHED_FIFO
    // on that core (see the header). On a platform with no affinity call the worker pins itself in
    // worker_main, which is all that platform can do.
    if (worker_cpu >= 0 && worker_cpu < CPU_SETSIZE)
    {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(worker_cpu, &set);
      pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    }
#endif
    pthread_t handle{};
    const int created = pthread_create(&handle, &attr, &RenderPool::worker_trampoline, start.get());
    pthread_attr_destroy(&attr);
    if (created != 0)
    {
      // `start` still owns the record, and goes out of scope with this iteration: nothing to free by
      // hand and nothing left dangling.
      degraded_reason_ = "render worker " + std::to_string(i) + " could not start (" +
                         std::strerror(created) + "); rendering on " + std::to_string(started) + " of " +
                         std::to_string(threads_) + " cores";
      break;
    }
    // The trampoline owns the record from here; releasing it is what stops this scope freeing the
    // memory the new thread is about to read.
    start.release();
    workers_.push_back(handle);
    ++started;
  }
  if (started < threads_)
  {
    threads_ = started;
    slots_.resize(threads_);
    participants_.resize(threads_);
  }

  // Wait until every worker has pinned itself and touched its stack, so the first block does not
  // race a worker that is still starting up.
  while (ready_.load(std::memory_order_acquire) < threads_)
    cpu_relax();
  return true;
}

void RenderPool::stop() noexcept
{
  for (unsigned i = 1; i < threads_ && i < slots_.size(); ++i)
  {
    slots_[i]->quit.store(true, std::memory_order_release);
    slots_[i]->go.store(++sequence_, std::memory_order_release);
    slots_[i]->go.notify_all();
  }
  for (pthread_t worker : workers_)
    pthread_join(worker, nullptr);
  workers_.clear();
  threads_ = 1;
  sequence_ = 0;
  slots_.clear();
  participants_.clear();
  degraded_reason_.clear();
}

void RenderPool::run(void* context, Work work, int frames) noexcept
{
  if (work == nullptr)
    return;

  if (threads_ <= 1)
  {
    work(context, 0, 1, frames);
    return;
  }

  // Both are only read by a worker after it has acquired `go` below, and the coordinator does not
  // write them again until every worker has published completion for this block.
  context_ = context;
  work_ = work;

  const uint64_t sequence = ++sequence_;

  // Publish the block to every worker before doing our own share, so the workers start while the
  // coordinator is still working rather than after it has finished.
  for (unsigned i = 1; i < threads_; ++i)
  {
    Slot& slot = *slots_[i];
    slot.frames = frames;
    slot.go.store(sequence, std::memory_order_release);
    // Only wake a worker that has actually parked: one that is spinning sees `go` on its own, which
    // is what keeps the steady state free of the wake.
    if (slot.parked.load(std::memory_order_acquire) != 0)
      slot.go.notify_one();
  }

  work(context, 0, threads_, frames);

  // Wait for them. The coordinator has nothing else to do with its core until the block is whole, so
  // it spins: a sleep here would cost more than the wait it replaces.
  for (unsigned i = 1; i < threads_; ++i)
  {
    Slot& slot = *slots_[i];
    while (slot.done.load(std::memory_order_acquire) != sequence)
      cpu_relax();
  }
}

void* RenderPool::worker_trampoline(void* arg)
{
  // The start record is owned by the trampoline for the life of the thread, so nothing has to be
  // kept alive on the coordinator's side and nothing is freed on the render path.
  std::unique_ptr<WorkerStart> start(static_cast<WorkerStart*>(arg));
  start->pool->worker_main(start->index);
  return nullptr;
}

void RenderPool::worker_main(unsigned index)
{
  Slot& slot = *slots_[index];

  const int cpu = first_cpu_ >= 0 ? first_cpu_ + static_cast<int>(index) : -1;
  participants_[index].scheduling = rt::apply_scheduling(priority_, cpu);

  // Touch the stack this thread will run on, so the first block does not take a page fault on the
  // render path. mlockall(MCL_CURRENT) runs after the pool starts and covers what is faulted here.
  {
    volatile unsigned char scratch[64 * 1024];
    for (std::size_t i = 0; i < sizeof(scratch); i += 4096)
      scratch[i] = 0;
  }

  ready_.fetch_add(1, std::memory_order_release);

  uint64_t seen = 0;
  for (;;)
  {
    uint64_t go = slot.go.load(std::memory_order_acquire);
    if (go == seen)
    {
      slot.parked.store(1, std::memory_order_release);
      // Re-read: a block published between the load above and the parked flag is not a wakeup we are
      // owed, and this is what stops it being missed.
      go = slot.go.load(std::memory_order_acquire);
      if (go == seen && !slot.quit.load(std::memory_order_acquire))
      {
        unsigned spins = 0;
        while (spins < kSpinRounds)
        {
          cpu_relax();
          go = slot.go.load(std::memory_order_acquire);
          if (go != seen || slot.quit.load(std::memory_order_acquire))
            break;
          ++spins;
        }
        // Still nothing to do: sleep until the coordinator publishes, which is when the wake below
        // is worth its syscall.
        while (go == seen && !slot.quit.load(std::memory_order_acquire))
        {
          slot.go.wait(go, std::memory_order_acquire);
          go = slot.go.load(std::memory_order_acquire);
        }
      }
      slot.parked.store(0, std::memory_order_release);
      if (slot.quit.load(std::memory_order_acquire))
        return;
      if (go == seen)
        continue; // spurious
    }

    if (slot.quit.load(std::memory_order_acquire))
      return;

    seen = go;
    work_(context_, index, threads_, slot.frames);
    slot.done.store(seen, std::memory_order_release);
  }
}

} // namespace daemon
} // namespace namchain
