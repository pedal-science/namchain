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

SchedulingState apply_scheduling(int priority, int cpu)
{
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

#else // not Linux

  #include <thread>

// A fallback that links and tells the truth: there is no scheduling state to read or set here, so a
// report from a non-Linux build says so rather than claiming a real-time thread it does not have.
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
  state.cpu_count = reported == 0 ? 1 : static_cast<int>(reported);
  state.detail = "scheduling introspection is only implemented on Linux";
  return state;
}

SchedulingState apply_scheduling(int, int)
{
  return current_scheduling();
}

std::string describe_scheduling(const SchedulingState& state)
{
  return "unspecified (" + state.detail + ")";
}

} // namespace rt
} // namespace namchain

#endif // __linux__
