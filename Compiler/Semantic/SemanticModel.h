#pragma once

#include <string>
#include <vector>

#include "Diagnostics/Diagnostics.h"
#include "Syntax/Ast/Ast.h"

namespace Rux {

using SemanticDiagnostic = Diagnostic;

struct SemanticSymbol {
    enum class Kind {
        Var,
        Func,
        Type,
        Const,
        Module,
        Interface,
    };

    Kind kind = Kind::Var;
    std::string name;
    std::string sourceName;
    SourceLocation location;
    std::string resolvedType;
    bool isMut = false;
};

// Persistent output of semantic analysis. Besides diagnostics and exported
// symbols it owns the ordered, validated module view consumed by lowering.
struct SemanticModel {
    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    std::vector<const Module *> modules;

    [[nodiscard]] bool HasErrors() const noexcept;
};

} // namespace Rux
