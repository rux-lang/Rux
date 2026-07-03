#include "Ir/Hir/Passes/PassManager.h"

#include <charconv>
#include <memory>
#include <optional>
#include <string>

namespace Rux {
void HirPassManager::Run(HirPackage &package) {
    for (auto &module : package.modules) {
        OptimizeModule(module);
    }
}

void HirPassManager::OptimizeModule(HirModule &module) {
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

void HirPassManager::OptimizeFunc(HirFunc &func) {
    constants.clear();
    if (func.body) {
        OptimizeBlock(*func.body);
    }
}

void HirPassManager::OptimizeBlock(HirBlock &block) {
    for (auto &stmt : block.stmts) {
        OptimizeStmt(stmt);
    }
}

void HirPassManager::OptimizeStmt(HirStmtPtr &stmt) {
    if (auto *exprStmt = dynamic_cast<HirExprStmt *>(stmt.get())) {
        OptimizeExpr(exprStmt->expr);
    }
    else if (auto *letStmt = dynamic_cast<HirLetStmt *>(stmt.get())) {
        if (letStmt->init) {
            OptimizeExpr(letStmt->init);

            // only track immutable bindings, mutable tracking is not supported (yet)
            if (!letStmt->isMut) {
                if (IsIntegerLiteral(letStmt->init.get())) {
                    if (const auto value = GetIntegerLiteral(letStmt->init.get())) {
                        constants[letStmt->name] = ConstantValue{false, *value, false, letStmt->init->type};
                    }
                }
                else if (IsBoolLiteral(letStmt->init.get())) {
                    constants[letStmt->name] = {true, 0, GetBoolLiteral(letStmt->init.get()), letStmt->init->type};
                }
            }
        }
    }
    else if (auto *retStmt = dynamic_cast<HirReturnStmt *>(stmt.get())) {
        if (retStmt->value) {
            OptimizeExpr(*retStmt->value);
        }
    }
}

void HirPassManager::OptimizeExpr(HirExprPtr &expr) {
    if (!expr) {
        return;
    }

    if (auto *var = dynamic_cast<HirVarExpr *>(expr.get())) {
        auto it = constants.find(var->name);

        if (it != constants.end()) {
            const auto &value = it->second;

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

bool HirPassManager::IsIntegerLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    return lit && lit->type.IsInteger();
}

bool HirPassManager::IsBoolLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    return lit && lit->type.IsBool();
}

std::optional<std::int64_t> HirPassManager::GetIntegerLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    if (!lit) {
        return std::nullopt;
    }
    std::int64_t result;
    auto [ptr, ec] = std::from_chars(lit->value.data(), lit->value.data() + lit->value.size(), result);
    if (ec == std::errc()) {
        return result;
    }
    return std::nullopt;
}

bool HirPassManager::GetBoolLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    if (!lit) {
        return false;
    }
    return lit->value == "true";
}

HirExprPtr HirPassManager::MakeIntegerLiteral(std::int64_t value, const TypeRef &type) {
    auto lit = std::make_unique<HirLiteralExpr>();
    lit->type = type;
    lit->value = std::to_string(value);
    return lit;
}

HirExprPtr HirPassManager::MakeBoolLiteral(bool value, const TypeRef &type) {
    auto lit = std::make_unique<HirLiteralExpr>();
    lit->type = type;
    lit->value = value ? "true" : "false";
    return lit;
}

bool HirPassManager::FoldBinary(HirExprPtr &expr) {
    auto *bin = dynamic_cast<HirBinaryExpr *>(expr.get());
    if (!bin) {
        return false;
    }

    const bool leftIsInt = IsIntegerLiteral(bin->left.get());
    const bool rightIsInt = IsIntegerLiteral(bin->right.get());
    const bool leftIsBool = IsBoolLiteral(bin->left.get());
    const bool rightIsBool = IsBoolLiteral(bin->right.get());

    // Integer folding
    if (leftIsInt && rightIsInt) {
        const auto lhsOpt = GetIntegerLiteral(bin->left.get());
        const auto rhsOpt = GetIntegerLiteral(bin->right.get());
        if (!lhsOpt || !rhsOpt) {
            return false;
        }

        const auto lhs = *lhsOpt;
        const auto rhs = *rhsOpt;

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

bool HirPassManager::FoldUnary(HirExprPtr &expr) {
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
        const auto valueOpt = GetIntegerLiteral(unary->operand.get());
        if (!valueOpt) {
            return false;
        }

        const auto value = *valueOpt;

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

bool HirPassManager::SimplifyBinary(HirExprPtr &expr) {
    auto *bin = dynamic_cast<HirBinaryExpr *>(expr.get());
    if (!bin) {
        return false;
    }

    switch (bin->op) {
    case TokenKind::Plus: {
        const auto leftOpt = IsIntegerLiteral(bin->left.get()) ? GetIntegerLiteral(bin->left.get()) : std::nullopt;
        if (leftOpt && *leftOpt == 0) {
            expr = std::move(bin->right);
            return true;
        }

        const auto rightOpt = IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
        if (rightOpt && *rightOpt == 0) {
            expr = std::move(bin->left);
            return true;
        }
        break;
    }

    case TokenKind::Minus: {
        const auto opt = IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
        if (opt && *opt == 0) {
            expr = std::move(bin->left);
            return true;
        }
        break;
    }

    case TokenKind::Star: {
        auto getVal = [](const HirExpr *e) -> std::optional<std::int64_t> {
            return IsIntegerLiteral(e) ? GetIntegerLiteral(e) : std::nullopt;
        };

        if (const auto v = getVal(bin->left.get())) {
            if (*v == 0) {
                expr = MakeIntegerLiteral(0, bin->type);
                return true;
            }
            if (*v == 1) {
                expr = std::move(bin->right);
                return true;
            }
        }

        if (const auto v = getVal(bin->right.get())) {
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
        const auto opt = IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
        if (opt && *opt == 1) {
            expr = std::move(bin->left);
            return true;
        }
        break;
    }

    case TokenKind::Percent: {
        const auto opt = IsIntegerLiteral(bin->right.get()) ? GetIntegerLiteral(bin->right.get()) : std::nullopt;
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
