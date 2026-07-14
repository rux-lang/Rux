#pragma once

// Runtime queries about the executing machine.
//
// Unlike the compile-time `Host*` constants in "Target/Target.h", these functions
// inspect the actual hardware at run time (CPUID, sysctl, /proc, ...). Results
// are cached, so repeated calls are cheap. Implemented in Source/Host.cpp.

#include "Target/Target.h"

namespace Rux::System {
// CPU feature flags, cache line size, and core counts of the executing machine.
[[nodiscard]] Target::RuntimeCpuInfo GetRuntimeCpuInfo() noexcept;

// Total and available physical RAM of the executing machine.
[[nodiscard]] Target::MemoryInfo GetRuntimeMemoryInfo() noexcept;

// True if the executing CPU supports every feature in `feature_mask`.
[[nodiscard]] bool HostSupports(Target::CpuFeatures feature_mask) noexcept;
} // namespace Rux::System
