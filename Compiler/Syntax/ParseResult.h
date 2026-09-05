#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Syntax/Ast/Ast.h"

#include <vector>

namespace Rux {
using ParserDiagnostic = Diagnostic;

/// One parsed module and everything that went wrong building it. Because the parser recovers, a result with errors
/// still carries the partial `module` it managed to build rather than nothing.
struct ParseResult {
    Module module;
    std::vector<ParserDiagnostic> diagnostics;
    [[nodiscard]] bool HasErrors() const noexcept;
};

} // namespace Rux
