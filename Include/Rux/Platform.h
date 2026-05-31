/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <algorithm>
#include <cstdint>

#if defined(_WIN32)
#  define RUX_OS_WINDOWS 1
#elif defined(__APPLE__)
#  define RUX_OS_MACOS 1
#elif defined(__linux__)
#  define RUX_OS_LINUX 1
#elif defined(__FreeBSD__)
#  define RUX_OS_FREEBSD 1
#elif defined(__OpenBSD__)
#  define RUX_OS_OPENBSD 1
#elif defined(__NetBSD__)
#  define RUX_OS_NETBSD 1
#elif defined(__DragonFly__)
#  define RUX_OS_DRAGONFLY 1
#elif defined(__illumos__)
#  define RUX_OS_ILLUMOS 1
#elif defined(__sun) && defined(__SVR4)
#  define RUX_OS_SOLARIS 1
#else
#  error "Unsupported operating system"
#endif

#ifndef RUX_OS_WINDOWS
#  define RUX_OS_WINDOWS 0
#endif
#ifndef RUX_OS_MACOS
#  define RUX_OS_MACOS 0
#endif
#ifndef RUX_OS_LINUX
#  define RUX_OS_LINUX 0
#endif
#ifndef RUX_OS_FREEBSD
#  define RUX_OS_FREEBSD 0
#endif
#ifndef RUX_OS_OPENBSD
#  define RUX_OS_OPENBSD 0
#endif
#ifndef RUX_OS_NETBSD
#  define RUX_OS_NETBSD 0
#endif
#ifndef RUX_OS_DRAGONFLY
#  define RUX_OS_DRAGONFLY 0
#endif
#ifndef RUX_OS_ILLUMOS
#  define RUX_OS_ILLUMOS 0
#endif
#ifndef RUX_OS_SOLARIS
#  define RUX_OS_SOLARIS 0
#endif

#define RUX_IS_BSD (RUX_OS_FREEBSD || RUX_OS_OPENBSD || RUX_OS_NETBSD || RUX_OS_DRAGONFLY || RUX_OS_MACOS)
#define RUX_IS_SUNOS (RUX_OS_SOLARIS || RUX_OS_ILLUMOS)

#if defined(__x86_64__) || defined(_M_X64)
#  define RUX_ARCH_X64 1
#elif defined(__i386__) || defined(_M_IX86)
#  define RUX_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define RUX_ARCH_ARM64 1
#elif defined(__arm__) || defined(_M_ARM)
#  define RUX_ARCH_ARM32 1
#elif defined(__riscv) && __riscv_xlen == 64
#  define RUX_ARCH_RISCV64 1
#else
#  error "Unsupported architecture"
#endif

#ifndef RUX_ARCH_X64
#  define RUX_ARCH_X64 0
#endif
#ifndef RUX_ARCH_X86
#  define RUX_ARCH_X86 0
#endif
#ifndef RUX_ARCH_ARM64
#  define RUX_ARCH_ARM64 0
#endif
#ifndef RUX_ARCH_ARM32
#  define RUX_ARCH_ARM32 0
#endif
#ifndef RUX_ARCH_RISCV64
#  define RUX_ARCH_RISCV64 0
#endif

#if defined(_MSC_VER)
#  define RUX_COMPILER_MSVC 1
#  define RUX_CPLUSPLUS _MSVC_LANG
#elif defined(__clang__)
#  define RUX_COMPILER_CLANG 1
#  define RUX_CPLUSPLUS __cplusplus
#elif defined(__GNUC__)
#  define RUX_COMPILER_GCC 1
#  define RUX_CPLUSPLUS __cplusplus
#else
#  error "Unsupported compiler"
#endif

#ifndef RUX_COMPILER_MSVC
#  define RUX_COMPILER_MSVC 0
#endif
#ifndef RUX_COMPILER_CLANG
#  define RUX_COMPILER_CLANG 0
#endif
#ifndef RUX_COMPILER_GCC
#  define RUX_COMPILER_GCC 0
#endif

#define RUX_CXX_14 (RUX_CPLUSPLUS >= 201402L)
#define RUX_CXX_17 (RUX_CPLUSPLUS >= 201703L)
#define RUX_CXX_20 (RUX_CPLUSPLUS >= 202002L)
#define RUX_CXX_23 (RUX_CPLUSPLUS >= 202302L)

#if RUX_COMPILER_MSVC
#  define RUX_FORCEINLINE __forceinline
#  define RUX_NOINLINE __declspec(noinline)
#  define RUX_RESTRICT __restrict
#  define RUX_LIKELY(x) (x)
#  define RUX_UNLIKELY(x) (x)
#  define RUX_CACHE_ALIGN __declspec(align(64))
#else
#  define RUX_FORCEINLINE inline __attribute__((always_inline))
#  define RUX_NOINLINE __attribute__((noinline))
#  define RUX_RESTRICT __restrict__
#  define RUX_LIKELY(x) __builtin_expect(!!(x), 1)
#  define RUX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define RUX_CACHE_ALIGN __attribute__((aligned(64)))
#endif

#define RUX_CACHE_LINE_SIZE 64

