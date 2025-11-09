#pragma once

namespace Rux
{

// Operating System Detection

#if defined(_WIN32) || defined(_WIN64) || defined(__WINDOWS__)
#define PLATFORM_WINDOWS 1

#if defined(_WIN64)
#define PLATFORM_WINDOWS_64 1
#define PLATFORM_BITS 64
#else
#define PLATFORM_WINDOWS_32 1
#define PLATFORM_BITS 32
#endif

// Windows version detection

#ifdef WINVER
#if WINVER >= 0x0A00    // Windows 10+
#define PLATFORM_OS_NAME "Windows 10+"
#elif WINVER >= 0x0603  // Windows 8.1
#define PLATFORM_OS_NAME "Windows 8.1"
#elif WINVER >= 0x0602  // Windows 8
#define PLATFORM_OS_NAME "Windows 8"
#elif WINVER >= 0x0601  // Windows 7
#define PLATFORM_OS_NAME "Windows 7"
#else
#define PLATFORM_OS_NAME "Windows"
#endif
#else
#define PLATFORM_OS_NAME "Windows"
#endif

#elif defined(__linux__) || defined(__linux) || defined(linux)
#define PLATFORM_LINUX 1
#define PLATFORM_OS_NAME "Linux"

#if defined(__ANDROID__)
#define PLATFORM_ANDROID 1
#undef PLATFORM_OS_NAME
#define PLATFORM_OS_NAME "Android"
#endif

#elif defined(__APPLE__) && defined(__MACH__)
#define PLATFORM_APPLE 1
#include <TargetConditionals.h>

#if TARGET_OS_IPHONE
#define PLATFORM_IOS 1
#define PLATFORM_OS_NAME "iOS"
#elif TARGET_OS_MAC
#define PLATFORM_MACOS 1
#define PLATFORM_OS_NAME "macOS"
#else
#define PLATFORM_OS_NAME "Apple"
#endif

#elif defined(__FreeBSD__)
#define PLATFORM_FREEBSD 1
#define PLATFORM_OS_NAME "FreeBSD"

#elif defined(__unix__) || defined(__unix)
#define PLATFORM_UNIX 1
#define PLATFORM_OS_NAME "Unix"

#else
#define PLATFORM_UNKNOWN 1
#define PLATFORM_OS_NAME "Unknown"
#endif

// Architecture / CPU Detection

// x86_64 / AMD64 (64-bit)
#if defined(__x86_64__) || defined(__x86_64) || defined(__amd64__) || \
    defined(__amd64) || defined(_M_X64) || defined(_M_AMD64)
#define ARCH_X64 1
#define ARCH_NAME "x64"
#define ARCH_BITS 64

// x86 (32-bit)
#elif defined(__i386__) || defined(__i386) || defined(_M_IX86) || \
      defined(_X86_) || defined(__X86__)
#define ARCH_X86 1
#define ARCH_NAME "x86"
#define ARCH_BITS 32

// ARM64 / AArch64 (64-bit)
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define ARCH_ARM64 1
#define ARCH_NAME "ARM64"
#define ARCH_BITS 64

// ARM (32-bit)
#elif defined(__arm__) || defined(_M_ARM) || defined(_ARM_)
#define ARCH_ARM 1
#define ARCH_NAME "ARM"
#define ARCH_BITS 32

// RISC-V 64-bit
#elif defined(__riscv) && (__riscv_xlen == 64)
#define ARCH_RISCV64 1
#define ARCH_NAME "RISC-V 64"
#define ARCH_BITS 64

// RISC-V 32-bit
#elif defined(__riscv) && (__riscv_xlen == 32)
#define ARCH_RISCV32 1
#define ARCH_NAME "RISC-V 32"
#define ARCH_BITS 32

// PowerPC 64-bit
#elif defined(__powerpc64__) || defined(__ppc64__)
#define ARCH_PPC64 1
#define ARCH_NAME "PowerPC 64"
#define ARCH_BITS 64

// PowerPC 32-bit
#elif defined(__powerpc__) || defined(__ppc__)
#define ARCH_PPC 1
#define ARCH_NAME "PowerPC"
#define ARCH_BITS 32

#else
#define ARCH_UNKNOWN 1
#define ARCH_NAME "Unknown"
#define ARCH_BITS 0
#endif

// Endianness Detection

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define ENDIAN_LITTLE 1
#define ENDIAN_NAME "Little Endian"
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
      __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define ENDIAN_BIG 1
#define ENDIAN_NAME "Big Endian"
#elif defined(_WIN32)
	// Windows is always little endian
#define ENDIAN_LITTLE 1
#define ENDIAN_NAME "Little Endian"
#else
#define ENDIAN_NAME "Unknown"
#endif

// Visual Studio Specific Platform Detection

#ifdef _MSC_VER
// Target architecture from Visual Studio
#if defined(_M_X64) || defined(_M_AMD64)
#define VS_TARGET_ARCH "x64"
#elif defined(_M_IX86)
#define VS_TARGET_ARCH "x86"
#elif defined(_M_ARM64)
#define VS_TARGET_ARCH "ARM64"
#elif defined(_M_ARM)
#define VS_TARGET_ARCH "ARM"
#else
#define VS_TARGET_ARCH "Unknown"
#endif

// Platform toolset (if available)
#ifdef _MSC_FULL_VER
#define VS_FULL_VERSION _MSC_FULL_VER
#endif
#endif

// Combined Target String

// Helper macros
#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

// Build target string: "OS-Architecture"
#if defined(PLATFORM_WINDOWS)
#if defined(ARCH_X64)
#define TARGET_STRING "windows-x86_64"
#elif defined(ARCH_X86)
#define TARGET_STRING "windows-x86"
#elif defined(ARCH_ARM64)
#define TARGET_STRING "windows-arm64"
#elif defined(ARCH_ARM)
#define TARGET_STRING "windows-arm"
#else
#define TARGET_STRING "windows-unknown"
#endif
#elif defined(PLATFORM_LINUX)
#if defined(ARCH_X64)
#define TARGET_STRING "linux-x86_64"
#elif defined(ARCH_X86)
#define TARGET_STRING "linux-x86"
#elif defined(ARCH_ARM64)
#define TARGET_STRING "linux-arm64"
#elif defined(ARCH_ARM)
#define TARGET_STRING "linux-arm"
#else
#define TARGET_STRING "linux-unknown"
#endif
#elif defined(PLATFORM_MACOS)
#if defined(ARCH_X64)
#define TARGET_STRING "macos-x86_64"
#elif defined(ARCH_ARM64)
#define TARGET_STRING "macos-arm64"
#else
#define TARGET_STRING "macos-unknown"
#endif
#else
#define TARGET_STRING "unknown-unknown"
#endif

// Full descriptive target
#define TARGET_FULL PLATFORM_OS_NAME " " ARCH_NAME

}
