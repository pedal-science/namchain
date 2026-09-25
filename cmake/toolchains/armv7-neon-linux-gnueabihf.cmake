# armv7 hard-float cross-compilation for boards known to have NEON + VFPv4
# (e.g. Cortex-A7/A15/A17: Tinker Board, 32-bit Pi 2/3/4 OS images).
#
#   cmake -S . -B build-armv7-neon -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/armv7-neon-linux-gnueabihf.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# Requires: g++-arm-linux-gnueabihf
#
# Unlike armv7-linux-gnueabihf.cmake (the generic Debian armhf baseline, VFPv3-D16, see
# plan.md G19), this build emits NEON and FMA unconditionally, which enables the A2 planar
# kernels. The binary will fault on a CPU without NEON. On a Cortex-A17 it takes A2-Full
# from ~133% of a core to ~52%.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(NAMCHAIN_ARCH_FLAGS "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
