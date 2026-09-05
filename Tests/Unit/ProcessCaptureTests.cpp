#include "ProcessProbe.h"
#include "System/Os.h"
#include "System/Process.h"
#include "System/WinApi.h"
#include "Target/Platform.h"

#include <array>
#include <barrier>
#include <cstdint>
#include <doctest.h>
#include <future>
#include <span>
#include <string>
#include <vector>

#if !RUX_OS_WINDOWS
    #include <spawn.h>
    #include <sys/stat.h>
    #include <sys/wait.h>
    #include <unistd.h>
extern char **environ;
#endif

using namespace Rux::System;

namespace {
std::filesystem::path ProbeExecutable() {
    return std::filesystem::path(RUX_TEST_BIN_DIR) / "Unit" / ExecutableFileName("rux-tests");
}

// Launch the probe the way the System layer must not: every inheritable handle or descriptor reaches the child. The
// isolation checks below mean nothing unless the probe detects this leak.
int RunLeaking(const std::filesystem::path &exe, const std::span<const std::string_view> args) {
#if RUX_OS_WINDOWS
    std::string command = '"' + exe.string() + '"';
    for (const auto argument : args) {
        command += ' ';
        command += argument;
    }
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
        return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    const bool known = GetExitCodeProcess(process.hProcess, &code) != FALSE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return known ? static_cast<int>(code) : -1;
#else
    std::vector<std::string> strings{exe.string()};
    for (const auto argument : args)
        strings.emplace_back(argument);
    std::vector<char *> argv;
    for (auto &argument : strings)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, strings.front().c_str(), nullptr, nullptr, argv.data(), environ) != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
#endif
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
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    REQUIRE(CreatePipe(&read, &write, &security, 0));
    DWORD written = 0;
    REQUIRE(WriteFile(write, Rux::Testing::handleProbeToken.data(),
                      static_cast<DWORD>(Rux::Testing::handleProbeToken.size()), &written, nullptr));
    const std::string value = std::to_string(reinterpret_cast<std::uintptr_t>(read));
    const std::array<std::string_view, 2> args{"--rux-handle-probe", value};
    const auto captured = RunCaptured(ProbeExecutable(), args);
    const auto inherited = RunInherited(ProbeExecutable(), args);
    const int leaking = RunLeaking(ProbeExecutable(), args);
    CloseHandle(write);
    CloseHandle(read);
    REQUIRE(captured.has_value());
    CHECK(captured->exitCode == 0);
    CHECK(inherited == 0);
    CHECK(leaking == 1);
}
#else
TEST_CASE("POSIX children inherit only the standard descriptors") {
    // Like an artifact stream, the pipe is deliberately not close-on-exec.
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    struct stat status{};
    REQUIRE(fstat(fds[0], &status) == 0);
    const std::string descriptor = std::to_string(fds[0]);
    const std::string inode = std::to_string(static_cast<unsigned long long>(status.st_ino));
    const std::array<std::string_view, 3> args{"--rux-descriptor-probe", descriptor, inode};
    const auto captured = RunCaptured(ProbeExecutable(), args);
    const auto inherited = RunInherited(ProbeExecutable(), args);
    const int leaking = RunLeaking(ProbeExecutable(), args);
    close(fds[1]);
    close(fds[0]);
    REQUIRE(captured.has_value());
    CHECK(captured->exitCode == 0);
    CHECK(inherited == 0);
    CHECK(leaking == 1);
}
#endif
