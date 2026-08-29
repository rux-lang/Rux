#include "Optimization/HirConstantFolder.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace Rux::Optimization {
namespace {
/// A literal's digits without its type suffix, so `42i64` can be parsed as a value once the type is already known from
/// the expression.
std::string_view WithoutIntegerSuffix(const std::string_view literal) {
    constexpr std::array suffixes{"i16", "i32", "i64", "u16", "u32", "u64", "i8", "u8", "i", "u"};
    for (const std::string_view suffix : suffixes) {
        if (literal.size() > suffix.size() && literal.ends_with(suffix)) {
            return literal.substr(0, literal.size() - suffix.size());
        }
    }
    return literal;
}
} // namespace

std::string_view HirConstantFolder::Name() const noexcept {
    return "hir-constant-folder";
}

PassChange HirConstantFolder::Run(HirPackage &package, const PassContext &) {
    changed = false;
    scopes.clear();
    for (auto &module : package.modules) {
        OptimizeModule(module);
    }
    return changed ? PassChange::Changed : PassChange::None;
}

void HirConstantFolder::OptimizeModule(HirModule &module) {
    for (auto &func : module.funcs) {
        OptimizeFunc(func);
    }
    for (auto &impl : module.impls) {
        for (auto &method : impl.methods) {
            OptimizeFunc(method);
        }
    }
    for (auto &constant : module.consts) {
        scopes.clear();
        PushScope();
        OptimizeExpr(constant.value);
        PopScope();
    }
}

void HirConstantFolder::OptimizeFunc(HirFunc &func) {
    scopes.clear();
    PushScope();
    for (const auto &param : func.params) {
        Declare(param.name);
    }
    if (func.body) {
        OptimizeBlock(*func.body, false);
    }
    PopScope();
}

void HirConstantFolder::PushScope() {
    scopes.emplace_back();
}

void HirConstantFolder::PopScope() {
    scopes.pop_back();
}

void HirConstantFolder::Declare(const std::string_view name, std::optional<TypedConstant> constant) {
    if (!name.empty()) {
        scopes.back().insert_or_assign(std::string{name}, BindingFact{std::move(constant)});
    }
}

const TypedConstant *HirConstantFolder::Lookup(const std::string_view name) const {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (const auto found = scope->find(std::string{name}); found != scope->end()) {
            return found->second.constant ? &*found->second.constant : nullptr;
        }
    }
    return nullptr;
}

void HirConstantFolder::Invalidate(const std::string_view name) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        if (const auto found = scope->find(std::string{name}); found != scope->end()) {
            found->second.constant.reset();
            return;
        }
    }
}

void HirConstantFolder::InvalidateAll() {
    for (auto &scope : scopes) {
        for (auto &[name, fact] : scope) {
            static_cast<void>(name);
            fact.constant.reset();
        }
    }
}

void HirConstantFolder::InvalidatePlace(const HirExpr &expr) {
    if (const auto *variable = dynamic_cast<const HirVarExpr *>(&expr)) {
        Invalidate(variable->name);
    }
    else if (const auto *field = dynamic_cast<const HirFieldExpr *>(&expr)) {
        InvalidatePlace(*field->object);
    }
    else if (const auto *index = dynamic_cast<const HirIndexExpr *>(&expr)) {
        InvalidatePlace(*index->object);
    }
    else if (const auto *unary = dynamic_cast<const HirUnaryExpr *>(&expr)) {
        InvalidatePlace(*unary->operand);
    }
}

void HirConstantFolder::DeclarePatternBindings(const HirPattern &pattern) {
    if (const auto *binding = dynamic_cast<const HirBindingPattern *>(&pattern)) {
        Declare(binding->name);
    }
    else if (const auto *range = dynamic_cast<const HirRangePattern *>(&pattern)) {
        DeclarePatternBindings(*range->lo);
        DeclarePatternBindings(*range->hi);
    }
    else if (const auto *enumeration = dynamic_cast<const HirEnumPattern *>(&pattern)) {
        for (const auto &argument : enumeration->args) {
            DeclarePatternBindings(*argument);
        }
    }
    else if (const auto *structure = dynamic_cast<const HirStructPattern *>(&pattern)) {
        for (const auto &field : structure->fields) {
            DeclarePatternBindings(*field.pattern);
        }
    }
    else if (const auto *tuple = dynamic_cast<const HirTuplePattern *>(&pattern)) {
        for (const auto &element : tuple->elements) {
            DeclarePatternBindings(*element);
        }
    }
    else if (const auto *guarded = dynamic_cast<const HirGuardedPattern *>(&pattern)) {
        DeclarePatternBindings(*guarded->inner);
    }
}

