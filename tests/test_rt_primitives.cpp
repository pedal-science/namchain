// Tests for the RT core primitives: arena, SPSC ring, epoch reclamation.
//
// These are the pieces the render path is built from, so two things are checked for each of them:
// that they behave (including under a real producer/consumer thread pair, which is what
// ThreadSanitizer is for), and that the render-thread calls allocate and free nothing.
#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "namchain/rt/arena.h"
#include "namchain/rt/reclaimer.h"
#include "namchain/rt/scheduling.h"
#include "namchain/rt/spsc_ring.h"
#include "namchain/rt_check.h"

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

TEST_CASE("arena hands out aligned storage and reports exhaustion instead of throwing")
{
  namchain::rt::Arena arena(1024, 64);
  CHECK(arena.capacity() >= 1024u);
  CHECK(arena.used() == 0u);

  void* first = arena.allocate(100, 64);
  REQUIRE(first != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(first) % 64 == 0);

  void* second = arena.allocate(100, 64);
  REQUIRE(second != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(second) % 64 == 0);
  CHECK(second != first);

  // Exhaustion is a plan-build error, not an exception: the render path cannot handle one. Three
  // quarters of a kilobyte will not fit in what is left.
  CHECK(arena.allocate(1024, 64) == nullptr);

  // The failed allocation must not have disturbed the arena.
  CHECK(arena.allocate(64, 8) != nullptr);
}

TEST_CASE("arena reset reuses storage and tracks the high-water mark")
{
  namchain::rt::Arena arena(4096, 64);
  void* a = arena.allocate(1000, 64);
  REQUIRE(a != nullptr);
  const std::size_t after_first = arena.used();
  CHECK(arena.high_water() == after_first);

  arena.reset();
  CHECK(arena.used() == 0u);
  CHECK(arena.high_water() == after_first); // reset does not erase the measurement

  void* b = arena.allocate(1000, 64);
  CHECK(b == a); // same storage, reused
  CHECK(arena.high_water() == after_first);
}

TEST_CASE("arena hands out objects with the right alignment for their type")
{
  namchain::rt::Arena arena(4096);
  double* doubles = arena.allocate_array<double>(16);
  REQUIRE(doubles != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(doubles) % alignof(double) == 0);

  struct alignas(32) Wide
  {
    unsigned char bytes[32];
  };
  Wide* wides = arena.allocate_array<Wide>(4);
  REQUIRE(wides != nullptr);
  CHECK(reinterpret_cast<uintptr_t>(wides) % 32 == 0);
}

// ---------------------------------------------------------------------------
// SPSC ring
// ---------------------------------------------------------------------------

TEST_CASE("ring rounds capacity up to a power of two and preserves order")
{
  namchain::rt::SpscRing<int> ring(10); // rounds to 16
  CHECK(ring.capacity() == 16u);
  CHECK(ring.empty());

  for (int i = 0; i < 16; ++i)
    CHECK(ring.push(i));
  CHECK(ring.full());
  CHECK(ring.size() == 16u);

  // Full: the value is refused and counted, and nothing already stored is overwritten.
  CHECK_FALSE(ring.push(999));
  CHECK(ring.refused() == 1u);

  for (int i = 0; i < 16; ++i)
  {
    int value = -1;
    REQUIRE(ring.pop(value));
    CHECK(value == i);
  }
  CHECK(ring.empty());
  int ignored = 0;
  CHECK_FALSE(ring.pop(ignored));
}

TEST_CASE("ring survives wrapping far past its capacity")
{
  namchain::rt::SpscRing<uint32_t> ring(8);
  uint32_t next_write = 0;
  uint32_t next_read = 0;
  const uint32_t total = 10000;

  while (next_read < total)
  {
    // Keep the producer somewhat ahead of the consumer without overflowing the ring.
    while (next_write < total && !ring.full())
      REQUIRE(ring.push(next_write++));

    uint32_t value = 0;
    REQUIRE(ring.pop(value));
    CHECK(value == next_read);
    ++next_read;
  }
  CHECK(next_write == total);
  CHECK(ring.refused() == 0u);
}

