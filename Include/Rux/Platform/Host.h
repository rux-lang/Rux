/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#pragma once

#include "ABI.h"
#include "Defines.h"
#include "Types.h"

namespace Rux::Platform {

    inline constexpr OS HostOS = []() noexcept {
        if constexpr (RUX_OS_WINDOWS) return OS::Windows;
        if constexpr (RUX_OS_LINUX) return OS::Linux;
        if constexpr (RUX_OS_MACOS) return OS::MacOS;
        if constexpr (RUX_OS_FREEBSD) return OS::FreeBSD;
        if constexpr (RUX_OS_OPENBSD) return OS::OpenBSD;
        if constexpr (RUX_OS_NETBSD) return OS::NetBSD;
        if constexpr (RUX_OS_DRAGONFLY) return OS::DragonFlyBSD;
        if constexpr (RUX_OS_SOLARIS) return OS::Solaris;
        if constexpr (RUX_OS_ILLUMOS) return OS::Illumos;
        return OS::Unknown;
    }();

    inline constexpr Arch HostArch = []() noexcept {
        if constexpr (RUX_ARCH_X64) return Arch::X86_64;
        if constexpr (RUX_ARCH_X86) return Arch::X86_32;
        if constexpr (RUX_ARCH_ARM64) return Arch::ARM64;
        if constexpr (RUX_ARCH_ARM32) return Arch::ARM32;
        if constexpr (RUX_ARCH_RISCV64) return Arch::RISCV64;
        if constexpr (RUX_ARCH_RISCV32) return Arch::RISCV32;
        return Arch::Unknown;
    }();

    inline constexpr DataModel HostDataModel = []() noexcept {
        if constexpr (RUX_OS_WINDOWS) {
            return (RUX_ARCH_X64 || RUX_ARCH_ARM64) ? DataModel::LLP64 : DataModel::ILP32;
        }
        else {
            return (RUX_ARCH_X64 || RUX_ARCH_ARM64 || RUX_ARCH_RISCV64) ? DataModel::LP64 : DataModel::ILP32;
        }
    }();

    inline constexpr std::size_t HostPointerSize = GetPointerSize(HostArch);

    inline constexpr Compiler HostCompiler = []() noexcept {
        if constexpr (RUX_COMPILER_MSVC) return Compiler::MSVC;
        if constexpr (RUX_COMPILER_CLANG) return Compiler::Clang;
        if constexpr (RUX_COMPILER_GCC) return Compiler::GCC;
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
        if constexpr (RUX_FEATURE_SSE2) f |= CpuFeature::SSE2;
        if constexpr (RUX_FEATURE_AVX) f |= CpuFeature::AVX;
        if constexpr (RUX_FEATURE_AVX2) f |= CpuFeature::AVX2;
        if constexpr (RUX_FEATURE_AVX512) f |= CpuFeature::AVX512;
        if constexpr (RUX_FEATURE_NEON) f |= CpuFeature::NEON;
        if constexpr (RUX_FEATURE_SVE) f |= CpuFeature::SVE;
        if constexpr (RUX_FEATURE_RVV) f |= CpuFeature::RVV;
        return f;
    }();

    inline constexpr ABIInfo HostABIDetails = GetABIInfo(HostOS, HostArch, HostDataModel);
    inline constexpr ABI HostABI = HostABIDetails.abi;
    inline constexpr CallingConv HostCC = HostABIDetails.cc;
} // namespace Rux::Platform