#if defined(NDEBUG)
#  define RUX_BUILD_RELEASE 1
#  define RUX_BUILD_DEBUG 0
#else
#  define RUX_BUILD_RELEASE 0
#  define RUX_BUILD_DEBUG 1
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86)
#  define RUX_FEATURE_SSE2 1
#else
#  define RUX_FEATURE_SSE2 0
#endif

#if defined(__AVX__)
#  define RUX_FEATURE_AVX 1
#else
#  define RUX_FEATURE_AVX 0
#endif

#if defined(__AVX2__)
#  define RUX_FEATURE_AVX2 1
#else
#  define RUX_FEATURE_AVX2 0
#endif

#if defined(__AVX512F__)
#  define RUX_FEATURE_AVX512 1
#else
#  define RUX_FEATURE_AVX512 0
#endif

#if defined(__ARM_NEON)
#  define RUX_FEATURE_NEON 1
#else
#  define RUX_FEATURE_NEON 0
#endif

#if defined(__riscv_vector)
#  define RUX_FEATURE_RVV 1
#else
#  define RUX_FEATURE_RVV 0
#endif

namespace Rux::Platform {

    enum class OS : std::uint32_t {
        Unknown = 0,
        Windows,
        Linux,
        MacOS,
        FreeBSD,
        OpenBSD,
        NetBSD,
        DragonFlyBSD,
        Solaris,
        Illumos
    };

    enum class Arch : std::uint32_t { Unknown = 0, X86, X64, ARM32, ARM64, RISCV32, RISCV64 };
    enum class Compiler : std::uint32_t { Unknown = 0, MSVC, Clang, GCC };
    enum class BuildMode : std::uint8_t { Debug, Release };
    enum class Endian : std::uint8_t { Little, Big };

    enum class CpuFeature : std::uint64_t {
        None = 0,
        SSE2 = 1ull << 0,
        SSE4_2 = 1ull << 1,
        AVX = 1ull << 2,
        AVX2 = 1ull << 3,
        AVX512 = 1ull << 4,
        BMI1 = 1ull << 10,
        BMI2 = 1ull << 11,
        POPCNT = 1ull << 12,
        NEON = 1ull << 20,
        SVE = 1ull << 21,
        RVV = 1ull << 22,
        FMA = 1ull << 30,
        LZCNT = 1ull << 31,
    };

    constexpr CpuFeature operator|(CpuFeature a, CpuFeature b) noexcept {
        return static_cast<CpuFeature>(static_cast<std::uint64_t>(a) | static_cast<std::uint64_t>(b));
    }
    constexpr CpuFeature operator&(CpuFeature a, CpuFeature b) noexcept {
        return static_cast<CpuFeature>(static_cast<std::uint64_t>(a) & static_cast<std::uint64_t>(b));
    }
    constexpr CpuFeature operator^(CpuFeature a, CpuFeature b) noexcept {
        return static_cast<CpuFeature>(static_cast<std::uint64_t>(a) ^ static_cast<std::uint64_t>(b));
    }
    constexpr CpuFeature operator~(CpuFeature a) noexcept {
        return static_cast<CpuFeature>(~static_cast<std::uint64_t>(a));
    }
    constexpr CpuFeature& operator|=(CpuFeature& a, CpuFeature b) noexcept {
        return a = a | b;
    }
    constexpr CpuFeature& operator&=(CpuFeature& a, CpuFeature b) noexcept {
        return a = a & b;
    }
    constexpr CpuFeature& operator^=(CpuFeature& a, CpuFeature b) noexcept {
        return a = a ^ b;
    }

    struct CpuFeatures {
        std::uint64_t bits{0};

        [[nodiscard]] constexpr bool Has(CpuFeature f) const noexcept {
            return (bits & static_cast<std::uint64_t>(f)) != 0;
        }

        template <CpuFeature... Fs>
        [[nodiscard]] constexpr bool HasAny() const noexcept {
            return (Has(Fs) || ...);
        }

        template <CpuFeature... Fs>
        [[nodiscard]] constexpr bool HasAll() const noexcept {
            return (Has(Fs) && ...);
        }

        [[nodiscard]] constexpr bool HasVectorISA() const noexcept {
            return HasAny<CpuFeature::AVX,
                          CpuFeature::AVX2,
                          CpuFeature::AVX512,
                          CpuFeature::NEON,
                          CpuFeature::SVE,
                          CpuFeature::RVV>();
        }

        constexpr CpuFeatures& operator|=(CpuFeature f) noexcept {
            bits |= static_cast<std::uint64_t>(f);
            return *this;
        }
    };

    struct MemoryInfo {
        std::uint64_t page_size{};
        std::uint64_t total_ram{};
        std::uint64_t available_ram{};
    };

    struct CpuTopology {
        std::uint32_t logical{};
        std::uint32_t physical{};
        bool smt{};
    };

    [[nodiscard]] MemoryInfo QueryMemory() noexcept;
    [[nodiscard]] const CpuTopology& QueryTopology() noexcept;
    [[nodiscard]] CpuFeatures DetectRuntimeCpuFeatures() noexcept;

