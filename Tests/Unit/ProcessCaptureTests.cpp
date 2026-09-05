#include "System/Os.h"
#include "System/Process.h"
#include "System/WinApi.h"
#include "Target/Platform.h"

#include <array>
#include <barrier>
#include <cstdint>
#include <doctest.h>
#include <future>
#include <string>
#include <vector>

using namespace Rux::System;

namespace {
std::filesystem::path ProbeExecutable() {
    return std::filesystem::path(RUX_TEST_BIN_DIR) / "Unit" / ExecutableFileName("rux-tests");
}
} // namespace

TEST_CASE("concurrent process captures preserve each argument stream and exit code") {
    constexpr std::size_t count = 12;
    std::barrier start(static_cast<std::ptrdiff_t>(count));
    std::vector<std::future<std::optional<RunResult>>> children;
    for (std::size_t index = 0; index < count; ++index) {
        children.push_back(std::async(std::launch::async, [&, index] {
            const std::string marker = "child " + std::to_string(index);
            start.arrive_and_wait();
            return RunCaptured(ProbeExecutable(), std::array<std::string_view, 5>{"--rux-process-probe", marker, "",
                                                                                  "quote\"inside", "trailing\\"});
        }));
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto result = children[index].get();
        REQUIRE(result.has_value());
        CHECK(result->exitCode == 7);
        const std::string marker = "child " + std::to_string(index);
        const std::string expected = std::to_string(marker.size()) + ':' + marker +
                                     ";0:;12:quote\"inside;9:trailing\\;" + std::string(128 * 1024, 'x');
        CHECK(result->output == expected);
    }
}

TEST_CASE("failed subprocess launches return an error on every host") {
    std::error_code error;
    const auto missing = ProbeExecutable().parent_path() / "missing-rux-process-probe";
    CHECK_FALSE(RunCaptured(missing, {}, &error));
    CHECK(error);
    CHECK_FALSE(RunInherited(missing, {}, &error));
    CHECK(error);
}

#if RUX_OS_WINDOWS
TEST_CASE("Windows children inherit only explicitly selected standard handles") {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE event = CreateEventA(&security, TRUE, FALSE, nullptr);
    REQUIRE(event != nullptr);
    const std::string value = std::to_string(reinterpret_cast<std::uintptr_t>(event));
    const std::array<std::string_view, 2> args{"--rux-handle-probe", value};
    const auto captured = RunCaptured(ProbeExecutable(), args);
    const auto inherited = RunInherited(ProbeExecutable(), args);
    CloseHandle(event);
    REQUIRE(captured.has_value());
    CHECK(captured->exitCode == 0);
    CHECK(inherited == 0);
}
#endif
