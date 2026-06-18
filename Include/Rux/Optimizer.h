#pragma once

#include "Rux/Hir.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace Rux {

class Optimizer {
public:
    static void Run(HirPackage &package);


private:
    static void OptimizeModule(HirModule &module);

    static void OptimizeFunc(HirFunc &func);

    static void OptimizeBlock(HirBlock &block);

    static void OptimizeStmt(HirStmtPtr &stmt);

    static void OptimizeExpr(HirExprPtr &expr);

    static bool FoldBinary(HirExprPtr &expr);

    static bool FoldUnary(HirExprPtr &expr);

    static bool IsIntegerLiteral(HirExpr const *expr);

    static std::optional<std::int64_t> GetIntegerLiteral(HirExpr const *expr);

    static HirExprPtr MakeIntegerLiteral(std::int64_t value, TypeRef const &type);

    static HirExprPtr MakeBoolLiteral(bool value, TypeRef const &type);

    static bool IsBoolLiteral(HirExpr const *expr);

    static bool GetBoolLiteral(HirExpr const *expr);

    static bool SimplifyBinary(HirExprPtr &expr);

    struct ConstantValue {
        bool isBool = false;
        std::int64_t intValue = 0;
        bool boolValue = false;
        TypeRef type;
    };

    static inline std::unordered_map<std::string, ConstantValue> constants;
};

} // namespace Rux