    [[nodiscard]] constexpr OS GetCompileTimeOS() noexcept {
        if constexpr (RUX_OS_WINDOWS) return OS::Windows;
        if constexpr (RUX_OS_LINUX) return OS::Linux;
        if constexpr (RUX_OS_MACOS) return OS::MacOS;
        if constexpr (RUX_OS_FREEBSD) return OS::FreeBSD;
        if constexpr (RUX_OS_OPENBSD) return OS::OpenBSD;
        if constexpr (RUX_OS_NETBSD) return OS::NetBSD;
        if constexpr (RUX_OS_DRAGONFLY) return OS::DragonFlyBSD;
        if constexpr (RUX_OS_ILLUMOS) return OS::Illumos;
        if constexpr (RUX_OS_SOLARIS) return OS::Solaris;
        return OS::Unknown;
    }

    [[nodiscard]] constexpr Arch GetCompileTimeArch() noexcept {
        if constexpr (RUX_ARCH_X64) return Arch::X64;
        if constexpr (RUX_ARCH_X86) return Arch::X86;
        if constexpr (RUX_ARCH_ARM64) return Arch::ARM64;
        if constexpr (RUX_ARCH_ARM32) return Arch::ARM32;
        if constexpr (RUX_ARCH_RISCV64) return Arch::RISCV64;
        return Arch::Unknown;
    }

    [[nodiscard]] constexpr Compiler GetCompileTimeCompiler() noexcept {
        if constexpr (RUX_COMPILER_MSVC) return Compiler::MSVC;
        if constexpr (RUX_COMPILER_CLANG) return Compiler::Clang;
        if constexpr (RUX_COMPILER_GCC) return Compiler::GCC;
        return Compiler::Unknown;
    }

    [[nodiscard]] constexpr BuildMode GetCompileTimeBuildMode() noexcept {
        if constexpr (RUX_BUILD_RELEASE) return BuildMode::Release;
        return BuildMode::Debug;
    }

    [[nodiscard]] constexpr Endian GetCompileTimeEndianness() noexcept {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        return Endian::Big;
#else
        return Endian::Little;
#endif
    }

    [[nodiscard]] constexpr CpuFeatures DetectCompileTimeCpuFeatures() noexcept {
        CpuFeatures f{};
        if constexpr (RUX_FEATURE_SSE2) f |= CpuFeature::SSE2;
        if constexpr (RUX_FEATURE_AVX) f |= CpuFeature::AVX;
        if constexpr (RUX_FEATURE_AVX2) f |= CpuFeature::AVX2;
        if constexpr (RUX_FEATURE_AVX512) f |= CpuFeature::AVX512;
        if constexpr (RUX_FEATURE_NEON) f |= CpuFeature::NEON;
        if constexpr (RUX_FEATURE_RVV) f |= CpuFeature::RVV;
        return f;
    }

    template <OS O, Arch A, Compiler C, BuildMode M, CpuFeatures CPU>
    struct Target {
        static constexpr OS os = O;
        static constexpr Arch arch = A;
        static constexpr Compiler compiler = C;
        static constexpr BuildMode mode = M;
        static constexpr CpuFeatures cpu = CPU;
    };

    template <typename T>
    struct Platform {
        [[nodiscard]] static constexpr bool Is64Bit() noexcept {
            return T::arch == Arch::X64 || T::arch == Arch::ARM64 || T::arch == Arch::RISCV64;
        }
        [[nodiscard]] static constexpr bool IsDebug() noexcept {
            return T::mode == BuildMode::Debug;
        }
        [[nodiscard]] static constexpr bool Has(CpuFeature f) noexcept {
            return T::cpu.Has(f);
        }
        [[nodiscard]] static constexpr bool HasSIMD() noexcept {
            return T::cpu.HasVectorISA();
        }
        [[nodiscard]] static constexpr bool IsX86() noexcept {
            return T::arch == Arch::X86 || T::arch == Arch::X64;
        }
    };

    template <typename Fn>
    struct DispatchTable {
        Fn scalar{};
        Fn sse2{};
        Fn avx{};
        Fn avx2{};
        Fn avx512{};
        Fn neon{};
    };

    template <typename Fn>
    [[nodiscard]] inline Fn SelectBest(const DispatchTable<Fn>& table, const CpuFeatures& runtime_features) noexcept {
        if (runtime_features.Has(CpuFeature::AVX512) && table.avx512) return table.avx512;
        if (runtime_features.Has(CpuFeature::AVX2) && table.avx2) return table.avx2;
        if (runtime_features.Has(CpuFeature::AVX) && table.avx) return table.avx;
        if (runtime_features.Has(CpuFeature::SSE2) && table.sse2) return table.sse2;
        if (runtime_features.Has(CpuFeature::NEON) && table.neon) return table.neon;
        return table.scalar;
    }

    using Current = Platform<Target<GetCompileTimeOS(),
                                    GetCompileTimeArch(),
                                    GetCompileTimeCompiler(),
                                    GetCompileTimeBuildMode(),
                                    DetectCompileTimeCpuFeatures()>>;
} // namespace Rux::Platform
