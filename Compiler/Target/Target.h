#pragma once

// Typed description of a compilation target (and, as a special case, the host).
//
// This is the C++ counterpart to the macros in "Target/Platform.h": strongly typed
// enums for OS / architecture / ABI, the CpuFeatures bitset, the compile-time
// `Host*` constants describing the machine the compiler itself runs on, and the
// `TargetContext` aggregate consumed by the back end. Everything here is
// `constexpr` and free of system headers.

#include "Target/Platform.h"

#include <cstdint>
#include <string_view>

namespace Rux::Target {
inline constexpr std::size_t CacheLineSize = 64;
inline constexpr std::size_t Pointer64 = 8;

// ---- Enumerations -----------------------------------------------------------
//
// Rux supports four operating systems and two architectures. Every enumerator
// below names something a supported target can actually produce; a name no
// build can reach does not belong here.

enum class OS : std::uint8_t {
    Unknown = 0,
    FreeBSD = 1,
    Linux = 2,
    MacOS = 3,
    Windows = 4,
};

enum class Arch : std::uint8_t {
    Unknown = 0,
    AArch64 = 1,
    X86_64 = 2,
};

enum class DataModel : std::uint8_t {
    Unknown = 0,
    LLP64 = 1,
    LP64 = 2,
};

enum class ABI : std::uint8_t {
    Unknown = 0,
    AAPCS64 = 1,
    SystemV = 2,
    WindowsX64 = 3,
};

enum class CallingConv : std::uint8_t {
    Default,
    C,
    SysV,
    Win64,
    AAPCS64,
};

enum class BuildMode : std::uint8_t {
    Debug = 0,
    Release = 1,
};

enum class Endian : std::uint8_t {
    Big = 0,
    Little = 1,
};

enum class ObjectFormat : std::uint8_t {
    Unknown = 0,
    COFF = 1,
    ELF = 2,
    MachO = 3,
};

// ---- CPU feature bitset -----------------------------------------------------

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

    constexpr CpuFeatures &operator|=(CpuFeatures other) noexcept {
        mask |= other.mask;
        return *this;
    }
};

