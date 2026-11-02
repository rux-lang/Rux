// Value blocks, `if` used as a value, and `is` tests that bind: the forms whose value or bindings depend on which
// branch runs. A value block yields the expression it ends with when that is written without its ';'; an `if` value
// is the match on its condition the parser writes for it; and an `is` test binds the names of its pattern where the
// test is known to be true, which is the body of the `if` or `while` it conditions and the right side of `&&`.

#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Analysis/MovePlace.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// The names a pattern binds, with where each is written.
void CollectBoundNames(const Pattern &pattern, std::vector<std::string> &names) {
    const auto visit = [&](const PatternPtr &child) {
        if (child) {
            CollectBoundNames(*child, names);
        }
    };
    if (const auto *identifier = AstCast<const IdentPattern>(&pattern)) {
        names.push_back(identifier->name);
    }
    else if (const auto *typed = AstCast<const TypedPattern>(&pattern)) {
        if (!typed->name.empty()) {
            names.push_back(typed->name);
        }
    }
    else if (const auto *presence = AstCast<const PresencePattern>(&pattern)) {
        visit(presence->inner);
    }
    else if (const auto *enumeration = AstCast<const EnumPattern>(&pattern)) {
        std::ranges::for_each(enumeration->args, visit);
        for (const EnumPattern::NamedArg &argument : enumeration->namedArgs) {
            visit(argument.pattern);
        }
    }
    else if (const auto *tuple = AstCast<const TuplePattern>(&pattern)) {
        std::ranges::for_each(tuple->elements, visit);
    }
    else if (const auto *structure = AstCast<const StructPattern>(&pattern)) {
        for (const StructPattern::Field &field : structure->fields) {
            visit(field.pattern);
        }
    }
    else if (const auto *guarded = AstCast<const GuardedPattern>(&pattern)) {
        visit(guarded->inner);
    }
}

/// The last statement of a block when it is an expression written with its ';'.
const ExprStmt *TerminatedTail(const Block &block) {
    if (block.stmts.empty()) {
        return nullptr;
    }
    const auto *last = AstCast<const ExprStmt>(block.stmts.back().get());
    return last && last->semicolon ? last : nullptr;
}
} // namespace

const ExprStmt *AnalysisContext::YieldedStatement(const Block &block) noexcept {
    if (block.stmts.empty()) {
        return nullptr;
    }
    const auto *last = AstCast<const ExprStmt>(block.stmts.back().get());
    return last && last->yieldsValue ? last : nullptr;
}

const IsExpr *AnalysisContext::FindBindingTest(const Expr &expression) {
    if (const auto *test = AstCast<const IsExpr>(&expression)) {
        return test->pattern && PatternBindsValue(*test->pattern) ? test : nullptr;
    }
    if (const auto *binary = AstCast<const BinaryExpr>(&expression);
        binary && (binary->op == TokenKind::AmpAmp || binary->op == TokenKind::PipePipe)) {
        if (const IsExpr *left = binary->left ? FindBindingTest(*binary->left) : nullptr) {
            return left;
        }
        return binary->right ? FindBindingTest(*binary->right) : nullptr;
    }
    if (const auto *unary = AstCast<const UnaryExpr>(&expression); unary && unary->op == TokenKind::Bang) {
        return unary->operand ? FindBindingTest(*unary->operand) : nullptr;
    }
    return nullptr;
}

void AnalysisContext::CollectConditionTests(const Expr &condition, std::vector<const Expr *> &tests) {
    if (const auto *binary = AstCast<const BinaryExpr>(&condition); binary && binary->op == TokenKind::AmpAmp) {
        if (binary->left) {
            CollectConditionTests(*binary->left, tests);
        }
        if (binary->right) {
            CollectConditionTests(*binary->right, tests);
        }
        return;
    }
    tests.push_back(&condition);
}

bool AnalysisContext::ConditionBinds(const Expr &condition) {
    std::vector<const Expr *> tests;
    CollectConditionTests(condition, tests);
    return std::ranges::any_of(tests, [](const Expr *test) {
        const auto *is = AstCast<const IsExpr>(test);
        return is && is->pattern && PatternBindsValue(*is->pattern);
    });
}

