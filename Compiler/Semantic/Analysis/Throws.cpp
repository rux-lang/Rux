// Errors: `-> T ! E` functions, `throw`, and the postfix error forms. A function that can fail returns the native
// fallible `T ! E`, and a call to it is a fallible value wherever nothing takes it apart. The operand of a postfix form
// is a throw site: its value is the success payload, and its error goes out of the function for `x!`, or to the arms
// for `x catch { arms }`, `x !! v` and `x !!;`. A `throw` always leaves the function. Each site records that target
// once, and the flow at each throw is an edge into its handler, so ownership and borrows are checked along every path
// an error takes.

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

/// Whether a declared return type is a fallible, `-> T ! E` or `-> !E`.
bool DeclaresThrows(const std::optional<TypeExprPtr> &returnType) noexcept {
    return returnType && AstCast<const FallibleTypeExpr>(returnType->get()) != nullptr;
}

/// The name a diagnostic gives the function being checked.
std::string FunctionName(const FuncDecl *function) {
    return function ? std::format("'{}'", function->name) : std::string("the function");
}
} // namespace

AnalysisContext::ThrowingCallee AnalysisContext::ThrowingCalleeOf(const Expr &expression) {
    ThrowingCallee callee;
    const auto spellingOf = [](const std::optional<TypeExprPtr> &returnType) {
        return DeclaresThrows(returnType) ? ThrowingCallee::Spelling::Throws : ThrowingCallee::Spelling::None;
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
        // A function value whose type throws is called as any throwing function is.
        if (binding->second.dispatch == ResolvedCallableBinding::DispatchKind::Indirect) {
            callee.spelling = ThrowingCallee::Spelling::Throws;
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
        callee.spelling = method ? spellingOf(method->returnType) : ThrowingCallee::Spelling::Throws;
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
    // The operand of an older `?` or `? else` is the outcome those operators take apart.
    if (&expression == outcomeOperand) {
        return outcome;
    }
    // Only the operand of a postfix `x!`, `x catch { ... }` or `x !! v` is a throw site, whatever it is: a call, or a
    // fallible kept in a local. Any other fallible is a value of its own, handled where the value is used.
    if (throwMarkFrames.empty() || throwMarkFrames.back().operand != &expression) {
        return outcome;
    }
    ThrowingCallee callee = ThrowingCalleeOf(expression);
    if (callee.spelling == ThrowingCallee::Spelling::None) {
        callee.location = expression.location;
        callee.description = "this value";
    }
    const TypeRef &success = outcome.FallibleSuccess();
    const TypeRef &error = outcome.FallibleError();
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
    site.markExpression = mark.expression;
    const ErrorFormExpr *handler = mark.expression->form == ErrorFormExpr::Form::Catch ? mark.expression : nullptr;
    if (RouteThrownError(site, error, callee.location, callee.description, handler)) {
        throwSites.insert_or_assign(&expression, site);
    }
    return success;
}

bool AnalysisContext::RouteThrownError(ResolvedThrowSite &site, const TypeRef &error, const SourceLocation location,
                                       const std::string &description, const ErrorFormExpr *handler) {
    if (handler) {
        // The operand of `catch` or `!!`: the error goes to that handler's arms.
        const auto frame = std::ranges::find(throwHandlerFrames, handler, &ThrowHandlerFrame::expression);
        if (frame != throwHandlerFrames.end()) {
            if (!error.IsUnknown() && std::ranges::find(frame->errors, error) == frame->errors.end()) {
                frame->errors.push_back(error);
            }
            ++frame->sites;
            frame->edges.push_back(ProjectTrackedFlow(SaveTrackedFlow(), frame->shape));
            site.handlerExpression = handler;
            return true;
        }
    }
    // Everything else leaves the function, unless a deferred statement or a destructor stands in the way.
    for (auto frame = throwHandlerFrames.rbegin(); frame != throwHandlerFrames.rend(); ++frame) {
        if (frame->kind == ThrowHandlerFrame::Kind::Handler) {
            continue;
        }
        const bool deferred = frame->kind == ThrowHandlerFrame::Kind::Deferred;
        EmitError(location,
                  deferred ? std::string("an error cannot leave a deferred statement")
                           : std::string("an error cannot leave a destructor"),
                  {deferred ? "a deferred statement runs while its scope is already being left"
                            : "a destructor runs while its value is already being destroyed"},
                  "handle it with '!!' or 'catch { ... }'");
        return false;
    }
    const std::string function = FunctionName(currentFunctionDecl);
    if (!currentReturnType.IsFallible()) {
        if (!currentReturnType.IsUnknown()) {
            const std::string declared =
                currentReturnType.IsOpaque() || currentReturnType.IsUnit()
                    ? std::format("-> !{}", error.DisplayString())
                    : std::format("-> {} ! {}", currentReturnType.DisplayString(), error.DisplayString());
            EmitError(
                location,
                std::format("{} can fail with '{}', but {} cannot fail", description, error.DisplayString(), function),
                {},
                std::format("handle the error with '!!' or 'catch {{ ... }}', or declare '{}' on {}", declared,
                            function));
        }
        return false;
    }
    const TypeRef &channel = currentReturnType.FallibleError();
    const bool dependent = MentionsTypeParameter(error) || MentionsTypeParameter(channel);
    if (!dependent && !error.IsUnknown() && !channel.IsUnknown() && !ErrorFitsChannel(error, channel)) {
        EmitError(location,
                  std::format("{} can fail with '{}', but {} fails with '{}'", description, error.DisplayString(),
                              function, channel.DisplayString()),
                  {"an error passes on only as itself, as a member of a sum, or as part of a larger sum; it is never "
                   "converted"},
                  std::format("handle the error with 'catch {{ ... }}', or add '{}' to the error of {}",
                              error.DisplayString(), function));
        return false;
    }
    return true;
}

std::optional<ResolvedThrowSite> AnalysisContext::CheckFail(const Expr *value, const SourceLocation location,
                                                            const bool throwSpelling) {
    const std::string_view keyword = throwSpelling ? "throw" : "fail";
    if (checkingDeferred) {
        EmitExitFromDeferred(location, keyword, std::format("move the '{}' out of the 'defer'", keyword));
    }
    // A case written without its type takes the type of the error side it leaves through.
    if (value && currentReturnType.IsFallible()) {
        ExpectContextualCaseArgument(value, currentReturnType.FallibleError());
    }
    const TypeRef valueType = value ? CheckExpr(*value) : TypeRef::MakeUnknown();
    std::optional<ResolvedThrowSite> accepted;
    if (!currentReturnType.IsFallible()) {
        if (!currentReturnType.IsUnknown()) {
            const bool nothing = currentReturnType.IsOpaque() || currentReturnType.IsUnit();
            const std::string returned =
                nothing ? std::string("returns no value") : std::format("returns '{}'", currentReturnType.ToString());
            EmitError(location,
                      std::format("'throw' needs a function that can fail, but {} {}",
                                  FunctionName(currentFunctionDecl), returned),
                      {},
                      nothing ? std::string("declare '-> !E' on the function")
                              : std::format("declare '-> {} ! E' on the function", currentReturnType.DisplayString()));
        }
    }
    else if (value && !valueType.IsUnknown()) {
        // The statement selects the error side itself, so only its operand converts, to that side's type.
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
    // An arm is outside its handler: an `x!` or a `throw` in it leaves the function.
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
                CheckStatementBranch(*block->block);
            }
            else {
                static_cast<void>(CheckExpr(*arm.body));
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
                                                         std::format("'catch' arm produces '{}', but the handled "
                                                                     "value has type '{}'",
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

TypeRef AnalysisContext::CheckErrorForm(const ErrorFormExpr &expression) {
    const bool handles = expression.form == ErrorFormExpr::Form::Catch;
    const bool asStatement = std::exchange(statementHandler, nullptr) == &expression;
    TypeRef expected = TypeRef::MakeUnknown();
    if (const auto found = expectedExpressionTypes.find(&expression); found != expectedExpressionTypes.end()) {
        expected = found->second;
    }
    // A case written without its type in a handled operand takes the type the context expects of the whole.
    if (handles && !expected.IsUnknown() && expression.operand) {
        ExpectArmValue(*expression.operand, expected);
    }
    const std::string_view spelling = !handles ? "!" : expression.replacesError ? "!!" : "catch";
    const auto rejectPlain = [&](const TypeRef &type) {
        if (type.IsUnknown() || type.IsFallible()) {
            return false;
        }
        if (spelling == "!") {
            EmitError(expression.location,
                      std::format("'!' passes an error on, and '{}' has none", type.DisplayString()), {},
                      type.IsOptional() ? std::optional<std::string>("write '?' to leave with 'none'") : std::nullopt);
        }
        else if (spelling == "!!") {
            EmitError(expression.location,
                      std::format("'!!' replaces an error, and '{}' has none", type.DisplayString()), {},
                      type.IsOptional() ? std::optional<std::string>("write '?"
                                                                     "?' to fill an absence")
                                        : std::nullopt);
        }
        else {
            EmitError(expression.catchLocation,
                      std::format("'catch' handles an error, and '{}' has none", type.DisplayString()));
        }
        return true;
    };

    if (!handles) {
        throwMarkFrames.push_back({&expression, 0, expression.operand.get()});
        const TypeRef type = expression.operand ? CheckExpr(*expression.operand) : TypeRef::MakeUnknown();
        const ThrowMarkFrame mark = throwMarkFrames.back();
        throwMarkFrames.pop_back();
        // A fallible operand is a throw site of the mark, which returned its success; anything else had no error.
        if (mark.sites == 0) {
            rejectPlain(type);
        }
        return type;
    }

    // `x catch { arms }`, `x !! v` and `x !!;` handle what their operand throws.
    ThrowHandlerFrame handler;
    handler.expression = &expression;
    handler.shape = SaveTrackedFlow();
    throwHandlerFrames.push_back(std::move(handler));
    throwMarkFrames.push_back({&expression, 0, expression.operand.get()});
    const TypeRef operandType = expression.operand ? CheckExpr(*expression.operand) : TypeRef::MakeUnknown();
    throwMarkFrames.pop_back();
    ThrowHandlerFrame frame = std::move(throwHandlerFrames.back());
    throwHandlerFrames.pop_back();
    if (frame.sites == 0) {
        rejectPlain(operandType);
    }
    const TypeRef error = HandlerErrorType(frame);
    if (!operandType.IsUnknown() && expression.operand && !asStatement) {
        // The value the operand produces moves into the result.
        ConsumeValue(*expression.operand, operandType, ValueConsumptionKind::ConstructorOperand,
                     expression.operand->location);
    }
    // The whole expression has the success type, or the type its context expects when the success converts to it,
    // as `int?` for `ParsePort(text) !! none`. With no expected type, `x !! none` makes the success optional.
    TypeRef result = operandType;
    if (!asStatement && !operandType.IsUnknown()) {
        if (!expected.IsUnknown() && expected != operandType &&
            CanAssignExprTo(*expression.operand, operandType, expected)) {
            result = expected;
        }
        else if (expected.IsUnknown() && expression.replacesError && expression.arms.size() == 1 &&
                 AstCast<const NoneExpr>(expression.arms.front().body.get()) && !operandType.IsOptional()) {
            result = TypeRef::MakeOptional(operandType);
        }
    }
    const TrackedFlow succeeded = SaveTrackedFlow();
    std::vector<TrackedFlow> exits = {succeeded};
    MergeTrackedFlows(frame.edges);
    if (expression.ignoresError) {
        exits.push_back(SaveTrackedFlow());
    }
    else {
        CheckHandlerArms(expression.arms, error, asStatement ? std::nullopt : std::optional<TypeRef>(result),
                         expression.catchLocation, exits);
    }
    MergeTrackedFlows(exits);
    if (!operandType.IsUnknown()) {
        ResolvedThrowHandler resolved{error, operandType, TypeRef::MakeUnknown(), false};
        if (result != operandType) {
            resolved.resultType = result;
        }
        resolved.discardsValue = asStatement;
        errorHandlers.insert_or_assign(&expression, resolved);
    }
    return asStatement ? TypeRef::MakeUnit() : result;
}
} // namespace Rux::SemanticDetail
