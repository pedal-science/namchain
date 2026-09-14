# The soak harness's command line and its backend layer, checked where they can actually break.
#
#   cmake -DNAMCHAIN_SOAK=... -DNAMCHAIN_RENDER=... -DNAMCHAIN_WAVCMP=... -DNAMCHAIN_MODEL=... \
#         -DNAMCHAIN_INPUT=... -DNAMCHAIN_WORK=... -P tests/cli/run_soak_backends.cmake
#
# Four things are checked, each of which has been wrong at least once:
#
#   1. The `file` backend produces exactly what the offline renderer produces. It is the same
#      engine driven from a different place, so anything but bit-identity is a bug -- and it was
#      one: the backend read one sample per period and copied it across the whole period, which
#      produced a plausible-looking waveform that was wrong everywhere.
#   2. `--model FILE in.wav` and `FILE in.wav` mean the same thing. They did not: the flag set one
#      variable and the positional parser read another, so the flag form was rejected with a usage
#      message while the positional form worked.
#   3. A backend that does not exist fails by name rather than falling back to one that does.
#   4. A tight `RLIMIT_MEMLOCK` does not kill the run. See the comment on that case: this is the
#      one failure that could not reproduce on a developer machine.
#
# The `null` backend is exercised too, with pacing off: it is the path CI can run without hardware,
# and in this test it is the functional half (the loop, the oracle, the report) that is checked --
# its pacing is covered deterministically by tests/test_backend.cpp.

foreach(_required NAMCHAIN_SOAK NAMCHAIN_RENDER NAMCHAIN_WAVCMP NAMCHAIN_MODEL NAMCHAIN_INPUT
                  NAMCHAIN_WORK)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "run_soak_backends.cmake: ${_required} is required")
  endif()
endforeach()

file(MAKE_DIRECTORY "${NAMCHAIN_WORK}")

function(_run_and_expect_success _label _expect)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL ${_expect})
    message(FATAL_ERROR "${_label}: expected exit ${_expect}, got ${_rc}\n--- stdout ---\n${_out}\n--- stderr ---\n${_err}")
  endif()
endfunction()

# --- 1. the file backend is the offline renderer ------------------------------------------------
set(_backend_wav "${NAMCHAIN_WORK}/file-backend.wav")
set(_render_wav "${NAMCHAIN_WORK}/renderer.wav")

_run_and_expect_success("file backend soak" 0
  "${NAMCHAIN_SOAK}" --backend file --model "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}"
  --output "${_backend_wav}" --seconds 2 --quantum 64)

_run_and_expect_success("offline renderer" 0
  "${NAMCHAIN_RENDER}" --quantum 64 --quiet "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}" "${_render_wav}")

execute_process(
  COMMAND "${NAMCHAIN_WAVCMP}" "${_render_wav}" "${_backend_wav}"
  RESULT_VARIABLE _cmp_rc
  OUTPUT_VARIABLE _cmp_out
  ERROR_VARIABLE _cmp_err)
if(NOT _cmp_rc EQUAL 0)
  message(FATAL_ERROR "the file backend and the renderer disagree (exit ${_cmp_rc}):\n${_cmp_out}${_cmp_err}")
endif()

# --- 2. the flag form and the positional form are the same thing --------------------------------
_run_and_expect_success("offline soak, --model flag" 0
  "${NAMCHAIN_SOAK}" --no-pacing --seconds 1 --quantum 64 --model "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}")

_run_and_expect_success("offline soak, positional model" 0
  "${NAMCHAIN_SOAK}" --no-pacing --seconds 1 --quantum 64 "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}")

# --- 3. the null backend runs the stream path with no device ------------------------------------
_run_and_expect_success("null backend soak" 0
  "${NAMCHAIN_SOAK}" --backend null --no-pacing --seconds 1 --quantum 64 --model "${NAMCHAIN_MODEL}"
  --self-oracle --oracle-window 1)

