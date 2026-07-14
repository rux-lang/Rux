#pragma once

// The diagnostic type shared by every compiler stage, and the two canonical
// output forms: one-per-line text on stderr and the `rux check --json`
// envelope on stdout. Stages accumulate diagnostics in their result structs;
// only the CLI/driver layer prints them.

#include "Source/SourceLocation.h"

#include <span>
#include <string>
#include <string_view>

namespace Rux {
struct Diagnostic {
    enum class Severity {
        Warning,
        Error,
    };

    Severity severity = Severity::Error;
    std::string sourceName; // source file path; empty when not tied to a file
    SourceLocation location;
    std::string message;

    [[nodiscard]] bool IsError() const noexcept {
        return severity == Severity::Error;
    }
};

// An error with no source position, e.g. a package-level failure. Prints
// without the "file:line:column:" prefix and serializes with line/column 0.
[[nodiscard]] Diagnostic ErrorDiagnostic(std::string message);

// "error" / "warning" — the spelling used in both text and JSON output.
[[nodiscard]] std::string_view SeverityName(Diagnostic::Severity severity) noexcept;

// Print "file:line:column: severity: message" to stderr, or
// "severity: message" when the diagnostic has no source file.
void PrintDiagnostic(const Diagnostic &diag);

// Print every diagnostic to stderr. Returns true if any is an error.
bool PrintDiagnostics(std::span<const Diagnostic> diags);

// Print the `rux check --json` envelope to stdout:
//   { "success": ..., "diagnostics": [ {"file": ..., "line": ..., ...} ] }
void PrintDiagnosticsJson(std::span<const Diagnostic> diags, bool success);
} // namespace Rux
