// Typed throws: `throws E` clauses, `throw`, and `try` in its four forms. A function that throws still returns the
// native fallible a `-> T ! E` function returns, so the only new facts are where each error goes. A call whose callee
// throws is a throw site wherever a `try` marks it: its value is the success payload, and its error goes to the
// innermost handler -- a `try` block, a `try ... catch` expression or a `try?` -- or out through the function's own
// `throws` channel. Each site records that target once, and the flow at each throw is an edge into its handler, so
// ownership and borrows are checked along every path an error takes.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// The spelling of an overloadable binary operator, or empty for one no method declares.
std::string_view OverloadedOperatorSpelling(const TokenKind op) noexcept {
    switch (op) {
    case TokenKind::Plus:
        return "+";
    case TokenKind::Minus:
        return "-";
    case TokenKind::Star:
        return "*";
    case TokenKind::Slash:
        return "/";
    case TokenKind::Percent:
        return "%";
    case TokenKind::Amp:
        return "&";
    case TokenKind::Pipe:
        return "|";
    case TokenKind::Caret:
        return "^";
    case TokenKind::LessLess:
        return "<<";
    case TokenKind::GreaterGreater:
        return ">>";
    case TokenKind::Equal:
        return "==";
    case TokenKind::Less:
        return "<";
    default:
        return {};
    }
}

/// Whether a declared return type is fallible, and whether it is spelled with `throws`.
std::pair<bool, bool> DeclaredSpelling(const std::optional<TypeExprPtr> &returnType) noexcept {
    const auto *fallible = returnType ? AstCast<const FallibleTypeExpr>(returnType->get()) : nullptr;
    return {fallible != nullptr, fallible && fallible->throwsClause};
}

/// The name a diagnostic gives the function being checked.
std::string FunctionName(const FuncDecl *function) {
    return function ? std::format("'{}'", function->name) : std::string("the function");
}
} // namespace

AnalysisContext::ThrowingCallee AnalysisContext::ThrowingCalleeOf(const Expr &expression) {
    ThrowingCallee callee;
    const auto spellingOf = [](const std::optional<TypeExprPtr> &returnType) {
        const auto [fallible, throws] = DeclaredSpelling(returnType);
        return !fallible ? ThrowingCallee::Spelling::None
                         : (throws ? ThrowingCallee::Spelling::Throws : ThrowingCallee::Spelling::Fallible);
    };
    if (const auto *call = AstCast<const CallExpr>(&expression)) {
        callee.location = CallSiteLocation(*call);
        const auto binding = callableBindings.find(call);
        if (binding == callableBindings.end()) {
            return callee;
        }
        if (binding->second.dispatch == ResolvedCallableBinding::DispatchKind::EnumVariant) {
            return callee;
        }
        if (const auto *function = AstCast<const FuncDecl>(binding->second.selectedDeclaration)) {
            callee.spelling = spellingOf(function->returnType);
            callee.description = std::format("call to '{}'", function->name);
            return callee;
        }
        // A function value carries its type and not its spelling, so a call through one keeps the older reading.
        if (binding->second.dispatch == ResolvedCallableBinding::DispatchKind::Indirect) {
            callee.spelling = ThrowingCallee::Spelling::Fallible;
            const auto *name = AstCast<const IdentExpr>(call->callee.get());
            callee.description = name ? std::format("call to '{}'", name->name) : std::string("call");
        }
        return callee;
    }
    if (const auto *binary = AstCast<const BinaryExpr>(&expression)) {
        const std::string_view spelling = OverloadedOperatorSpelling(binary->op);
        const auto left = expressionTypes.find(binary->left.get());
        const auto right = expressionTypes.find(binary->right.get());
        if (spelling.empty() || left == expressionTypes.end() || right == expressionTypes.end()) {
            return callee;
        }
        callee.location = binary->location;
        callee.description = std::format("operator '{}'", spelling);
        const FuncDecl *method = LookupOperatorMethod(left->second, std::string(spelling), {right->second});
        callee.spelling = method ? spellingOf(method->returnType) : ThrowingCallee::Spelling::Fallible;
        return callee;
    }
    if (const auto *index = AstCast<const IndexExpr>(&expression)) {
        const auto resolved = indexOperators.find(index);
        if (resolved == indexOperators.end() || !resolved->second.method) {
            return callee;
        }
        callee.location = index->location;
        callee.description = "operator '[]'";
        callee.spelling = spellingOf(resolved->second.method->returnType);
    }
    return callee;
}

