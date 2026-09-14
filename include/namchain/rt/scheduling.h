#pragma once

// The thread-level half of real-time discipline: what the process asked the kernel for, and what it
// actually got.
//
// This is here rather than in the soak because it is the engine's job, not the harness's. Until
// 2026-09-14 the soak was run under an externally applied `chrt -f 80 taskset -c 3`, which means the
// measurement depended on a command line no user of this library would type, and plan.md §9.3 -- which
// has listed `SCHED_FIFO` via `sched_setscheduler` and a pinned audio thread since the beginning --
// described something no code did. The gap was measurable: pinning alone took the tail from 47 to 25
// missed deadlines per 90 s, in the same configuration, with nothing else changed (§9.5 item 6).
//
// The interface follows the same rule as `mlockall` in the soak: apply what was asked for, then *read
// back* what was actually achieved and report it. A run without RT priority is a different claim from
// one with it, and the report has to be able to say which it was rather than assume.
#include <string>

namespace namchain
{
namespace rt
{

/// What the calling thread's scheduling currently is, read from the kernel rather than remembered.
struct SchedulingState
{
  int policy = 0;              ///< SCHED_OTHER, SCHED_FIFO or SCHED_RR.
  int priority = 0;            ///< The priority within that policy; meaningless for SCHED_OTHER.
  int cpu = -1;                ///< The one CPU the thread is confined to, or -1 if it may use any.
  unsigned cpu_count = 0;      ///< CPUs in the affinity mask; > 1 means it is not confined.
  bool realtime = false;       ///< True when the policy is SCHED_FIFO or SCHED_RR.
  bool priority_applied = false; ///< False when the requested priority was refused (usually EPERM).
  bool affinity_applied = false; ///< False when the requested CPU was refused.
  std::string detail;          ///< Why either failed, empty when both did what was asked.

  /// The CPUs, in ascending order, when a caller wants to show a mask rather than a single choice.
  std::string affinity_text() const;
};

/// \brief Ask for SCHED_FIFO at \p priority and confinement to \p cpu, then report what resulted.
///
/// \param priority Requested SCHED_FIFO priority; 0 means "leave the policy alone".
/// \param cpu Requested CPU, or -1 for "leave the affinity alone".
/// \return the state *after* the attempt, with `detail` saying what was refused and why.
///
/// Never throws and never aborts: refusing to be real-time is not a reason to fail a run, it is a
/// reason for the run to say it was not real-time. Unprivileged callers get EPERM on the priority and
/// a successful affinity change, which is exactly the case a unit test can exercise in CI.
SchedulingState apply_scheduling(int priority, int cpu);

/// Read the current policy, priority and affinity without changing any of them.
SchedulingState current_scheduling();

/// One line, for a report: "SCHED_FIFO priority 80 confined to cpu 3", or as close as was achieved.
std::string describe_scheduling(const SchedulingState& state);

} // namespace rt
} // namespace namchain
