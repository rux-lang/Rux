#pragma once

#include "Diagnostics/Diagnostics.h"

#include <string>
#include <vector>

namespace Rux::Linting {
/// What linting found. Style findings are warnings, so a clean exit code and a non-empty result are both normal.
struct LintResult {
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool HasErrors() const noexcept;
};

/// Check one source file for style problems the compiler itself accepts.
///
/// @param sourceName Names the file in diagnostics; it is never opened
[[nodiscard]] LintResult Lint(std::string source, std::string sourceName);
} // namespace Rux::Linting
