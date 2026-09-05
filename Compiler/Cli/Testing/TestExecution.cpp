#include "Cli/Testing/TestPackages.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildStats.h"
#include "System/Process.h"

#include <format>

namespace Rux::CliSupport {
using namespace Driver;
using namespace System;

TestOutcome RunTestPackage(const TestPackage &package, CompileOptions copts, const bool color) {
    TestOutcome outcome;
    const auto started = std::chrono::steady_clock::now();
    auto Finish = [&]() {
        outcome.duration = ElapsedMs(started);
        return outcome;
    };

    for (const auto &dependency : package.manifest.dependencies) {
        if (!dependency.IsPath()) {
            outcome.status = TestStatus::BuildError;
            outcome.output = std::format(
                "error: test dependency '{}' must use a local Path entry in '{}'; registry dependencies are "
                "not allowed\n",
                dependency.importName.Text(), (package.dir / "Rux.toml").string());
            outcome.diagnostics = std::move(outcome.output);
            outcome.output.clear();
            return Finish();
        }
    }

    // Build the package quietly (suppress per-file build output for tests).
    copts.manifestPath = package.dir / "Rux.toml";
    copts.manifest = package.manifest;
    copts.localDependenciesOnly = true;
    copts.isTest = true;
    copts.emitDiagnostic = [&](const Diagnostic &diagnostic, const SourceLineLookup &sourceLineLookup) {
        outcome.diagnostics += RenderDiagnostic(diagnostic, color, sourceLineLookup);
    };
    CompilerDriver driver(std::move(copts));
    const CompileResult result = driver.Compile();
    if (!result.ok) {
        outcome.status = TestStatus::BuildError;
        return Finish();
    }
    const auto exePath = result.primaryArtifactPath;

    std::error_code artifactError;
    if (!std::filesystem::exists(exePath, artifactError)) {
        outcome.diagnostics = std::format("error: built test executable was not found at '{}'\n", exePath.string());
        if (artifactError)
            outcome.diagnostics +=
                std::format("  note: system error {}: {}\n", artifactError.value(), artifactError.message());
        outcome.status = TestStatus::LaunchError;
        return Finish();
    }

    outcome.executable = exePath;

    // Execute the test binary, capturing its combined stdout/stderr.
    std::error_code launchError;
    auto run = RunCaptured(exePath, {}, &launchError);
    if (!run) {
        if (launchError) {
            outcome.diagnostics =
                std::format("error: could not launch test executable '{}'\n  note: system error {}: {}\n",
                            exePath.string(), launchError.value(), launchError.message());
        }
        else {
            outcome.diagnostics = std::format("error: could not launch test executable '{}'\n", exePath.string());
        }
        outcome.status = TestStatus::LaunchError;
        return Finish();
    }
    outcome.exitCode = run->exitCode;
    outcome.output = std::move(run->output);

    outcome.status = outcome.exitCode == 0 ? TestStatus::Passed : TestStatus::Failed;
    return Finish();
}
} // namespace Rux::CliSupport
