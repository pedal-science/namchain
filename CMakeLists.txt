cmake_minimum_required(VERSION 3.22)

project(namchain VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  message(STATUS "No build type specified; defaulting to Release")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NAMCHAIN_BUILD_TOOLS "Build namchain CLI tools" ON)
option(NAMCHAIN_BUILD_TESTS "Build unit tests" ON)
# NOTE: the vendored core has no CMake option for float samples; NAM_SAMPLE_FLOAT is only
# an #ifdef in NAM/dsp.h. Passing -DNAM_SAMPLE_FLOAT=ON to the core is silently ignored
# (CMake warns "Manually-specified variables were not used"). It must be a compile
# definition, which is what this option does.
option(NAMCHAIN_SAMPLE_FLOAT "Compile the NAM core with float samples (defines NAM_SAMPLE_FLOAT)" OFF)
option(NAMCHAIN_A2_FAST "Enable the NAM A2 fast path (A2-Full/A2-Lite)" ON)
option(NAMCHAIN_APPLY_VENDOR_PATCHES "Apply vendor/patches/*.patch to the vendored NAM core" ON)
option(NAMCHAIN_REQUIRE_A2_PARITY "Fail instead of skip the A2 planar bit-identity test when the build is unoptimised" OFF)
option(NAMCHAIN_RTSAN "Build with RealtimeSanitizer (needs clang >= 20)" OFF)
# Render-path allocation interposer (§10.4). Works on GCC/aarch64 where RTSan cannot, but it
# replaces global operator new/delete, so a production daemon builds with this OFF.
option(NAMCHAIN_RT_CHECK "Compile the render-path allocation interposer" ON)
# ThreadSanitizer, for the SPSC ring and reclaimer concurrency tests (§10.1). Not for the audio
# path: it is far too slow to run in real time, so it is a correctness instrument only.
option(NAMCHAIN_TSAN "Build with ThreadSanitizer" OFF)
option(NAMCHAIN_WERROR "Treat warnings as errors" OFF)

set(NAMCHAIN_NAM_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/NeuralAmpModelerCore"
    CACHE PATH "Path to the vendored NeuralAmpModelerCore checkout")

