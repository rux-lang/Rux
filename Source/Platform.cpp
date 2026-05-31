/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#include "Rux/Platform.h"

#include <array>
#include <thread>
#include <vector>

#if RUX_OS_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif RUX_OS_LINUX || RUX_IS_SUNOS
#  include <sys/sysinfo.h>
#  include <unistd.h>
#elif RUX_IS_BSD
#  include <sys/sysctl.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#if RUX_ARCH_X64 || RUX_ARCH_X86
#  if RUX_COMPILER_MSVC
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#endif

namespace Rux::Platform {

    [[nodiscard]] MemoryInfo QueryMemory() noexcept {
        static const auto static_info = []() {
            MemoryInfo m{};
#if RUX_OS_WINDOWS
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            m.page_size = si.dwPageSize;
            MEMORYSTATUSEX mem{};
            mem.dwLength = sizeof(mem);
            GlobalMemoryStatusEx(&mem);
            m.total_ram = mem.ullTotalPhys;
#elif RUX_OS_LINUX
            m.page_size = sysconf(_SC_PAGESIZE);
            struct sysinfo info;
            if (sysinfo(&info) == 0) {
                m.total_ram = static_cast<std::uint64_t>(info.totalram) * info.mem_unit;
            }
#elif RUX_IS_SUNOS
            m.page_size = sysconf(_SC_PAGESIZE);
            m.total_ram = static_cast<std::uint64_t>(sysconf(_SC_PHYS_PAGES)) * m.page_size;
#elif RUX_IS_BSD
            m.page_size = sysconf(_SC_PAGESIZE);
            std::uint64_t size = 0;
            size_t len = sizeof(size);
#  if RUX_OS_MACOS
            sysctlbyname("hw.memsize", &size, &len, nullptr, 0);
#  else
            int mib[2] = {CTL_HW, HW_PHYSMEM};
            sysctl(mib, 2, &size, &len, nullptr, 0);
#  endif
            m.total_ram = size;
#endif
            return m;
        }();

        MemoryInfo m = static_info;

#if RUX_OS_WINDOWS
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        m.available_ram = mem.ullAvailPhys;
#elif RUX_OS_LINUX
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            m.available_ram = static_cast<std::uint64_t>(info.freeram) * info.mem_unit;
        }
#elif RUX_IS_SUNOS
        m.available_ram = static_cast<std::uint64_t>(sysconf(_SC_AVPHYS_PAGES)) * m.page_size;
#elif RUX_IS_BSD
        m.available_ram = m.total_ram; // Accurate live RAM on BSDs requires deeper parsing
#endif
        return m;
    }

    [[nodiscard]] const CpuTopology& QueryTopology() noexcept {
        static const CpuTopology t = []() {
            CpuTopology top{};
            top.logical = std::max(1u, std::thread::hardware_concurrency());

#if RUX_OS_WINDOWS
            DWORD len = 0;
            GetLogicalProcessorInformation(nullptr, &len);
            std::vector<std::uint8_t> buffer(len);
            auto* ptr = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buffer.data());

            if (GetLogicalProcessorInformation(ptr, &len)) {
                std::uint32_t cores = 0;
                size_t count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                for (size_t i = 0; i < count; i++) {
                    if (ptr[i].Relationship == RelationProcessorCore) cores++;
                }
                top.physical = cores > 0 ? cores : top.logical;
            }
            else {
                top.physical = top.logical;
            }
#elif RUX_OS_LINUX || RUX_IS_SUNOS
            top.logical = sysconf(_SC_NPROCESSORS_ONLN);
            top.physical = top.logical / 2; // Heuristic fallback
#elif RUX_IS_BSD
            int mib[2] = {CTL_HW, HW_NCPU};
            size_t len = sizeof(top.logical);
            if (sysctl(mib, 2, &top.logical, &len, nullptr, 0) != 0) {
                top.logical = 1;
            }
            top.physical = top.logical; // Simplistic fallback
#else
            top.physical = top.logical;
#endif
            top.smt = (top.logical > top.physical);
            return top;
        }();
        return t;
    }

#if RUX_ARCH_X64 || RUX_ARCH_X86
    inline void cpuid(int leaf, int subleaf, std::array<int, 4>& r) noexcept {
#  if RUX_COMPILER_MSVC
        __cpuidex(r.data(), leaf, subleaf);
#  else
        __cpuid_count(leaf, subleaf, r[0], r[1], r[2], r[3]);
#  endif
    }

    [[nodiscard]] CpuFeatures DetectRuntimeCpuFeatures() noexcept {
        static const CpuFeatures features = []() {
            CpuFeatures f{};
            std::array<int, 4> r{};

            cpuid(1, 0, r);
            if (r[3] & (1 << 26)) f |= CpuFeature::SSE2;
            if (r[2] & (1 << 20)) f |= CpuFeature::SSE4_2;
            if (r[2] & (1 << 28)) f |= CpuFeature::AVX;

            cpuid(7, 0, r);
            if (r[1] & (1 << 5)) f |= CpuFeature::AVX2;
            if (r[1] & (1 << 16)) f |= CpuFeature::AVX512;
            if (r[1] & (1 << 3)) f |= CpuFeature::BMI1;
            if (r[1] & (1 << 8)) f |= CpuFeature::BMI2;
            return f;
        }();
        return features;
    }
#else
    [[nodiscard]] CpuFeatures DetectRuntimeCpuFeatures() noexcept {
        return DetectCompileTimeCpuFeatures();
    }
#endif

} // namespace Rux::Platform
