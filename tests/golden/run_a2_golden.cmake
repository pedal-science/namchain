# Golden test: our offline renderer vs the vendored core's own `render` tool.
#
#   cmake -DNAMCHAIN_RENDER=... -DNAMCHAIN_WAVCMP=... -DNAMCHAIN_CORE_RENDER=... \
#         -DNAMCHAIN_MODEL=... -DNAMCHAIN_INPUT=... -DNAMCHAIN_WORK=... \
#         -DNAMCHAIN_TOLERANCE=... -P tests/golden/run_a2_golden.cmake
#
# Both tools are driven over the same model and the same input WAV, then compared sample by
# sample. The comparison is tolerance-based, NOT bit-exact, and that is deliberate: the core
# compiles its own tools with -Ofast in Release (tools/CMakeLists.txt), while our build is -O3
# with no -ffast-math, and bit-identity across those two flag sets is not a property either
# implementation promises (plan.md §4.2). The tolerance is set by the caller from a measured
# value.
#
# A limitation worth stating: the core's `render` tool is a *mono* pipeline, so this compares
# the mono path only.

foreach(_required NAMCHAIN_RENDER NAMCHAIN_WAVCMP NAMCHAIN_CORE_RENDER NAMCHAIN_MODEL NAMCHAIN_INPUT
                  NAMCHAIN_WORK NAMCHAIN_TOLERANCE)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "run_a2_golden.cmake: ${_required} is required")
  endif()
endforeach()

if(NOT EXISTS "${NAMCHAIN_CORE_RENDER}")
  message(FATAL_ERROR "core render tool not found at ${NAMCHAIN_CORE_RENDER}")
endif()
if(NOT EXISTS "${NAMCHAIN_MODEL}")
  message(FATAL_ERROR "model not found at ${NAMCHAIN_MODEL}")
endif()
if(NOT EXISTS "${NAMCHAIN_INPUT}")
  message(FATAL_ERROR "input WAV not found at ${NAMCHAIN_INPUT}")
endif()

file(MAKE_DIRECTORY "${NAMCHAIN_WORK}")
set(_ours "${NAMCHAIN_WORK}/ours.wav")
set(_ref "${NAMCHAIN_WORK}/ref.wav")

execute_process(
  COMMAND "${NAMCHAIN_RENDER}" --quantum 64 --quiet "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}" "${_ours}"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "namchain-render failed (${_rc}):\n${_out}\n${_err}")
endif()

execute_process(
  COMMAND "${NAMCHAIN_CORE_RENDER}" "${NAMCHAIN_MODEL}" "${NAMCHAIN_INPUT}" "${_ref}"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "the core's render tool failed (${_rc}):\n${_out}\n${_err}")
endif()

execute_process(
  COMMAND "${NAMCHAIN_WAVCMP}" --max-abs "${NAMCHAIN_TOLERANCE}" "${_ref}" "${_ours}"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message(STATUS "golden comparison vs core render:\n${_out}${_err}")
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR
    "golden comparison FAILED against the core's render output (tolerance ${NAMCHAIN_TOLERANCE})")
endif()

message(STATUS "golden comparison PASSED (tolerance ${NAMCHAIN_TOLERANCE})")
