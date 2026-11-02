// Lowering for value blocks, `if` values and `is` tests. A block yields the value of its last expression, held in a
// hidden binding until its locals are destroyed; an `if` value is the match on its condition the parser wrote; and an
// `is` test is a two-arm match whose arm binding the pattern's names holds the code the test guards. A condition that
// binds names is lowered as a chain of nested tests, the innermost of which runs the guarded body and sets a flag the
// `else` branch is taken by, so no branch is lowered twice.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
bool PatternBinds(const Pattern &pattern) {
    const auto binds = [](const PatternPtr &child) { return child && PatternBinds(*child); };
    if (AstCast<const IdentPattern>(&pattern)) {
        return true;
    }
    if (const auto *typed = AstCast<const TypedPattern>(&pattern)) {
        return !typed->name.empty();
    }
    if (const auto *presence = AstCast<const PresencePattern>(&pattern)) {
        return binds(presence->inner);
    }
    if (const auto *enumeration = AstCast<const EnumPattern>(&pattern)) {
        return std::ranges::any_of(enumeration->args, binds) ||
               std::ranges::any_of(enumeration->namedArgs,
                                   [&](const EnumPattern::NamedArg &named) { return binds(named.pattern); });
    }
    if (const auto *structure = AstCast<const StructPattern>(&pattern)) {
        return std::ranges::any_of(structure->fields,
                                   [&](const StructPattern::Field &field) { return binds(field.pattern); });
    }
    if (const auto *tuple = AstCast<const TuplePattern>(&pattern)) {
        return std::ranges::any_of(tuple->elements, binds);
    }
    if (const auto *guarded = AstCast<const GuardedPattern>(&pattern)) {
        return binds(guarded->inner);
    }
    return false;
}

/// The operands of a chain of `&&`, in evaluation order.
void CollectTests(const Expr &condition, std::vector<const Expr *> &tests) {
    if (const auto *binary = AstCast<const BinaryExpr>(&condition); binary && binary->op == TokenKind::AmpAmp) {
        CollectTests(*binary->left, tests);
        CollectTests(*binary->right, tests);
        return;
    }
    tests.push_back(&condition);
}

std::vector<const Expr *> ConditionTests(const Expr &condition) {
    std::vector<const Expr *> tests;
    CollectTests(condition, tests);
    return tests;
}

HirExprPtr BoolLiteral(const bool value, const SourceLocation location) {
    auto literal = std::make_unique<HirLiteralExpr>();
    literal->location = location;
    literal->type = TypeRef::MakeBool();
    literal->value = value ? "true" : "false";
    return literal;
}

HirExprPtr Variable(const std::string &name, const TypeRef &type, const SourceLocation location) {
    auto variable = std::make_unique<HirVarExpr>();
    variable->location = location;
    variable->name = name;
    variable->type = type;
    return variable;
}

/// A hidden local with no drop flag: a condition's flag, or a value being carried out of a scope.
HirStmtPtr HiddenLocal(const std::string &name, const TypeRef &type, HirExprPtr init, const SourceLocation location) {
    auto local = std::make_unique<HirLetStmt>();
    local->location = location;
    local->isMut = true;
    local->name = name;
    local->type = type;
    local->init = std::move(init);
    return local;
}

HirStmtPtr Store(const std::string &name, const TypeRef &type, HirExprPtr value, const SourceLocation location) {
    auto store = std::make_unique<HirAssignExpr>();
    store->location = location;
    store->op = TokenKind::Assign;
    store->target = Variable(name, type, location);
    store->value = std::move(value);
    store->type = type;
    auto statement = std::make_unique<HirExprStmt>();
    statement->location = location;
    statement->expr = std::move(store);
    return statement;
}

HirStmtPtr Scope(HirBlock block, const SourceLocation location) {
    auto scope = std::make_unique<HirScopeStmt>();
    scope->location = location;
    scope->block = std::move(block);
    return scope;
}

HirMatchArm OtherwiseArm(HirExprPtr body, const SourceLocation location) {
    HirMatchArm otherwise;
    otherwise.location = location;
    auto wildcard = std::make_unique<HirWildcardPattern>();
    wildcard->location = location;
    otherwise.pattern = std::move(wildcard);
    otherwise.body = std::move(body);
    return otherwise;
}

