#pragma once

// Conditional compilation: folding of `when` chains.
//
// A `when` has the shape of an `if`, but its condition is evaluated by the
// compiler rather than by the running program. This pass runs before semantic
// analysis: it evaluates each condition against the program's compile-time
// constants, splices the taken branch into the enclosing statement or
// declaration list, and discards the branches that were not taken. Code in a
// discarded branch is parsed but never resolved, type-checked or lowered, so it
// may reference symbols that do not exist on the current build.

#include "Diagnostics/Diagnostics.h"
#include "Semantic/CompileTimeContext.h"
#include "Syntax/Ast/Ast.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Rux {
struct CompileTimeEnumValue {
    std::string type;
    std::string variant;

    bool operator==(const CompileTimeEnumValue &) const = default;
};

using CompileTimeValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string, CompileTimeEnumValue>;

struct CompileTimeEvaluation {
    std::optional<CompileTimeValue> value;
    std::vector<Diagnostic> diagnostics;
};

struct CompileTimeConditionEvaluation {
    bool value = false;
    std::vector<Diagnostic> diagnostics;
};

struct CompileTimeMatchEvaluation {
    int arm = -1;
    std::vector<Diagnostic> diagnostics;
};

/// Evaluates compile-time expressions against an immutable view of the parsed declarations. Evaluation never rewrites
/// the AST; the conditional-compilation folder below is the sole owner of branch selection and splicing.
class ConditionalEvaluator {
public:
    ConditionalEvaluator(const CompileTimeContext &context, const std::vector<Module *> &modules);
    ~ConditionalEvaluator();

    ConditionalEvaluator(const ConditionalEvaluator &) = delete;
    ConditionalEvaluator &operator=(const ConditionalEvaluator &) = delete;
    ConditionalEvaluator(ConditionalEvaluator &&) noexcept;
    ConditionalEvaluator &operator=(ConditionalEvaluator &&) noexcept;

    void SetSourceContext(std::string_view file, std::string_view modulePath, std::string_view function);
    void SetImports(const Module &module);
    void RegisterDeclarations(const std::vector<DeclPtr> &decls);
    void RegisterConstant(const ConstDecl &decl);

    [[nodiscard]] CompileTimeEvaluation Evaluate(const Expr &expr);
    [[nodiscard]] CompileTimeEvaluation EvaluateConstant(std::string_view name);
    [[nodiscard]] CompileTimeConditionEvaluation EvaluateCondition(const Expr *condition, SourceLocation location);
    [[nodiscard]] CompileTimeMatchEvaluation
    SelectMatchArm(const Expr &subject, const std::vector<std::vector<const Expr *>> &arms, SourceLocation location);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};

/// Folds every `when` in `modules` in place using the same context later consumed by semantic analysis and lowering.
void ResolveConditionalCompilation(const std::vector<Module *> &modules, const CompileTimeContext &context,
                                   std::vector<Diagnostic> &diags);

/// Compatibility overload used by embedders and focused tests which only need to select an OS. Empty means the host.
void ResolveConditionalCompilation(const std::vector<Module *> &modules, std::string_view targetSystem,
                                   std::vector<Diagnostic> &diags);
} // namespace Rux
