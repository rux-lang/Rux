#include "Parallel/WorkerPool.h"

#include <algorithm>

namespace Rux::Parallel {
std::size_t DefaultJobs() noexcept {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

std::size_t ResolveJobs(const std::size_t requested) noexcept {
    return requested == 0 ? DefaultJobs() : requested;
}

WorkerPool::WorkerPool(const std::size_t jobs) {
    const std::size_t threads = std::max<std::size_t>(jobs, 1) - 1;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
        workers.emplace_back([this] { WorkerLoop(); });
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
    if (count == 0)
        return;
    if (workers.empty() || count == 1) {
        for (std::size_t index = 0; index < count; ++index)
            body(index);
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
    Drain();
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

void WorkerPool::Drain() {
    std::unique_lock lock(mutex);
    while (current != nullptr && nextIndex < total && !failure) {
        const auto index = nextIndex++;
        const auto *body = current;
        ++busy;
        lock.unlock();
        std::exception_ptr thrown;
        try {
            (*body)(index);
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

void WorkerPool::WorkerLoop() {
    std::size_t seen = 0;
    while (true) {
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping || generation != seen; });
            if (stopping)
                return;
            seen = generation;
        }
        Drain();
    }
}
} // namespace Rux::Parallel
