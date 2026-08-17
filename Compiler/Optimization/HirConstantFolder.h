#pragma once

#include "Optimization/ConstantEvaluator.h"
#include "Optimization/Pass.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux::Optimization {
/**
 * @brief Evaluates what it can at compile time and rewrites HIR in place.
 *
 * Folding happens in HIR rather than LIR so the result is still expressed in source terms: a folded branch can then be
 * removed as a whole statement, which is not something the flattened LIR form makes visible.
 *
 * Correctness rests on knowing when a binding stops being known. Anything that could write through a pointer or call
 * out of the function invalidates the facts it could have touched, so an unfolded assignment is always safe and only a
 * missed opportunity.
 */
class HirConstantFolder final : public HirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(HirPackage &package, const PassContext &context) override;

private:
    /// What is known about one binding. An engaged `constant` means the value is known here; a declared binding with
    /// none is known to be unknown, which is different from a name this scope never saw.
    struct BindingFact {
        std::optional<TypedConstant> constant;
    };

    /// One lexical scope's facts. The folder keeps a stack of these so leaving a block forgets exactly the bindings
    /// that went out of scope with it.
    using Scope = std::unordered_map<std::string, BindingFact>;

    void OptimizeModule(HirModule &module);
    void OptimizeFunc(HirFunc &func);
    void OptimizeBlock(HirBlock &block, bool introduceScope = true);
    void OptimizeStmt(HirStmtPtr &stmt);
    void OptimizeExpr(HirExprPtr &expr, bool allowSubstitution = true);
    void OptimizePattern(HirPattern &pattern);
    /// Each arm folds against the state that reached the match, not against what the previous arm left behind, since
    /// only one arm ever runs.
    void OptimizeMatchArm(HirMatchArm &arm, const std::vector<Scope> &incomingScopes);

    void PushScope();
    void PopScope();
    void Declare(std::string_view name, std::optional<TypedConstant> constant = std::nullopt);
    void DeclarePatternBindings(const HirPattern &pattern);
    [[nodiscard]] const TypedConstant *Lookup(std::string_view name) const;
    void Invalidate(std::string_view name);

    /// Forget every known value, leaving the bindings themselves declared. The conservative answer wherever the folder
    /// stops knowing exactly what ran: around loops and branches, and after a call whose effects it cannot see.
    void InvalidateAll();

    /// Forget what an assignment through `expr` could have changed, walking field, index, and dereference expressions
    /// down to the name underneath. An expression with no name at its base invalidates nothing on its own, so callers
    /// pair it with `InvalidateAll`.
    void InvalidatePlace(const HirExpr &expr);

    [[nodiscard]] std::optional<TypedConstant> Constant(const HirExpr &expr) const;
    [[nodiscard]] static HirExprPtr MakeLiteral(const TypedConstant &constant, const SourceLocation &location);
    [[nodiscard]] static bool IsDiscardable(const HirExpr &expr);
    [[nodiscard]] static bool CanInline(const HirBlock &block);
    [[nodiscard]] static std::optional<bool> BooleanLiteral(const HirExpr &expr);

    bool FoldBinary(HirExprPtr &expr);
    bool FoldUnary(HirExprPtr &expr);
    bool FoldCast(HirExprPtr &expr);
    bool SimplifyBinary(HirExprPtr &expr);

    bool changed = false;
    std::vector<Scope> scopes;
};
} // namespace Rux::Optimization