std::vector<const Symbol *> AnalysisContext::ConditionBindings(const Expr &condition) const {
    std::vector<const Expr *> tests;
    CollectConditionTests(condition, tests);
    std::vector<std::string> names;
    for (const Expr *test : tests) {
        if (const auto *is = AstCast<const IsExpr>(test); is && is->pattern) {
            CollectBoundNames(*is->pattern, names);
        }
    }
    std::vector<const Symbol *> symbols;
    for (const std::string &name : names) {
        if (const Symbol *symbol = currentScope->Lookup(name)) {
            symbols.push_back(symbol);
        }
    }
    return symbols;
}

AnalysisContext::TrackedFlow AnalysisContext::ProjectTrackedFlow(const TrackedFlow &flow, const TrackedFlow &shape) {
    // The borrows the left scope's names held end with it, and one that ended inside it stays ended.
    BorrowSnapshot borrows;
    for (const ActiveBorrow &borrow : flow.borrows) {
        if (std::ranges::find(shape.borrows, borrow.alias, &ActiveBorrow::alias) != shape.borrows.end()) {
            borrows.push_back(borrow);
        }
    }
    return TrackedFlow{MoveStateTracker::Project(flow.states, shape.states), std::move(borrows), flow.reachable};
}

bool AnalysisContext::BlockCannotComplete(const Block &block) const {
    if (BlockDefinitelyReturns(block)) {
        return true;
    }
    if (block.stmts.empty()) {
        return false;
    }
    const Stmt &last = *block.stmts.back();
    return AstCast<const BreakStmt>(&last) != nullptr || AstCast<const ContinueStmt>(&last) != nullptr;
}

void AnalysisContext::CheckYieldedValue(const ExprStmt &tail, TypeRef &type) {
    const TypeRef checked = CheckExpr(*tail.expr);
    if (IsDivergingExpression(*tail.expr)) {
        trackedFlowReachable = false;
        type = TypeRef::MakeUnknown();
        return;
    }
    // An assignment or a call that returns nothing yields no value, which a block spells `()`.
    if (checked.IsOpaque()) {
        type = TypeRef::MakeUnit();
        return;
    }
    // The value leaves the block before the block's own locals are destroyed, so it is handed over inside the block.
    if (!checked.IsUnknown()) {
        ConsumeValue(*tail.expr, checked, ValueConsumptionKind::ConditionalArm, tail.expr->location);
    }
    type = checked;
}

TypeRef AnalysisContext::CheckValueBlock(const BlockExpr &expression) {
    const Block &block = *expression.block;
    const ExprStmt *tail = YieldedStatement(block);
    if (tail) {
        if (const auto expected = expectedExpressionTypes.find(&expression);
            expected != expectedExpressionTypes.end()) {
            const TypeRef forwarded = expected->second;
            expectedExpressionTypes.insert_or_assign(tail->expr.get(), forwarded);
        }
    }
    TypeRef yielded = TypeRef::MakeUnit();
    if (expression.isUnsafe) {
        BeginUnsafeBlock(expression);
    }
    CheckBlock(block, tail, &yielded);
    if (expression.isUnsafe) {
        EndUnsafeBlock(expression);
    }
    if (BlockCannotComplete(block)) {
        return TypeRef::MakeUnknown();
    }
    return tail ? yielded : TypeRef::MakeUnit();
}

void AnalysisContext::CheckStatementBranch(const Block &block) {
    const ExprStmt *tail = YieldedStatement(block);
    TypeRef yielded = TypeRef::MakeUnit();
    CheckBlock(block, tail, &yielded);
    ReportUnusedBranchValue(tail, yielded);
}

void AnalysisContext::ReportUnusedBranchValue(const ExprStmt *tail, const TypeRef &yielded) {
    if (!tail || yielded.IsUnknown() || yielded.IsUnit()) {
        return;
    }
    EmitError(tail->location,
              std::format("this branch yields a value of type '{}' that is not used", yielded.DisplayString()),
              {"a branch of a statement yields nothing, so its last expression needs a ';'"},
              "add ';' or use the value");
}

