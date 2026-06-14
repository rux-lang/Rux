#pragma once

#include "Rux/Hir.h"

namespace Rux {

    class Optimizer {
    public:
        static void Run(HirPackage& package);

    private:
        static void OptimizeModule(HirModule& module);

        static void OptimizeFunc(HirFunc& func);

        static void OptimizeBlock(HirBlock& block);

        static void OptimizeStmt(HirStmtPtr& stmt);

        static void OptimizeExpr(HirExprPtr& expr);

        static bool FoldBinary(HirExprPtr& expr);

        static bool FoldUnary(HirExprPtr& expr);

        static bool IsIntegerLiteral(const HirExpr* expr);

        static std::int64_t GetIntegerLiteral(const HirExpr* expr);

        static HirExprPtr MakeIntegerLiteral(std::int64_t value, const TypeRef& type);

        static HirExprPtr MakeBoolLiteral(bool value, const TypeRef& type);

        static bool IsBoolLiteral(const HirExpr* expr);

        static bool GetBoolLiteral(const HirExpr* expr);

        static bool SimplifyBinary(HirExprPtr& expr);
        
    };

}