# Cross-compile the compiler for FreeBSD AArch64 on a FreeBSD x86-64 host.
#
# There is no hosted FreeBSD runner and no accelerated AArch64 virtualization
# on any hosted runner, so CI builds the freebsd-aarch64 compiler in its KVM
# x86-64 guest with the guest's own Clang 23 and lld, against the AArch64 base
# system unpacked as a sysroot. The file is selected through the
# CMAKE_TOOLCHAIN_FILE environment variable, which CMake honours on the first
# configure, so Run.sh needs no new option: it still supplies the compiler
# through -DCMAKE_CXX_COMPILER, and this file only retargets it.
#
#   export CMAKE_TOOLCHAIN_FILE=$PWD/Compiler/CMake/Toolchains/FreeBSD-AArch64.cmake
#   export RUX_FREEBSD_AARCH64_SYSROOT=/opt/sysroot/freebsd-aarch64
#   sh Run.sh build --no-pch

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSTEM_VERSION 15.1)

if (NOT DEFINED ENV{RUX_FREEBSD_AARCH64_SYSROOT})
    message(FATAL_ERROR "RUX_FREEBSD_AARCH64_SYSROOT must name an unpacked FreeBSD 15.1 arm64 base system")
endif ()
set(CMAKE_SYSROOT "$ENV{RUX_FREEBSD_AARCH64_SYSROOT}")
if (NOT EXISTS "${CMAKE_SYSROOT}/usr/lib/libc.so")
    message(FATAL_ERROR "'${CMAKE_SYSROOT}' holds no FreeBSD base system (usr/lib/libc.so is missing)")
endif ()

set(CMAKE_CXX_COMPILER_TARGET aarch64-unknown-freebsd15.1)

# lld links against the sysroot. The libgcc and libgcc_s it finds there are
# FreeBSD's own compiler-rt and libunwind, which the FreeBSD driver links by
# default, so no runtime library needs to come from the host's LLVM.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Libraries and headers come from the sysroot only; programs, such as the
# compiler itself, from the host.
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
