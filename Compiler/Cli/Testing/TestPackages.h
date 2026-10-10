#pragma once

#include "Driver/CompilerDriver.h"

#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Rux::CliSupport {
struct TestRoot {
    std::filesystem::path dir;
    std::string labelPrefix;
};

struct TestPackage {
    std::filesystem::path dir;
    std::string label;
    Manifest manifest;
};

struct TestDiscovery {
    std::vector<TestPackage> packages;
    std::vector<ManifestDiagnostic> diagnostics;
    bool anyRootExists = false;
};

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
    std::string diagnostics;
    std::filesystem::path executable;
    std::chrono::milliseconds duration{0};
};

[[nodiscard]] TestDiscovery DiscoverTestPackages(std::span<const TestRoot> roots);
/// Whether any source under `packageDir/Src` marks a function `#Test()`. A plain text search, so a package whose tests
/// are commented out is still built in test mode and then reports that it has none.
[[nodiscard]] bool SourcesDeclareTests(const std::filesystem::path &packageDir);
/// Builds `package` in test mode and runs it with `arguments`, which a generated test runner reads as its filters.
[[nodiscard]] TestOutcome RunTestPackage(const TestPackage &package, Driver::CompileOptions options, bool color,
                                         std::span<const std::string_view> arguments = {});
} // namespace Rux::CliSupport