# --- 4. a modest memlock limit must not poison the run ------------------------------------------
# `mlockall(MCL_CURRENT|MCL_FUTURE)` charges every mapping made after it against `RLIMIT_MEMLOCK`, so
# a limit that was enough for the pages already mapped can still be too small for the next large
# allocation -- and the failure is `std::bad_alloc` inside a process that has just been told its
# memory is locked. GitHub's runner hit exactly that on the `null` backend, with no device and no
# audio involved: `mlockall` succeeded at ~6 MB of RSS and the next allocation, a 44 MB block-timer
# reserve, came back `EAGAIN`. This development machine allows ~1 GB of memlock, so it never saw it,
# which is why CI was the first place the bug appeared.
#
# The runner's own limit has not been read; what is reproduced here is the *band* that produces the
# failure, which is any limit between the process's RSS and its next big allocation. 8 MiB is inside
# that band on both machines, and it aborted the soak before the fix.
#
# The assertion is "the run survives", not "the memory ended up locked": at a limit this tight
# `mlockall` may legitimately fail, and the harness reports that as a caveat rather than an error.
# What must never happen is the process aborting.
if(UNIX)
  set(_memlock_cmd "ulimit -l 8192 2>/dev/null; exec \"$1\" --backend null --no-pacing --seconds 1 --quantum 64 --model \"$2\" --self-oracle --oracle-window 1")
  execute_process(
    COMMAND bash -c "${_memlock_cmd}" _bash "${NAMCHAIN_SOAK}" "${NAMCHAIN_MODEL}"
    RESULT_VARIABLE _memlock_rc
    OUTPUT_VARIABLE _memlock_out
    ERROR_VARIABLE _memlock_err)
  if(NOT _memlock_rc EQUAL 0)
    message(FATAL_ERROR "null backend soak under an 8 MiB memlock limit: expected exit 0, got ${_memlock_rc}\n--- stdout ---\n${_memlock_out}\n--- stderr ---\n${_memlock_err}")
  endif()
endif()

# --- 5. a backend that does not exist is refused ------------------------------------------------
_run_and_expect_success("pipewire backend is refused" 2
  "${NAMCHAIN_SOAK}" --backend pipewire --no-pacing --seconds 1 "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}")

# --- 6. the program applies its own scheduling, and says what it got ----------------------------
# Until 2026-09-14 nothing in this project set SCHED_FIFO or thread affinity: the soak was run under
# an externally applied `chrt`/`taskset`, so the measurement depended on a command line no user would
# type, and plan.md 9.3 described code that did not exist. Pinning alone was worth half the
# deadline-miss tail, so the engine does it itself now and reports the state it actually holds.
#
# The assertion is about the *report*, not about privilege: CI runs unprivileged, where the priority
# request is refused, and this machine is not. Both are legitimate. What is not legitimate is a run
# that claims a real-time thread it does not have. `--cpu 0` works either way, because affinity needs
# no privilege.
execute_process(
  COMMAND "${NAMCHAIN_SOAK}" --backend null --no-pacing --seconds 1 --cpu 0 --model "${NAMCHAIN_MODEL}"
  RESULT_VARIABLE _sched_rc
  OUTPUT_VARIABLE _sched_out
  ERROR_VARIABLE _sched_err)
if(NOT _sched_rc EQUAL 0)
  message(FATAL_ERROR "scheduling run: expected exit 0, got ${_sched_rc}\n--- stdout ---\n${_sched_out}\n--- stderr ---\n${_sched_err}")
endif()
if(NOT _sched_out MATCHES "scheduling:[^\n]*cpu 0")
  message(FATAL_ERROR "the report did not say the thread was confined to cpu 0:\n--- stdout ---\n${_sched_out}")
endif()

# --- 7. the output-queue target is applied or refused, never silently ignored --------------------
# The target is the latency/dropout trade (plan.md 9.5 item 7): it buys tolerance for the loop being
# late by holding that much processed audio in the output ring. Only the ALSA backend has such a ring,
# so asking any other backend for one is refused by name rather than accepted -- a run that reports a
# latency it is not carrying is worse than one that will not start. Zero is not "a target of zero", it
# is "no target", and it has to keep working, because that is what every earlier measurement used.
execute_process(
  COMMAND "${NAMCHAIN_SOAK}" --backend null --no-pacing --seconds 1 --periods 4 --output-queue-periods 2
          --model "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}"
  RESULT_VARIABLE _target_rc
  OUTPUT_VARIABLE _target_out
  ERROR_VARIABLE _target_err)
if(NOT _target_rc EQUAL 2)
  message(FATAL_ERROR "a queue target on a backend with no ring: expected exit 2, got ${_target_rc}\n--- stdout ---\n${_target_out}\n--- stderr ---\n${_target_err}")
endif()
if(NOT _target_err MATCHES "output ring")
  message(FATAL_ERROR "the refusal did not name the missing output ring:\n--- stderr ---\n${_target_err}")
endif()

_run_and_expect_success("zero means no queue target, not a target of zero" 0
  "${NAMCHAIN_SOAK}" --backend null --no-pacing --seconds 1 --periods 4 --output-queue-periods 0
  --model "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}")

message(STATUS "soak CLI and backend checks passed")