void HirConstantFolder::OptimizePattern(HirPattern &pattern) {
    if (auto *guarded = dynamic_cast<HirGuardedPattern *>(&pattern)) {
        OptimizeExpr(guarded->guard);
    }
}

void HirConstantFolder::OptimizeMatchArm(HirMatchArm &arm, const std::vector<Scope> &incomingScopes) {
    scopes = incomingScopes;
    PushScope();
    DeclarePatternBindings(*arm.pattern);
    OptimizePattern(*arm.pattern);
    OptimizeExpr(arm.body);
    PopScope();
}

void HirConstantFolder::OptimizeBlock(HirBlock &block, const bool introduceScope) {
    if (introduceScope) {
        PushScope();
    }

    std::vector<HirStmtPtr> optimized;
    bool unreachable = false;
    for (auto &stmt : block.stmts) {
        if (!stmt) {
            continue;
        }
        if (unreachable) {
            changed = true;
            continue;
        }

        OptimizeStmt(stmt);
        if (auto *conditional = dynamic_cast<HirIfStmt *>(stmt.get())) {
            const auto condition = BooleanLiteral(*conditional->condition);
            HirBlock *selected = nullptr;
            bool allConditionsConstant = condition.has_value();
            if (condition && *condition) {
                selected = &conditional->thenBlock;
            }
            else if (condition) {
                for (auto &elseIf : conditional->elseIfs) {
                    const auto elseCondition = BooleanLiteral(*elseIf.condition);
                    if (!elseCondition) {
                        allConditionsConstant = false;
                        break;
                    }
                    if (*elseCondition) {
                        selected = &elseIf.block;
                        break;
                    }
                }
                if (!selected && allConditionsConstant && conditional->elseBlock) {
                    selected = &*conditional->elseBlock;
                }
            }

            if (allConditionsConstant && (!selected || CanInline(*selected))) {
                if (selected) {
                    for (auto &nested : selected->stmts) {
                        if (nested) {
                            const bool terminates = dynamic_cast<HirReturnStmt *>(nested.get()) ||
                                                    dynamic_cast<HirBreakStmt *>(nested.get()) ||
                                                    dynamic_cast<HirContinueStmt *>(nested.get());
                            optimized.push_back(std::move(nested));
                            unreachable = unreachable || terminates;
                        }
                    }
                }
                changed = true;
                continue;
            }
        }

        unreachable = dynamic_cast<HirReturnStmt *>(stmt.get()) || dynamic_cast<HirBreakStmt *>(stmt.get()) ||
                      dynamic_cast<HirContinueStmt *>(stmt.get());
        optimized.push_back(std::move(stmt));
    }
    block.stmts = std::move(optimized);

    if (introduceScope) {
        PopScope();
    }
}

