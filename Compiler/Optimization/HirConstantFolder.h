#pragma once

#include "Optimization/ConstantEvaluator.h"
#include "Optimization/Pass.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux::Optimization {
class HirConstantFolder final : public HirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(HirPackage &package, const PassContext &context) override;

private:
    struct BindingFact {
        std::optional<TypedConstant> constant;
    };

    using Scope = std::unordered_map<std::string, BindingFact>;

    void OptimizeModule(HirModule &module);
    void OptimizeFunc(HirFunc &func);
    void OptimizeBlock(HirBlock &block, bool introduceScope = true);
    void OptimizeStmt(HirStmtPtr &stmt);
    void OptimizeExpr(HirExprPtr &expr, bool allowSubstitution = true);
    void OptimizePattern(HirPattern &pattern);
    void OptimizeMatchArm(HirMatchArm &arm, const std::vector<Scope> &incomingScopes);

    void PushScope();
    void PopScope();
    void Declare(std::string_view name, std::optional<TypedConstant> constant = std::nullopt);
    void DeclarePatternBindings(const HirPattern &pattern);
    [[nodiscard]] const TypedConstant *Lookup(std::string_view name) const;
    void Invalidate(std::string_view name);
    void InvalidateAll();
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

    bool changed_ = false;
    std::vector<Scope> scopes_;
};
} // namespace Rux::Optimization
