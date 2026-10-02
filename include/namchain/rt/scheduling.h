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

  /// \brief macOS: the thread carries a `THREAD_TIME_CONSTRAINT_POLICY`.
  ///
  /// There is no `SCHED_FIFO` an unprivileged macOS process can set, and this policy is what the
  /// platform offers instead: the kernel schedules the thread inside a period/computation window it
  /// promises to meet. It is the same facility CoreAudio's own IO threads use, and it is what makes a
  /// 32-frame period (0.67 ms) survivable. False everywhere else, and false on macOS until a caller
  /// asks for it with a priority above zero.
  bool time_constraint = false;
  /// macOS only: the window that policy is set to, in microseconds. 0 when it is not in force.
  unsigned constraint_period_us = 0;
  unsigned constraint_computation_us = 0;

  /// The CPUs, in ascending order, when a caller wants to show a mask rather than a single choice.
  std::string affinity_text() const;
};

/// \brief Ask for real-time scheduling, and confinement to \p cpu, then report what resulted.
///
/// \param priority Requested priority; 0 means "leave the scheduling policy alone". On Linux this is
///        a `SCHED_FIFO` priority. On macOS there are no priorities to name: any value above zero asks
///        for the time-constraint policy, and the number itself is not used.
/// \param cpu Requested CPU, or -1 for "leave the affinity alone". macOS has no per-thread affinity
///        and reports that as a refusal rather than pretending otherwise.
/// \param period_seconds The audio period this thread has to finish inside, or 0 when the caller does
///        not know it yet. Linux's `SCHED_FIFO` has no such parameter and ignores it; macOS's
///        time-constraint policy is *made* of it, so a caller that knows the period should say so — a
///        window sized for a 1024-frame period gives a thread working to a 32-frame deadline a great
///        deal more rope than it should have.
/// \return the state *after* the attempt, with `detail` saying what was refused and why.
///
/// Never throws and never aborts: refusing to be real-time is not a reason to fail a run, it is a
/// reason for the run to say it was not real-time. Unprivileged callers get EPERM on the Linux
/// priority and a successful affinity change, which is exactly the case a unit test can exercise in
/// CI; on macOS the time-constraint policy needs no privilege at all, so the same test gets it.
SchedulingState apply_scheduling(int priority, int cpu, double period_seconds = 0.0);

/// Read the current policy, priority and affinity without changing any of them.
SchedulingState current_scheduling();

/// One line, for a report: "SCHED_FIFO priority 80 confined to cpu 3", or as close as was achieved.
std::string describe_scheduling(const SchedulingState& state);

} // namespace rt
} // namespace namchain