void HirConstantFolder::OptimizeStmt(HirStmtPtr &stmt) {
    if (auto *expression = dynamic_cast<HirExprStmt *>(stmt.get())) {
        OptimizeExpr(expression->expr);
    }
    else if (auto *binding = dynamic_cast<HirLetStmt *>(stmt.get())) {
        OptimizeExpr(binding->init);
        std::optional<TypedConstant> fact;
        if (!binding->isMut && binding->init) {
            const TypeRef &type = binding->type.IsUnknown() ? binding->init->type : binding->type;
            if (const auto *literal = dynamic_cast<const HirLiteralExpr *>(binding->init.get())) {
                fact = ParseConstant(WithoutIntegerSuffix(literal->value), type);
            }
        }
        Declare(binding->name, std::move(fact));
        if (binding->pattern) {
            DeclarePatternBindings(*binding->pattern);
        }
    }
    else if (auto *returned = dynamic_cast<HirReturnStmt *>(stmt.get())) {
        if (returned->value) {
            OptimizeExpr(*returned->value);
        }
    }
    else if (auto *conditional = dynamic_cast<HirIfStmt *>(stmt.get())) {
        OptimizeExpr(conditional->condition);
        const auto incoming = scopes;
        OptimizeBlock(conditional->thenBlock);
        for (auto &elseIf : conditional->elseIfs) {
            scopes = incoming;
            OptimizeExpr(elseIf.condition);
            OptimizeBlock(elseIf.block);
        }
        if (conditional->elseBlock) {
            scopes = incoming;
            OptimizeBlock(*conditional->elseBlock);
        }
        scopes = incoming;
        InvalidateAll();
    }
    else if (auto *whileStmt = dynamic_cast<HirWhileStmt *>(stmt.get())) {
        OptimizeExpr(whileStmt->condition);
        InvalidateAll();
        OptimizeBlock(whileStmt->body);
        InvalidateAll();
    }
    else if (auto *doWhileStmt = dynamic_cast<HirDoWhileStmt *>(stmt.get())) {
        InvalidateAll();
        OptimizeBlock(doWhileStmt->body);
        OptimizeExpr(doWhileStmt->condition);
        InvalidateAll();
    }
    else if (auto *loopStmt = dynamic_cast<HirLoopStmt *>(stmt.get())) {
        InvalidateAll();
        OptimizeBlock(loopStmt->body);
        InvalidateAll();
    }
    else if (auto *forStmt = dynamic_cast<HirForStmt *>(stmt.get())) {
        OptimizeExpr(forStmt->iterable);
        InvalidateAll();
        PushScope();
        Declare(forStmt->variable);
        OptimizeBlock(forStmt->body, false);
        PopScope();
        InvalidateAll();
    }
    else if (auto *match = dynamic_cast<HirMatchStmt *>(stmt.get())) {
        OptimizeExpr(match->subject);
        const auto incoming = scopes;
        for (auto &arm : match->arms) {
            OptimizeMatchArm(arm, incoming);
        }
        scopes = incoming;
        InvalidateAll();
    }
    else if (auto *local = dynamic_cast<HirLocalDecl *>(stmt.get())) {
        if (local->hasConstant) {
            OptimizeExpr(local->constantValue);
        }
    }
}

std::optional<TypedConstant> HirConstantFolder::Constant(const HirExpr &expr) const {
    const auto *literal = dynamic_cast<const HirLiteralExpr *>(&expr);
    if (!literal) {
        return std::nullopt;
    }
    return ParseConstant(WithoutIntegerSuffix(literal->value), literal->type);
}

HirExprPtr HirConstantFolder::MakeLiteral(const TypedConstant &constant, const SourceLocation &location) {
    auto literal = std::make_unique<HirLiteralExpr>();
    literal->location = location;
    literal->type = constant.Type();
    literal->value = constant.ToLiteral();
    return literal;
}

std::optional<bool> HirConstantFolder::BooleanLiteral(const HirExpr &expr) {
    const auto *literal = dynamic_cast<const HirLiteralExpr *>(&expr);
    if (!literal || !literal->type.IsBool()) {
        return std::nullopt;
    }
    if (literal->value == "true") {
        return true;
    }
    if (literal->value == "false") {
        return false;
    }
    return std::nullopt;
}

