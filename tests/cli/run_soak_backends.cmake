# The soak harness's command line and its backend layer, checked where they can actually break.
#
#   cmake -DNAMCHAIN_SOAK=... -DNAMCHAIN_RENDER=... -DNAMCHAIN_WAVCMP=... -DNAMCHAIN_MODEL=... \
#         -DNAMCHAIN_INPUT=... -DNAMCHAIN_WORK=... -P tests/cli/run_soak_backends.cmake
#
# Three things are checked, each of which has been wrong at least once:
#
#   1. The `file` backend produces exactly what the offline renderer produces. It is the same
#      engine driven from a different place, so anything but bit-identity is a bug -- and it was
#      one: the backend read one sample per period and copied it across the whole period, which
#      produced a plausible-looking waveform that was wrong everywhere.
#   2. `--model FILE in.wav` and `FILE in.wav` mean the same thing. They did not: the flag set one
#      variable and the positional parser read another, so the flag form was rejected with a usage
#      message while the positional form worked.
#   3. A backend that does not exist fails by name rather than falling back to one that does.
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

# --- the null backend runs the stream path with no device ---------------------------------------
_run_and_expect_success("null backend soak" 0
  "${NAMCHAIN_SOAK}" --backend null --no-pacing --seconds 1 --quantum 64 --model "${NAMCHAIN_MODEL}"
  --self-oracle --oracle-window 1)

# --- 3. a backend that does not exist is refused -----------------------------------------------
_run_and_expect_success("pipewire backend is refused" 2
  "${NAMCHAIN_SOAK}" --backend pipewire --no-pacing --seconds 1 "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}")

message(STATUS "soak CLI and backend checks passed")
