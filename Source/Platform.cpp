/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Platform/Platform.h"

#include "Rux/Platform/Host.h"

#include <thread>

#if RUX_OS_WINDOWS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif RUX_OS_LINUX
#  include <sys/sysinfo.h>
#  include <unistd.h>
#elif RUX_OS_MACOS || RUX_IS_BSD
#  include <sys/sysctl.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace Rux::Platform {

    RuntimeCpuInfo GetRuntimeCpuInfo() noexcept {
        RuntimeCpuInfo info{};

        // 1. Get Logical Cores (Standard C++)
        info.logical_cores = std::thread::hardware_concurrency();
        if (info.logical_cores == 0) info.logical_cores = 1; // Fallback

        // 2. Get OS-specific Cache Line and Physical Cores
#if RUX_OS_WINDOWS
        DWORD buffer_size = 0;
        GetLogicalProcessorInformation(nullptr, &buffer_size);
        // ... (You would allocate a buffer and call it again here for deep topology)
        info.cache_line_size = 64; // Standard assumption for x64/ARM64

#elif RUX_OS_LINUX
        info.cache_line_size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
        if (info.cache_line_size <= 0) info.cache_line_size = 64;
#else
        info.cache_line_size = 64;
#endif

        // 3. Get CPU Features
        // In a real systems compiler, you'd execute the `cpuid` assembly instruction
        // for x86_64, or read `/proc/cpuinfo` / `getauxval` on ARM.
        // For now, we safely fallback to the compile-time host features:
        info.features = HostCpuFeatures;

        return info;
    }

    MemoryInfo GetRuntimeMemoryInfo() noexcept {
        MemoryInfo info{};

#if RUX_OS_WINDOWS
        MEMORYSTATUSEX mem_status;
        mem_status.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&mem_status)) {
            info.total_bytes = mem_status.ullTotalPhys;
            info.available_bytes = mem_status.ullAvailPhys;
        }

#elif RUX_OS_LINUX
        struct sysinfo sys_info;
        if (sysinfo(&sys_info) == 0) {
            info.total_bytes = static_cast<std::uint64_t>(sys_info.totalram) * sys_info.mem_unit;
            info.available_bytes = static_cast<std::uint64_t>(sys_info.freeram) * sys_info.mem_unit;
        }

#elif RUX_OS_MACOS || RUX_IS_BSD
        int mib[2] = {CTL_HW, HW_MEMSIZE};
        std::uint64_t physical_memory = 0;
        size_t length = sizeof(physical_memory);
        sysctl(mib, 2, &physical_memory, &length, nullptr, 0);
        info.total_bytes = physical_memory;

        // Available memory is complex on MacOS (vm_stat), defaulting to total for basic impl.
        info.available_bytes = physical_memory;
#endif

        return info;
    }

    bool HostSupports(CpuFeatures feature_mask) noexcept {
        // Fetch the runtime features and check if the requested mask is present
        RuntimeCpuInfo info = GetRuntimeCpuInfo();
        return info.features.Has(feature_mask);
    }

} // namespace Rux::Platform
