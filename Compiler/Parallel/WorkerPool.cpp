#include "Parallel/WorkerPool.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <algorithm>
#include <bit>
#include <cstdint>

#if RUX_OS_LINUX
    #include <sched.h>
#elif RUX_OS_FREEBSD
    #include <sys/param.h>
    #include <sys/cpuset.h>
#endif

namespace Rux::Parallel {
namespace {
/// The logical cores this process may run on, or 0 when the platform cannot tell. A build started under a restricted
/// affinity (`start /affinity`, `taskset`, `cpuset`) gets that many workers rather than one per core of the machine,
/// which would only time-slice them on the cores it was given.
std::size_t AffinityCores() noexcept {
#if RUX_OS_WINDOWS
    // A process mask describes one processor group; on a machine with several, threads may use them all.
    if (GetActiveProcessorGroupCount() > 1)
        return 0;
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) == 0)
        return 0;
    return static_cast<std::size_t>(std::popcount(static_cast<std::uint64_t>(processMask)));
#elif RUX_OS_LINUX
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        return 0;
    return static_cast<std::size_t>(CPU_COUNT(&set));
#elif RUX_OS_FREEBSD
    cpuset_t set;
    CPU_ZERO(&set);
    if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(set), &set) != 0)
        return 0;
    return static_cast<std::size_t>(CPU_COUNT(&set));
#else
    return 0;
#endif
}
} // namespace

std::size_t DefaultJobs() noexcept {
    const std::size_t cores = AffinityCores();
    if (cores != 0)
        return cores;
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

std::size_t ResolveJobs(const std::size_t requested) noexcept {
    return requested == 0 ? DefaultJobs() : requested;
}

WorkerPool::WorkerPool(const std::size_t jobs) {
    const std::size_t threads = std::max<std::size_t>(jobs, 1) - 1;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
        workers.emplace_back([this, i] { WorkerLoop(i + 1); });
}

WorkerPool::~WorkerPool() {
    {
        const std::scoped_lock lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    for (auto &worker : workers)
        worker.join();
}

void WorkerPool::ForEach(const std::size_t count, const std::function<void(std::size_t)> &body) {
    ForEachOnWorker(count, [&body](const std::size_t index, std::size_t) { body(index); });
}

void WorkerPool::ForEachOnWorker(const std::size_t count, const std::function<void(std::size_t, std::size_t)> &body) {
    if (count == 0)
        return;
    if (workers.empty() || count == 1) {
        for (std::size_t index = 0; index < count; ++index)
            body(index, 0);
        return;
    }
    {
        const std::scoped_lock lock(mutex);
        current = &body;
        total = count;
        nextIndex = 0;
        busy = 0;
        failure = nullptr;
        ++generation;
    }
    wake.notify_all();
    Drain(0);
    std::unique_lock lock(mutex);
    done.wait(lock, [this] { return (nextIndex >= total || failure) && busy == 0; });
    current = nullptr;
    total = 0;
    if (failure) {
        const auto rethrown = failure;
        failure = nullptr;
        std::rethrow_exception(rethrown);
    }
}

void WorkerPool::Drain(const std::size_t worker) {
    std::unique_lock lock(mutex);
    while (current != nullptr && nextIndex < total && !failure) {
        const auto index = nextIndex++;
        const auto *body = current;
        ++busy;
        lock.unlock();
        std::exception_ptr thrown;
        try {
            (*body)(index, worker);
        }
        catch (...) {
            thrown = std::current_exception();
        }
        lock.lock();
        --busy;
        if (thrown && !failure)
            failure = thrown;
        if ((nextIndex >= total || failure) && busy == 0)
            done.notify_all();
    }
}

void WorkerPool::WorkerLoop(const std::size_t worker) {
    std::size_t seen = 0;
    while (true) {
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || generation != seen; });
            if (stopping)
                return;
            seen = generation;
        }
        Drain(worker);
    }
}
} // namespace Rux::Parallel