bool AnalysisContext::ReportStrayTailSemicolon(const Expr &body, const TypeRef &expected) {
    const auto *block = AstCast<const BlockExpr>(&body);
    if (!block || expected.IsUnknown() || expected.IsUnit() || YieldedStatement(*block->block)) {
        return false;
    }
    const ExprStmt *last = TerminatedTail(*block->block);
    if (!last || IsDivergingExpression(*last->expr)) {
        return false;
    }
    const auto checked = expressionTypes.find(last->expr.get());
    if (checked == expressionTypes.end() || checked->second.IsUnit() || checked->second.IsOpaque()) {
        return false;
    }
    EmitError(*last->semicolon, "this block yields '()' because of the ';'",
              {std::format("the block must yield a value of type '{}'", expected.DisplayString())},
              "remove the ';' to yield the last expression");
    return true;
}

void AnalysisContext::UnifyArm(ArmUnification &unification, const Expr &body, const TypeRef &armType,
                               const SourceLocation location) {
    const auto reportMismatch = [&](const Expr &mismatched, const TypeRef &type, const TypeRef &expected,
                                    const SourceLocation where) {
        if (ReportStrayTailSemicolon(mismatched, expected)) {
            return;
        }
        EmitError(where, AssignmentErrorMessage(mismatched, expected,
                                                std::format("{} type mismatch: expected '{}', found '{}'",
                                                            unification.construct, expected.ToString(),
                                                            type.DisplayString())));
    };
    if (armType.IsUnknown()) {
        return;
    }
    if (unification.expected) {
        // The annotation decided the type, so each arm converts to it or is reported where it is.
        if (!CanAssignExprTo(body, armType, *unification.expected)) {
            reportMismatch(body, armType, *unification.expected, location);
        }
        return;
    }
    // A block that yields `()` decides nothing while another arm yields a value, whichever arm comes first; it is
    // checked against the arms' type once every arm has been seen.
    if (armType.IsUnit() && AstCast<const BlockExpr>(&body)) {
        unification.unitBlocks.push_back({&body, armType, location});
        return;
    }
    TypeRef &resultType = unification.result;
    if (resultType.IsUnknown()) {
        resultType = armType;
    }
    else if (CanAssignExprTo(body, armType, resultType)) {
        // The arm fits the type the earlier arms settled on.
    }
    else if (resultType.MentionsIncompleteNative() && !armType.MentionsIncompleteNative()) {
        // `none`, `.Success(v)` and `.Failure(e)` leave part of their type open, so the first arm that has a whole
        // type decides the match, and the open arms before it are checked against that type.
        resultType = armType;
        for (const ArmUnification::Arm &earlier : unification.accepted) {
            if (!CanAssignExprTo(*earlier.body, earlier.type, resultType)) {
                reportMismatch(*earlier.body, earlier.type, resultType, earlier.location);
            }
        }
    }
    else if (!resultType.MentionsIncompleteNative() && !armType.MentionsIncompleteNative()) {
        reportMismatch(body, armType, resultType, location);
    }
    // Otherwise no arm so far has a whole type, as with `.Success(v)` beside `.Failure(e)`. The match keeps its open
    // type and the expected type of its context checks every arm.
    unification.accepted.push_back({&body, armType, location});
}

TypeRef AnalysisContext::FinishArmUnification(ArmUnification &unification) {
    for (const ArmUnification::Arm &unit : unification.unitBlocks) {
        if (unification.result.IsUnknown() || unification.result.IsUnit()) {
            unification.result = unit.type;
            continue;
        }
        if (ReportStrayTailSemicolon(*unit.body, unification.result)) {
            continue;
        }
        EmitError(unit.location,
                  std::format("{} type mismatch: expected '{}', found '()'", unification.construct,
                              unification.result.ToString()),
                  {}, "end the block with the value it yields, written without a ';'");
    }
    return unification.result;
}

void AnalysisContext::ExpectArmValue(const Expr &body, const TypeRef &type) {
    if (IsContextualCaseExpression(body)) {
        ExpectContextualCaseArgument(&body, type);
        return;
    }
    // A block or a nested `if` passes the type on to the value it yields.
    const auto *match = AstCast<const MatchExpr>(&body);
    if (AstCast<const BlockExpr>(&body) || (match && match->form == MatchExpr::Form::If)) {
        if (!type.IsUnknown()) {
            expectedExpressionTypes.insert_or_assign(&body, type);
        }
    }
}

