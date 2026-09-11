# aarch64 cross-compilation (Raspberry Pi 4/5, Pi 500, generic arm64 Linux).
#
#   cmake -S . -B build-aarch64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# Requires: g++-aarch64-linux-gnu
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Cortex-A76 (Pi 5 / Pi 500). Keep the baseline at armv8-a so the same binary runs on
# Pi 4 (Cortex-A72) and in qemu; tune for the A76 pipeline explicitly.
set(NAMCHAIN_ARCH_FLAGS "-march=armv8-a -mtune=cortex-a76")
set(CMAKE_C_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")

set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
