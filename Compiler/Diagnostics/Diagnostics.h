#pragma once

// The diagnostic type shared by every compiler stage, and the two canonical
// output forms: one-per-line text on stderr and the `rux check --json`
// envelope on stdout. Stages accumulate diagnostics in their result structs;
// only the CLI/driver layer prints them.

#include "SourceModel/SourceLocation.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    // Supplemental human context is deliberately trailing so existing
    // four-field aggregate initialization remains source-compatible.
    std::vector<std::string> notes;
    std::optional<std::string> help;
    std::optional<std::string> documentationUrl;

    [[nodiscard]] bool IsError() const noexcept {
        return severity == Severity::Error;
    }
};

// An error with no source position, e.g. a package-level failure. Prints
// without the "file:line:column:" prefix and serializes with line/column 0.
[[nodiscard]] Diagnostic ErrorDiagnostic(std::string message, std::vector<std::string> notes = {},
                                         std::optional<std::string> help = {},
                                         std::optional<std::string> documentationUrl = {});

// "error" / "warning" — the spelling used in both text and JSON output.
[[nodiscard]] std::string_view SeverityName(Diagnostic::Severity severity) noexcept;

// Escape one UTF-8 string for use between JSON quotes. Bytes outside the ASCII
// control range are preserved; quotes, backslashes, and control characters are
// encoded according to RFC 8259.
[[nodiscard]] std::string EscapeJson(std::string_view value);

// Render the canonical human form. Supplemental fields are indented below the
// primary line in note/help/docs order. Empty supplemental values are omitted,
// and embedded control bytes are escaped so they cannot forge terminal lines.
// Color is a resolved policy supplied by the caller; this component performs
// no terminal or environment detection.
[[nodiscard]] std::string RenderDiagnostic(const Diagnostic &diag, bool color = false);

// Print the plain canonical human form to stderr.
void PrintDiagnostic(const Diagnostic &diag);

// Print every diagnostic to stderr. Returns true if any is an error.
bool PrintDiagnostics(std::span<const Diagnostic> diags);

// Render the stable `rux check --json` envelope. Supplemental human context is
// intentionally excluded to preserve the existing five-key diagnostic schema.
[[nodiscard]] std::string RenderDiagnosticsJson(std::span<const Diagnostic> diags, bool success);

// Print the JSON envelope to stdout:
//   { "success": ..., "diagnostics": [ {"file": ..., "line": ..., ...} ] }
void PrintDiagnosticsJson(std::span<const Diagnostic> diags, bool success);
} // namespace Rux
