#include "System/Host.h"

#include "System/WinApi.h"

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

#if RUX_OS_LINUX
    #include <sys/auxv.h>
    #include <sys/sysinfo.h>
    #include <unistd.h>
#elif RUX_OS_MACOS
    #include <mach/mach.h>
    #include <mach/mach_host.h>
    #include <sys/sysctl.h>
    #include <unistd.h>
#elif RUX_IS_BSD
    #include <sys/sysctl.h>
    #include <unistd.h>
#elif RUX_IS_SUNOS
    #include <unistd.h>
#endif

#if RUX_ARCH_X86 || RUX_ARCH_X64
    #if RUX_COMPILER_MSVC
        #include <intrin.h>
    #else
        #include <cpuid.h>
    #endif
#endif

namespace Rux::System {
using namespace Target;

namespace {
#if RUX_ARCH_X86 || RUX_ARCH_X64

[[nodiscard]] inline bool HasOSXSAVE() noexcept {
    int r[4]{};

    #if RUX_COMPILER_MSVC
    __cpuid(r, 1);
    #else
    __cpuid(1, r[0], r[1], r[2], r[3]);
    #endif

    return r[2] & (1 << 27);
}

[[nodiscard]] inline uint64_t XGetBV0() noexcept {
    #if RUX_COMPILER_MSVC
    return _xgetbv(0);
    #else
    uint32_t a, d;
    __asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    return (uint64_t(d) << 32) | a;
    #endif
}

#endif

[[nodiscard]] CpuFeatures DetectCpuFeaturesImpl() noexcept {
    CpuFeatures f = CpuFeature::None;

#if RUX_ARCH_X86 || RUX_ARCH_X64

    int r[4]{};

    #if RUX_COMPILER_MSVC
    __cpuid(r, 1);
    #else
    __cpuid(1, r[0], r[1], r[2], r[3]);
    #endif

    if (r[3] & (1 << 26)) {
        f |= CpuFeature::SSE2;
    }
    if (r[2] & (1 << 0)) {
        f |= CpuFeature::SSE3;
    }
    if (r[2] & (1 << 9)) {
        f |= CpuFeature::SSSE3;
    }
    if (r[2] & (1 << 19)) {
        f |= CpuFeature::SSE41;
    }
    if (r[2] & (1 << 20)) {
        f |= CpuFeature::SSE42;
    }

    // AVX requires OS support
    const bool avx_hw = r[2] & (1 << 28);

    if (avx_hw && HasOSXSAVE()) {
        uint64_t xcr0 = XGetBV0();
        if ((xcr0 & 0x6) == 0x6) {
            f |= CpuFeature::AVX;
        }
    }

    #if RUX_COMPILER_MSVC
    __cpuidex(r, 7, 0);
    #else
    __cpuid_count(7, 0, r[0], r[1], r[2], r[3]);
    #endif

    if (r[1] & (1 << 5)) {
        f |= CpuFeature::AVX2;
    }

    // NOTE: simplified AVX-512 detection (still OS-dependent in real
    // systems)
    if (r[1] & (1 << 16)) {
        f |= CpuFeature::AVX512;
    }

#elif RUX_ARCH_ARM64 || RUX_ARCH_ARM32

    #if RUX_OS_LINUX
    unsigned long hw = getauxval(AT_HWCAP);
    if (hw & HWCAP_ASIMD) {
        f |= CpuFeature::NEON;
    }
    if (hw & HWCAP_SVE) {
        f |= CpuFeature::SVE;
    }
    #elif RUX_OS_MACOS
    f |= CpuFeature::NEON;
    #else
    f = HostCpuFeatures;
    #endif

#elif RUX_ARCH_RISCV64 || RUX_ARCH_RISCV32

    #if RUX_OS_LINUX
    unsigned long hw = getauxval(AT_HWCAP);
    if (hw & (1 << ('V' - 'A'))) {
        f |= CpuFeature::RVV;
    }
    #else
    f = HostCpuFeatures;
    #endif

#else
    f = HostCpuFeatures;
#endif

    return f;
}

[[nodiscard]] RuntimeCpuInfo DetectRuntimeCpuInfo() noexcept {
    RuntimeCpuInfo info{};

    info.logical_cores = (std::max)(1u, std::thread::hardware_concurrency());

    info.features = DetectCpuFeaturesImpl();

#if RUX_OS_WINDOWS

    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);

    std::vector<uint8_t> buffer(len);