void HirConstantFolder::OptimizeExpr(HirExprPtr &expr, const bool allowSubstitution) {
    if (!expr) {
        return;
    }
    if (auto *variable = dynamic_cast<HirVarExpr *>(expr.get())) {
        if (allowSubstitution) {
            if (const TypedConstant *constant = Lookup(variable->name)) {
                expr = MakeLiteral(*constant, variable->location);
                changed = true;
            }
        }
        return;
    }
    if (auto *binary = dynamic_cast<HirBinaryExpr *>(expr.get())) {
        OptimizeExpr(binary->left, allowSubstitution);
        OptimizeExpr(binary->right, allowSubstitution);
        if (FoldBinary(expr) || SimplifyBinary(expr)) {
            changed = true;
        }
    }
    else if (auto *unary = dynamic_cast<HirUnaryExpr *>(expr.get())) {
        const bool maySubstituteOperand = allowSubstitution && unary->op != TokenKind::At;
        OptimizeExpr(unary->operand, maySubstituteOperand);
        if (FoldUnary(expr)) {
            changed = true;
        }
    }
    else if (auto *postfix = dynamic_cast<HirPostfixExpr *>(expr.get())) {
        OptimizeExpr(postfix->operand, false);
        InvalidatePlace(*postfix->operand);
        InvalidateAll();
    }
    else if (auto *copy = dynamic_cast<HirCopyExpr *>(expr.get())) {
        OptimizeExpr(copy->value, false);
        InvalidateAll();
    }
    else if (auto *move = dynamic_cast<HirMoveExpr *>(expr.get())) {
        OptimizeExpr(move->value, false);
        InvalidateAll();
    }
    else if (auto *assignment = dynamic_cast<HirAssignExpr *>(expr.get())) {
        OptimizeExpr(assignment->target, false);
        OptimizeExpr(assignment->value, allowSubstitution);
        InvalidatePlace(*assignment->target);
        InvalidateAll();
    }
    else if (auto *ternary = dynamic_cast<HirTernaryExpr *>(expr.get())) {
        OptimizeExpr(ternary->condition, allowSubstitution);
        if (const auto condition = BooleanLiteral(*ternary->condition)) {
            HirExprPtr selected = *condition ? std::move(ternary->thenExpr) : std::move(ternary->elseExpr);
            OptimizeExpr(selected, allowSubstitution);
            expr = std::move(selected);
            changed = true;
        }
        else {
            const auto incoming = scopes;
            OptimizeExpr(ternary->thenExpr, allowSubstitution);
            scopes = incoming;
            OptimizeExpr(ternary->elseExpr, allowSubstitution);
            scopes = incoming;
            InvalidateAll();
        }
    }
    else if (auto *range = dynamic_cast<HirRangeExpr *>(expr.get())) {
        OptimizeExpr(range->lo, allowSubstitution);
        OptimizeExpr(range->hi, allowSubstitution);
    }
    else if (auto *call = dynamic_cast<HirCallExpr *>(expr.get())) {
        OptimizeExpr(call->callee, allowSubstitution);
        for (auto &argument : call->args) {
            OptimizeExpr(argument, allowSubstitution);
        }
        InvalidateAll();
    }
    else if (auto *coercion = dynamic_cast<HirCoerceToInterfaceExpr *>(expr.get())) {
        OptimizeExpr(coercion->value, allowSubstitution);
    }
    else if (auto *view = dynamic_cast<HirArrayToSliceExpr *>(expr.get())) {
        OptimizeExpr(view->value, allowSubstitution);
    }
    else if (auto *interfaceCall = dynamic_cast<HirInterfaceCallExpr *>(expr.get())) {
        OptimizeExpr(interfaceCall->fatPtrExpr, allowSubstitution);
        for (auto &argument : interfaceCall->args) {
            OptimizeExpr(argument, allowSubstitution);
        }
        InvalidateAll();
    }
    else if (auto *index = dynamic_cast<HirIndexExpr *>(expr.get())) {
        OptimizeExpr(index->object, allowSubstitution);
        OptimizeExpr(index->index, allowSubstitution);
    }
    else if (auto *field = dynamic_cast<HirFieldExpr *>(expr.get())) {
        OptimizeExpr(field->object, allowSubstitution);
    }
    else if (auto *structure = dynamic_cast<HirStructInitExpr *>(expr.get())) {
        for (auto &initializer : structure->fields) {
            OptimizeExpr(initializer.value, allowSubstitution);
        }
    }
    else if (auto *array = dynamic_cast<HirArrayExpr *>(expr.get())) {
        if (array->repeatedElement) {
            OptimizeExpr(array->repeatedElement, allowSubstitution);
            if ((array->repeatCount > 0 && array->repeatCopyPlan.kind != HirCopyPlan::Kind::Trivial) ||
                !array->repeatElementDropGlue.empty()) {
                InvalidateAll();
            }
        }
        for (auto &element : array->elements) {
            OptimizeExpr(element, allowSubstitution);
        }
    }
    else if (auto *tuple = dynamic_cast<HirTupleExpr *>(expr.get())) {
        for (auto &element : tuple->elements) {
            OptimizeExpr(element, allowSubstitution);
        }
    }
    else if (auto *cast = dynamic_cast<HirCastExpr *>(expr.get())) {
        OptimizeExpr(cast->operand, allowSubstitution);
        if (FoldCast(expr)) {
            changed = true;
        }
    }
    else if (auto *typeTest = dynamic_cast<HirIsExpr *>(expr.get())) {
        OptimizeExpr(typeTest->operand, allowSubstitution);
    }
    else if (auto *block = dynamic_cast<HirBlockExpr *>(expr.get())) {
        OptimizeBlock(block->block);
        InvalidateAll();
    }
    else if (auto *match = dynamic_cast<HirMatchExpr *>(expr.get())) {
        OptimizeExpr(match->subject, allowSubstitution);
        const auto incoming = scopes;
        for (auto &arm : match->arms) {
            OptimizeMatchArm(arm, incoming);
        }
        scopes = incoming;
        InvalidateAll();
    }
    else if (auto *enumeration = dynamic_cast<HirEnumConstructExpr *>(expr.get())) {
        for (auto &payload : enumeration->payloads) {
            OptimizeExpr(payload, allowSubstitution);
        }
    }
}