TypeRef AnalysisContext::HandlerErrorType(const ThrowHandlerFrame &frame) {
    return frame.errors.empty() ? TypeRef::MakeUnknown() : TypeRef::MakeSum(frame.errors);
}

TypeRef AnalysisContext::CheckThrowSite(const Expr &expression, const TypeRef &outcome) {
    // The operand of an older `?`, `? else` or `catch` is the outcome those operators take apart.
    if (&expression == outcomeOperand) {
        return outcome;
    }
    const ThrowingCallee callee = ThrowingCalleeOf(expression);
    if (callee.spelling == ThrowingCallee::Spelling::None) {
        return outcome;
    }
    const TypeRef &success = outcome.FallibleSuccess();
    const TypeRef &error = outcome.FallibleError();
    const bool covered =
        !throwMarkFrames.empty() && (throwMarkFrames.back().block || throwMarkFrames.back().expression);
    if (!covered) {
        // A callee declared `-> T ! E` still returns its outcome as a value where nothing marks the call, so code not
        // yet migrated keeps working; a `throws` callee's call always leaves through its error.
        if (callee.spelling == ThrowingCallee::Spelling::Fallible) {
            return outcome;
        }
        EmitError(callee.location,
                  std::format("{} can throw '{}' but is not marked with 'try'", callee.description, error.ToString()),
                  {}, "write 'try' before the expression");
        return success;
    }
    ThrowMarkFrame &mark = throwMarkFrames.back();
    ++mark.sites;
    const auto validatePayload = [&](const TypeRef &payload) {
        if (MentionsTypeParameter(payload) && currentFunctionDecl) {
            deferredOutcomeChecks[currentFunctionDecl].push_back({payload, callee.location, true});
        }
        else {
            static_cast<void>(ValidateOutcomePayload(payload, callee.location, true));
        }
    };
    validatePayload(success);
    validatePayload(error);

    ResolvedThrowSite site;
    site.payloadType = success;
    site.errorType = error;
    site.markBlock = mark.block;
    site.markExpression = mark.expression;
    if (RouteThrownError(site, error, callee.location, callee.description)) {
        throwSites.insert_or_assign(&expression, site);
    }
    return success;
}

bool AnalysisContext::RouteThrownError(ResolvedThrowSite &site, const TypeRef &error, const SourceLocation location,
                                       const std::string &description) {
    if (!throwHandlerFrames.empty()) {
        ThrowHandlerFrame &frame = throwHandlerFrames.back();
        if (frame.kind != ThrowHandlerFrame::Kind::Handler) {
            const bool deferred = frame.kind == ThrowHandlerFrame::Kind::Deferred;
            EmitError(location,
                      deferred ? std::string("an error cannot leave a deferred statement")
                               : std::string("an error cannot leave a destructor"),
                      {deferred ? "a deferred statement runs while its scope is already being left"
                                : "a destructor runs while its value is already being destroyed"},
                      "handle it with 'catch' or discard it with 'try?'");
            return false;
        }
        if (!error.IsUnknown() && std::ranges::find(frame.errors, error) == frame.errors.end()) {
            frame.errors.push_back(error);
        }
        ++frame.sites;
        frame.edges.push_back(ProjectTrackedFlow(SaveTrackedFlow(), frame.shape));
        site.handlerBlock = frame.block;
        site.handlerExpression = frame.expression;
        return true;
    }
    const std::string function = FunctionName(currentFunctionDecl);
    if (!currentReturnType.IsFallible()) {
        if (!currentReturnType.IsUnknown()) {
            EmitError(location,
                      std::format("{} can throw '{}', but {} does not throw", description, error.ToString(), function),
                      {},
                      std::format("handle the error with 'try ... catch', or declare 'throws {}' on {}",
                                  error.ToString(), function));
        }
        return false;
    }
    const TypeRef &channel = currentReturnType.FallibleError();
    const bool dependent = MentionsTypeParameter(error) || MentionsTypeParameter(channel);
    if (!dependent && !error.IsUnknown() && !channel.IsUnknown() && !ErrorFitsChannel(error, channel)) {
        EmitError(location,
                  std::format("{} can throw '{}', but {} throws '{}'", description, error.ToString(), function,
                              channel.ToString()),
                  {"an error passes on only as itself, as a member of a sum, or as part of a larger sum; it is never "
                   "converted"},
                  std::format("handle the error with 'try ... catch', or add '{}' to the 'throws' clause of {}",
                              error.ToString(), function));
        return false;
    }
    return true;
}