std::unique_ptr<HirBlockExpr> EmptyBlock(const SourceLocation location) {
    auto block = std::make_unique<HirBlockExpr>();
    block->location = location;
    block->block.location = location;
    return block;
}

const ExprStmt *YieldedStatement(const Block &block) {
    if (block.stmts.empty()) {
        return nullptr;
    }
    const auto *last = AstCast<const ExprStmt>(block.stmts.back().get());
    return last && last->yieldsValue ? last : nullptr;
}

bool IsScalar(const TypeRef &type) {
    return type.IsNumeric() || type.IsBool() || type.IsChar();
}
} // namespace

bool AstToHirContext::ConditionBinds(const Expr &condition) {
    const std::vector<const Expr *> tests = ConditionTests(condition);
    return std::ranges::any_of(tests, [](const Expr *test) {
        const auto *is = AstCast<const IsExpr>(test);
        return is && is->pattern && PatternBinds(*is->pattern);
    });
}

HirExprPtr AstToHirContext::LowerYielded(const Expr &value, const std::optional<TypeRef> &target) {
    if (!target) {
        return LowerExpr(value);
    }
    HirExprPtr lowered = LowerExprAs(value, *target);
    return IsScalar(*target) ? ConvertScalar(std::move(lowered), *target) : std::move(lowered);
}

HirExprPtr AstToHirContext::LowerValueBlock(const BlockExpr &expression, const std::optional<TypeRef> &target) {
    auto lowered = std::make_unique<HirBlockExpr>();
    lowered->location = expression.location;
    const ExprStmt *tail = YieldedStatement(*expression.block);
    const TypeRef *recorded = model.TryGetType(expression);
    // A block that yields `()` or cannot complete is the statements it holds. One yielding `null` has no recorded
    // type and takes the one its context wants.
    if (!tail || (!recorded && !target) || (recorded && recorded->IsUnit())) {
        lowered->block = LowerBlock(*expression.block);
        return lowered;
    }
    BlockValue value;
    value.tail = tail;
    value.target = target;
    value.type = target ? *target : SubstituteCurrentType(*recorded);
    value.name = std::format("$block.{}", blockValueOrdinal++);
    lowered->type = value.type;
    lowered->block = LowerBlock(*expression.block, &value);
    lowered->value = Variable(value.name, value.type, tail->location);
    return lowered;
}

HirExprPtr AstToHirContext::LowerIsTest(const IsExpr &expression, const std::function<HirExprPtr()> &whenMatched) {
    auto match = std::make_unique<HirMatchExpr>();
    match->location = expression.location;
    match->type = TypeRef::MakeBool();
    match->subject = LowerMatchSubject(*expression.operand);
    const bool ownsPayload = match->subject->consumption.has_value();
    HirMatchArm taken;
    taken.location = expression.location;
    PushScope();
    const bool savedOwnership = std::exchange(patternBindingsOwnPayload, ownsPayload);
    taken.pattern = LowerPattern(*expression.pattern, match->subject->type);
    patternBindingsOwnPayload = savedOwnership;
    taken.body = whenMatched ? whenMatched() : BoolLiteral(true, expression.location);
    taken.cleanups = CurrentScopeCleanups();
    PopScope();
    match->arms.push_back(std::move(taken));
    match->arms.push_back(OtherwiseArm(BoolLiteral(false, expression.location), expression.location));
    return match;
}

HirExprPtr AstToHirContext::LowerBindingConjunction(const BinaryExpr &expression) {
    const std::vector<const Expr *> tests = ConditionTests(expression);
    // The tests from `index` on, each binding test a match whose matched arm evaluates the rest with its names bound.
    const auto lowerFrom = [&](this const auto &self, const std::size_t index) -> HirExprPtr {
        const Expr &test = *tests[index];
        const bool last = index + 1 == tests.size();
        if (const auto *is = AstCast<const IsExpr>(&test); is && is->pattern) {
            return last ? LowerIsTest(*is, {}) : LowerIsTest(*is, [&] { return self(index + 1); });
        }
        if (last) {
            return LowerExpr(test);
        }
        auto conjunction = std::make_unique<HirBinaryExpr>();
        conjunction->location = expression.location;
        conjunction->op = TokenKind::AmpAmp;
        conjunction->type = TypeRef::MakeBool();
        conjunction->left = LowerExpr(test);
        conjunction->right = self(index + 1);
        return conjunction;
    };
    return lowerFrom(0);
}

