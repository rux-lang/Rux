#include "Optimization/LegacyHirOptimizer.h"

#include <charconv>
#include <memory>
#include <optional>
#include <string>

namespace Rux::Optimization {
std::string_view LegacyHirOptimizer::Name() const noexcept {
    return "legacy-hir-optimizer";
}

PassChange LegacyHirOptimizer::Run(HirPackage &package, const PassContext &) {
    changed_ = false;
    constants_.clear();
    for (auto &module : package.modules) {
        OptimizeModule(module);
    }
    return changed_ ? PassChange::Changed : PassChange::None;
}

void LegacyHirOptimizer::OptimizeModule(HirModule &module) {
    for (auto &func : module.funcs) {
        OptimizeFunc(func);
    }

    for (auto &impl : module.impls) {
        for (auto &method : impl.methods) {
            OptimizeFunc(method);
        }
    }

    for (auto &c : module.consts) {
        constants_.clear();
        OptimizeExpr(c.value);
    }
}

void LegacyHirOptimizer::OptimizeFunc(HirFunc &func) {
    constants_.clear();
    if (func.body) {
        OptimizeBlock(*func.body);
    }
}

void LegacyHirOptimizer::OptimizeBlock(HirBlock &block) {
    std::vector<HirStmtPtr> optimizedStmts;
    bool unreachable = false;

    for (auto &stmt : block.stmts) {
        if (!stmt)
            continue;

        if (unreachable) {
            // Élimination du code mort
            changed_ = true;
            continue;
        }

        // On optimise le statement de manière itérative s'il se simplifie en un nouveau IfStmt
        bool stmtProcessed = false;
        while (stmt && !stmtProcessed) {
            OptimizeStmt(stmt);

            if (auto *ifStmt = dynamic_cast<HirIfStmt *>(stmt.get())) {
                if (ifStmt->condition && IsBoolLiteral(ifStmt->condition.get())) {
                    bool condValue = GetBoolLiteral(ifStmt->condition.get());
                    if (condValue) {
                        // La condition est true : on insère les statements du thenBlock
                        for (auto &subStmt : ifStmt->thenBlock.stmts) {
                            if (subStmt) {
                                if (unreachable)
                                    continue;
                                if (dynamic_cast<const HirReturnStmt *>(subStmt.get()) ||
                                    dynamic_cast<const HirBreakStmt *>(subStmt.get()) ||
                                    dynamic_cast<const HirContinueStmt *>(subStmt.get())) {
                                    unreachable = true;
                                }
                                optimizedStmts.push_back(std::move(subStmt));
                            }
                        }
                        changed_ = true;
                        stmt.reset();
                        stmtProcessed = true;
                    }
                    else {
                        // La condition est false
                        if (ifStmt->elseIfs.empty()) {
                            if (ifStmt->elseBlock) {
                                // On insère le contenu de elseBlock
                                for (auto &subStmt : ifStmt->elseBlock->stmts) {
                                    if (subStmt) {
                                        if (unreachable)
                                            continue;
                                        if (dynamic_cast<const HirReturnStmt *>(subStmt.get()) ||
                                            dynamic_cast<const HirBreakStmt *>(subStmt.get()) ||
                                            dynamic_cast<const HirContinueStmt *>(subStmt.get())) {
                                            unreachable = true;
                                        }
                                        optimizedStmts.push_back(std::move(subStmt));
                                    }
                                }
                            }
                            changed_ = true;
                            stmt.reset();
                            stmtProcessed = true;
                        }
                        else {
                            // On transforme le elseIf en un nouveau IfStmt et on continue l'optimisation
                            auto firstElseIf = std::move(ifStmt->elseIfs[0]);
                            auto newIfStmt = std::make_unique<HirIfStmt>();
                            newIfStmt->location = firstElseIf.location;
                            newIfStmt->condition = std::move(firstElseIf.condition);
                            newIfStmt->thenBlock = std::move(firstElseIf.block);
                            newIfStmt->elseIfs.assign(std::make_move_iterator(ifStmt->elseIfs.begin() + 1),
                                                      std::make_move_iterator(ifStmt->elseIfs.end()));
                            newIfStmt->elseBlock = std::move(ifStmt->elseBlock);

                            stmt = std::move(newIfStmt);
                            changed_ = true;
                            // La boucle continue pour optimiser ce nouveau IfStmt
                        }
                    }
                }
                else {
                    stmtProcessed = true;
                }
            }
            else {
                stmtProcessed = true;
            }
        }

        if (stmt && stmtProcessed) {
            if (dynamic_cast<const HirReturnStmt *>(stmt.get()) || dynamic_cast<const HirBreakStmt *>(stmt.get()) ||
                dynamic_cast<const HirContinueStmt *>(stmt.get())) {
                unreachable = true;
            }
            optimizedStmts.push_back(std::move(stmt));
        }
    }
    block.stmts = std::move(optimizedStmts);
}

void LegacyHirOptimizer::OptimizeStmt(HirStmtPtr &stmt) {
    if (auto *exprStmt = dynamic_cast<HirExprStmt *>(stmt.get())) {
        OptimizeExpr(exprStmt->expr);
    }
    else if (auto *letStmt = dynamic_cast<HirLetStmt *>(stmt.get())) {
        if (letStmt->init) {
            OptimizeExpr(letStmt->init);

            // only track immutable bindings, mutable tracking is not supported (yet)
            if (!letStmt->isMut) {
                // The binding's own type, not the literal's. A `let flag: bool16 =
                // false` has an init of the default bool width, and substituting
                // the literal at its own type would drop the binding down to that
                // width: a later address-of would then hand out a slot narrower
                // than the type it stands for, and whoever read it back at the
                // declared width would take in the bytes above it.
                const TypeRef &type = letStmt->type.IsUnknown() ? letStmt->init->type : letStmt->type;
                if (IsIntegerLiteral(letStmt->init.get())) {
                    if (const auto value = GetIntegerLiteral(letStmt->init.get())) {
                        constants_[letStmt->name] = ConstantValue{false, *value, false, type};
                    }
                }
                else if (IsBoolLiteral(letStmt->init.get())) {
                    constants_[letStmt->name] = {true, 0, GetBoolLiteral(letStmt->init.get()), type};
                }
            }
        }
    }
    else if (auto *retStmt = dynamic_cast<HirReturnStmt *>(stmt.get())) {
        if (retStmt->value) {
            OptimizeExpr(*retStmt->value);
        }
    }
    else if (auto *ifStmt = dynamic_cast<HirIfStmt *>(stmt.get())) {
        OptimizeExpr(ifStmt->condition);
        OptimizeBlock(ifStmt->thenBlock);
        for (auto &elif : ifStmt->elseIfs) {
            OptimizeExpr(elif.condition);
            OptimizeBlock(elif.block);
        }
        if (ifStmt->elseBlock) {
            OptimizeBlock(*ifStmt->elseBlock);
        }
    }
    else if (auto *whileStmt = dynamic_cast<HirWhileStmt *>(stmt.get())) {
        OptimizeExpr(whileStmt->condition);
        OptimizeBlock(whileStmt->body);
    }
    else if (auto *doWhileStmt = dynamic_cast<HirDoWhileStmt *>(stmt.get())) {
        OptimizeBlock(doWhileStmt->body);
        OptimizeExpr(doWhileStmt->condition);
    }
    else if (auto *loopStmt = dynamic_cast<HirLoopStmt *>(stmt.get())) {
        OptimizeBlock(loopStmt->body);
    }
    else if (auto *forStmt = dynamic_cast<HirForStmt *>(stmt.get())) {
        OptimizeExpr(forStmt->iterable);
        OptimizeBlock(forStmt->body);
    }
    else if (auto *matchStmt = dynamic_cast<HirMatchStmt *>(stmt.get())) {
        OptimizeExpr(matchStmt->subject);
        for (auto &arm : matchStmt->arms) {
            OptimizeExpr(arm.body);
        }
    }
}

void LegacyHirOptimizer::OptimizeExpr(HirExprPtr &expr) {
    if (!expr) {
        return;
    }

    if (auto *var = dynamic_cast<HirVarExpr *>(expr.get())) {
        auto it = constants_.find(var->name);

        if (it != constants_.end()) {
            const auto &value = it->second;

            if (value.isBool) {
                expr = MakeBoolLiteral(value.boolValue, value.type);
            }
            else {
                expr = MakeIntegerLiteral(value.intValue, value.type);
            }

            changed_ = true;
            return;
        }
    }

    if (auto *bin = dynamic_cast<HirBinaryExpr *>(expr.get())) {
        OptimizeExpr(bin->left);
        OptimizeExpr(bin->right);

        if (FoldBinary(expr)) {
            changed_ = true;
            return;
        }

        if (SimplifyBinary(expr)) {
            changed_ = true;
        }
    }
    else if (auto *unary = dynamic_cast<HirUnaryExpr *>(expr.get())) {
        OptimizeExpr(unary->operand);
        if (FoldUnary(expr)) {
            changed_ = true;
        }
    }
    else if (auto *tern = dynamic_cast<HirTernaryExpr *>(expr.get())) {
        OptimizeExpr(tern->condition);
        OptimizeExpr(tern->thenExpr);
        OptimizeExpr(tern->elseExpr);
    }
    else if (auto *assign = dynamic_cast<HirAssignExpr *>(expr.get())) {
        OptimizeExpr(assign->target);
        OptimizeExpr(assign->value);
    }
    else if (auto *range = dynamic_cast<HirRangeExpr *>(expr.get())) {
        OptimizeExpr(range->lo);
        OptimizeExpr(range->hi);
    }
    else if (auto *call = dynamic_cast<HirCallExpr *>(expr.get())) {
        OptimizeExpr(call->callee);
        for (auto &arg : call->args) {
            OptimizeExpr(arg);
        }
    }
    else if (auto *coerce = dynamic_cast<HirCoerceToInterfaceExpr *>(expr.get())) {
        OptimizeExpr(coerce->value);
    }
    else if (auto *arrayView = dynamic_cast<HirArrayToSliceExpr *>(expr.get())) {
        OptimizeExpr(arrayView->value);
    }
    else if (auto *ifaceCall = dynamic_cast<HirInterfaceCallExpr *>(expr.get())) {
        OptimizeExpr(ifaceCall->fatPtrExpr);
        for (auto &arg : ifaceCall->args) {
            OptimizeExpr(arg);
        }
    }
    else if (auto *idx = dynamic_cast<HirIndexExpr *>(expr.get())) {
        OptimizeExpr(idx->object);
        OptimizeExpr(idx->index);
    }
    else if (auto *field = dynamic_cast<HirFieldExpr *>(expr.get())) {
        OptimizeExpr(field->object);
    }
    else if (auto *structInit = dynamic_cast<HirStructInitExpr *>(expr.get())) {
        for (auto &f : structInit->fields) {
            OptimizeExpr(f.value);
        }
    }
    else if (auto *slice = dynamic_cast<HirArrayExpr *>(expr.get())) {
        for (auto &el : slice->elements) {
            OptimizeExpr(el);
        }
    }
    else if (auto *tuple = dynamic_cast<HirTupleExpr *>(expr.get())) {
        for (auto &el : tuple->elements) {
            OptimizeExpr(el);
        }
    }
    else if (auto *cast = dynamic_cast<HirCastExpr *>(expr.get())) {
        OptimizeExpr(cast->operand);
    }
    else if (auto *isExpr = dynamic_cast<HirIsExpr *>(expr.get())) {
        OptimizeExpr(isExpr->operand);
    }
    else if (auto *blockExpr = dynamic_cast<HirBlockExpr *>(expr.get())) {
        OptimizeBlock(blockExpr->block);
    }
    else if (auto *matchExpr = dynamic_cast<HirMatchExpr *>(expr.get())) {
        OptimizeExpr(matchExpr->subject);
        for (auto &arm : matchExpr->arms) {
            OptimizeExpr(arm.body);
        }
    }
    else if (auto *enumConst = dynamic_cast<HirEnumConstructExpr *>(expr.get())) {
        for (auto &payload : enumConst->payloads) {
            OptimizeExpr(payload);
        }
    }
}

bool LegacyHirOptimizer::IsIntegerLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    return lit && lit->type.IsInteger();
}