TEST_CASE("ring transfers every item exactly once across two real threads")
{
  // The case the ring exists for, and the one a single-threaded test cannot check: with two
  // threads touching head_ and tail_ concurrently, only a race detector can say whether the
  // acquire/release pairing is right.
  constexpr uint64_t kItems = 200000;
  namchain::rt::SpscRing<uint64_t> ring(1024);

  std::atomic<bool> producer_done{false};
  uint64_t received = 0;
  uint64_t expected_next = 0;
  bool order_ok = true;
  bool duplicates = false;

  std::thread producer([&] {
    for (uint64_t i = 0; i < kItems;)
    {
      if (ring.push(i))
        ++i;
      else
        std::this_thread::yield(); // full: let the consumer drain
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    uint64_t value = 0;
    while (received < kItems)
    {
      if (ring.pop(value))
      {
        if (value != expected_next)
        {
          order_ok = false;
          if (value < expected_next)
            duplicates = true;
        }
        expected_next = value + 1;
        ++received;
      }
      else if (producer_done.load(std::memory_order_acquire) && ring.empty())
      {
        break;
      }
      else
      {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  CHECK(received == kItems);
  CHECK(order_ok);
  CHECK_FALSE(duplicates);
  // Deliberately no assertion on refused(): the producer retries when the ring is full, so a
  // non-zero count is normal rather than a fault. What matters is that every item arrived exactly
  // once, in order, with no duplicates -- the three checks above.
}

TEST_CASE("ring push and pop allocate nothing")
{
  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped: allocation interposer not compiled in");
    return;
  }

  namchain::rt::SpscRing<uint64_t> ring(64);
  uint64_t value = 0;
  ring.push(1);

  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int i = 0; i < 1000; ++i)
    {
      ring.push(static_cast<uint64_t>(i));
      ring.pop(value);
    }
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);
}

// ---------------------------------------------------------------------------
// Epoch reclamation
// ---------------------------------------------------------------------------

namespace
{

struct Tracked
{
  int value = 0;
};

std::atomic<int> g_freed{0};

void tracked_deleter(void* object)
{
  delete static_cast<Tracked*>(object);
  g_freed.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("reclaimer holds objects for the grace window, then frees them in order")
{
  g_freed = 0;
  namchain::rt::Reclaimer reclaimer(/*grace_epochs=*/2, /*capacity=*/16);

  auto* first = new Tracked{1};
  auto* second = new Tracked{2};

  CHECK(reclaimer.retire(first, &tracked_deleter));
  reclaimer.advance_epoch(); // epoch 1
  CHECK(reclaimer.retire(second, &tracked_deleter));
  reclaimer.advance_epoch(); // epoch 2

  CHECK(reclaimer.pending() == 2u);
  CHECK(reclaimer.retired() == 2u);

  // `first` was retired at epoch 0 and the grace is 2, so it may go once the epoch reaches 2.
  CHECK(reclaimer.collect() == 1u);
  CHECK(g_freed.load() == 1);

  // `second` was retired at epoch 1, so it needs the epoch to reach 3.
  CHECK(reclaimer.collect() == 0u);
  reclaimer.advance_epoch(); // epoch 3
  CHECK(reclaimer.collect() == 1u);
  CHECK(g_freed.load() == 2);

  CHECK(reclaimer.pending() == 0u);
  CHECK(reclaimer.collected() == 2u);
  CHECK_FALSE(reclaimer.has_pending());
}

TEST_CASE("reclaimer counts overflow rather than growing or corrupting")
{
  g_freed = 0;
  namchain::rt::Reclaimer reclaimer(1, /*capacity=*/4);
  std::vector<Tracked*> objects;
  for (int i = 0; i < 4; ++i)
    objects.push_back(new Tracked{i});

  for (auto* object : objects)
    CHECK(reclaimer.retire(object, &tracked_deleter));

  // The fifth is refused and counted: an engine bug to be reported, not a load condition to absorb.
  auto* overflow = new Tracked{99};
  CHECK_FALSE(reclaimer.retire(overflow, &tracked_deleter));
  CHECK(reclaimer.dropped() == 1u);
  delete overflow;

  reclaimer.advance_epoch();
  reclaimer.advance_epoch();
  CHECK(reclaimer.collect() == 4u);
  CHECK(g_freed.load() == 4);
}

TEST_CASE("reclaimer retire and advance_epoch allocate nothing")
{
  if (!namchain::rt_check_available())
  {
    MESSAGE("skipped: allocation interposer not compiled in");
    return;
  }

  g_freed = 0;
  namchain::rt::Reclaimer reclaimer(2, 64);

  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    for (int i = 0; i < 100; ++i)
      reclaimer.advance_epoch();
  }
  CHECK(namchain::rt_allocation_count() == 0);

  // Retiring an object must not allocate either: only the pointer and the deleter are stored.
  Tracked* object = new Tracked{1};
  namchain::rt_reset_counters();
  {
    namchain::RtScope scope;
    CHECK(reclaimer.retire(object, &tracked_deleter));
  }
  CHECK(namchain::rt_allocation_count() == 0);
  CHECK(namchain::rt_deallocation_count() == 0);

  reclaimer.advance_epoch();
  reclaimer.advance_epoch();
  reclaimer.advance_epoch();
  CHECK(reclaimer.collect() == 1u);
  CHECK(g_freed.load() == 1);
}

// ---------------------------------------------------------------------------
// Scheduling: what the thread asked for and what it holds
// ---------------------------------------------------------------------------
TEST_CASE("scheduling state can be read back, and a refusal is reported rather than assumed")
{
  // This is deliberately side-effect free so it can run anywhere, including an unprivileged CI
  // container. The one thing it must never do is let a run claim real-time priority it does not
  // have -- which is why the state is read from the kernel after the request rather than remembered.
  const namchain::rt::SchedulingState before = namchain::rt::current_scheduling();
  CHECK(before.cpu_count >= 1);
  CHECK_FALSE(namchain::rt::describe_scheduling(before).empty());

  // Asking for nothing changes nothing, and says so: neither request counts as applied.
  const namchain::rt::SchedulingState unchanged = namchain::rt::apply_scheduling(0, -1);
  CHECK_FALSE(unchanged.affinity_applied);
  CHECK_FALSE(unchanged.priority_applied);
  CHECK(unchanged.cpu_count >= 1);

  // A CPU index this build cannot name is refused with a reason instead of being masked into a
  // CPU_SET that writes past the end of its mask. No privilege and no side effect are involved.
  const namchain::rt::SchedulingState bogus = namchain::rt::apply_scheduling(0, 100000);
  CHECK_FALSE(bogus.affinity_applied);
  CHECK_FALSE(bogus.detail.empty());

  // SCHED_FIFO needs privilege, so both outcomes are legitimate here (CI is unprivileged, the Pi is
  // not). What is checked is the invariant that the report matches the state: if it says it holds
  // real-time priority then the kernel's own answer must be a real-time policy at that priority.
  const namchain::rt::SchedulingState rt = namchain::rt::apply_scheduling(80, -1);
  CHECK(rt.realtime == (rt.policy == SCHED_FIFO || rt.policy == SCHED_RR));
  if (rt.priority_applied)
    CHECK(rt.policy == SCHED_FIFO);
  else
    CHECK_FALSE(rt.detail.empty());
}

#if defined(__APPLE__)
// ---------------------------------------------------------------------------
TEST_CASE("macOS gets real-time scheduling without privilege, and re-asking does not fail")
{
  // macOS has no `SCHED_FIFO` an unprivileged process can reach by the Linux route, and this project
  // reported "not real-time" for months because of it. `pthread_setschedparam` does work here, and the
  // time-constraint window is what makes a short period survivable, so this test asserts the thing
  // that used to be untrue: the policy is set, and the kernel reads it back.
  //
  // In a thread of its own, because scheduling is per-thread and this is exactly the daemon's
  // situation: a fresh render thread that asks once at start-up and again on every device open. (The
  // test above has already promoted the thread it runs on, and a thread that holds a window cannot
  // have its priority changed again -- see the last block here, which pins that down.)
  const double period = 32.0 / 48000.0; // 0.67 ms, the period this app runs at
  namchain::rt::SchedulingState asked;
  namchain::rt::SchedulingState again;
  namchain::rt::SchedulingState changed;
  namchain::rt::SchedulingState untouched;
  std::thread worker([&] {
    asked = namchain::rt::apply_scheduling(60, -1, period);
    again = namchain::rt::apply_scheduling(60, -1, 512.0 / 48000.0);
    changed = namchain::rt::apply_scheduling(70, -1, period);
    untouched = namchain::rt::apply_scheduling(0, -1);
  });
  worker.join();

  CHECK(asked.realtime);
  CHECK(asked.policy == SCHED_FIFO);
  CHECK(asked.priority == 60);
  CHECK(asked.priority_applied);
  CHECK(asked.time_constraint);
  CHECK(asked.constraint_period_us > 0);
  // The window is the period asked for, not the 10 ms a caller that does not know its period gets.
  CHECK(asked.constraint_period_us <= 1000);
  CHECK(asked.constraint_computation_us < asked.constraint_period_us);

  // Asking again is what the daemon does on every device open, and it must not report a failure: once
  // a window is in force, a further `pthread_setschedparam` answers EINVAL (measured on macOS 27), so
  // the implementation skips the call when the thread already holds what was asked for.
  CHECK(again.priority_applied);
  CHECK(again.detail.find("EINVAL") == std::string::npos);
  CHECK(again.detail.find("Invalid argument") == std::string::npos);
  CHECK(again.constraint_period_us > asked.constraint_period_us); // revised, not refused

  // Changing the priority *after* a window is in force cannot be done at all on this macOS: the call
  // answers EINVAL whatever the new priority is. What matters is that the refusal is reported instead
  // of the run claiming a priority it does not hold -- and the thread stays real-time either way.
  const bool refused_by_name = changed.detail.find("EINVAL") != std::string::npos ||
                               changed.detail.find("Invalid argument") != std::string::npos;
  CHECK_FALSE(changed.priority_applied);
  CHECK(refused_by_name);
  CHECK(changed.realtime);
  CHECK(changed.priority == 60); // the priority it does hold

  // And zero still means "leave the policy alone", which is not the same as asking for SCHED_OTHER.
  CHECK(untouched.realtime); // the policy set above is still in force
  CHECK_FALSE(untouched.priority_applied);
}
#endif
