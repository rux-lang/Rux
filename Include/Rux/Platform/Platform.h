// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "./Types.h"

namespace Rux::Platform {

    // Retrieves the CPU features, cache line size, and core counts of the
    // executing machine.
    [[nodiscard]] RuntimeCpuInfo GetRuntimeCpuInfo() noexcept;

    // Retrieves the available and total RAM of the host machine.
    [[nodiscard]] MemoryInfo GetRuntimeMemoryInfo() noexcept;

    // Helper to check if the current host CPU supports a specific feature set.
    [[nodiscard]] bool HostSupports(CpuFeatures feature_mask) noexcept;

} // namespace Rux::Platform