bool HirConstantFolder::FoldBinary(HirExprPtr &expr) {
    auto *binary = dynamic_cast<HirBinaryExpr *>(expr.get());
    if (!binary) {
        return false;
    }
    const auto left = Constant(*binary->left);
    const auto right = Constant(*binary->right);
    if (!left || !right) {
        return false;
    }
    auto result = EvaluateBinary(binary->op, *left, *right);
    if (!result) {
        return false;
    }
    if (result->Type() != binary->type) {
        result = CastConstant(*result, binary->type);
        if (!result) {
            return false;
        }
    }
    expr = MakeLiteral(*result, binary->location);
    return true;
}

bool HirConstantFolder::FoldUnary(HirExprPtr &expr) {
    auto *unary = dynamic_cast<HirUnaryExpr *>(expr.get());
    if (!unary) {
        return false;
    }
    if (unary->op == TokenKind::Bang) {
        if (auto *inner = dynamic_cast<HirUnaryExpr *>(unary->operand.get()); inner && inner->op == TokenKind::Bang) {
            expr = std::move(inner->operand);
            return true;
        }
    }
    const auto operand = Constant(*unary->operand);
    if (!operand) {
        return false;
    }
    auto result = EvaluateUnary(unary->op, *operand);
    if (!result) {
        return false;
    }
    if (result->Type() != unary->type) {
        result = CastConstant(*result, unary->type);
        if (!result) {
            return false;
        }
    }
    expr = MakeLiteral(*result, unary->location);
    return true;
}

bool HirConstantFolder::FoldCast(HirExprPtr &expr) {
    auto *cast = dynamic_cast<HirCastExpr *>(expr.get());
    if (!cast) {
        return false;
    }
    const auto operand = Constant(*cast->operand);
    if (!operand) {
        return false;
    }
    const auto result = CastConstant(*operand, cast->targetType);
    if (!result) {
        return false;
    }
    expr = MakeLiteral(*result, cast->location);
    return true;
}