if(NOT EXISTS "${NAMCHAIN_NAM_CORE_DIR}/NAM/dsp.h")
  message(FATAL_ERROR
    "Vendored NAM core not found at ${NAMCHAIN_NAM_CORE_DIR}.\n"
    "Run: git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# Vendored-core patches (plan.md §4.2, §7.8)
#
# Patches live in vendor/patches/ and are applied to the pinned submodule at configure time.
# Each must stay upstream-PR-shaped and is deleted once its PR merges. The apply is
# idempotent: `git apply --reverse --check` succeeds only if the patch is already in place.
#
# This MUST run before the source glob below, because patches add source files (e.g.
# NAM/wavenet/a2_planar.cpp) that the glob has to pick up.
# ---------------------------------------------------------------------------
if(NAMCHAIN_APPLY_VENDOR_PATCHES)
  file(GLOB NAMCHAIN_VENDOR_PATCHES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/patches/*.patch")
  find_package(Git REQUIRED)
  foreach(_patch IN LISTS NAMCHAIN_VENDOR_PATCHES)
    get_filename_component(_patch_name "${_patch}" NAME)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
      WORKING_DIRECTORY "${NAMCHAIN_NAM_CORE_DIR}"
      RESULT_VARIABLE _patch_applied
      OUTPUT_QUIET ERROR_QUIET)
    if(_patch_applied EQUAL 0)
      message(STATUS "vendor patch already applied: ${_patch_name}")
    else()
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
        WORKING_DIRECTORY "${NAMCHAIN_NAM_CORE_DIR}"
        RESULT_VARIABLE _patch_rc
        OUTPUT_VARIABLE _patch_out
        ERROR_VARIABLE _patch_err)
      if(NOT _patch_rc EQUAL 0)
        message(FATAL_ERROR
          "Failed to apply ${_patch_name} to ${NAMCHAIN_NAM_CORE_DIR}:\n${_patch_err}\n"
          "The patch is pinned to the submodule commit in .gitmodules; if the submodule moved, "
          "rebase the patch rather than forcing it.")
      endif()
      message(STATUS "applied vendor patch: ${_patch_name}")
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Vendored NAM core + libnamchain (M0 subset: lookback calculator)
#
# The core registers architectures through file-scope static initializers
# (`static ConfigParserHelper _register_WaveNet(...)`). Nothing references those object
# files, so any intermediate STATIC archive drops them and the loader dies at runtime
# with "No config parser registered for architecture: WaveNet" -- this happens both if
# the core is a static library and if it is folded into one. The core is therefore an
# OBJECT library whose objects are linked straight into each executable, which is also
# what upstream does (it compiles NAM_SOURCES into every tool).
# ---------------------------------------------------------------------------
file(GLOB NAMC_CORE_SOURCES
  "${NAMCHAIN_NAM_CORE_DIR}/NAM/*.cpp"
  "${NAMCHAIN_NAM_CORE_DIR}/NAM/*/*.cpp")
# Third-party code: never let its warnings fail our build.
set_source_files_properties(${NAMC_CORE_SOURCES} PROPERTIES COMPILE_FLAGS "-w")

# AudioDSPTools' WAV reader. Using the same loader as the core's own `render` tool means the
# golden comparison compares DSP results rather than two different WAV decoders.
set(NAMCHAIN_AUDIODSPTOOLS_DIR "${NAMCHAIN_NAM_CORE_DIR}/Dependencies/AudioDSPTools")
set(NAMCHAIN_AUDIODSPTOOLS_WAV "${NAMCHAIN_AUDIODSPTOOLS_DIR}/dsp/wav.cpp")
set_source_files_properties(${NAMCHAIN_AUDIODSPTOOLS_WAV} PROPERTIES COMPILE_FLAGS "-w")

# Our own code is held to -Wall -Wextra (and -Werror where CI asks for it). Set before the
# library target so that libnamchain's own sources are covered too, not just the tools and
# tests -- third-party sources opt out individually via COMPILE_FLAGS "-w" above.
if(NAMCHAIN_WERROR)
  set(NAMCHAIN_WARN_FLAGS -Wall -Wextra -Werror)
else()
  set(NAMCHAIN_WARN_FLAGS -Wall -Wextra)
endif()

add_library(namchain OBJECT
  src/nam/lookback.cpp
  src/io/wav.cpp
  src/io/alsa_params.cpp
  src/io/pcm.cpp
  src/io/backend.cpp
  src/io/alsa_raw.cpp
  src/offline/render.cpp
  src/rt/telemetry.cpp
  src/rt/rt_check.cpp
  src/rt/arena.cpp
  src/rt/reclaimer.cpp
  src/graph/plan.cpp
  src/graph/reference.cpp
  src/graph/rig.cpp
  src/latency/latency.cpp
  src/glitch/glitch.cpp
  src/nodes/builtin.cpp
  src/soak/soak.cpp
  ${NAMCHAIN_AUDIODSPTOOLS_WAV}
  ${NAMC_CORE_SOURCES})

target_include_directories(namchain SYSTEM PUBLIC
  "${NAMCHAIN_NAM_CORE_DIR}"
  "${NAMCHAIN_NAM_CORE_DIR}/Dependencies/eigen"
  "${NAMCHAIN_NAM_CORE_DIR}/Dependencies/nlohmann")
# Private: the WAV loader is an implementation detail of libnamchain, not part of its surface.
target_include_directories(namchain PRIVATE "${NAMCHAIN_AUDIODSPTOOLS_DIR}/dsp")
target_include_directories(namchain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(namchain PUBLIC cxx_std_20)
target_compile_options(namchain PRIVATE ${NAMCHAIN_WARN_FLAGS})

if(NAMCHAIN_A2_FAST)
  target_compile_definitions(namchain PUBLIC NAM_ENABLE_A2_FAST)
endif()
if(NAMCHAIN_SAMPLE_FLOAT)
  target_compile_definitions(namchain PUBLIC NAM_SAMPLE_FLOAT)
endif()
# The two instruments cannot both own operator new: a sanitizer runtime installs its own
# interceptors, which bypass a replacement (observed: allocations uncounted while frees were
# counted, in the RTSan CI job). RTSan covers allocations itself, so prefer it and drop ours.
set(_namchain_rt_check ${NAMCHAIN_RT_CHECK})
if((NAMCHAIN_RTSAN OR NAMCHAIN_TSAN) AND _namchain_rt_check)
  message(STATUS "a sanitizer is on: allocation interposer disabled (sanitizer runtimes intercept operator new, which bypasses a replacement)")
  set(_namchain_rt_check OFF)
endif()

if(_namchain_rt_check)
  target_compile_definitions(namchain PRIVATE NAMCHAIN_RT_CHECK_ENABLED)
endif()

if(NAMCHAIN_RTSAN)
  target_compile_options(namchain PUBLIC -fsanitize=realtime)
  target_link_options(namchain PUBLIC -fsanitize=realtime)
  message(STATUS "RealtimeSanitizer enabled (requires clang >= 20)")
endif()
if(NAMCHAIN_TSAN)
  target_compile_options(namchain PUBLIC -fsanitize=thread)
  target_link_options(namchain PUBLIC -fsanitize=thread)
  message(STATUS "ThreadSanitizer enabled")
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(NAMCHAIN_BUILD_TOOLS)
  add_executable(namchain-bench tools/bench/namchain-bench.cpp)
  target_link_libraries(namchain-bench PRIVATE namchain)
  target_compile_options(namchain-bench PRIVATE ${NAMCHAIN_WARN_FLAGS})

  add_executable(namchain-render tools/render/namchain-render.cpp)
  target_link_libraries(namchain-render PRIVATE namchain)
  target_compile_options(namchain-render PRIVATE ${NAMCHAIN_WARN_FLAGS})

  add_executable(namchain-wavcmp tools/wavcmp/namchain-wavcmp.cpp)
  target_link_libraries(namchain-wavcmp PRIVATE namchain)
  target_compile_options(namchain-wavcmp PRIVATE ${NAMCHAIN_WARN_FLAGS})

  # The M1 acceptance harness (plan.md §1 criterion 3, §10.5).
  add_executable(namchain-soak tools/soak/namchain-soak.cpp)
  target_link_libraries(namchain-soak PRIVATE namchain)
  target_compile_options(namchain-soak PRIVATE ${NAMCHAIN_WARN_FLAGS})

  # True round-trip latency over a loopback or a patch cable (plan.md §10.5.1).
  add_executable(namchain-latency tools/latency/namchain-latency.cpp)
  target_link_libraries(namchain-latency PRIVATE namchain)
  target_compile_options(namchain-latency PRIVATE ${NAMCHAIN_WARN_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(NAMCHAIN_BUILD_TESTS)
  enable_testing()

  # The A2-Full / A2-Lite test models are extracted from the vendored A2.nam at build
  # time rather than committed, so the repository carries no derived model data.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(_namchain_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(_a2_src "${NAMCHAIN_NAM_CORE_DIR}/example_models/A2.nam")
  add_custom_command(
    OUTPUT "${_namchain_gen_dir}/A2-Full.nam" "${_namchain_gen_dir}/A2-Lite.nam" "${_namchain_gen_dir}/CabIR.nam"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_namchain_gen_dir}"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_a2_submodels.py"
            "${_a2_src}" "${_namchain_gen_dir}"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_test_ir.py"
            "${_namchain_gen_dir}/CabIR.nam" 2048
    DEPENDS "${_a2_src}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_a2_submodels.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_test_ir.py"
    COMMENT "Generating A2-Full / A2-Lite / CabIR test models"
    VERBATIM)
  add_custom_target(namchain-test-data
    DEPENDS "${_namchain_gen_dir}/A2-Full.nam" "${_namchain_gen_dir}/A2-Lite.nam" "${_namchain_gen_dir}/CabIR.nam")

  # The example rigs reference the generated fixtures by bare name, and rig paths resolve against
  # the rig's own directory, so copying them next to the models makes `namchain-soak --graph
  # <build>/generated/guitar-chain.json` work without editing anything.
  file(GLOB NAMCHAIN_EXAMPLE_RIGS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/rigs/*.json")
  add_custom_target(namchain-example-rigs ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NAMCHAIN_EXAMPLE_RIGS} "${_namchain_gen_dir}"
    DEPENDS ${NAMCHAIN_EXAMPLE_RIGS}
    COMMENT "Copying example rigs next to the generated test models")
  add_dependencies(namchain-example-rigs namchain-test-data)

  add_executable(namchain-tests
    tests/test_lookback.cpp
    tests/test_a2_planar_parity.cpp
    tests/test_render.cpp
    tests/test_soak.cpp
    tests/test_alsa_params.cpp
    tests/test_rt_check.cpp
    tests/test_pcm.cpp
    tests/test_wav.cpp
    tests/test_backend.cpp
    tests/test_rt_primitives.cpp
    tests/test_graph.cpp
    tests/test_nodes.cpp
    tests/test_rig.cpp
    tests/test_latency.cpp
    tests/test_glitch.cpp)
  add_dependencies(namchain-tests namchain-test-data)
  target_link_libraries(namchain-tests PRIVATE namchain)
  target_include_directories(namchain-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_compile_definitions(namchain-tests PRIVATE
    NAMCHAIN_TEST_DATA_DIR="${_namchain_gen_dir}"
    NAMCHAIN_EXAMPLE_MODELS_DIR="${NAMCHAIN_NAM_CORE_DIR}/example_models"
    NAMCHAIN_EXAMPLE_AUDIO_DIR="${NAMCHAIN_NAM_CORE_DIR}/example_audio")
  if(NAMCHAIN_REQUIRE_A2_PARITY)
    target_compile_definitions(namchain-tests PRIVATE NAMCHAIN_REQUIRE_A2_PARITY)
  endif()
  # The planar bit-identity gate is an -O3 property: at -O2 the reference A2FastModel contracts
  # its FMAs differently, so the comparison fails even though nothing is wrong with either side.
  # Measured on aarch64/GCC 13. Gate the test on the actual flag rather than assuming.
  # Check the flags for the build type actually selected: CMAKE_CXX_FLAGS_RELEASE is populated
  # whatever the configuration, so testing it directly would enable the gate for a -O2 build too.
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _namchain_build_type_upper)
  set(_namchain_opt_flags "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_namchain_build_type_upper}}")
  if(_namchain_opt_flags MATCHES "-O3")
    target_compile_definitions(namchain-tests PRIVATE NAMCHAIN_A2_PARITY_O3)
    message(STATUS "A2 planar parity gate: enabled (-O3 build)")
  else()
    message(STATUS "A2 planar parity gate: will self-skip (this build is not -O3)")
  endif()
  add_test(NAME unit COMMAND namchain-tests)

  # The soak harness's command line and its backend layer. Runs without hardware and without a
  # clock (the file backend and the unpaced null backend), which is what makes the rest of the
  # harness testable in CI at all -- see tests/cli/run_soak_backends.cmake.
  if(NAMCHAIN_BUILD_TOOLS)
    add_test(NAME soak_cli
      COMMAND ${CMAKE_COMMAND}
        -DNAMCHAIN_SOAK=$<TARGET_FILE:namchain-soak>
        -DNAMCHAIN_RENDER=$<TARGET_FILE:namchain-render>
        -DNAMCHAIN_WAVCMP=$<TARGET_FILE:namchain-wavcmp>
        -DNAMCHAIN_MODEL=${_namchain_gen_dir}/A2-Lite.nam
        -DNAMCHAIN_INPUT=${NAMCHAIN_NAM_CORE_DIR}/example_audio/input.wav
        -DNAMCHAIN_WORK=${CMAKE_CURRENT_BINARY_DIR}/soak-cli
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/run_soak_backends.cmake")
  endif()

  # -------------------------------------------------------------------------
  # Golden comparison against the vendored core's own `render` tool (§10.2).
  #
  # The reference is built in its own CMake tree because upstream defines no library target
  # -- its tools compile NAM_SOURCES directly. It is built by a CTest fixture rather than by
  # the default build, so an ordinary build does not pay for a second full compile of the
  # core. Skipped when cross-compiling, where we could not run the reference anyway.
  # -------------------------------------------------------------------------
  option(NAMCHAIN_BUILD_GOLDEN_REFERENCE "Compare our offline render against the core's render tool" ON)
  # Measured differences from the core's render tool, which is itself built -Ofast:
  #   Cortex-A76 / GCC 13 -O3              -> 0        (byte-identical)
  #   Cortex-A76 / clang 20 -O2 + RTSan    -> 0        (byte-identical)
  #   x86_64     / GCC -O3                 -> 1.71e-07
  # On AArch64 the planar kernels fix the arithmetic in NEON intrinsics, so the result does not
  # move with the compiler or the optimisation level. On x86, A2 takes the Eigen path, where
  # -O3 and -Ofast legitimately differ. 1e-5 is ~58x above the only non-zero measurement and
  # ~-100 dBFS against a signal peaking at 0.19 -- far tighter than any real pipeline error
  # (wrong model, dropped frames, wrong rate) while leaving room for toolchain variation.
  set(NAMCHAIN_GOLDEN_TOLERANCE "1e-5" CACHE STRING
      "Maximum |difference| allowed against the core's render output (see tests/golden)")

  if(NAMCHAIN_BUILD_GOLDEN_REFERENCE AND NOT CMAKE_CROSSCOMPILING AND NAMCHAIN_BUILD_TOOLS)
    set(_namchain_ref_build "${CMAKE_CURRENT_BINARY_DIR}/namcore-ref")
    set(_namchain_ref_render "${_namchain_ref_build}/tools/render")

    add_custom_command(
      OUTPUT "${_namchain_ref_render}"
      COMMAND ${CMAKE_COMMAND} -S "${NAMCHAIN_NAM_CORE_DIR}" -B "${_namchain_ref_build}"
              -DCMAKE_BUILD_TYPE=Release
      COMMAND ${CMAKE_COMMAND} --build "${_namchain_ref_build}" --target render -j
      DEPENDS "${NAMCHAIN_NAM_CORE_DIR}/tools/render.cpp"
              "${NAMCHAIN_NAM_CORE_DIR}/NAM/dsp.h"
      COMMENT "Building the vendored core's render tool (golden reference)"
      VERBATIM)
    add_custom_target(namchain-golden-reference DEPENDS "${_namchain_ref_render}")

    add_test(NAME golden_setup
      COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}" --target namchain-golden-reference)
    set_tests_properties(golden_setup PROPERTIES FIXTURES_SETUP namchain_golden)

    add_test(NAME golden_a2_lite
      COMMAND ${CMAKE_COMMAND}
        -DNAMCHAIN_RENDER=$<TARGET_FILE:namchain-render>
        -DNAMCHAIN_WAVCMP=$<TARGET_FILE:namchain-wavcmp>
        -DNAMCHAIN_CORE_RENDER=${_namchain_ref_render}
        -DNAMCHAIN_MODEL=${_namchain_gen_dir}/A2-Lite.nam
        -DNAMCHAIN_INPUT=${NAMCHAIN_NAM_CORE_DIR}/example_audio/input.wav
        -DNAMCHAIN_WORK=${CMAKE_CURRENT_BINARY_DIR}/golden
        -DNAMCHAIN_TOLERANCE=${NAMCHAIN_GOLDEN_TOLERANCE}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/run_a2_golden.cmake")
    set_tests_properties(golden_a2_lite PROPERTIES
      FIXTURES_REQUIRED namchain_golden
      DEPENDS unit)
  endif()
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
set(NAMCHAIN_A2_PLANAR_ACTIVE OFF)
if(NAMCHAIN_A2_FAST AND NAMCHAIN_APPLY_VENDOR_PATCHES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8)")
  set(NAMCHAIN_A2_PLANAR_ACTIVE ON)
endif()

# The planar-kernel parity gate is only meaningful in an optimised build: at -O0 the reference
# does not contract to FMA, so the comparison fails through no fault of either implementation.
# See tests/test_a2_planar_parity.cpp and plan.md §7.8.
if(NAMCHAIN_A2_PLANAR_ACTIVE AND (NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug"))
  message(WARNING
    "A2 planar kernels are active but CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}': the bit-identity "
    "parity test will self-skip. Configure Release to actually exercise it.")
endif()

message(STATUS "namchain ${PROJECT_VERSION}")
message(STATUS "  NAM core:        ${NAMCHAIN_NAM_CORE_DIR}")
message(STATUS "  A2 fast path:    ${NAMCHAIN_A2_FAST}")
message(STATUS "  A2 planar NEON:  ${NAMCHAIN_A2_PLANAR_ACTIVE}")
message(STATUS "  float samples:   ${NAMCHAIN_SAMPLE_FLOAT}")
message(STATUS "  RTSan:           ${NAMCHAIN_RTSAN}")