bool LegacyHirOptimizer::IsBoolLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    return lit && lit->type.IsBool();
}

std::optional<std::int64_t> LegacyHirOptimizer::GetIntegerLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    if (!lit) {
        return std::nullopt;
    }

    std::string text;
    text.reserve(lit->value.size());
    for (const char c : lit->value) {
        if (c != '_') {
            text.push_back(c);
        }
    }

    for (const std::string_view suffix :
         {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "i", "u", "f"}) {
        if (text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0) {
            text.erase(text.size() - suffix.size());
            break;
        }
    }

    bool isNegative = false;
    std::string_view digits(text);
    if (!digits.empty() && digits[0] == '-') {
        isNegative = true;
        digits.remove_prefix(1);
    }
    else if (!digits.empty() && digits[0] == '+') {
        digits.remove_prefix(1);
    }

    int base = 10;
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }

    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t uvalue = 0;
    const auto *first = digits.data();
    const auto *last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, uvalue, base);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }

    std::int64_t svalue = static_cast<std::int64_t>(uvalue);
    if (isNegative) {
        svalue = -static_cast<std::int64_t>(uvalue);
    }
    return svalue;
}

bool LegacyHirOptimizer::GetBoolLiteral(const HirExpr *expr) {
    const auto *lit = dynamic_cast<const HirLiteralExpr *>(expr);
    if (!lit) {
        return false;
    }
    return lit->value == "true";
}

