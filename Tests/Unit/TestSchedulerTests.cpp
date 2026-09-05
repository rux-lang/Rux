#include "Cli/Testing/TestScheduler.h"

#include <array>
#include <atomic>
#include <barrier>
#include <doctest.h>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Rux::CliSupport;

TEST_CASE("test workers overlap while reporting stays ordered on the caller") {
    std::array<TestTask, 4> tasks;
    std::barrier ready(4);
    std::atomic<unsigned> running{0};
    std::atomic<unsigned> peak{0};
    const auto caller = std::this_thread::get_id();
    std::vector<std::size_t> reports;
    bool callerReported = true;
    RunTestTasks(
        tasks, 4,
        [&](const std::size_t) {
            ++running;
            ready.arrive_and_wait();
            peak.store(running.load());
            ready.arrive_and_wait();
            --running;
        },
        [&](const std::size_t index) {
            callerReported &= std::this_thread::get_id() == caller;
            reports.push_back(index);
        });
    CHECK(peak == 4);
    CHECK(callerReported);
    CHECK(reports == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("test artifact conflicts preserve discovery order across aliases and nested paths") {
    const std::array<TestTask, 4> tasks{{{{"scheduler-out/A/../same.exe"}},
                                         {{"scheduler-out/same.exe"}},
                                         {{"scheduler-out"}},
                                         {{"scheduler-out/other.exe"}}}};
    std::vector<std::size_t> executions;
    // Every task depends on its predecessor, including the directory reservation at index 2.
    RunTestTasks(tasks, 4, [&](const std::size_t index) { executions.push_back(index); }, [](std::size_t) {});
    CHECK(executions == std::vector<std::size_t>{0, 1, 2, 3});
}

TEST_CASE("serial and parallel scheduling produce the same outcomes and continue after failures") {
    std::array<TestTask, 16> tasks;
    auto Run = [&](const std::size_t jobs) {
        std::array<int, 16> outcomes{};
        std::vector<int> reported;
        RunTestTasks(
            tasks, jobs, [&](const std::size_t index) { outcomes[index] = index % 3 == 0 ? 1 : 0; },
            [&](const std::size_t index) { reported.push_back(outcomes[index]); });
        return reported;
    };
    CHECK(Run(1) == Run(4));
    CHECK(Run(4).size() == 16);
    std::vector<std::size_t> emptyReports;
    RunTestTasks({}, 4, [](std::size_t) {}, [&](std::size_t index) { emptyReports.push_back(index); });
    CHECK(emptyReports.empty());
}

TEST_CASE("worker exceptions reach the caller after other scheduled tasks finish") {
    std::array<TestTask, 4> tasks;
    std::atomic<unsigned> finished{0};
    CHECK_THROWS_AS(RunTestTasks(
                        tasks, 4,
                        [&](std::size_t index) {
                            if (index == 0)
                                throw std::runtime_error("probe");
                            ++finished;
                        },
                        [](std::size_t) {}),
                    std::runtime_error);
    CHECK(finished == 3);
}