namespace CpuFeature {
inline constexpr CpuFeatures None{0};

// x86
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

// ---- Enum helpers -----------------------------------------------------------

[[nodiscard]] constexpr std::string_view ToString(OS os) noexcept {
    switch (os) {
    case OS::FreeBSD:
        return "FreeBSD";
    case OS::Linux:
        return "Linux";
    case OS::MacOS:
        return "macOS";
    case OS::Windows:
        return "Windows";
    default:
        return "unknown";
    }
}

[[nodiscard]] constexpr std::string_view ToString(Arch arch) noexcept {
    switch (arch) {
    case Arch::AArch64:
        return "aarch64";
    case Arch::X86_64:
        return "x86_64";
    default:
        return "unknown";
    }
}

[[nodiscard]] constexpr std::string_view ToDisplayString(Arch arch) noexcept {
    switch (arch) {
    case Arch::AArch64:
        return "AArch64";
    case Arch::X86_64:
        return "x86-64";
    default:
        return ToString(arch);
    }
}

/// Every supported architecture is 64-bit, so pointer size does not vary.
[[nodiscard]] constexpr std::size_t GetPointerSize(Arch) noexcept {
    return Pointer64;
}

[[nodiscard]] constexpr ObjectFormat GetObjectFormat(OS os) noexcept {
    switch (os) {
    case OS::Windows:
        return ObjectFormat::COFF;
    case OS::MacOS:
        return ObjectFormat::MachO;
    case OS::FreeBSD:
    case OS::Linux:
        return ObjectFormat::ELF;
    default:
        return ObjectFormat::Unknown;
    }
}

// ---- ABI -------------------------------------------------------------------

struct ABIInfo {
    ABI abi{ABI::Unknown};
    CallingConv cc{CallingConv::Default};
    bool shadow_space{false};
    std::size_t stack_alignment{0};
};

[[nodiscard]] constexpr ABIInfo GetABIInfo(OS os, Arch arch, DataModel model) noexcept {
    if (arch == Arch::X86_64) {
        if (os == OS::Windows && model == DataModel::LLP64) {
            return {ABI::WindowsX64, CallingConv::Win64, true, 16};
        }
        if (model == DataModel::LP64) {
            // FreeBSD, Linux, macOS
            return {ABI::SystemV, CallingConv::SysV, false, 16};
        }
    }

    if (arch == Arch::AArch64) {
        return {ABI::AAPCS64, CallingConv::AAPCS64, false, 16}; // Applies to every supported OS
    }

    return {ABI::Unknown, CallingConv::Default, false, 0};
}

// ---- Host constants ---------------------------------------------------------
//
// Compile-time description of the machine this compiler binary runs on, derived
// directly from the macros in "Target/Platform.h".

inline constexpr OS HostOS = []() noexcept {
    if constexpr (RUX_OS_WINDOWS) {
        return OS::Windows;
    }
    if constexpr (RUX_OS_LINUX) {
        return OS::Linux;
    }
    if constexpr (RUX_OS_MACOS) {
        return OS::MacOS;
    }
    if constexpr (RUX_OS_FREEBSD) {
        return OS::FreeBSD;
    }
    return OS::Unknown;
}();

inline constexpr Arch HostArch = []() noexcept {
    if constexpr (RUX_ARCH_X86_64) {
        return Arch::X86_64;
    }
    if constexpr (RUX_ARCH_AARCH64) {
        return Arch::AArch64;
    }
    return Arch::Unknown;
}();

inline constexpr DataModel HostDataModel = RUX_OS_WINDOWS ? DataModel::LLP64 : DataModel::LP64;

inline constexpr std::size_t HostPointerSize = GetPointerSize(HostArch);

inline constexpr BuildMode HostBuildMode = RUX_BUILD_RELEASE ? BuildMode::Release : BuildMode::Debug;

inline constexpr Endian HostEndianness = []() noexcept {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return Endian::Big;
#else
    return Endian::Little;
#endif
}();

inline constexpr CpuFeatures HostCpuFeatures = []() noexcept {
    CpuFeatures f = CpuFeature::None;
    if constexpr (RUX_FEATURE_SSE2) {
        f |= CpuFeature::SSE2;
    }
    if constexpr (RUX_FEATURE_AVX) {
        f |= CpuFeature::AVX;
    }
    if constexpr (RUX_FEATURE_AVX2) {
        f |= CpuFeature::AVX2;
    }
    if constexpr (RUX_FEATURE_AVX512) {
        f |= CpuFeature::AVX512;
    }
    if constexpr (RUX_FEATURE_NEON) {
        f |= CpuFeature::NEON;
    }
    if constexpr (RUX_FEATURE_SVE) {
        f |= CpuFeature::SVE;
    }
    return f;
}();

inline constexpr ABIInfo HostABIDetails = GetABIInfo(HostOS, HostArch, HostDataModel);
inline constexpr ABI HostABI = HostABIDetails.abi;
inline constexpr CallingConv HostCC = HostABIDetails.cc;
} // namespace Rux::Target

namespace Rux {
/// Fully resolved description of the machine code is being generated for. Created from the host today; a
/// cross-compilation front end can populate it explicitly.
struct TargetContext {
    Target::OS os;
    Target::Arch arch;
    Target::DataModel data_model;
    Target::ABI abi;
    Target::CallingConv default_cc;
    Target::Endian endianness;
    Target::ObjectFormat object_format;
    std::size_t pointer_size;
    Target::CpuFeatures cpu_features;

    [[nodiscard]]
    static TargetContext CreateNative() noexcept {
        return TargetContext{.os = Target::HostOS,
                             .arch = Target::HostArch,
                             .data_model = Target::HostDataModel,
                             .abi = Target::HostABI,
                             .default_cc = Target::HostCC,
                             .endianness = Target::HostEndianness,
                             .object_format = Target::GetObjectFormat(Target::HostOS),
                             .pointer_size = Target::HostPointerSize,
                             .cpu_features = Target::HostCpuFeatures};
    }
};
} // namespace Rux
