#include "namchain/rt/scheduling.h"

#if defined(__linux__)

  #include <cerrno>
  #include <cstring>
  #include <sched.h>
  #include <string>

namespace namchain
{
namespace rt
{
namespace
{

std::string errno_text(const std::string& what)
{
  return what + ": " + std::strerror(errno);
}

/// The highest CPU index we are willing to name, so a bad `--cpu` is a reported refusal rather than a
/// CPU_SET that writes past the end of its mask. CPU_SETSIZE is the kernel's own limit.
bool valid_cpu(int cpu)
{
  return cpu >= 0 && cpu < CPU_SETSIZE;
}

} // namespace

std::string SchedulingState::affinity_text() const
{
  if (cpu < 0)
    return "any of " + std::to_string(cpu_count) + " cpus";
  if (cpu_count <= 1)
    return "cpu " + std::to_string(cpu);
  return "cpu " + std::to_string(cpu) + " of " + std::to_string(cpu_count) + " in the mask";
}

SchedulingState current_scheduling()
{
  SchedulingState state;

  state.policy = ::sched_getscheduler(0);
  if (state.policy >= 0)
    state.realtime = state.policy == SCHED_FIFO || state.policy == SCHED_RR;

  struct sched_param param;
  std::memset(&param, 0, sizeof(param));
  if (::sched_getparam(0, &param) == 0)
    state.priority = param.sched_priority;

  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (::sched_getaffinity(0, sizeof(mask), &mask) == 0)
  {
    state.cpu_count = 0;
    int first = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i)
    {
      if (!CPU_ISSET(i, &mask))
        continue;
      ++state.cpu_count;
      if (first < 0)
        first = i;
    }
    state.cpu = state.cpu_count == 1 ? first : -1;
  }

  return state;
}

SchedulingState apply_scheduling(int priority, int cpu, double /*period_seconds*/)
{
  // The period is macOS's business: `SCHED_FIFO` has no deadline in it, so there is nothing here for
  // the parameter to do.
  SchedulingState state = current_scheduling();
  std::string failures;

  if (priority > 0)
  {
    struct sched_param param;
    std::memset(&param, 0, sizeof(param));
    param.sched_priority = priority;
    // SCHED_FIFO with no time slice: the audio thread runs until it blocks, which is the discipline
    // the whole design assumes. An unprivileged caller gets EPERM here, and that is reported.
    if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0)
      failures = errno_text("sched_setscheduler(SCHED_FIFO priority " + std::to_string(priority) + ")");
  }

  if (cpu >= 0)
  {
    if (!valid_cpu(cpu))
    {
      failures += (failures.empty() ? "" : "; ") + std::string("requested cpu ") + std::to_string(cpu) +
                  " is outside the range this build can name";
    }
    else
    {
      cpu_set_t mask;
      CPU_ZERO(&mask);
      CPU_SET(cpu, &mask);
      if (::sched_setaffinity(0, sizeof(mask), &mask) != 0)
        failures += (failures.empty() ? "" : "; ") + errno_text("sched_setaffinity(cpu " + std::to_string(cpu) + ")");
    }
  }

  // Read back rather than trust the calls: a successful ioctl is not the same claim as the state the
  // thread is actually in, and the report has to make the second one.
  const SchedulingState achieved = current_scheduling();
  SchedulingState result = achieved;
  result.detail = failures;
  result.priority_applied = priority <= 0 ? false : (achieved.realtime && achieved.priority == priority);
  result.affinity_applied = cpu < 0 ? false : achieved.cpu == cpu;
  if (cpu < 0)
    result.cpu = -1; // leave the "may run anywhere" reading intact when we did not ask
  return result;
}

std::string describe_scheduling(const SchedulingState& state)
{
  std::string text;
  switch (state.policy)
  {
  case SCHED_FIFO: text = "SCHED_FIFO"; break;
  case SCHED_RR: text = "SCHED_RR"; break;
  case SCHED_OTHER: text = "SCHED_OTHER"; break;
  case SCHED_BATCH: text = "SCHED_BATCH"; break;
  case SCHED_IDLE: text = "SCHED_IDLE"; break;
  default: text = "policy " + std::to_string(state.policy); break;
  }
  text += " priority " + std::to_string(state.priority);
  text += ", " + state.affinity_text();
  if (!state.detail.empty())
    text += " (not as requested: " + state.detail + ")";
  return text;
}

} // namespace rt
} // namespace namchain

#elif defined(__APPLE__)

  #include <mach/mach.h>
  #include <mach/mach_time.h>
  #include <mach/thread_act.h>
  #include <mach/thread_policy.h>
  #include <pthread.h>
  #include <sched.h>

  #include <cstring>
  #include <thread>

