#pragma once

// Compile-time platform detection.
//
// Every macro below resolves to 1 or 0 (never just defined/undefined) so it can
// be used uniformly with `#if` and with C++ `if constexpr`/`&&`. Include this
// header whenever you need to branch on the operating system, architecture,
// Clang frontend, or available CPU features at compile time.
//
//   #if RUX_OS_WINDOWS         // preprocessor
//   if constexpr (RUX_IS_UNIX) // C++
//
// For the typed view of the same information (enums, host constants, ABI
// tables) include "Target/Target.h"; for runtime hardware queries include
// "System/Host.h".

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

// ---- Operating system families ----------------------------------------------

#define RUX_IS_UNIX (RUX_OS_LINUX || RUX_OS_MACOS || RUX_OS_FREEBSD)
#define RUX_IS_ELF_OS (RUX_OS_LINUX || RUX_OS_FREEBSD)

// ---- Architecture -----------------------------------------------------------

#if defined(__x86_64__) || defined(_M_X64)
    #define RUX_ARCH_X86_64 1
#else
    #define RUX_ARCH_X86_64 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    #define RUX_ARCH_AARCH64 1
#else
    #define RUX_ARCH_AARCH64 0
#endif

// ---- C++ compiler -----------------------------------------------------------

#ifndef __clang__
    #error "Rux requires Clang 22.1 or newer"
#endif
#ifdef __apple_build_version__
    #error "Rux requires upstream Clang; Apple Clang is unsupported"
#endif
#if __clang_major__ < 22 || (__clang_major__ == 22 && __clang_minor__ < 1)
    #error "Rux requires Clang 22.1 or newer"
#endif

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
// the executing machine supports. For the latter use Rux::System::HostSupports
// from "System/Host.h".

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

#ifdef __ARM_FEATURE_SVE
    #define RUX_FEATURE_SVE 1
#else
    #define RUX_FEATURE_SVE 0
#endif
