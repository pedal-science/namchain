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
option(NAMCHAIN_RTSAN "Build with RealtimeSanitizer (needs clang >= 20)" OFF)
option(NAMCHAIN_WERROR "Treat warnings as errors" OFF)

set(NAMCHAIN_NAM_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/NeuralAmpModelerCore"
    CACHE PATH "Path to the vendored NeuralAmpModelerCore checkout")

if(NOT EXISTS "${NAMCHAIN_NAM_CORE_DIR}/NAM/dsp.h")
  message(FATAL_ERROR
    "Vendored NAM core not found at ${NAMCHAIN_NAM_CORE_DIR}.\n"
    "Run: git submodule update --init --recursive")
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

add_library(namchain OBJECT
  src/nam/lookback.cpp
  ${NAMC_CORE_SOURCES})

target_include_directories(namchain SYSTEM PUBLIC
  "${NAMCHAIN_NAM_CORE_DIR}"
  "${NAMCHAIN_NAM_CORE_DIR}/Dependencies/eigen"
  "${NAMCHAIN_NAM_CORE_DIR}/Dependencies/nlohmann")
target_include_directories(namchain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(namchain PUBLIC cxx_std_20)
target_compile_options(namchain PRIVATE -Wall -Wextra)

if(NAMCHAIN_A2_FAST)
  target_compile_definitions(namchain PUBLIC NAM_ENABLE_A2_FAST)
endif()
if(NAMCHAIN_SAMPLE_FLOAT)
  target_compile_definitions(namchain PUBLIC NAM_SAMPLE_FLOAT)
endif()

if(NAMCHAIN_RTSAN)
  target_compile_options(namchain PUBLIC -fsanitize=realtime)
  target_link_options(namchain PUBLIC -fsanitize=realtime)
  message(STATUS "RealtimeSanitizer enabled (requires clang >= 20)")
endif()

if(NAMCHAIN_WERROR)
  set(NAMCHAIN_WARN_FLAGS -Wall -Wextra -Werror)
else()
  set(NAMCHAIN_WARN_FLAGS -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(NAMCHAIN_BUILD_TOOLS)
  add_executable(namchain-bench tools/bench/namchain-bench.cpp)
  target_link_libraries(namchain-bench PRIVATE namchain)
  target_compile_options(namchain-bench PRIVATE ${NAMCHAIN_WARN_FLAGS})
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

  add_executable(namchain-tests tests/test_lookback.cpp)
  add_dependencies(namchain-tests namchain-test-data)
  target_link_libraries(namchain-tests PRIVATE namchain)
  target_include_directories(namchain-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_compile_definitions(namchain-tests PRIVATE
    NAMCHAIN_TEST_DATA_DIR="${_namchain_gen_dir}"
    NAMCHAIN_EXAMPLE_MODELS_DIR="${NAMCHAIN_NAM_CORE_DIR}/example_models")
  add_test(NAME unit COMMAND namchain-tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "namchain ${PROJECT_VERSION}")
message(STATUS "  NAM core:        ${NAMCHAIN_NAM_CORE_DIR}")
message(STATUS "  A2 fast path:    ${NAMCHAIN_A2_FAST}")
message(STATUS "  float samples:   ${NAMCHAIN_SAMPLE_FLOAT}")
message(STATUS "  RTSan:           ${NAMCHAIN_RTSAN}")
