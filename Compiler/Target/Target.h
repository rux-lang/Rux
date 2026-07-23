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
inline constexpr std::size_t Pointer32 = 4;
inline constexpr std::size_t Pointer64 = 8;

// ---- Enumerations -----------------------------------------------------------

enum class OS : std::uint8_t {
    Unknown = 0,
    AIX = 1,
    Android = 2,
    DragonFlyBSD = 3,
    FreeBSD = 4,
    Fuchsia = 5,
    Haiku = 6,
    Illumos = 7,
    iOS = 8,
    Linux = 9,
    MacOS = 10,
    NetBSD = 11,
    OpenBSD = 12,
    QNX = 13,
    Redox = 14,
    Solaris = 15,
    Windows = 16,
};

enum class Arch : std::uint8_t {
    Unknown = 0,
    ARM32 = 1,
    AArch64 = 2,
    RISCV32 = 3,
    RISCV64 = 4,
    X86_32 = 5,
    X86_64 = 6,
};

enum class DataModel : std::uint8_t {
    Unknown = 0,
    ILP32 = 1,
    LLP64 = 2,
    LP64 = 3,
};

enum class ABI : std::uint8_t {
    Unknown = 0,
    AAPCS = 1,
    AAPCS64 = 2,
    RISCV_ILP32 = 3,
    RISCV_LP64 = 4,
    SystemV = 5,
    WindowsX64 = 6,
    WindowsX86 = 7,
};

enum class CallingConv : std::uint8_t {
    Default,
    C,
    SysV,
    Win64,
    StdCall,
    AAPCS,
    AAPCS64,
    RISCV,
};

