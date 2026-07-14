#pragma once

#include "Diagnostics/Diagnostics.h"

#include <string>
#include <vector>

namespace Rux::Linting {
struct LintResult {
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool HasErrors() const noexcept;
};

[[nodiscard]] LintResult Lint(std::string source, std::string sourceName);
} // namespace Rux::Linting
