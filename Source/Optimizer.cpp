#include "Rux/Optimizer.h"

#include <memory>
#include <string>
#include <print>

namespace Rux {
    void Optimizer::Run(HirPackage& package) {
        for (auto& module : package.modules) {
            OptimizeModule(module);
        }
    }

    void Optimizer::OptimizeModule(HirModule& module) {
        for (auto& func : module.funcs) {
            OptimizeFunc(func);
        }

        for (auto& impl : module.impls) {
            for (auto& method : impl.methods) {
                OptimizeFunc(method);
            }
        }

        for (auto& c : module.consts) {
            OptimizeExpr(c.value);
        }
    }

    void Optimizer::OptimizeFunc(HirFunc& func) {
        if (func.body) {
            OptimizeBlock(*func.body);
        }
    }

    void Optimizer::OptimizeBlock(HirBlock& block) {
        for (auto& stmt : block.stmts) {
            OptimizeStmt(stmt);
        }
    }

    void Optimizer::OptimizeStmt(HirStmtPtr& stmt) {
        if (auto* s = dynamic_cast<HirExprStmt*>(stmt.get())) {
            OptimizeExpr(s->expr);
        }
        else if (auto* s = dynamic_cast<HirLetStmt*>(stmt.get())) {
            if (s->init) {
                OptimizeExpr(s->init);
            }
        }
        else if (auto* s = dynamic_cast<HirReturnStmt*>(stmt.get())) {
            if (s->value) {
                OptimizeExpr(*s->value);
            }
        }
    }

    void Optimizer::OptimizeExpr(HirExprPtr& expr) {
        if (!expr) {
            return;
        }

        if (auto* bin = dynamic_cast<HirBinaryExpr*>(expr.get())) {
            OptimizeExpr(bin->left);
            OptimizeExpr(bin->right);
            
            if (FoldBinary(expr)) {
                return;
            }

            SimplifyBinary(expr);
        }
        else if (auto* unary = dynamic_cast<HirUnaryExpr*>(expr.get())) {
            OptimizeExpr(unary->operand);
            FoldUnary(expr);
        }
    }

    bool Optimizer::IsIntegerLiteral(const HirExpr* expr) {
        const auto* lit = dynamic_cast<const HirLiteralExpr*>(expr);
        return lit && lit->type.IsInteger();
    }

    bool Optimizer::IsBoolLiteral(const HirExpr* expr) {
        const auto* lit = dynamic_cast<const HirLiteralExpr*>(expr);
        return lit && lit->type.IsBool();
    }

    std::int64_t Optimizer::GetIntegerLiteral(const HirExpr* expr) {
        const auto* lit = dynamic_cast<const HirLiteralExpr*>(expr);
        if (!lit) {
            return 0;
        }
        return std::stoll(lit->value);
    }

    bool Optimizer::GetBoolLiteral(const HirExpr* expr) {
        const auto* lit = dynamic_cast<const HirLiteralExpr*>(expr);
        if (!lit) {
            return false;
        }
        return lit->value == "true";
    }

    HirExprPtr Optimizer::MakeIntegerLiteral(std::int64_t value,
                                             const TypeRef& type) {
        auto lit = std::make_unique<HirLiteralExpr>();
        lit->type = type;
        lit->value = std::to_string(value);
        return lit;
    }

    HirExprPtr Optimizer::MakeBoolLiteral(bool value, const TypeRef& type) {
        auto lit = std::make_unique<HirLiteralExpr>();
        lit->type = type;
        lit->value = value ? "true" : "false";
        return lit;
    }