bool HirConstantFolder::IsDiscardable(const HirExpr &expr) {
    if (dynamic_cast<const HirLiteralExpr *>(&expr) || dynamic_cast<const HirVarExpr *>(&expr) ||
        dynamic_cast<const HirSelfExpr *>(&expr) || dynamic_cast<const HirPathExpr *>(&expr)) {
        return true;
    }
    if (const auto *unary = dynamic_cast<const HirUnaryExpr *>(&expr)) {
        return unary->op != TokenKind::Star && IsDiscardable(*unary->operand);
    }
    if (const auto *binary = dynamic_cast<const HirBinaryExpr *>(&expr)) {
        if (binary->op == TokenKind::Slash || binary->op == TokenKind::Percent || binary->op == TokenKind::LessLess ||
            binary->op == TokenKind::GreaterGreater || binary->op == TokenKind::GreaterGreaterGreater) {
            return false;
        }
        return IsDiscardable(*binary->left) && IsDiscardable(*binary->right);
    }
    if (const auto *ternary = dynamic_cast<const HirTernaryExpr *>(&expr)) {
        return IsDiscardable(*ternary->condition) && IsDiscardable(*ternary->thenExpr) &&
               IsDiscardable(*ternary->elseExpr);
    }
    if (const auto *range = dynamic_cast<const HirRangeExpr *>(&expr)) {
        return (!range->lo || IsDiscardable(*range->lo)) && (!range->hi || IsDiscardable(*range->hi));
    }
    if (const auto *coercion = dynamic_cast<const HirCoerceToInterfaceExpr *>(&expr)) {
        return IsDiscardable(*coercion->value);
    }
    if (const auto *view = dynamic_cast<const HirArrayToSliceExpr *>(&expr)) {
        return IsDiscardable(*view->value);
    }
    if (const auto *structure = dynamic_cast<const HirStructInitExpr *>(&expr)) {
        return std::ranges::all_of(structure->fields,
                                   [](const HirStructInitField &field) { return IsDiscardable(*field.value); });
    }
    if (const auto *array = dynamic_cast<const HirArrayExpr *>(&expr)) {
        const bool repeatHasEffects =
            array->repeatedElement &&
            ((array->repeatCount > 0 && array->repeatCopyPlan.kind != HirCopyPlan::Kind::Trivial) ||
             !array->repeatElementDropGlue.empty());
        return !repeatHasEffects && (!array->repeatedElement || IsDiscardable(*array->repeatedElement)) &&
               std::ranges::all_of(array->elements, [](const HirExprPtr &element) { return IsDiscardable(*element); });
    }
    if (const auto *tuple = dynamic_cast<const HirTupleExpr *>(&expr)) {
        return std::ranges::all_of(tuple->elements, [](const HirExprPtr &element) { return IsDiscardable(*element); });
    }
    if (const auto *cast = dynamic_cast<const HirCastExpr *>(&expr)) {
        return IsDiscardable(*cast->operand);
    }
    if (const auto *typeTest = dynamic_cast<const HirIsExpr *>(&expr)) {
        return IsDiscardable(*typeTest->operand);
    }
    if (const auto *enumeration = dynamic_cast<const HirEnumConstructExpr *>(&expr)) {
        return std::ranges::all_of(enumeration->payloads,
                                   [](const HirExprPtr &payload) { return IsDiscardable(*payload); });
    }
    return false;
}

bool HirConstantFolder::CanInline(const HirBlock &block) {
    return std::ranges::none_of(block.stmts, [](const HirStmtPtr &statement) {
        return dynamic_cast<const HirLetStmt *>(statement.get()) || dynamic_cast<const HirLocalDecl *>(statement.get());
    });
}

bool HirConstantFolder::SimplifyBinary(HirExprPtr &expr) {
    auto *binary = dynamic_cast<HirBinaryExpr *>(expr.get());
    if (!binary) {
        return false;
    }
    const auto left = Constant(*binary->left);
    const auto right = Constant(*binary->right);
    const auto is = [](const std::optional<TypedConstant> &value, const std::uint64_t bits) {
        return value &&
               (value->GetKind() == TypedConstant::Kind::SignedInteger ||
                value->GetKind() == TypedConstant::Kind::UnsignedInteger) &&
               value->Bits() == WideInteger::FromUnsigned(bits, value->Width());
    };
    const auto makeLiteral = [&expr, binary](const TypedConstant &value) {
        std::optional<TypedConstant> converted = value;
        if (value.Type() != binary->type) {
            converted = CastConstant(value, binary->type);
        }
        if (!converted) {
            return false;
        }
        expr = MakeLiteral(*converted, binary->location);
        return true;
    };
    const auto moveOperand = [&expr, binary](HirExprPtr &operand) {
        if (operand->type != binary->type) {
            return false;
        }
        expr = std::move(operand);
        return true;
    };

    switch (binary->op) {
    case TokenKind::Plus:
        if (is(left, 0)) {
            return moveOperand(binary->right);
        }
        if (is(right, 0)) {
            return moveOperand(binary->left);
        }
        break;
    case TokenKind::Minus:
        if (is(right, 0)) {
            return moveOperand(binary->left);
        }
        break;
    case TokenKind::Star:
        if (is(left, 0) && IsDiscardable(*binary->right)) {
            return makeLiteral(*left);
        }
        if (is(right, 0) && IsDiscardable(*binary->left)) {
            return makeLiteral(*right);
        }
        if (is(left, 1)) {
            return moveOperand(binary->right);
        }
        if (is(right, 1)) {
            return moveOperand(binary->left);
        }
        break;
    case TokenKind::Slash:
        if (is(right, 1)) {
            return moveOperand(binary->left);
        }
        break;
    case TokenKind::Percent:
        if (is(right, 1) && IsDiscardable(*binary->left)) {
            return makeLiteral(*EvaluateBinary(TokenKind::Minus, *right, *right));
        }
        break;
    default:
        break;
    }
    return false;
}
} // namespace Rux::Optimization