void AstToHirContext::LowerConditionChain(const std::vector<const Expr *> &tests, const std::size_t index,
                                          HirBlock &into, const std::function<void(HirBlock &)> &whenTrue) {
    if (index == tests.size()) {
        whenTrue(into);
        return;
    }
    const Expr &test = *tests[index];
    if (const auto *is = AstCast<const IsExpr>(&test); is && is->pattern) {
        auto match = std::make_unique<HirMatchStmt>();
        match->location = is->location;
        match->subject = LowerMatchSubject(*is->operand);
        const bool ownsPayload = match->subject->consumption.has_value();
        HirMatchArm taken;
        taken.location = is->location;
        PushScope();
        const bool savedOwnership = std::exchange(patternBindingsOwnPayload, ownsPayload);
        taken.pattern = LowerPattern(*is->pattern, match->subject->type);
        patternBindingsOwnPayload = savedOwnership;
        auto body = EmptyBlock(is->location);
        LowerConditionChain(tests, index + 1, body->block, whenTrue);
        taken.body = std::move(body);
        taken.cleanups = CurrentScopeCleanups();
        PopScope();
        match->arms.push_back(std::move(taken));
        match->arms.push_back(OtherwiseArm(EmptyBlock(is->location), is->location));
        into.stmts.push_back(std::move(match));
        return;
    }
    auto conditional = std::make_unique<HirIfStmt>();
    conditional->location = test.location;
    conditional->condition = LowerExpr(test);
    conditional->thenBlock.location = test.location;
    LowerConditionChain(tests, index + 1, conditional->thenBlock, whenTrue);
    into.stmts.push_back(std::move(conditional));
}

HirStmtPtr AstToHirContext::LowerIfBranches(const IfStmt &statement, const std::size_t branch) {
    const Expr &condition = branch == 0 ? *statement.condition : *statement.elseIfs[branch - 1].condition;
    const Block &body = branch == 0 ? *statement.thenBlock : *statement.elseIfs[branch - 1].block;
    const SourceLocation location = branch == 0 ? statement.location : statement.elseIfs[branch - 1].location;
    const bool hasRest = branch < statement.elseIfs.size() || statement.elseBlock;
    const auto lowerRest = [&]() {
        HirBlock rest;
        rest.location = location;
        if (branch < statement.elseIfs.size()) {
            rest.stmts.push_back(LowerIfBranches(statement, branch + 1));
        }
        else {
            rest = LowerBlock(*statement.elseBlock);
        }
        return rest;
    };

    if (!ConditionBinds(condition)) {
        auto lowered = std::make_unique<HirIfStmt>();
        lowered->location = location;
        lowered->condition = LowerExpr(condition);
        lowered->thenBlock = LowerBlock(body);
        if (hasRest) {
            lowered->elseBlock = lowerRest();
        }
        return lowered;
    }

    HirBlock scope;
    scope.location = location;
    const std::string taken = std::format("$if.{}", blockValueOrdinal++);
    if (hasRest) {
        scope.stmts.push_back(HiddenLocal(taken, TypeRef::MakeBool(), BoolLiteral(false, location), location));
    }
    LowerConditionChain(ConditionTests(condition), 0, scope, [&](HirBlock &into) {
        if (hasRest) {
            into.stmts.push_back(Store(taken, TypeRef::MakeBool(), BoolLiteral(true, location), location));
        }
        into.stmts.push_back(Scope(LowerBlock(body), body.location));
    });
    if (hasRest) {
        auto otherwise = std::make_unique<HirIfStmt>();
        otherwise->location = location;
        auto notTaken = std::make_unique<HirUnaryExpr>();
        notTaken->location = location;
        notTaken->op = TokenKind::Bang;
        notTaken->type = TypeRef::MakeBool();
        notTaken->operand = Variable(taken, TypeRef::MakeBool(), location);
        otherwise->condition = std::move(notTaken);
        otherwise->thenBlock = lowerRest();
        scope.stmts.push_back(std::move(otherwise));
    }
    return Scope(std::move(scope), location);
}

