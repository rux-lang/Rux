#pragma once

// CLI presentation for semantic compiler-driver progress events. Keeping this
// adapter at the command boundary lets each workflow select its own stream.

#include "Cli/Reporter.h"
#include "Driver/CompilerDriver.h"

#include <format>

namespace Rux::CliSupport {
/// Point the driver's diagnostic callback at this reporter.
///
/// Diagnostics stay visible under `--quiet`, which is why they are written at `Always` visibility rather than as
/// ordinary output. The callback captures the reporter by reference, so it must outlive the options.
inline void ConfigureCompileDiagnostics(Driver::CompileOptions &options, const Reporter &reporter) {
    options.emitDiagnostic = [&reporter](const Diagnostic &diagnostic, const SourceLineLookup &sourceLineLookup) {
        reporter.Write(RenderDiagnostic(diagnostic, reporter.Style().enabled, sourceLineLookup),
                       MessageVisibility::Always);
    };
}

/// Report one pipeline phase as verbose output. Phase names come from `CompilePhaseName` so `--verbose` and the
/// `--stats` report name the pipeline identically.
inline void ReportCompileProgress(const Reporter &reporter, const Driver::CompileProgress &progress) {
    if (progress.phase == Driver::CompilePhase::LoadingDependency) {
        reporter.Verbose(std::format("{} '{}' from '{}'", Driver::CompilePhaseName(progress.phase), progress.subject,
                                     progress.path.string()));
        return;
    }
    reporter.Verbose(std::format("{} '{}'", Driver::CompilePhaseName(progress.phase), progress.subject));
}
} // namespace Rux::CliSupport
