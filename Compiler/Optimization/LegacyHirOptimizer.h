#pragma once

#include "Optimization/Pass.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace Rux::Optimization {
class LegacyHirOptimizer final : public HirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(HirPackage &package, const PassContext &context) override;

private:
    void OptimizeModule(HirModule &module);

    void OptimizeFunc(HirFunc &func);

    void OptimizeBlock(HirBlock &block);

    void OptimizeStmt(HirStmtPtr &stmt);

    void OptimizeExpr(HirExprPtr &expr);

    bool FoldBinary(HirExprPtr &expr);

    bool FoldUnary(HirExprPtr &expr);

    [[nodiscard]] static bool IsIntegerLiteral(const HirExpr *expr);

    [[nodiscard]] static std::optional<std::int64_t> GetIntegerLiteral(const HirExpr *expr);

    [[nodiscard]] static HirExprPtr MakeIntegerLiteral(std::int64_t value, const TypeRef &type);

    [[nodiscard]] static HirExprPtr MakeBoolLiteral(bool value, const TypeRef &type);

    [[nodiscard]] static bool IsBoolLiteral(const HirExpr *expr);

    [[nodiscard]] static bool GetBoolLiteral(const HirExpr *expr);

    bool SimplifyBinary(HirExprPtr &expr);

    struct ConstantValue {
        bool isBool = false;
        std::int64_t intValue = 0;
        bool boolValue = false;
        TypeRef type;
    };

    bool changed_ = false;
    std::unordered_map<std::string, ConstantValue> constants_;
};
} // namespace Rux::Optimization
