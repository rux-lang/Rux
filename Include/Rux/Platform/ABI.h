// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#pragma once
#include "Types.h"

namespace Rux::Platform {

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
        if (model == DataModel::LP64) { // Linux, MacOS, BSDs
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
    if (arch == Arch::ARM64) {
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

} // namespace Rux::Platform
