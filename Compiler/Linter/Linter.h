#pragma once

#include <string>
#include <vector>

#include "Diagnostics/Diagnostics.h"

namespace Rux::Linting {

struct LintResult {
    std::vector<Diagnostic> diagnostics;
    [[nodiscard]] bool HasErrors() const noexcept;
};

[[nodiscard]] LintResult Lint(std::string source, std::string sourceName);

} // namespace Rux::Linting
