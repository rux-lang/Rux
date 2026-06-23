#pragma once

// Compile-time platform detection.
//
// Every macro below resolves to 1 or 0 (never just defined/undefined) so it can
// be used uniformly with `#if` and with C++ `if constexpr`/`&&`. Include this
// header whenever you need to branch on the operating system, architecture,
// compiler, or available CPU features at compile time.
//
//   #if RUX_OS_WINDOWS        // preprocessor
//   if constexpr (RUX_IS_BSD) // C++
//
// For the typed view of the same information (enums, host constants, ABI
// tables) include "Rux/Target.h"; for runtime hardware queries include
// "Rux/Host.h".

// ---- Operating system -------------------------------------------------------

#ifdef _WIN32
    #define RUX_OS_WINDOWS 1
#else
    #define RUX_OS_WINDOWS 0
#endif

#ifdef __APPLE__
    #define RUX_OS_MACOS 1
#else
    #define RUX_OS_MACOS 0
#endif

#ifdef __linux__
    #define RUX_OS_LINUX 1
#else
    #define RUX_OS_LINUX 0
#endif

#ifdef __FreeBSD__
    #define RUX_OS_FREEBSD 1
#else
    #define RUX_OS_FREEBSD 0
#endif

#ifdef __OpenBSD__
    #define RUX_OS_OPENBSD 1
#else
    #define RUX_OS_OPENBSD 0
#endif

#ifdef __NetBSD__
    #define RUX_OS_NETBSD 1
#else
    #define RUX_OS_NETBSD 0
#endif

#ifdef __DragonFly__
    #define RUX_OS_DRAGONFLY 1
#else
    #define RUX_OS_DRAGONFLY 0
#endif

#ifdef __illumos__
    #define RUX_OS_ILLUMOS 1
#else
    #define RUX_OS_ILLUMOS 0
#endif

#if defined(__sun) && defined(__SVR4) && !defined(__illumos__)
    #define RUX_OS_SOLARIS 1
#else
    #define RUX_OS_SOLARIS 0
#endif

// ---- Operating system families ----------------------------------------------

#define RUX_IS_BSD (RUX_OS_FREEBSD || RUX_OS_OPENBSD || RUX_OS_NETBSD || RUX_OS_DRAGONFLY)
#define RUX_IS_SUNOS (RUX_OS_SOLARIS || RUX_OS_ILLUMOS)
#define RUX_IS_UNIX (RUX_OS_LINUX || RUX_OS_MACOS || RUX_IS_BSD || RUX_IS_SUNOS)
#define RUX_IS_ELF_OS (RUX_OS_LINUX || RUX_IS_BSD || RUX_IS_SUNOS)

// ---- Architecture -----------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
    #define RUX_ARCH_X64 1
#else
    #define RUX_ARCH_X64 0
#endif

#if defined(__i386__) || defined(_M_IX86)
    #define RUX_ARCH_X86 1
#else
    #define RUX_ARCH_X86 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    #define RUX_ARCH_ARM64 1
#else
    #define RUX_ARCH_ARM64 0
#endif

#if defined(__arm__) || defined(_M_ARM)
    #define RUX_ARCH_ARM32 1
#else
    #define RUX_ARCH_ARM32 0
#endif

#ifdef __riscv
    #if __riscv_xlen == 64
        #define RUX_ARCH_RISCV64 1
    #else
        #define RUX_ARCH_RISCV64 0
    #endif
    #if __riscv_xlen == 32
        #define RUX_ARCH_RISCV32 1
    #else
        #define RUX_ARCH_RISCV32 0
    #endif
#else
    #define RUX_ARCH_RISCV64 0
    #define RUX_ARCH_RISCV32 0
#endif

// ---- Compiler ---------------------------------------------------------------

#ifdef _MSC_VER
    #define RUX_COMPILER_MSVC 1
    #define RUX_COMPILER_CLANG 0
    #define RUX_COMPILER_GCC 0
    #define RUX_CPLUSPLUS _MSVC_LANG
#elifdef __clang__
    #define RUX_COMPILER_MSVC 0
    #define RUX_COMPILER_CLANG 1
    #define RUX_COMPILER_GCC 0
    #define RUX_CPLUSPLUS __cplusplus
#elifdef __GNUC__
    #define RUX_COMPILER_MSVC 0
    #define RUX_COMPILER_CLANG 0
    #define RUX_COMPILER_GCC 1
    #define RUX_CPLUSPLUS __cplusplus
#else
    #error "Unsupported compiler"
#endif

#define RUX_CXX_14 (RUX_CPLUSPLUS >= 201402L)
#define RUX_CXX_17 (RUX_CPLUSPLUS >= 201703L)
#define RUX_CXX_20 (RUX_CPLUSPLUS >= 202002L)
#define RUX_CXX_23 (RUX_CPLUSPLUS >= 202302L)

// ---- Build mode -------------------------------------------------------------

#ifdef NDEBUG
    #define RUX_BUILD_RELEASE 1
    #define RUX_BUILD_DEBUG 0
#else
    #define RUX_BUILD_RELEASE 0
    #define RUX_BUILD_DEBUG 1
#endif

// ---- Compile-time CPU features ----------------------------------------------
//
// These reflect what the compiler was told it may emit (e.g. -mavx2), not what
// the executing machine supports. For the latter use Rux::Platform::HostSupports
// from "Rux/Host.h".

#if defined(__SSE2__) || defined(_M_X64)
    #define RUX_FEATURE_SSE2 1
#else
    #define RUX_FEATURE_SSE2 0
#endif

#ifdef __AVX__
    #define RUX_FEATURE_AVX 1
#else
    #define RUX_FEATURE_AVX 0
#endif

#ifdef __AVX2__
    #define RUX_FEATURE_AVX2 1
#else
    #define RUX_FEATURE_AVX2 0
#endif

#ifdef __AVX512F__
    #define RUX_FEATURE_AVX512 1
#else
    #define RUX_FEATURE_AVX512 0
#endif

#ifdef __ARM_NEON
    #define RUX_FEATURE_NEON 1
#else
    #define RUX_FEATURE_NEON 0
#endif

#ifdef __riscv_vector
    #define RUX_FEATURE_RVV 1
#else
    #define RUX_FEATURE_RVV 0
#endif

#ifdef __ARM_FEATURE_SVE
    #define RUX_FEATURE_SVE 1
#else
    #define RUX_FEATURE_SVE 0
#endif