std::optional<ResolvedThrowSite> AnalysisContext::CheckFail(const Expr *value, const SourceLocation location,
                                                            const bool throwSpelling) {
    const std::string_view keyword = throwSpelling ? "throw" : "fail";
    // Inside a `try` block or a `try` expression with `catch` or `?`, the error goes to that handler.
    if (!throwHandlerFrames.empty() && throwHandlerFrames.back().kind == ThrowHandlerFrame::Kind::Handler) {
        // A case written without its type takes the error type the handler has seen so far, when that is one type.
        const TypeRef expected = HandlerErrorType(throwHandlerFrames.back());
        if (value && !expected.IsUnknown()) {
            ExpectContextualCaseArgument(value, expected);
        }
        const TypeRef valueType = value ? CheckExpr(*value) : TypeRef::MakeUnknown();
        std::optional<ResolvedThrowSite> accepted;
        if (value && !valueType.IsUnknown() && !valueType.IsIncompleteNative()) {
            ConsumeRecordedValue(*value, ValueConsumptionKind::Return, location);
            ResolvedThrowSite site;
            site.payloadType = TypeRef::MakeUnit();
            site.errorType = valueType;
            if (RouteThrownError(site, valueType, location, std::string(keyword))) {
                accepted = site;
            }
        }
        trackedFlowReachable = false;
        return accepted;
    }
    if (checkingDeferred) {
        EmitExitFromDeferred(location, keyword, std::format("move the '{}' out of the 'defer'", keyword));
    }
    // A case written without its type takes the type of the error channel it fails through.
    if (value && currentReturnType.IsFallible()) {
        ExpectContextualCaseArgument(value, currentReturnType.FallibleError());
    }
    const TypeRef valueType = value ? CheckExpr(*value) : TypeRef::MakeUnknown();
    std::optional<ResolvedThrowSite> accepted;
    if (!currentReturnType.IsFallible()) {
        if (!currentReturnType.IsUnknown()) {
            const std::string returned = currentReturnType.IsOpaque()
                                           ? std::string("returns no value")
                                           : std::format("returns '{}'", currentReturnType.ToString());
            if (throwSpelling) {
                EmitError(location,
                          std::format("'throw' needs a function that throws, but {} {}",
                                      FunctionName(currentFunctionDecl), returned),
                          {}, "declare 'throws E' on the function, or throw inside 'try { ... } catch { ... }'");
            }
            else {
                EmitError(location,
                          std::format("'fail' needs an enclosing fallible function, but this function {}", returned),
                          {}, "declare the function's error channel, as in '-> T ! E' or '-> ! E'");
            }
        }
    }
    else if (value && !valueType.IsUnknown()) {
        // The statement selects the outer failure channel itself, so only its operand converts, to that channel's
        // payload.
        const TypeRef &error = currentReturnType.FallibleError();
        if (CanAssignExprTo(*value, valueType, error)) {
            ConsumeRecordedValue(*value, ValueConsumptionKind::Return, location);
            ResolvedThrowSite site;
            site.payloadType = TypeRef::MakeUnit();
            site.errorType = error;
            accepted = site;
        }
        else {
            EmitError(location,
                      AssignmentErrorMessage(*value, error,
                                             std::format("'{}' value must have type '{}', but found '{}'", keyword,
                                                         error.ToString(), valueType.DisplayString())));
        }
    }
    trackedFlowReachable = false;
    return accepted;
}

void AnalysisContext::CheckBehindThrowBarrier(const ThrowHandlerFrame::Kind kind, const std::function<void()> &check) {
    ThrowHandlerFrame barrier;
    barrier.kind = kind;
    throwHandlerFrames.push_back(std::move(barrier));
    throwMarkFrames.emplace_back();
    check();
    throwMarkFrames.pop_back();
    throwHandlerFrames.pop_back();
}

