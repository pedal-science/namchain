# armv7 hard-float cross-compilation (32-bit Raspberry Pi OS).
#
#   cmake -S . -B build-armv7 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/armv7-linux-gnueabihf.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# Requires: g++-arm-linux-gnueabihf
#
# NOTE (plan.md G19): the Debian armhf ABI baseline is VFPv3-D16, which has no NEON, so
# NEON must be runtime-detected (getauxval(AT_HWCAP) & HWCAP_NEON) before use. The build
# itself therefore stays at -mfpu=vfpv3-d16 and must not emit NEON unconditionally.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(NAMCHAIN_ARCH_FLAGS "-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