enum class Compiler : std::uint8_t {
    Unknown,
    MSVC,
    Clang,
    GCC,
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
    Wasm = 4,
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

// ---- Enum helpers -----------------------------------------------------------

[[nodiscard]] constexpr std::string_view ToString(OS os) noexcept {
    switch (os) {
    case OS::AIX:
        return "AIX";
    case OS::Android:
        return "Android";
    case OS::DragonFlyBSD:
        return "Dragonfly";
    case OS::FreeBSD:
        return "FreeBSD";
    case OS::Fuchsia:
        return "Fuchsia";
    case OS::Haiku:
        return "Haiku";
    case OS::Illumos:
        return "Illumos";
    case OS::iOS:
        return "iOS";
    case OS::Linux:
        return "Linux";
    case OS::MacOS:
        return "macOS";
    case OS::NetBSD:
        return "NetBSD";
    case OS::OpenBSD:
        return "OpenBSD";
    case OS::QNX:
        return "QNX";
    case OS::Redox:
        return "Redox";
    case OS::Solaris:
        return "Solaris";
    case OS::Windows:
        return "Windows";
    default:
        return "unknown";
    }
}

[[nodiscard]] constexpr std::string_view ToString(Arch arch) noexcept {
    switch (arch) {
    case Arch::X86_32:
        return "x86";
    case Arch::X86_64:
        return "x86_64";
    case Arch::ARM32:
        return "arm32";
    case Arch::AArch64:
        return "aarch64";
    case Arch::RISCV32:
        return "riscv32";
    case Arch::RISCV64:
        return "riscv64";
    default:
        return "unknown";
    }
}

[[nodiscard]] constexpr std::string_view ToDisplayString(Arch arch) noexcept {
    switch (arch) {
    case Arch::X86_64:
        return "x86-64";
    case Arch::AArch64:
        return "AArch64";
    default:
        return ToString(arch);
    }
}

[[nodiscard]] constexpr bool Is64Bit(Arch arch) noexcept {
    switch (arch) {
    case Arch::X86_64:
    case Arch::AArch64:
    case Arch::RISCV64:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr std::size_t GetPointerSize(Arch arch) noexcept {
    return Is64Bit(arch) ? Pointer64 : Pointer32;
}

[[nodiscard]] constexpr ObjectFormat GetObjectFormat(OS os) noexcept {
    switch (os) {
    case OS::Windows:
        return ObjectFormat::COFF;
    case OS::MacOS:
        return ObjectFormat::MachO;
    case OS::DragonFlyBSD:
    case OS::FreeBSD:
    case OS::Illumos:
    case OS::Linux:
    case OS::NetBSD:
    case OS::OpenBSD:
    case OS::Solaris:
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
    // x86_64
    if (arch == Arch::X86_64) {
        if (os == OS::Windows && model == DataModel::LLP64) {
            return {ABI::WindowsX64, CallingConv::Win64, true, 16};
        }
        if (model == DataModel::LP64) {
            // Linux, macOS, BSDs
            return {ABI::SystemV, CallingConv::SysV, false, 16};
        }
    }

    // x86_32
    if (arch == Arch::X86_32) {
        if (os == OS::Windows) {
            return {ABI::WindowsX86, CallingConv::StdCall, false, 4};
        }
        if (os == OS::Linux) {
            return {ABI::SystemV, CallingConv::C, false, 4};
        }
    }

    // ARM
    if (arch == Arch::AArch64) {
        return {ABI::AAPCS64, CallingConv::AAPCS64, false, 16}; // Applies to Win/Lin/Mac
    }
    if (arch == Arch::ARM32) {
        return {ABI::AAPCS, CallingConv::AAPCS, false, 8};
    }

    // RISC-V
    if (arch == Arch::RISCV64) {
        return {ABI::RISCV_LP64, CallingConv::RISCV, false, 16};
    }
    if (arch == Arch::RISCV32) {
        return {ABI::RISCV_ILP32, CallingConv::RISCV, false, 16};
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
    if constexpr (RUX_OS_OPENBSD) {
        return OS::OpenBSD;
    }
    if constexpr (RUX_OS_NETBSD) {
        return OS::NetBSD;
    }
    if constexpr (RUX_OS_DRAGONFLY) {
        return OS::DragonFlyBSD;
    }
    if constexpr (RUX_OS_SOLARIS) {
        return OS::Solaris;
    }
    if constexpr (RUX_OS_ILLUMOS) {
        return OS::Illumos;
    }
    return OS::Unknown;
}();

inline constexpr Arch HostArch = []() noexcept {
    if constexpr (RUX_ARCH_X86_64) {
        return Arch::X86_64;
    }
    if constexpr (RUX_ARCH_X86) {
        return Arch::X86_32;
    }
    if constexpr (RUX_ARCH_AARCH64) {
        return Arch::AArch64;
    }
    if constexpr (RUX_ARCH_ARM32) {
        return Arch::ARM32;
    }
    if constexpr (RUX_ARCH_RISCV64) {
        return Arch::RISCV64;
    }
    if constexpr (RUX_ARCH_RISCV32) {
        return Arch::RISCV32;
    }
    return Arch::Unknown;
}();

inline constexpr DataModel HostDataModel = []() noexcept {
    if constexpr (RUX_OS_WINDOWS) {
        return (RUX_ARCH_X86_64 || RUX_ARCH_AARCH64) ? DataModel::LLP64 : DataModel::ILP32;
    }
    else {
        return (RUX_ARCH_X86_64 || RUX_ARCH_AARCH64 || RUX_ARCH_RISCV64) ? DataModel::LP64 : DataModel::ILP32;
    }
}();

inline constexpr std::size_t HostPointerSize = GetPointerSize(HostArch);

inline constexpr Compiler HostCompiler = []() noexcept {
    if constexpr (RUX_COMPILER_MSVC) {
        return Compiler::MSVC;
    }
    if constexpr (RUX_COMPILER_CLANG) {
        return Compiler::Clang;
    }
    if constexpr (RUX_COMPILER_GCC) {
        return Compiler::GCC;
    }
    return Compiler::Unknown;
}();

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
    if constexpr (RUX_FEATURE_RVV) {
        f |= CpuFeature::RVV;
    }
    return f;
}();

inline constexpr ABIInfo HostABIDetails = GetABIInfo(HostOS, HostArch, HostDataModel);
inline constexpr ABI HostABI = HostABIDetails.abi;
inline constexpr CallingConv HostCC = HostABIDetails.cc;
} // namespace Rux::Target

namespace Rux {
// Fully resolved description of the machine code is being generated for. Created
// from the host today; a cross-compilation front end can populate it explicitly.
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