HirStmtPtr AstToHirContext::LowerBindingWhile(const WhileStmt &statement) {
    auto lowered = std::make_unique<HirLoopStmt>();
    lowered->location = statement.location;
    lowered->label = statement.label;
    const CleanupPlanner::LoopToken cleanupLoop = BeginLoop(lowered->label);
    const SourceLocation location = statement.location;
    const std::string taken = std::format("$while.{}", blockValueOrdinal++);
    HirBlock &body = lowered->body;
    body.location = location;
    body.stmts.push_back(HiddenLocal(taken, TypeRef::MakeBool(), BoolLiteral(false, location), location));
    LowerConditionChain(ConditionTests(*statement.condition), 0, body, [&](HirBlock &into) {
        into.stmts.push_back(Store(taken, TypeRef::MakeBool(), BoolLiteral(true, location), location));
        into.stmts.push_back(Scope(LowerBlock(*statement.body), statement.body->location));
    });
    // A test that fails leaves the loop.
    auto leave = std::make_unique<HirIfStmt>();
    leave->location = location;
    auto notTaken = std::make_unique<HirUnaryExpr>();
    notTaken->location = location;
    notTaken->op = TokenKind::Bang;
    notTaken->type = TypeRef::MakeBool();
    notTaken->operand = Variable(taken, TypeRef::MakeBool(), location);
    leave->condition = std::move(notTaken);
    leave->thenBlock.location = location;
    auto exit = std::make_unique<HirBreakStmt>();
    exit->location = location;
    exit->label = statement.label;
    exit->cleanups = cleanupPlanner.LoopExitActions(statement.label);
    leave->thenBlock.stmts.push_back(std::move(exit));
    body.stmts.push_back(std::move(leave));
    EndLoop(cleanupLoop);
    return lowered;
}

HirExprPtr AstToHirContext::LowerBindingIfValue(const MatchExpr &expression) {
    const SourceLocation location = expression.location;
    const TypeRef type = ResolvedExpressionType(expression);
    const bool yieldsValue = !type.IsUnit() && !type.IsUnknown() && !type.IsOpaque();
    const std::string taken = std::format("$if.{}", blockValueOrdinal++);
    const std::string result = std::format("$if.value.{}", blockValueOrdinal++);
    auto lowered = std::make_unique<HirBlockExpr>();
    lowered->location = location;
    lowered->type = type;
    lowered->block.location = location;
    // A branch that yields a value stores it; one that leaves, or yields `()`, is only run.
    const auto runBranch = [&](HirBlock &into, const Expr &branch) {
        const TypeRef *branchType = model.TryGetType(branch);
        if (!branchType || branchType->IsUnit() || !yieldsValue) {
            auto statement = std::make_unique<HirExprStmt>();
            statement->location = branch.location;
            statement->expr = LowerExpr(branch);
            into.stmts.push_back(std::move(statement));
            return;
        }
        into.stmts.push_back(Store(result, type, LowerYielded(branch, type), branch.location));
    };
    lowered->block.stmts.push_back(HiddenLocal(taken, TypeRef::MakeBool(), BoolLiteral(false, location), location));
    if (yieldsValue) {
        lowered->block.stmts.push_back(HiddenLocal(result, type, nullptr, location));
    }
    LowerConditionChain(ConditionTests(*expression.subject), 0, lowered->block, [&](HirBlock &into) {
        into.stmts.push_back(Store(taken, TypeRef::MakeBool(), BoolLiteral(true, location), location));
        runBranch(into, *expression.arms.front().body);
    });
    auto otherwise = std::make_unique<HirIfStmt>();
    otherwise->location = location;
    auto notTaken = std::make_unique<HirUnaryExpr>();
    notTaken->location = location;
    notTaken->op = TokenKind::Bang;
    notTaken->type = TypeRef::MakeBool();
    notTaken->operand = Variable(taken, TypeRef::MakeBool(), location);
    otherwise->condition = std::move(notTaken);
    otherwise->thenBlock.location = location;
    runBranch(otherwise->thenBlock, *expression.arms.back().body);
    lowered->block.stmts.push_back(std::move(otherwise));
    if (yieldsValue) {
        lowered->value = Variable(result, type, location);
    }
    return lowered;
}
} // namespace Rux::AstToHirDetail