void AnalysisContext::CheckHandlerArms(const std::vector<MatchExpr::Arm> &arms, const TypeRef &error,
                                       const std::optional<TypeRef> &value, const SourceLocation location,
                                       std::vector<TrackedFlow> &exits) {
    // An arm is outside its handler: a call in it needs a `try` of its own, and what it throws goes further out.
    throwMarkFrames.emplace_back();
    const TrackedFlow entry = SaveTrackedFlow();
    std::vector<const Pattern *> patterns;
    const TypeProperties errorProperties = ClassifyTypeProperties(error);
    for (const auto &arm : arms) {
        RestoreTrackedFlow(entry);
        PushScope();
        const bool savedTakesParts = std::exchange(currentPatternTakesParts, true);
        CheckPattern(*arm.pattern, error);
        currentPatternTakesParts = savedTakesParts;
        if (errorProperties.IsResolved() && !errorProperties.IsMovable() && PatternBindsValue(*arm.pattern)) {
            EmitError(arm.pattern->location,
                      std::format("'catch' cannot bind error payload '{}' by value because moving it is prohibited",
                                  error.ToString()),
                      {"a catch arm owns the error it binds"},
                      "match the error with a non-binding pattern, or remove '#NoMove' from the error type");
        }
        if (!value) {
            // A statement's arm is run for its effect, as a match statement's is.
            if (const auto *block = AstCast<const BlockExpr>(arm.body.get())) {
                if (block->isUnsafe) {
                    BeginUnsafeBlock(*block);
                }
                CheckStatementBranch(*block->block);
                if (block->isUnsafe) {
                    EndUnsafeBlock(*block);
                }
            }
            else {
                const TypeRef armType = CheckExpr(*arm.body);
                if (!IsDivergingExpression(*arm.body)) {
                    CheckFallibleDiscard(armType, arm.body->location);
                }
            }
        }
        else {
            const TypeRef &recovered = *value;
            ExpectArmValue(*arm.body, recovered);
            const TypeRef bodyType = CheckExpr(*arm.body);
            const auto *blockArm = AstCast<const BlockExpr>(arm.body.get());
            if (!IsDivergingExpression(*arm.body)) {
                if (blockArm && bodyType.IsUnit() && !recovered.IsUnit() && !recovered.IsUnknown()) {
                    if (!ReportStrayTailSemicolon(*arm.body, recovered)) {
                        EmitError(arm.location,
                                  std::format("a block arm completes with '()', but 'catch' must produce a value of "
                                              "type '{}'",
                                              recovered.ToString()),
                                  {},
                                  "end the block with the value written without a ';', or leave it with 'throw', "
                                  "'return', or a call to 'Panic'");
                    }
                }
                else if (!bodyType.IsUnknown() && !recovered.IsUnknown()) {
                    if (CanAssignExprTo(*arm.body, bodyType, recovered)) {
                        ConsumeValue(*arm.body, bodyType, ValueConsumptionKind::ConditionalArm, arm.location);
                    }
                    else {
                        EmitError(arm.location,
                                  AssignmentErrorMessage(*arm.body, recovered,
                                                         std::format("'catch' arm produces '{}', but the 'try' "
                                                                     "operand has type '{}'",
                                                                     bodyType.ToString(), recovered.ToString())));
                    }
                }
            }
        }
        PopScope();
        exits.push_back(SaveTrackedFlow());
        patterns.push_back(arm.pattern.get());
    }
    throwMarkFrames.pop_back();
    if (error.IsUnknown()) {
        return;
    }
    // The arms handle every error, as a match covers its subject.
    ValidateMatchPatterns(patterns, error);
    if (!MatchPatternsAreExhaustive(patterns, error) && !IsNativeMatchSubject(error) &&
        !(error->kind == TypeRef::Kind::Named && EnumNamed(BaseTypeName(error->name)))) {
        EmitError(location, std::format("'catch' arms do not cover every error of type '{}'", error.ToString()), {},
                  "add a '_' arm, or bind the whole error, as in 'e => ...'");
    }
}