// macOS has no `SCHED_FIFO` an unprivileged process can set -- `sched_setscheduler` is not the way,
// and the polite fiction that it is has cost this project before. What the platform does have, and
// what CoreAudio's own IO threads use, is the **time-constraint policy**: the thread tells the kernel
// "I have this much work to do every this often", and the kernel schedules it to meet that. It needs
// no privilege, it is read back with `thread_policy_get`, and it is the difference between a 32-frame
// period being comfortable and being a stream of xruns.
namespace namchain
{
namespace rt
{
namespace
{

/// \brief Mach absolute-time ticks per nanosecond.
///
/// The SDK's rule is the other way round from the way it reads: `mach_timebase_info` gives
/// `numer/denom` such that **nanoseconds = ticks * numer / denom**, so the number of ticks in a
/// nanosecond is `denom/numer`. On Apple Silicon that is 3/125 = 0.024 (one tick is 41.67 ns, because
/// the counter runs at 24 MHz); multiplying by `numer/denom` instead -- which is what this function
/// did first -- produced a tick count 1736 times too large, and the kernel answered with
/// `KERN_INVALID_ARGUMENT` rather than accepting a period that overflowed its `uint32_t`.
double ticks_per_nanosecond()
{
  static const double ticks = [] {
    mach_timebase_info_data_t info{};
    if (mach_timebase_info(&info) != KERN_SUCCESS || info.numer == 0)
      return 1.0;
    return static_cast<double>(info.denom) / static_cast<double>(info.numer);
  }();
  return ticks;
}

unsigned microseconds_from_ticks(uint32_t ticks)
{
  return static_cast<unsigned>(static_cast<double>(ticks) / ticks_per_nanosecond() / 1000.0 + 0.5);
}

std::string mach_text(kern_return_t status)
{
  const char* text = mach_error_string(status);
  return (text != nullptr ? std::string(text) : std::string("kern_return_t " + std::to_string(status)));
}

} // namespace

std::string SchedulingState::affinity_text() const
{
  // No affinity API: every CPU is one this thread may run on. Saying "any of N" is the same claim the
  // Linux path makes from an affinity mask that allows everything.
  return "any of " + std::to_string(cpu_count) + " cpus";
}

SchedulingState current_scheduling()
{
  SchedulingState state;
  const unsigned reported = std::thread::hardware_concurrency();
  state.cpu_count = reported == 0 ? 1 : reported;

  // The scheduling policy, which on Darwin `pthread_getschedparam` *does* report: SCHED_OTHER is 1,
  // SCHED_FIFO 4, SCHED_RR 2. This is the verifiable half of macOS real-time, and it is the half the
  // Linux path's fields already mean, so they are filled the same way.
  int policy = 0;
  struct sched_param param;
  std::memset(&param, 0, sizeof(param));
  if (::pthread_getschedparam(::pthread_self(), &policy, &param) == 0)
  {
    state.policy = policy;
    state.realtime = policy == SCHED_FIFO || policy == SCHED_RR;
    // Darwin gives an ordinary thread priority 31, which is not a real-time priority in anybody's
    // sense and has no counterpart in the Linux reading (where SCHED_OTHER reports 0). Reporting it
    // would make a daemon that asked for nothing look as if it had been given something.
    state.priority = state.realtime ? param.sched_priority : 0;
  }

  // ...and the time-constraint window, which macOS 27 accepts and then declines to report: a set
  // followed immediately by a get reads back as the default, for this policy and for the extended one
  // (measured 2026-10-02, Apple Silicon). So the *window numbers* are what a caller asked for, and
  // `detail` says so rather than presenting them as a reading.
  thread_time_constraint_policy_data_t window{};
  mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
  boolean_t is_default = TRUE;
  const kern_return_t status = thread_policy_get(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                                                reinterpret_cast<thread_policy_t>(&window), &count, &is_default);
  if (status == KERN_SUCCESS && is_default == FALSE)
  {
    state.time_constraint = true;
    state.constraint_period_us = microseconds_from_ticks(window.period);
    state.constraint_computation_us = microseconds_from_ticks(window.computation);
  }
  return state;
}

SchedulingState apply_scheduling(int priority, int cpu, double period_seconds)
{
  std::string failures;
  bool window_accepted = false;
  unsigned window_period_us = 0;
  unsigned window_computation_us = 0;
  const SchedulingState before = current_scheduling();

  if (priority > 0)
  {
    // The priority. `pthread_setschedparam(SCHED_FIFO)` works for an unprivileged macOS process
    // (measured: rc 0, and `pthread_getschedparam` reads the priority back), which is what makes the
    // report below a reading rather than a hope. Linux needs root for the same thing, which is the
    // asymmetry this project has been reporting for months.
    //
    // **Skipped when the thread already is what was asked for**, and that is not an optimisation: once
    // a time-constraint window is in force, a further `pthread_setschedparam` fails with `EINVAL` on
    // this macOS (measured 2026-10-02, Apple Silicon). The daemon re-applies scheduling once per
    // device open, so without this the second call would report a failure for a thread that is already
    // scheduled exactly as asked.
    if (before.realtime && before.priority == priority)
    {
      // Already SCHED_FIFO at this priority; only the window below can need updating.
    }
    else
    {
      struct sched_param param;
      std::memset(&param, 0, sizeof(param));
      param.sched_priority = priority;
      const int rc = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param);
      if (rc != 0)
        failures = std::string("pthread_setschedparam(SCHED_FIFO, ") + std::to_string(priority) + "): " +
                   std::strerror(rc);
    }

    // The window: how much work this thread has to do, and how often. A caller that does not know its
    // period gets 10 ms, which is a conservative audio period and still an enormous improvement on
    // being scheduled as ordinary work; the computation is three quarters of it, so a thread that
    // overruns its block is throttled until the next period rather than running away with the machine.
    const double period = period_seconds > 0.0 ? period_seconds : 0.010;
    thread_time_constraint_policy_data_t policy{};
    policy.period = static_cast<uint32_t>(period * 1e9 * ticks_per_nanosecond());
    policy.computation = static_cast<uint32_t>(policy.period * 0.75);
    policy.constraint = policy.period;
    // Preemptible: the window is a promise about *when* the thread runs, not a licence to hold a core
    // for its computation. On a laptop somebody else is using, an engine that cannot be preempted is a
    // worse failure than one that can -- and the output cushion is what absorbs the preemption.
    policy.preemptible = 1;
    const kern_return_t status = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                                                  reinterpret_cast<thread_policy_t>(&policy),
                                                  THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (status != KERN_SUCCESS)
      failures += (failures.empty() ? "" : "; ") +
                  ("thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY): " + mach_text(status));
    else
    {
      window_accepted = true;
      window_period_us = microseconds_from_ticks(policy.period);
      window_computation_us = microseconds_from_ticks(policy.computation);
    }
  }