    bool Optimizer::FoldBinary(HirExprPtr& expr) {
        auto* bin = dynamic_cast<HirBinaryExpr*>(expr.get());
        if (!bin) {
            return false;
        }

        const bool leftIsInt = IsIntegerLiteral(bin->left.get());
        const bool rightIsInt = IsIntegerLiteral(bin->right.get());
        const bool leftIsBool = IsBoolLiteral(bin->left.get());
        const bool rightIsBool = IsBoolLiteral(bin->right.get());

        // Integer folding
        if (leftIsInt && rightIsInt) {
            const auto lhs = GetIntegerLiteral(bin->left.get());
            const auto rhs = GetIntegerLiteral(bin->right.get());

            switch (bin->op) {
            case TokenKind::Plus:
                expr = MakeIntegerLiteral(lhs + rhs, bin->type);
                return true;
            case TokenKind::Minus:
                expr = MakeIntegerLiteral(lhs - rhs, bin->type);
                return true;
            case TokenKind::Star:
                expr = MakeIntegerLiteral(lhs * rhs, bin->type);
                return true;
            case TokenKind::Slash:
                if (rhs == 0) {
                    return false;
                }
                expr = MakeIntegerLiteral(lhs / rhs, bin->type);
                return true;
            case TokenKind::Percent:
                if (rhs == 0) {
                    return false;
                }
                expr = MakeIntegerLiteral(lhs % rhs, bin->type);
                return true;
            case TokenKind::Equal:
                expr = MakeBoolLiteral(lhs == rhs, bin->type);
                return true;
            case TokenKind::BangEqual:
                expr = MakeBoolLiteral(lhs != rhs, bin->type);
                return true;
            case TokenKind::Less:
                expr = MakeBoolLiteral(lhs < rhs, bin->type);
                return true;
            case TokenKind::LessEqual:
                expr = MakeBoolLiteral(lhs <= rhs, bin->type);
                return true;
            case TokenKind::Greater:
                expr = MakeBoolLiteral(lhs > rhs, bin->type);
                return true;
            case TokenKind::GreaterEqual:
                expr = MakeBoolLiteral(lhs >= rhs, bin->type);
                return true;
            default:
                return false;
            }
        }

        // Bool folding
        if (leftIsBool && rightIsBool) {
            const bool lhs = GetBoolLiteral(bin->left.get());
            const bool rhs = GetBoolLiteral(bin->right.get());

            switch (bin->op) {
            case TokenKind::Equal:
                expr = MakeBoolLiteral(lhs == rhs, bin->type);
                return true;
            case TokenKind::BangEqual:
                expr = MakeBoolLiteral(lhs != rhs, bin->type);
                return true;
            case TokenKind::AmpAmp:
                expr = MakeBoolLiteral(lhs && rhs, bin->type);
                return true;
            case TokenKind::PipePipe:
                expr = MakeBoolLiteral(lhs || rhs, bin->type);
                return true;            
            default:
                return false;
            }
        }

        return false;
    }

    bool Optimizer::FoldUnary(HirExprPtr& expr) {
        auto* unary = dynamic_cast<HirUnaryExpr*>(expr.get());
        if (!unary) {
            return false;
        }

        if (IsIntegerLiteral(unary->operand.get())) {
            const auto value = GetIntegerLiteral(unary->operand.get());

            switch (unary->op) {
            case TokenKind::Minus:
                expr = MakeIntegerLiteral(-value, unary->type);
                return true;
            case TokenKind::Plus:
                expr = MakeIntegerLiteral(value, unary->type);
                return true;
            case TokenKind::Tilde:
                expr = MakeIntegerLiteral(~value, unary->type);
                return true;    
            default:
                return false;
            }
        }

        if (IsBoolLiteral(unary->operand.get())) {
            const bool value = GetBoolLiteral(unary->operand.get());

            switch (unary->op) {
            case TokenKind::Bang:
                expr = MakeBoolLiteral(!value, unary->type);
                return true;
            default:
                return false;
            }
        }

        return false;
    }

    bool Optimizer::SimplifyBinary(HirExprPtr& expr) {
        auto* bin = dynamic_cast<HirBinaryExpr*>(expr.get());
        if (!bin) {
            return false;
        }

        switch (bin->op) {
        case TokenKind::Plus:
            if (IsIntegerLiteral(bin->left.get()) &&
                GetIntegerLiteral(bin->left.get()) == 0) {
                expr = std::move(bin->right);
                return true;
            }

            if (IsIntegerLiteral(bin->right.get()) &&
                GetIntegerLiteral(bin->right.get()) == 0) {
                expr = std::move(bin->left);
                return true;
            }
            break;

        case TokenKind::Minus:
            if (IsIntegerLiteral(bin->right.get()) &&
                GetIntegerLiteral(bin->right.get()) == 0) {
                expr = std::move(bin->left);
                return true;
            }
            break;

        case TokenKind::Star:
            if (IsIntegerLiteral(bin->left.get())) {
                const auto value = GetIntegerLiteral(bin->left.get());

                if (value == 0) {
                    expr = MakeIntegerLiteral(0, bin->type);
                    return true;
                }

                if (value == 1) {
                    expr = std::move(bin->right);
                    return true;
                }
            }

            if (IsIntegerLiteral(bin->right.get())) {
                const auto value = GetIntegerLiteral(bin->right.get());

                if (value == 0) {
                    expr = MakeIntegerLiteral(0, bin->type);
                    return true;
                }

                if (value == 1) {
                    expr = std::move(bin->left);
                    return true;
                }
            }
            break;

        case TokenKind::Slash:
            if (IsIntegerLiteral(bin->right.get()) &&
                GetIntegerLiteral(bin->right.get()) == 1) {
                expr = std::move(bin->left);
                return true;
            }
            break;
            
        case TokenKind::Percent:
            if (IsIntegerLiteral(bin->right.get()) &&
                GetIntegerLiteral(bin->right.get()) == 1) {
                expr = MakeIntegerLiteral(0, bin->type);
                return true;
            }
            break;

        default:
            break;
        }

        return false;
    }
} // namespace Rux