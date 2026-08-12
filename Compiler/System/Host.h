#pragma once

// Runtime queries about the executing machine.
//
// Unlike the compile-time `Host*` constants in "Target/Target.h", these functions
// inspect the actual hardware at run time (CPUID, sysctl, /proc, ...). Results
// are cached, so repeated calls are cheap. Implemented in Source/Host.cpp.

#include "Target/Target.h"

namespace Rux::System {
// Architectures relevant to child-process execution. `processArch` describes
// this compiler binary; `nativeArch` describes the operating system's native
// machine, which can differ when the compiler runs under translation.
struct HostArchitectureInfo {
    Target::Arch processArch = Target::Arch::Unknown;
    Target::Arch nativeArch = Target::Arch::Unknown;
};

// Runtime architecture information for the current host. On Windows this uses
// the native machine reported by IsWow64Process2, with native-system
// information as a compatibility fallback; on macOS it detects Rosetta. An
// x86-64 process translated on either OS therefore reports AArch64 as
// `nativeArch`. Other hosts report the process architecture for both fields.
[[nodiscard]] HostArchitectureInfo GetHostArchitectureInfo() noexcept;

// CPU feature flags, cache line size, and core counts of the executing machine.
[[nodiscard]] Target::RuntimeCpuInfo GetRuntimeCpuInfo() noexcept;

// Total and available physical RAM of the executing machine.
[[nodiscard]] Target::MemoryInfo GetRuntimeMemoryInfo() noexcept;

// True if the executing CPU supports every feature in `feature_mask`.
[[nodiscard]] bool HostSupports(Target::CpuFeatures feature_mask) noexcept;
} // namespace Rux::System
