#include "Cli/Testing/TestScheduler.h"

#include "Target/Target.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

namespace Rux::CliSupport {
namespace {
std::filesystem::path Normalize(const std::filesystem::path &path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        // An existing executable may be temporarily inaccessible. Its containing directory still gives
        // us the canonical identity; leave inaccessible-file diagnostics to the actual build operation.
        const auto parent = std::filesystem::weakly_canonical(absolute.parent_path(), error);
        result = error ? absolute : parent / absolute.filename();
    }
    if constexpr (Target::HostOS == Target::OS::Windows || Target::HostOS == Target::OS::MacOS) {
        // Conservative on case-sensitive volumes too: an unnecessary serialization is safer than an artifact race.
        auto spelling = result.generic_u8string();
        // Non-ASCII filesystem case folding varies by volume. Reserve the volume in this uncommon case rather than
        // assume ASCII folding captures every alias (including Unicode normalization on macOS).
        if (std::ranges::any_of(spelling, [](const char8_t ch) { return ch >= 128; }))
            return result.root_path();
        std::ranges::transform(spelling, spelling.begin(), [](const char8_t ch) {
            return ch >= u8'A' && ch <= u8'Z' ? static_cast<char8_t>(ch + (u8'a' - u8'A')) : ch;
        });
        result = std::filesystem::path(spelling);
    }
    return result;
}

bool Overlap(const TestTask &left, const TestTask &right) {
    for (const auto &a : left.artifacts) {
        for (const auto &b : right.artifacts) {
            const auto [aEnd, bEnd] = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
            if (aEnd == a.end() || bEnd == b.end())
                return true;
        }
    }
    return false;
}
} // namespace

void RunTestTasks(const std::span<const TestTask> tasks, const std::size_t jobs,
                  const std::function<void(std::size_t)> &execute, const std::function<void(std::size_t)> &report) {
    if (jobs <= 1 || tasks.size() <= 1) {
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            execute(index);
            report(index);
        }
        return;
    }
    std::vector<TestTask> normalized(tasks.begin(), tasks.end());
    for (auto &task : normalized)
        for (auto &path : task.artifacts)
            path = Normalize(path);
    // Dependencies point only backwards, so waiting tasks cannot form a cycle.
    std::vector<std::vector<std::size_t>> predecessors(tasks.size());
    for (std::size_t index = 0; index < tasks.size(); ++index)
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            if (Overlap(normalized[index], normalized[earlier]))
                predecessors[index].push_back(earlier);

    enum class State {
        Pending,
        Running,
        Done
    };
    std::vector<State> states(tasks.size(), State::Pending);
    std::vector<std::exception_ptr> failures(tasks.size());
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t remaining = tasks.size();
    auto Pick = [&] {
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            if (states[index] == State::Pending && std::ranges::all_of(predecessors[index], [&](std::size_t earlier) {
                    return states[earlier] == State::Done;
                }))
                return index;
        }
        return tasks.size();
    };
    std::vector<std::thread> workers;

    struct JoinWorkers {
        std::vector<std::thread> &threads;

        ~JoinWorkers() {
            for (auto &thread : threads)
                thread.join();
        }
    } joinWorkers{workers};

    // These workers finish all queued tasks and never use cancellation. Joining ordinary threads also avoids the
    // Windows address-wait API imported by the standard library's unused stop-token machinery.
    for (std::size_t worker = 0; worker < std::min(jobs, tasks.size()); ++worker) {
        workers.emplace_back([&] {
            for (;;) {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return remaining == 0 || Pick() != tasks.size(); });
                if (remaining == 0)
                    return;
                const std::size_t index = Pick();
                states[index] = State::Running;
                lock.unlock();
                try {
                    execute(index);
                }
                catch (...) {
                    failures[index] = std::current_exception();
                }
                lock.lock();
                states[index] = State::Done;
                --remaining;
                lock.unlock();
                changed.notify_all();
            }
        });
    }
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return states[index] == State::Done; });
        lock.unlock();
        if (failures[index])
            std::rethrow_exception(failures[index]);
        report(index);
    }
}
} // namespace Rux::CliSupport