static std::int64_t TruncateToType(std::int64_t value, const TypeRef &type) {
    if (!type.IsInteger()) {
        return value;
    }

    int bits = 64;
    switch (type.kind) {
    case TypeRef::Kind::Int8:
    case TypeRef::Kind::UInt8:
        bits = 8;
        break;
    case TypeRef::Kind::Int16:
    case TypeRef::Kind::UInt16:
        bits = 16;
        break;
    case TypeRef::Kind::Int32:
    case TypeRef::Kind::UInt32:
        bits = 32;
        break;
    default:
        bits = 64;
        break;
    }

    if (bits == 64) {
        return value;
    }

    std::uint64_t mask = (1ULL << bits) - 1;
    std::uint64_t uval = static_cast<std::uint64_t>(value) & mask;

    if (type.IsSigned()) {
        std::uint64_t sign_bit = 1ULL << (bits - 1);
        if (uval & sign_bit) {
            uval |= ~mask;
        }
    }
    return static_cast<std::int64_t>(uval);
}

HirExprPtr LegacyHirOptimizer::MakeIntegerLiteral(std::int64_t value, const TypeRef &type) {
    auto lit = std::make_unique<HirLiteralExpr>();
    lit->type = type;
    lit->value = std::to_string(TruncateToType(value, type));
    return lit;
}

HirExprPtr LegacyHirOptimizer::MakeBoolLiteral(bool value, const TypeRef &type) {
    auto lit = std::make_unique<HirLiteralExpr>();
    lit->type = type;
    lit->value = value ? "true" : "false";
    return lit;
}

bool LegacyHirOptimizer::FoldBinary(HirExprPtr &expr) {
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

bool LegacyHirOptimizer::FoldUnary(HirExprPtr &expr) {
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

bool LegacyHirOptimizer::SimplifyBinary(HirExprPtr &expr) {
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
} // namespace Rux::Optimization