TypeRef AnalysisContext::CheckIfExpression(const MatchExpr &expression) {
    const MatchExpr::Arm &taken = expression.arms.front();
    const MatchExpr::Arm &otherwise = expression.arms.back();

    ArmUnification unification;
    unification.construct = "'if' branch";
    std::optional<TypeRef> expectedType;
    if (const auto expected = expectedExpressionTypes.find(&expression); expected != expectedExpressionTypes.end()) {
        expectedType = expected->second;
        if ((expected->second.IsOptional() || expected->second.IsFallible() || expected->second.IsSum()) &&
            !expected->second.MentionsIncompleteNative()) {
            unification.expected = expected->second;
            unification.result = expected->second;
        }
    }

    // The condition's bindings belong to the then branch alone, so they live in a scope the else branch is outside.
    const TrackedFlow outside = SaveTrackedFlow();
    PushScope();
    conditionReceivesBindings = true;
    const TypeRef condition = ReadBorrowedScalar(*expression.subject, CheckExpr(*expression.subject));
    conditionReceivesBindings = false;
    CheckBooleanCondition(condition, expression.subject->location, "if");
    CheckPattern(*taken.pattern, TypeRef::MakeBool());
    const TrackedFlow afterCondition = SaveTrackedFlow();

    const auto checkBranch = [&](const MatchExpr::Arm &arm) {
        if (unification.expected) {
            expectedExpressionTypes.insert_or_assign(arm.body.get(), *unification.expected);
        }
        else if (expectedType) {
            ExpectArmValue(*arm.body, *expectedType);
        }
        else if (!unification.result.IsUnknown() && !unification.result.MentionsIncompleteNative()) {
            ExpectArmValue(*arm.body, unification.result);
        }
        const TypeRef checked = CheckExpr(*arm.body);
        const TypeRef armType = IsDivergingExpression(*arm.body) ? TypeRef::MakeUnknown() : checked;
        ConsumeValue(*arm.body, armType, ValueConsumptionKind::ConditionalArm, arm.location);
        return armType;
    };

    const TypeRef thenType = checkBranch(taken);
    PopScope();
    const TrackedFlow thenExit = SaveTrackedFlow();
    RestoreTrackedFlow(ProjectTrackedFlow(afterCondition, outside));
    PushScope();
    CheckPattern(*otherwise.pattern, TypeRef::MakeBool());
    const TypeRef elseType = checkBranch(otherwise);
    PopScope();
    const TrackedFlow elseExit = SaveTrackedFlow();
    MergeTrackedFlows({thenExit, elseExit});

    UnifyArm(unification, *taken.body, thenType, taken.location);
    UnifyArm(unification, *otherwise.body, elseType, otherwise.location);
    exhaustiveMatchExpressions.insert(&expression);
    return FinishArmUnification(unification);
}

TypeRef AnalysisContext::CheckBindingConjunction(const BinaryExpr &expression, const bool receivesBindings) {
    // Outside a condition the bindings are scoped to the conjunction itself; inside one they reach the body too.
    if (!receivesBindings) {
        PushScope();
    }
    conditionReceivesBindings = true;
    const TypeRef left = CheckExpr(*expression.left);
    const TrackedFlow shortCircuitExit = SaveTrackedFlow();
    conditionReceivesBindings = true;
    const TypeRef right = CheckExpr(*expression.right);
    conditionReceivesBindings = false;
    const TrackedFlow evaluatedExit = SaveTrackedFlow();
    MergeTrackedFlows({shortCircuitExit, evaluatedExit});
    const TypeRef result =
        CheckBinary(expression.op, left, right, *expression.left, *expression.right, expression.location);
    if (!receivesBindings) {
        PopScope();
    }
    return result;
}

