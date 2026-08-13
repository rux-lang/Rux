#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Semantic/CompileTimeContext.h"
#include "Semantic/Type.h"
#include "Syntax/Ast/Ast.h"

#include <string>
#include <unordered_map>
#include <vector>

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
// symbols it owns the ordered, validated module view and resolved type facts
// consumed by lowering. The model does not own the AST: every Module supplied
// to SemanticAnalyzer must outlive the model and remain unchanged while its
// node-keyed facts are queried.
struct SemanticModel {
    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    std::vector<const Module *> modules;
    CompileTimeContext compileTimeContext;

    SemanticModel(std::vector<SemanticDiagnostic> inputDiagnostics, std::vector<SemanticSymbol> inputSymbols,
                  std::vector<const Module *> inputModules, CompileTimeContext inputCompileTimeContext,
                  std::unordered_map<const Expr *, TypeRef> inputExpressionTypes,
                  std::unordered_map<const TypeExpr *, TypeRef> inputTypeNodeTypes,
                  std::unordered_map<const Pattern *, TypeRef> inputPatternTypes);

    [[nodiscard]] bool HasErrors() const noexcept;

    // Returns null when analysis did not accept the node with a resolved type.
    // Returned pointers remain valid for the lifetime of this model.
    [[nodiscard]] const TypeRef *TryGetType(const Expr &expression) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const TypeExpr &typeNode) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const Pattern &pattern) const noexcept;

private:
    std::unordered_map<const Expr *, TypeRef> expressionTypes;
    std::unordered_map<const TypeExpr *, TypeRef> typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> patternTypes;
};
} // namespace Rux
