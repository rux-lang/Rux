// `rux test` — build and run every test package under Tests/.

#include "Cli/Cli.h"
#include "Driver/BuildTarget.h"
#include "Driver/Driver.h"
#include "Package/Manifest.h"
#include "Platform/Platform.h"
#include "Platform/Terminal.h"

#if RUX_OS_WINDOWS
    #include "Platform/WinApi.h"
#else
    #include <fcntl.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Misc;

int Cli::RunTest(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool isRelease = false;
    for (auto &arg : args) {
        if (arg == "--release") {
            isRelease = true;
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("test");
            return 0;
        }
        PrintUnknownOption(arg, "test");
        return 1;
    }
    std::optional<std::filesystem::path> manifestPath;
    if (!opts.manifest.empty()) {
        manifestPath = opts.manifest;
        if (!std::filesystem::exists(*manifestPath)) {
            std::print(stderr, "error: specified manifest '{}' not found\n", manifestPath->string());
            return 1;
        }
    }
    else {
        manifestPath = Manifest::Find();
    }
    std::filesystem::path projectRoot;
    std::filesystem::path testsDir;
    if (manifestPath) {
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) {
            return 1;
        }
        if (!opts.quiet) {
            std::print("Testing {} v{}\n", manifest->package.name, manifest->package.version);
        }
        projectRoot = manifestPath->parent_path();
        testsDir = projectRoot / "Tests";
    }
    else {
        projectRoot = std::filesystem::current_path();
        testsDir = projectRoot / "Tests";
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            static_cast<void>(RequireManifest()); // Prints standard "Rux.toml not found" error
            return 1;
        }
        if (!opts.quiet) {
            std::print("Testing workspace\n");
        }
    }
    const std::string_view profileName = isRelease ? "Release" : "Debug";
    // Collect test package directories: any subdirectory of Tests/ that
    // contains a Rux.toml with Type = "bin".
    std::vector<std::filesystem::path> testPackages;
    {
        std::error_code ec;
        if (!std::filesystem::exists(testsDir, ec)) {
            if (!opts.quiet) {
                std::print("  No Tests/ directory found — nothing to run.\n");
            }
            return 0;
        }
        for (const auto &entry : std::filesystem::directory_iterator(testsDir, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto toml = entry.path() / "Rux.toml";
            if (!std::filesystem::exists(toml)) {
                continue;
            }
            auto pkgManifest = Manifest::Load(toml);
            if (!pkgManifest) {
                continue;
            }
            // Only run binary packages (not DLLs / shared libraries).
            const auto &type = pkgManifest->package.type;
            if (type != "bin" && type != "Bin") {
                continue;
            }
            testPackages.push_back(entry.path());
        }
        std::sort(testPackages.begin(), testPackages.end());
    }
    if (testPackages.empty()) {
        if (!opts.quiet) {
            std::print("  No test packages found in Tests/.\n");
        }
        return 0;
    }

    // Outcome of running a single test package. On failure the combined
    // stdout/stderr of the test binary is captured so it can be replayed in a
    // dedicated `failures:` section after the whole suite has run.
    enum class TestStatus {
        Passed,
        Failed,
        BuildError,
        LaunchError
    };

    struct TestOutcome {
        TestStatus status = TestStatus::Passed;
        int exitCode = 0;
        std::string output;
        std::chrono::milliseconds duration{0};
    };

    // Helper: build a test package quietly, then execute the resulting binary
    // with its output captured.
    auto runOne = [&](const std::filesystem::path &pkgDir) -> TestOutcome {
        TestOutcome outcome;

        // Load the package manifest to derive the executable name and output path.
        auto pkgManifest = Manifest::Load(pkgDir / "Rux.toml");
        if (!pkgManifest) {
            std::print(stderr, "error: failed to parse '{}'\n", (pkgDir / "Rux.toml").string());
            outcome.status = TestStatus::BuildError;
            return outcome;
        }

        // Build the package quietly (suppress per-file build output for tests).
        CompileOptions copts;
        copts.manifestPath = pkgDir / "Rux.toml";
        copts.manifest = std::move(*pkgManifest);
        copts.targetName = HostTargetTriple();
        copts.profileName = std::string(profileName);
        copts.quiet = true;
        Driver driver(std::move(copts));
        const CompileResult result = driver.Compile();
        if (!result.ok) {
            outcome.status = TestStatus::BuildError;
            return outcome;
        }
        const auto exePath = result.executablePath;

        if (!std::filesystem::exists(exePath)) {
            std::print(stderr, "error: built executable not found at '{}'\n", exePath.string());
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }

        if (opts.verbose)
            std::print("     Running `{}`\n", exePath.string());

        const auto start = std::chrono::steady_clock::now();

        // Execute the test binary, capturing its combined stdout/stderr.
#if RUX_OS_WINDOWS
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        // The read end must stay in this process only.
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        HANDLE hNul =
            CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string cmdLine = "\"" + exePath.string() + "\"";
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.hStdInput = hNul != INVALID_HANDLE_VALUE ? hNul : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.dwFlags = STARTF_USESTDHANDLES;
        if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            std::print(stderr, "error: failed to launch '{}' (code {})\n", exePath.string(), GetLastError());
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            if (hNul != INVALID_HANDLE_VALUE)
                CloseHandle(hNul);
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        // Close our copy of the write end so ReadFile returns EOF once the
        // child exits and no writable handle remains.
        CloseHandle(writePipe);
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
            outcome.output.append(buf, n);
        }
        CloseHandle(readPipe);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (hNul != INVALID_HANDLE_VALUE)
            CloseHandle(hNul);
        outcome.exitCode = static_cast<int>(exitCode);
#else
        const std::string exeStr = exePath.string();
        const char *argv[] = {exeStr.c_str(), nullptr};
        int fds[2];
        if (pipe(fds) != 0) {
            std::print(stderr, "error: pipe failed\n");
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        const pid_t pid = fork();
        if (pid < 0) {
            std::print(stderr, "error: fork failed\n");
            close(fds[0]);
            close(fds[1]);
            outcome.status = TestStatus::LaunchError;
            return outcome;
        }
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, 0);
                close(devnull);
            }
            dup2(fds[1], 1);
            dup2(fds[1], 2);
            close(fds[0]);
            close(fds[1]);
            execv(exeStr.c_str(), const_cast<char *const *>(argv));
            _exit(127);
        }
        close(fds[1]);
        char buf[4096];
        ssize_t n = 0;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            outcome.output.append(buf, static_cast<std::size_t>(n));
        }
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        outcome.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif

        outcome.duration = ElapsedMs(start);
        outcome.status = outcome.exitCode == 0 ? TestStatus::Passed : TestStatus::Failed;
        return outcome;
    };
    const AnsiStyle style{ColorEnabled(opts.color)};
    if (!opts.quiet) {
        std::print("Running {} {}\n\n", testPackages.size(), testPackages.size() == 1 ? "test" : "tests");
    }
    // Width of the test-name column, so the trailing timings line up.
    std::size_t nameWidth = 0;
    for (const auto &pkgDir : testPackages) {
        nameWidth = std::max(nameWidth, pkgDir.filename().string().size());
    }

    // Run every discovered test package and tally results, buffering the output
    // of any failures for replay after the run.
    struct Failure {
        std::string label;
        TestStatus status;
        int exitCode;
        std::string output;
    };

    std::vector<Failure> failures;
    int passed = 0;
    int failed = 0;
    const auto suiteStart = std::chrono::steady_clock::now();
    for (const auto &pkgDir : testPackages) {
        const std::string label = pkgDir.filename().string();
        TestOutcome outcome = runOne(pkgDir);
        // Trailing detail: a timing for tests that ran, otherwise the failure kind.
        std::string detail;
        switch (outcome.status) {
        case TestStatus::BuildError:
            detail = "build error";
            break;
        case TestStatus::LaunchError:
            detail = "launch error";
            break;
        default:
            detail = std::format("{} ms", outcome.duration.count());
            break;
        }
        if (outcome.status == TestStatus::Passed) {
            ++passed;
            if (!opts.quiet) {
                std::print("{}[PASSED]{} {:<{}} ({})\n", style.Green(), style.Reset(), label, nameWidth, detail);
            }
        }
        else {
            ++failed;
            if (!opts.quiet) {
                std::print("{}[FAILED]{} {:<{}} ({})\n", style.Red(), style.Reset(), label, nameWidth, detail);
            }
            failures.push_back({label, outcome.status, outcome.exitCode, std::move(outcome.output)});
        }
    }
    const double elapsed = ElapsedSeconds(suiteStart);
    // Detail each failure, replaying any captured output.
    if (!failures.empty() && !opts.quiet) {
        std::print("\nFailures:\n\n");
        int index = 0;
        for (const auto &f : failures) {
            ++index;
            std::print("{}) {}\n", index, f.label);
            switch (f.status) {
            case TestStatus::Failed:
                std::print("   Exit code: {}\n", f.exitCode);
                break;
            case TestStatus::BuildError:
                std::print("   Build error\n");
                break;
            default:
                std::print("   Launch error\n");
                break;
            }
            if (!f.output.empty()) {
                std::print("   Output:\n");
                std::string_view out{f.output};
                std::size_t pos = 0;
                while (pos < out.size()) {
                    const auto nl = out.find('\n', pos);
                    const auto line = out.substr(pos, nl == std::string_view::npos ? out.size() - pos : nl - pos);
                    std::print("     {}\n", line);
                    if (nl == std::string_view::npos) {
                        break;
                    }
                    pos = nl + 1;
                }
            }
            std::print("\n");
        }
    }
    // Summary block.
    const int total = passed + failed;
    if (!opts.quiet || failed > 0) {
        std::print("Test Result:\n");
        std::print("  Passed: {}{}{}\n", style.Green(), passed, style.Reset());
        if (failed > 0) {
            std::print("  Failed: {}{}{}\n", style.Red(), failed, style.Reset());
        }
        else {
            std::print("  Failed: {}\n", failed);
        }
        std::print("  Total : {}\n", total);
        std::print("  Time  : {:.2f}s\n", elapsed);
    }
    return failed == 0 ? 0 : 1;
}
