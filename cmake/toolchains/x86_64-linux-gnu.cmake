# x86_64 cross-compilation: build the desktop/server target from an aarch64 host.
#
#   cmake -S . -B build-x86_64 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-linux-gnu.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# Requires: g++-x86-64-linux-gnu, plus qemu-user-static to run what it builds. This is
# how CI keeps x86_64 coverage without an x86_64 machine: compilation is native speed
# on the aarch64 host, and only the test binaries are emulated.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER x86_64-linux-gnu-g++)

# Baseline x86-64 (SSE2), which is what a native build on the same distribution gets
# by default. Keeping it at the baseline matters for the golden number: Eigen selects
# its vectorisation from these flags, and `-march=native` on an AVX-512 host would
# measure a different path than the one this configuration is meant to cover.
set(NAMCHAIN_ARCH_FLAGS "-march=x86-64 -mtune=generic")
set(CMAKE_C_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${NAMCHAIN_ARCH_FLAGS}")

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Run cross-built binaries under qemu-user: this is what makes `ctest` work, and what
# lets the golden reference tool (built for x86_64) run at configure/test time. Tests
# that spawn a helper binary directly rely on the host's binfmt_misc registration for
# x86_64, which the runner image sets up alongside this sysroot.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-x86_64-static -L /usr/x86_64-linux-gnu)
