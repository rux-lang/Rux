#pragma once

#include "Defines.h"
#include "Host.h"
#include "Types.h"

namespace Rux {
    struct TargetContext {
        Platform::OS os;
        Platform::Arch arch;
        Platform::DataModel data_model;
        Platform::ABI abi;
        Platform::CallingConv default_cc;
        Platform::Endian endianness;
        std::size_t pointer_size;
        Platform::CpuFeatures cpu_features;

        [[nodiscard]]
        static TargetContext CreateNative() noexcept {
            return TargetContext{.os = Platform::HostOS,
                                 .arch = Platform::HostArch,
                                 .data_model = Platform::HostDataModel,
                                 .abi = Platform::HostABI,
                                 .default_cc = Platform::HostCC,
                                 .endianness = Platform::HostEndianness,
                                 .pointer_size = Platform::HostPointerSize,
                                 .cpu_features = Platform::HostCpuFeatures};
        }
    };
} // namespace Rux