    if (len > 0 &&
        GetLogicalProcessorInformation(reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION *>(buffer.data()), &len)) {
        auto *entries = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION *>(buffer.data());

        size_t count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);

        for (size_t i = 0; i < count; ++i) {
            const auto &e = entries[i];

            if (e.Relationship == RelationProcessorCore) {
                ++info.physical_cores;
            }

            else if (e.Relationship == RelationCache && e.Cache.Level == 1 && e.Cache.Type == CacheData) {
                info.cache_line_size = e.Cache.LineSize;
            }
        }
    }

#elif RUX_OS_LINUX

    {
        long v = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
        info.cache_line_size = (v > 0) ? (uint32_t)v : 0;
    }

    info.physical_cores = info.logical_cores;

#elif RUX_IS_SUNOS

    info.physical_cores = info.logical_cores;

#elif RUX_OS_MACOS || (RUX_IS_BSD && !RUX_OS_OPENBSD)

    size_t s = sizeof(info.physical_cores);
    sysctlbyname("hw.physicalcpu", &info.physical_cores, &s, nullptr, 0);

    s = sizeof(info.cache_line_size);
    sysctlbyname("hw.cachelinesize", &info.cache_line_size, &s, nullptr, 0);

#elif RUX_OS_OPENBSD

    int mib_cores[2] = {CTL_HW, HW_NCPU};
    size_t s = sizeof(info.physical_cores);
    sysctl(mib_cores, 2, &info.physical_cores, &s, nullptr, 0);

    #ifdef HW_CACHELINE
    int mib_cache[2] = {CTL_HW, HW_CACHELINE};
    s = sizeof(info.cache_line_size);
    sysctl(mib_cache, 2, &info.cache_line_size, &s, nullptr, 0);
    #endif

#endif

    if (!info.cache_line_size) {
        info.cache_line_size = CacheLineSize;
    }

    if (!info.physical_cores) {
        info.physical_cores = info.logical_cores;
    }

    return info;
}

[[nodiscard]] const RuntimeCpuInfo &CachedCpuInfo() noexcept {
    static const RuntimeCpuInfo info = DetectRuntimeCpuInfo();
    return info;
}
} // namespace

RuntimeCpuInfo GetRuntimeCpuInfo() noexcept {
    return CachedCpuInfo();
}

MemoryInfo GetRuntimeMemoryInfo() noexcept {
    MemoryInfo info{};

#if RUX_OS_WINDOWS

    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);

    if (GlobalMemoryStatusEx(&m)) {
        info.total_bytes = m.ullTotalPhys;
        info.available_bytes = m.ullAvailPhys;
    }

#elif RUX_OS_LINUX

    struct sysinfo s{};
    if (sysinfo(&s) == 0) {
        info.total_bytes = uint64_t(s.totalram) * s.mem_unit;

        info.available_bytes = uint64_t(s.freeram) * s.mem_unit;
    }

#elif RUX_IS_SUNOS

    {
        long pages = sysconf(_SC_PHYS_PAGES);
        long avpages = sysconf(_SC_AVPHYS_PAGES);
        long psize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && psize > 0) {
            info.total_bytes = uint64_t(pages) * uint64_t(psize);
            info.available_bytes = uint64_t(avpages > 0 ? avpages : pages) * uint64_t(psize);
        }
    }

#elif RUX_OS_MACOS

    int mib[2] = {CTL_HW, HW_MEMSIZE};
    size_t len = sizeof(info.total_bytes);

    sysctl(mib, 2, &info.total_bytes, &len, nullptr, 0);

    vm_statistics64 vm{};
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;

    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &c) == KERN_SUCCESS) {
        auto page = sysconf(_SC_PAGESIZE);
        info.available_bytes = uint64_t(vm.free_count + vm.inactive_count) * page;
    }
    else {
        info.available_bytes = info.total_bytes;
    }

#elif RUX_IS_BSD

    #if defined(HW_MEMSIZE)
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    #elif defined(HW_REALMEM)
    int mib[2] = {CTL_HW, HW_REALMEM};
    #else
    int mib[2] = {CTL_HW, HW_PHYSMEM};
    #endif
    size_t len = sizeof(info.total_bytes);

    sysctl(mib, 2, &info.total_bytes, &len, nullptr, 0);
    info.available_bytes = info.total_bytes;

#endif

    return info;
}

bool HostSupports(CpuFeatures f) noexcept {
    return CachedCpuInfo().features.Has(f);
}
} // namespace Rux::System