void AnalysisContext::RejectUnreachableBindings(const Expr &expression) {
    const IsExpr *test = FindBindingTest(expression);
    if (!test || !unreachableBindingTests.insert(test).second) {
        return;
    }
    const auto *unary = AstCast<const UnaryExpr>(&expression);
    EmitError(test->pattern->location, std::format("an 'is' test under '{}' cannot bind names", unary ? "!" : "||"),
              {unary ? "the names would be in scope only where the test is false, where nothing was bound"
                     : "the names would be in scope where the other operand may have been the one that held"},
              "test with '_' in place of the names, or bind them in a separate 'if'");
}

TypeRef AnalysisContext::CheckIsPattern(const IsExpr &expression, const bool receivesBindings) {
    // Outside a condition the names a test binds are in scope nowhere, so they get a scope of their own.
    if (!receivesBindings) {
        PushScope();
    }
    const TypeRef expressionType = CheckExpr(*expression.operand);
    const bool reference = expressionType->kind == TypeRef::Kind::Reference && !expressionType->inner.empty();
    const TypeRef subjectType = reference ? expressionType->inner.front() : expressionType;

    // The subject is evaluated once. A temporary, or one transferred with `<-`, is owned by the test the way a match
    // owns it, so its bindings take what they bind; a named or borrowed subject stays with its owner and its bindings
    // borrow from it.
    const MovePlace place = AnalyzeMovePlace(*expression.operand);
    const bool transferred = AstCast<const MoveExpr>(expression.operand.get()) != nullptr;
    const bool binds = PatternBindsValue(*expression.pattern);
    const bool owned = transferred || (!reference && !place.IsNamedStorage() && !place.IsBorrowedStorage());
    bool takesParts = false;
    PatternBorrow borrow = PatternBorrow::Owned;
    if (owned) {
        takesParts = ConsumeMatchSubject(*expression.operand, expressionType, binds, expression.location);
        borrow = IsNativeMatchSubject(subjectType) ? MatchSubjectBorrow(*expression.operand, expressionType)
                                                   : PatternBorrow::Owned;
    }
    else if (IsNativeMatchSubject(subjectType)) {
        borrow = reference || place.IsBorrowedStorage() ? MatchSubjectBorrow(*expression.operand, expressionType)
                                                        : PatternBorrow::Shared;
    }

    const PatternBorrow savedBorrow = std::exchange(currentPatternBorrow, borrow);
    const bool savedTakesParts = std::exchange(currentPatternTakesParts, takesParts);
    CheckPattern(*expression.pattern, subjectType);
    currentPatternTakesParts = savedTakesParts;
    currentPatternBorrow = savedBorrow;

    // A test is not exhaustive: it is checked as the first arm of a match whose `_` takes everything else.
    auto &otherwise = isTestDefaultArms.emplace_back();
    otherwise.location = expression.location;
    ValidateMatchPatterns({expression.pattern.get(), &otherwise}, subjectType);

    // A binding of a subject the test does not own borrows the subject while it is in use; the reference an optional
    // reference holds borrows what the optional borrowed, however the subject is held.
    if (binds && IsOptionalReference(subjectType) && trackedFlowReachable) {
        RegisterBorrowedPayloadBindings(*expression.pattern, *expression.operand, subjectType);
    }
    else if (binds && !owned && trackedFlowReachable) {
        std::vector<std::string> names;
        CollectBoundNames(*expression.pattern, names);
        for (const std::string &name : names) {
            if (const Symbol *symbol = currentScope->Lookup(name)) {
                RegisterReferenceBinding(*symbol, *expression.operand, TypeRef::MakeReference(subjectType));
            }
        }
    }
    if (!receivesBindings) {
        PopScope();
    }
    return TypeRef::MakeBool();
}

void AnalysisContext::RegisterBorrowedPayloadBindings(const Pattern &pattern, const Expr &subject,
                                                      const TypeRef &subjectType) {
    std::vector<std::string> names;
    CollectBoundNames(pattern, names);
    const TypeRef reference = BorrowedReference(subjectType);
    for (const std::string &name : names) {
        if (const Symbol *symbol = currentScope->Lookup(name); symbol && HoldsBorrow(symbol->type)) {
            RegisterReferenceBinding(*symbol, subject, reference);
        }
    }
}
} // namespace Rux::SemanticDetail
