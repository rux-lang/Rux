#include "Rux/Optimizer.h"

#include <charconv>
#include <memory>
#include <optional>
#include <string>

namespace Rux {
void Optimizer::Run(HirPackage &package) {
    for (auto &module : package.modules) {
        OptimizeModule(module);
    }
}

void Optimizer::OptimizeModule(HirModule &module) {
    for (auto &func : module.funcs) {
        OptimizeFunc(func);
    }

    for (auto &impl : module.impls) {
        for (auto &method : impl.methods) {
            OptimizeFunc(method);
        }
    }

    for (auto &c : module.consts) {
        OptimizeExpr(c.value);
    }
}

void Optimizer::OptimizeFunc(HirFunc &func) {
    constants.clear();
    if (func.body) {
        OptimizeBlock(*func.body);
    }
}

void Optimizer::OptimizeBlock(HirBlock &block) {
    for (auto &stmt : block.stmts) {
        OptimizeStmt(stmt);
    }
}

void Optimizer::OptimizeStmt(HirStmtPtr &stmt) {
    if (auto *s = dynamic_cast<HirExprStmt *>(stmt.get())) {
        OptimizeExpr(s->expr);
    }
    else if (auto *s = dynamic_cast<HirLetStmt *>(stmt.get())) {
        if (s->init) {
            OptimizeExpr(s->init);

            // only track immutable bindings, mutable tracking is not supported (yet)
            if (!s->isMut) {
                bool sExists = false;

                if (IsIntegerLiteral(s->init.get())) {
                    sExists = true;
                }

                if (IsIntegerLiteral(s->init.get())) {
                    constants[s->name] = ConstantValue{
                        false, GetIntegerLiteral(s->init.get()).value(), false, s->init->type};
                }
                else if (IsBoolLiteral(s->init.get())) {
                    constants[s->name] = {true, 0, GetBoolLiteral(s->init.get()), s->init->type};
                }
            }
        }
    }
    else if (auto *s = dynamic_cast<HirReturnStmt *>(stmt.get())) {
        if (s->value) {
            OptimizeExpr(*s->value);
        }
    }
}

void Optimizer::OptimizeExpr(HirExprPtr &expr) {
    if (!expr) {
        return;
    }

    if (auto *var = dynamic_cast<HirVarExpr *>(expr.get())) {
        auto it = constants.find(var->name);

        if (it != constants.end()) {
            auto const &value = it->second;

            if (value.isBool) {
                expr = MakeBoolLiteral(value.boolValue, value.type);
            }
            else {
                expr = MakeIntegerLiteral(value.intValue, value.type);
            }

            return;
        }
    }

    if (auto *bin = dynamic_cast<HirBinaryExpr *>(expr.get())) {
        OptimizeExpr(bin->left);
        OptimizeExpr(bin->right);

        if (FoldBinary(expr)) {
            return;
        }

        SimplifyBinary(expr);
    }
    else if (auto *unary = dynamic_cast<HirUnaryExpr *>(expr.get())) {
        OptimizeExpr(unary->operand);
        FoldUnary(expr);
    }
}

bool Optimizer::IsIntegerLiteral(HirExpr const *expr) {
    auto const *lit = dynamic_cast<HirLiteralExpr const *>(expr);
    return lit && lit->type.IsInteger();
}

bool Optimizer::IsBoolLiteral(HirExpr const *expr) {
    auto const *lit = dynamic_cast<HirLiteralExpr const *>(expr);
    return lit && lit->type.IsBool();
}

std::optional<std::int64_t> Optimizer::GetIntegerLiteral(HirExpr const *expr) {
    auto const *lit = dynamic_cast<HirLiteralExpr const *>(expr);
    if (!lit) {
        return std::nullopt;
    }
    std::int64_t result;
    auto [ptr, ec] =
        std::from_chars(lit->value.data(), lit->value.data() + lit->value.size(), result);
    if (ec == std::errc()) {
        return result;
    }
    return std::nullopt;
}

bool Optimizer::GetBoolLiteral(HirExpr const *expr) {
    auto const *lit = dynamic_cast<HirLiteralExpr const *>(expr);
    if (!lit) {
        return false;
    }
    return lit->value == "true";
}

HirExprPtr Optimizer::MakeIntegerLiteral(std::int64_t value, TypeRef const &type) {
    auto lit = std::make_unique<HirLiteralExpr>();
    lit->type = type;
    lit->value = std::to_string(value);
    return lit;
}

HirExprPtr Optimizer::MakeBoolLiteral(bool value, TypeRef const &type) {
    auto lit = std::make_unique<HirLiteralExpr>();
    lit->type = type;
    lit->value = value ? "true" : "false";
    return lit;
}

bool Optimizer::FoldBinary(HirExprPtr &expr) {
    auto *bin = dynamic_cast<HirBinaryExpr *>(expr.get());
    if (!bin) {
        return false;
    }

    bool const leftIsInt = IsIntegerLiteral(bin->left.get());
    bool const rightIsInt = IsIntegerLiteral(bin->right.get());
    bool const leftIsBool = IsBoolLiteral(bin->left.get());
    bool const rightIsBool = IsBoolLiteral(bin->right.get());

    // Integer folding
    if (leftIsInt && rightIsInt) {
        auto const lhsOpt = GetIntegerLiteral(bin->left.get());
        auto const rhsOpt = GetIntegerLiteral(bin->right.get());
        if (!lhsOpt || !rhsOpt) {
            return false;
        }

        auto const lhs = *lhsOpt;
        auto const rhs = *rhsOpt;

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
        bool const lhs = GetBoolLiteral(bin->left.get());
        bool const rhs = GetBoolLiteral(bin->right.get());

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

bool Optimizer::FoldUnary(HirExprPtr &expr) {
    auto *unary = dynamic_cast<HirUnaryExpr *>(expr.get());

    if (!unary) {
        return false;
    }

    if (unary->op == TokenKind::Bang) {
        if (auto *inner = dynamic_cast<HirUnaryExpr *>(unary->operand.get())) {
            if (inner->op == TokenKind::Bang) {
                expr = std::move(inner->operand);
                return true;
            }
        }
    }

    if (IsIntegerLiteral(unary->operand.get())) {
        auto const valueOpt = GetIntegerLiteral(unary->operand.get());
        if (!valueOpt) {
            return false;
        }

        auto const value = *valueOpt;

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
        bool const value = GetBoolLiteral(unary->operand.get());

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

bool Optimizer::SimplifyBinary(HirExprPtr &expr) {
    auto *bin = dynamic_cast<HirBinaryExpr *>(expr.get());
    if (!bin) {
        return false;
    }

    switch (bin->op) {
    case TokenKind::Plus: {
        auto const leftOpt =
            IsIntegerLiteral(bin->left.get()) ? GetIntegerLiteral(bin->left.get()) : std::nullopt;
        if (leftOpt && *leftOpt == 0) {
            expr = std::move(bin->right);
            return true;
        }

        auto const rightOpt =
            IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
        if (rightOpt && *rightOpt == 0) {
            expr = std::move(bin->left);
            return true;
        }
        break;
    }

    case TokenKind::Minus: {
        auto const opt =
            IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
        if (opt && *opt == 0) {
            expr = std::move(bin->left);
            return true;
        }
        break;
    }

    case TokenKind::Star: {
        auto getVal = [](HirExpr const *e) -> std::optional<std::int64_t> {
            return IsIntegerLiteral(e) ? GetIntegerLiteral(e) : std::nullopt;
        };

        if (auto const v = getVal(bin->left.get())) {
            if (*v == 0) {
                expr = MakeIntegerLiteral(0, bin->type);
                return true;
            }
            if (*v == 1) {
                expr = std::move(bin->right);
                return true;
            }
        }

        if (auto const v = getVal(bin->right.get())) {
            if (*v == 0) {
                expr = MakeIntegerLiteral(0, bin->type);
                return true;
            }
            if (*v == 1) {
                expr = std::move(bin->left);
                return true;
            }
        }
        break;
    }

    case TokenKind::Slash: {
        auto const opt =
            IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
        if (opt && *opt == 1) {
            expr = std::move(bin->left);
            return true;
        }
        break;
    }

    case TokenKind::Percent: {
        auto const opt =
            IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
        if (opt && *opt == 1) {
            expr = MakeIntegerLiteral(0, bin->type);
            return true;
        }
        break;
    }

    default:
        break;
    }

    return false;
}
} // namespace Rux
