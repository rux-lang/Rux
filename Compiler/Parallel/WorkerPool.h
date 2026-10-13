#pragma once

// A fixed set of worker threads that runs index-parallel loops for one compilation. Work is handed out by index, so a
// loop body writes its result into the slot for its index and the caller merges slots in order afterwards: what a
// compilation produces never depends on how many threads ran it or in which order they finished.

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Rux::Parallel {
/// The number of threads a compilation uses when the user asks for none in particular: one per logical core.
[[nodiscard]] std::size_t DefaultJobs() noexcept;

/// Resolve a requested job count, where 0 means `DefaultJobs()`.
[[nodiscard]] std::size_t ResolveJobs(std::size_t requested) noexcept;

class WorkerPool {
public:
    /// Start `jobs - 1` workers; the thread calling `ForEach` is the last one. A pool of one job starts no thread and
    /// runs every loop inline.
    explicit WorkerPool(std::size_t jobs);
    ~WorkerPool();
    WorkerPool(const WorkerPool &) = delete;
    WorkerPool &operator=(const WorkerPool &) = delete;

    [[nodiscard]] std::size_t Jobs() const noexcept {
        return workers.size() + 1;
    }

    /**
     * @brief Run `body(index)` once for every index in [0, count) and return when all have finished.
     *
     * Bodies run concurrently and must not touch state another index writes. The first exception a body throws is
     * rethrown here once the loop has drained; indices not yet started are skipped. Loops do not nest: a body must not
     * call `ForEach` on the same pool.
     */
    void ForEach(std::size_t count, const std::function<void(std::size_t)> &body);

    /// `ForEach` that also tells each body which thread runs it, as an index in [0, Jobs()), so a loop can keep
    /// per-thread scratch state. Which thread runs an index varies between runs; results must not depend on it.
    void ForEachOnWorker(std::size_t count, const std::function<void(std::size_t index, std::size_t worker)> &body);

private:
    void WorkerLoop(std::size_t worker);
    void Drain(std::size_t worker);

    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    const std::function<void(std::size_t, std::size_t)> *current = nullptr;
    std::size_t total = 0;
    std::size_t nextIndex = 0;
    std::size_t generation = 0;
    std::size_t busy = 0;
    bool stopping = false;
    std::exception_ptr failure;
};
} // namespace Rux::Parallel