  if (cpu >= 0)
    failures += (failures.empty() ? "" : "; ") +
                std::string("macOS has no per-thread cpu affinity, so the requested confinement was not "
                            "applied");

  // Read back rather than trust the call, the same rule the Linux path follows: what the thread is
  // actually scheduled as is the claim a report has to make.
  SchedulingState result = current_scheduling();
  if (window_accepted)
  {
    // The kernel took the window and will not tell us it holds it, so the numbers are what was asked
    // for and the sentence says exactly that much.
    result.time_constraint = true;
    result.constraint_period_us = window_period_us;
    result.constraint_computation_us = window_computation_us;
  }
  result.detail = failures;
  if (failures.empty() && window_accepted)
    result.detail = "time-constraint window accepted; macOS does not report this policy back, so the "
                    "numbers are what was asked for";
  else if (failures.empty())
    result.detail = "no real-time scheduling was requested";
  result.priority_applied = priority > 0 && result.realtime && result.priority == priority;
  result.affinity_applied = false;
  if (cpu < 0)
    result.cpu = -1;
  return result;
}

std::string describe_scheduling(const SchedulingState& state)
{
  std::string text;
  switch (state.policy)
  {
  case SCHED_FIFO: text = "SCHED_FIFO"; break;
  case SCHED_RR: text = "SCHED_RR"; break;
  case SCHED_OTHER: text = "SCHED_OTHER"; break;
  default: text = "policy " + std::to_string(state.policy); break;
  }
  text += " priority " + std::to_string(state.priority);
  if (state.time_constraint)
    text += ", time-constraint window " + std::to_string(state.constraint_period_us) + " us period / " +
            std::to_string(state.constraint_computation_us) + " us computation";
  text += ", " + state.affinity_text();
  if (!state.detail.empty())
    text += " (" + state.detail + ")";
  return text;
}

} // namespace rt
} // namespace namchain

#else // not Linux and not macOS

  #include <thread>

// A fallback that links and tells the truth: there is no scheduling state to read or set here, so a
// report from such a build says so rather than claiming a real-time thread it does not have.
namespace namchain
{
namespace rt
{

std::string SchedulingState::affinity_text() const
{
  return "unspecified";
}

SchedulingState current_scheduling()
{
  SchedulingState state;
  // The policy is not readable here, but the CPU count is, and it is a different claim. Leaving it
  // zero would report a machine with no processors -- a lie where "I cannot tell you the policy" is
  // the truth. There is no affinity API to narrow it, so every CPU is one this thread may run on,
  // which is exactly what the Linux path derives from the affinity mask.
  const unsigned reported = std::thread::hardware_concurrency();
  state.cpu_count = reported == 0 ? 1 : static_cast<unsigned>(reported);
  state.detail = "scheduling introspection is only implemented on Linux and macOS";
  return state;
}

SchedulingState apply_scheduling(int, int, double)
{
  return current_scheduling();
}

std::string describe_scheduling(const SchedulingState& state)
{
  return "unspecified (" + state.detail + ")";
}

} // namespace rt
} // namespace namchain

#endif // __linux__ / __APPLE__ / other
