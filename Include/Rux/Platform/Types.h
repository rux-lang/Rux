#pragma once

#include <cstdint>
#include <string_view>

namespace Rux::Platform {

    inline constexpr std::size_t CacheLineSize = 64;
    inline constexpr std::size_t Pointer32 = 4;
    inline constexpr std::size_t Pointer64 = 8;

    enum class OS : std::uint8_t {
        Unknown,
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

    enum class Arch : std::uint8_t { Unknown, X86_32, X86_64, ARM32, ARM64, RISCV32, RISCV64 };
    enum class DataModel : std::uint8_t { Unknown, ILP32, LP64, LLP64 };
    enum class ABI : std::uint8_t { Unknown, SystemV, WindowsX86, WindowsX64, AAPCS, AAPCS64, RISCV_ILP32, RISCV_LP64 };
    enum class CallingConv : std::uint8_t { Default, C, SysV, Win64, StdCall, AAPCS, AAPCS64, RISCV };
    enum class Compiler : std::uint8_t { Unknown, MSVC, Clang, GCC };
    enum class BuildMode : std::uint8_t { Debug, Release };
    enum class Endian : std::uint8_t { Little, Big };


    struct CpuFeatures {
        std::uint64_t mask{0};

        constexpr CpuFeatures() = default;
        constexpr explicit CpuFeatures(std::uint64_t m)
            : mask(m) {
        }

        [[nodiscard]] constexpr bool Has(CpuFeatures other) const noexcept {
            return (mask & other.mask) == other.mask;
        }

        constexpr CpuFeatures operator|(CpuFeatures other) const noexcept {
            return CpuFeatures(mask | other.mask);
        }
        constexpr CpuFeatures operator&(CpuFeatures other) const noexcept {
            return CpuFeatures(mask & other.mask);
        }
        constexpr CpuFeatures& operator|=(CpuFeatures other) noexcept {
            mask |= other.mask;
            return *this;
        }
    };

    namespace CpuFeature {

        inline constexpr CpuFeatures None{0};

        // x86/x64
        inline constexpr CpuFeatures SSE2{1ull << 0};
        inline constexpr CpuFeatures SSE3{1ull << 1};
        inline constexpr CpuFeatures SSSE3{1ull << 2};
        inline constexpr CpuFeatures SSE41{1ull << 3};
        inline constexpr CpuFeatures SSE42{1ull << 4};

        inline constexpr CpuFeatures AVX{1ull << 5};
        inline constexpr CpuFeatures AVX2{1ull << 6};
        inline constexpr CpuFeatures AVX512{1ull << 7};

        // ARM
        inline constexpr CpuFeatures NEON{1ull << 16};
        inline constexpr CpuFeatures SVE{1ull << 17};

        // RISC-V
        inline constexpr CpuFeatures RVV{1ull << 24};

    } // namespace CpuFeature

    struct RuntimeCpuInfo {
        CpuFeatures features;
        std::size_t cache_line_size{64};
        std::size_t logical_cores{1};
        std::size_t physical_cores{1};
    };

    struct MemoryInfo {
        std::uint64_t total_bytes{0};
        std::uint64_t available_bytes{0};
    };

    [[nodiscard]] constexpr std::string_view ToString(OS os) noexcept {
        switch (os) {
        case OS::Windows:
            return "Windows";
        case OS::Linux:
            return "Linux";
        case OS::MacOS:
            return "macOS";
        case OS::FreeBSD:
            return "FreeBSD";
        case OS::OpenBSD:
            return "OpenBSD";
        case OS::NetBSD:
            return "NetBSD";
        case OS::DragonFlyBSD:
            return "Dragonfly";
        case OS::Solaris:
            return "Solaris";
        case OS::Illumos:
            return "Illumos";
        default:
            return "unknown";
        }
    }

    [[nodiscard]] constexpr std::string_view ToString(Arch arch) noexcept {
        switch (arch) {
        case Arch::X86_32:
            return "x86";
        case Arch::X86_64:
            return "x64";
        case Arch::ARM32:
            return "arm32";
        case Arch::ARM64:
            return "aarch64";
        case Arch::RISCV32:
            return "riscv32";
        case Arch::RISCV64:
            return "riscv64";
        default:
            return "unknown";
        }
    }

    [[nodiscard]] constexpr bool Is64Bit(Arch arch) noexcept {
        switch (arch) {
        case Arch::X86_64:
        case Arch::ARM64:
        case Arch::RISCV64:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] constexpr std::size_t GetPointerSize(Arch arch) noexcept {
        return Is64Bit(arch) ? Pointer64 : Pointer32;
    }
} // namespace Rux::Platform