TypeRef AnalysisContext::CheckTryMark(const TryMarkExpr &expression) {
    // The type the context expects of the whole expression is the operand's type too, so a case written without its
    // type, as in `try .Success(Read()) catch { e => .Failure(e) }`, takes it; `try?` expects the optional's payload.
    if (const auto expected = expectedExpressionTypes.find(&expression);
        expected != expectedExpressionTypes.end() && expression.operand) {
        TypeRef operandExpected = expected->second;
        if (expression.form == TryMarkExpr::Form::Optional && operandExpected.IsOptional() &&
            !operandExpected->inner.empty()) {
            operandExpected = operandExpected->inner.front();
        }
        ExpectArmValue(*expression.operand, operandExpected);
    }
    if (expression.form == TryMarkExpr::Form::Propagate) {
        throwMarkFrames.push_back({nullptr, &expression, 0});
        const TypeRef type = expression.operand ? CheckExpr(*expression.operand) : TypeRef::MakeUnknown();
        const ThrowMarkFrame mark = throwMarkFrames.back();
        throwMarkFrames.pop_back();
        if (mark.sites == 0 && !type.IsUnknown()) {
            EmitWarning(expression.location, "'try' has no call that can throw");
        }
        return type;
    }

    // `try?` and `try ... catch` handle what their operand throws.
    ThrowHandlerFrame handler;
    handler.expression = &expression;
    handler.shape = SaveTrackedFlow();
    throwHandlerFrames.push_back(std::move(handler));
    throwMarkFrames.push_back({nullptr, &expression, 0});
    const TypeRef operandType = expression.operand ? CheckExpr(*expression.operand) : TypeRef::MakeUnknown();
    throwMarkFrames.pop_back();
    ThrowHandlerFrame frame = std::move(throwHandlerFrames.back());
    throwHandlerFrames.pop_back();
    if (frame.sites == 0 && !operandType.IsUnknown()) {
        EmitWarning(expression.location, "'try' has no call that can throw");
    }
    const TypeRef error = HandlerErrorType(frame);
    if (!operandType.IsUnknown() && expression.operand) {
        // The value the operand produces moves into the result.
        ConsumeValue(*expression.operand, operandType, ValueConsumptionKind::ConstructorOperand,
                     expression.operand->location);
    }
    const TrackedFlow succeeded = SaveTrackedFlow();
    std::vector<TrackedFlow> exits = {succeeded};
    if (expression.form == TryMarkExpr::Form::Optional) {
        // A thrown error is dropped where it is caught, and the result is `none`.
        exits.insert(exits.end(), frame.edges.begin(), frame.edges.end());
        MergeTrackedFlows(exits);
        if (operandType.IsUnknown()) {
            return operandType;
        }
        tryExpressionHandlers.insert_or_assign(&expression, ResolvedThrowHandler{error, operandType});
        return TypeRef::MakeOptional(operandType);
    }
    MergeTrackedFlows(frame.edges);
    CheckHandlerArms(expression.arms, error, operandType, expression.catchLocation, exits);
    MergeTrackedFlows(exits);
    if (!operandType.IsUnknown()) {
        tryExpressionHandlers.insert_or_assign(&expression, ResolvedThrowHandler{error, operandType});
    }
    return operandType;
}

void AnalysisContext::CheckTryStatement(const TryStmt &statement) {
    ThrowHandlerFrame handler;
    handler.block = &statement;
    handler.shape = SaveTrackedFlow();
    throwHandlerFrames.push_back(std::move(handler));
    throwMarkFrames.push_back({&statement, nullptr, 0});
    if (statement.body) {
        CheckBlock(*statement.body);
    }
    throwMarkFrames.pop_back();
    ThrowHandlerFrame frame = std::move(throwHandlerFrames.back());
    throwHandlerFrames.pop_back();
    if (frame.sites == 0) {
        EmitWarning(statement.location, "'try' has no call that can throw");
    }
    const TypeRef error = HandlerErrorType(frame);
    std::vector<TrackedFlow> exits = {SaveTrackedFlow()};
    MergeTrackedFlows(frame.edges);
    CheckHandlerArms(statement.arms, error, std::nullopt, statement.catchLocation, exits);
    MergeTrackedFlows(exits);
    tryBlockHandlers.insert_or_assign(&statement, ResolvedThrowHandler{error, TypeRef::MakeUnit()});
}
} // namespace Rux::SemanticDetail
