// Statement and control-flow checking, including whether a function body
// definitely returns and whether a match covers its subject.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
bool IsNullLiteral(const Expr &expression) {
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expression);
    return literal && literal->token.kind == TokenKind::NullKeyword;
}

/// A stable textual key for a pattern, so two arms written differently but matching the same values compare equal. This
/// is what lets a duplicate or already-covered arm be reported without implementing full pattern subsumption.
std::string PatternKey(const Pattern &pattern) {
    if (const auto *literal = dynamic_cast<const LiteralPattern *>(&pattern)) {
        return "literal:" + literal->value.text;
    }
    if (dynamic_cast<const WildcardPattern *>(&pattern) || dynamic_cast<const IdentPattern *>(&pattern)) {
        return "_";
    }
    if (const auto *range = dynamic_cast<const RangePattern *>(&pattern)) {
        return "range:" + PatternKey(*range->lo) + (range->inclusive ? "..=" : "..") + PatternKey(*range->hi);
    }
    if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        std::string key = "tuple:(";
        for (const auto &element : tuple->elements) {
            key += PatternKey(*element) + ",";
        }
        return key + ")";
    }
    if (const auto *enumerator = dynamic_cast<const EnumPattern *>(&pattern)) {
        std::string key = "enum:";
        for (const auto &segment : enumerator->path) {
            key += segment + "::";
        }
        key += "(";
        for (const auto &argument : enumerator->args) {
            const std::string argumentKey = PatternKey(*argument);
            if (argumentKey.empty()) {
                return {};
            }
            key += argumentKey + ",";
        }
        for (const auto &argument : enumerator->namedArgs) {
            const std::string argumentKey = PatternKey(*argument.pattern);
            if (argumentKey.empty()) {
                return {};
            }
            key += argument.name + ":" + argumentKey + ",";
        }
        key += ")";
        return key;
    }
    return {};
}

/// Whether the pattern is irrefutable, which makes any arm after it unreachable and completes an exhaustiveness check
/// whatever the subject type is.
bool PatternMatchesEveryValue(const Pattern &pattern) {
    return dynamic_cast<const WildcardPattern *>(&pattern) != nullptr ||
           dynamic_cast<const IdentPattern *>(&pattern) != nullptr;
}

/// Whether an enum pattern accounts for one variant, including its payload. Exhaustiveness over an enum is decided
/// variant by variant, so a variant left unmatched is what the diagnostic names.
bool EnumPatternCoversVariant(const EnumPattern &pattern, const EnumDecl::Variant &variant) {
    const std::size_t fieldCount = variant.fields.size() + variant.namedFields.size();
    if (pattern.args.size() + pattern.namedArgs.size() < fieldCount) {
        return false;
    }
    return std::ranges::all_of(pattern.args,
                               [](const auto &argument) { return PatternMatchesEveryValue(*argument); }) &&
           std::ranges::all_of(pattern.namedArgs,
                               [](const auto &argument) { return PatternMatchesEveryValue(*argument.pattern); });
}

} // namespace

void SemanticAnalyzerContext::EmitError(const SourceLocation location, std::string message,
                                        std::vector<std::string> notes, std::optional<std::string> help) const {
    diags.push_back({SemanticDiagnostic::Severity::Error,
                     currentFile,
                     location,
                     std::move(message),
                     std::move(notes),
                     std::move(help),
                     {}});
}

void SemanticAnalyzerContext::EmitWarning(const SourceLocation location, std::string message) const {
    diags.push_back({SemanticDiagnostic::Severity::Warning, currentFile, location, std::move(message), {}, {}, {}});
}

void SemanticAnalyzerContext::EmitUndefinedName(const SourceLocation location, const std::string &name) const {
    std::optional<std::string> help;
    if (const Symbol *suggestion = currentScope->Suggest(name)) {
        help = std::format("did you mean '{}'?", suggestion->name);
    }
    EmitError(location, std::format("name '{}' is not defined in this scope", name), {}, std::move(help));
}

void SemanticAnalyzerContext::PushScope() {
    currentScope = &programIndex.CreateScope(*currentScope);
    moveStates.BeginScope();
}

void SemanticAnalyzerContext::PopScope() {
    assert(currentScope->Parent() != nullptr && "cannot pop global scope");
    EndBorrowScope(*currentScope);
    moveStates.EndScope();
    currentScope = currentScope->Parent();
}

void SemanticAnalyzerContext::CheckBlock(const Block &block) {
    const Stmt *savedBorrowStatement = currentBorrowStatement;
    auto savedEndedProvenance = std::move(endedBorrowProvenance);
    endedBorrowProvenance.clear();
    PushScope();
    for (const auto &statement : block.stmts) {
        currentBorrowStatement = statement.get();
        endedBorrowProvenance.clear();
        if (trackedFlowReachable) {
            CheckStatement(*statement);
            ExpireDeadBorrowsAfter(*statement);
            endedBorrowProvenance.clear();
            continue;
        }

        const TrackedFlow unreachableEntry = SaveTrackedFlow();
        CheckStatement(*statement);
        ExpireDeadBorrowsAfter(*statement);
        RestoreTrackedFlow(unreachableEntry);
        endedBorrowProvenance.clear();
    }
    PopScope();
    currentBorrowStatement = savedBorrowStatement;
    endedBorrowProvenance = std::move(savedEndedProvenance);
}

void SemanticAnalyzerContext::CheckFunctionBody(const Block &block, const FuncDecl &function,
                                                const TypeRef &returnType) {
    CheckBlock(block);
    if (!returnType.IsUnknown() && !returnType.IsOpaque() && !function.isNoReturn && !BlockDefinitelyReturns(block)) {
        EmitError(function.location,
                  std::format("function '{}' must return a value of type '{}' on every control-flow path",
                              function.name, returnType.ToString()));
    }
}

void SemanticAnalyzerContext::CheckBooleanCondition(const TypeRef &type, const SourceLocation location,
                                                    const std::string_view construct) const {
    if (!type.IsUnknown() && !type.IsBool()) {
        EmitError(location,
                  std::format("condition for '{}' must have type 'bool', but found '{}'", construct, type.ToString()));
    }
}

void SemanticAnalyzerContext::CheckStatement(const Stmt &statement) {
    if (const auto *expressionStatement = dynamic_cast<const ExprStmt *>(&statement)) {
        CheckExpr(*expressionStatement->expr);
        if (const auto *call = dynamic_cast<const CallExpr *>(expressionStatement->expr.get())) {
            const auto binding = callableBindings.find(call);
            const auto *function = binding == callableBindings.end()
                                     ? nullptr
                                     : dynamic_cast<const FuncDecl *>(binding->second.selectedDeclaration);
            const auto *callee = dynamic_cast<const IdentExpr *>(call->callee.get());
            const Symbol *symbol = callee ? currentScope->Lookup(callee->name) : nullptr;
            if ((function && function->isNoReturn) || (symbol && symbol->intrinsicName == "Panic")) {
                trackedFlowReachable = false;
            }
        }
    }
    else if (const auto *letStatement = dynamic_cast<const LetStmt *>(&statement)) {
        if (letStatement->type) {
            ValidateArrayType(**letStatement->type, false);
        }
        TypeRef initializerType = letStatement->init ? CheckExpr(*letStatement->init) : TypeRef::MakeUnknown();
        TypeRef declarationType = letStatement->type ? ResolveType(**letStatement->type) : initializerType;
        const FuncDecl *defaultConstructor = nullptr;
        if (!letStatement->init && letStatement->isMut && !declarationType.IsUnknown()) {
            std::vector<const FuncDecl *> eligible;
            for (const FuncDecl *candidate : ConstructorCandidates(declarationType)) {
                const bool acceptsNoArguments = std::ranges::all_of(candidate->params, [](const Param &parameter) {
                    return parameter.isVariadic || parameter.defaultValue.has_value();
                });
                if (acceptsNoArguments) {
                    eligible.push_back(candidate);
                }
            }
            if (eligible.size() == 1) {
                defaultConstructor = eligible.front();
                const auto substitutions = MethodTypeSubstitutions(declarationType);
                QueueGenericInstantiation(*defaultConstructor, substitutions);
                defaultConstructors.insert_or_assign(letStatement,
                                                     ResolvedDefaultConstructor{defaultConstructor, declarationType});
                EmitCallSiteDiagnostics(*defaultConstructor, letStatement->location);
            }
            else if (eligible.size() > 1) {
                EmitError(letStatement->location,
                          std::format("default construction of '{}' is ambiguous", declarationType.ToString()),
                          {"more than one constructor can be called without arguments"},
                          "remove defaults so exactly one constructor accepts no arguments");
            }
        }
        if (declarationType.kind != TypeRef::Kind::Reference) {
            ValidateStoredType(declarationType, letStatement->location, "local variable");
        }

        if (!letStatement->init && !letStatement->type) {
            EmitError(letStatement->location, "uninitialized variable requires an explicit type");
        }

        if (!letStatement->init && !letStatement->isMut) {
            EmitError(letStatement->location, "immutable variable requires an initializer");
        }

        if (!letStatement->init && letStatement->pattern) {
            EmitError(letStatement->location, "destructuring declaration requires an initializer");
        }

        if (!letStatement->type && declarationType.IsUnknown() && !letStatement->pattern) {
            EmitWarning(letStatement->location, std::format("cannot infer type of '{}'", letStatement->name));
        }

        const bool initializerAccepted =
            letStatement->init && !initializerType.IsUnknown() && !declarationType.IsUnknown() &&
            (!letStatement->type || CanAssignExprTo(*letStatement->init, initializerType, declarationType));
        const bool nullReferenceInitializer = letStatement->init && declarationType.kind == TypeRef::Kind::Reference &&
                                              IsNullLiteral(*letStatement->init);
        if (nullReferenceInitializer) {
            EmitError(letStatement->init->location,
                      std::format("null cannot initialize non-null reference '{}'", declarationType.ToString()));
        }
        if (letStatement->init && letStatement->type && !initializerType.IsUnknown() && !declarationType.IsUnknown() &&
            !initializerAccepted) {
            EmitError(letStatement->location,
                      AssignmentErrorMessage(*letStatement->init, declarationType,
                                             std::format("cannot assign '{}' to '{}'", initializerType.ToString(),
                                                         declarationType.ToString())));
        }
        if (initializerAccepted && !nullReferenceInitializer && declarationType.kind != TypeRef::Kind::Reference) {
            ConsumeValue(*letStatement->init, initializerType, ValueConsumptionKind::Initialization,
                         letStatement->location);
        }

        if (letStatement->pattern) {
            CheckLetPattern(*letStatement->pattern, declarationType, letStatement->isMut);
            return;
        }

        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = letStatement->name;
        symbol.location = letStatement->location;
        symbol.type = declarationType;
        symbol.isMut = letStatement->isMut;
        Symbol *defined = DefineTrackedLocal(std::move(symbol), letStatement->init != nullptr || defaultConstructor);
        if (defined && initializerAccepted && declarationType.kind == TypeRef::Kind::Reference) {
            RegisterReferenceBinding(*defined, *letStatement->init, declarationType);
        }
    }
    else if (const auto *ifStatement = dynamic_cast<const IfStmt *>(&statement)) {
        TypeRef condition = CheckExpr(*ifStatement->condition);
        if (!condition.IsUnknown() && !condition.IsBool()) {
            EmitError(ifStatement->condition->location,
                      std::format("condition for 'if' must have type 'bool', but found '{}'", condition.ToString()));
        }
        TrackedFlow fallthrough = SaveTrackedFlow();
        std::vector<TrackedFlow> exits;

        RestoreTrackedFlow(fallthrough);
        CheckBlock(*ifStatement->thenBlock);
        exits.push_back(SaveTrackedFlow());
        for (const auto &elseIf : ifStatement->elseIfs) {
            RestoreTrackedFlow(fallthrough);
            TypeRef elseIfCondition = CheckExpr(*elseIf.condition);
            if (!elseIfCondition.IsUnknown() && !elseIfCondition.IsBool()) {
                EmitError(elseIf.condition->location,
                          std::format("condition for 'else if' must have type 'bool', but found '{}'",
                                      elseIfCondition.ToString()));
            }
            fallthrough = SaveTrackedFlow();
            CheckBlock(*elseIf.block);
            exits.push_back(SaveTrackedFlow());
        }
        if (ifStatement->elseBlock) {
            RestoreTrackedFlow(fallthrough);
            CheckBlock(*ifStatement->elseBlock);
            exits.push_back(SaveTrackedFlow());
        }
        else {
            exits.push_back(std::move(fallthrough));
        }
        MergeTrackedFlows(exits);
    }
    else if (const auto *whileStatement = dynamic_cast<const WhileStmt *>(&statement)) {
        if (!whileStatement->label.empty()) {
            activeLabels.insert(whileStatement->label);
        }

        TypeRef condition = CheckExpr(*whileStatement->condition);
        if (!condition.IsUnknown() && !condition.IsBool()) {
            EmitError(whileStatement->condition->location,
                      std::format("condition for 'while' must have type 'bool', but found '{}'", condition.ToString()));
        }

        const TrackedFlow loopEntry = SaveTrackedFlow();
        BeginTrackedLoop(whileStatement->label);
        ++loopDepth;
        CheckBlock(*whileStatement->body);
        --loopDepth;
        const TrackedFlow bodyExit = SaveTrackedFlow();
        TrackedLoop loop = EndTrackedLoop();
        const auto *literal = dynamic_cast<const LiteralExpr *>(whileStatement->condition.get());
        const bool alwaysTrue =
            literal && literal->token.kind == TokenKind::BoolLiteral && literal->token.text == "true";
        const bool alwaysFalse =
            literal && literal->token.kind == TokenKind::BoolLiteral && literal->token.text == "false";
        std::vector<TrackedFlow> exits = std::move(loop.breaks);
        if (!alwaysTrue) {
            exits.push_back(loopEntry);
        }
        if (!alwaysTrue && !alwaysFalse) {
            exits.insert(exits.end(), loop.continues.begin(), loop.continues.end());
            exits.push_back(bodyExit);
        }
        MergeTrackedFlows(exits);
        if (!whileStatement->label.empty()) {
            activeLabels.erase(whileStatement->label);
        }
    }
    else if (const auto *doWhileStatement = dynamic_cast<const DoWhileStmt *>(&statement)) {
        if (!doWhileStatement->label.empty()) {
            activeLabels.insert(doWhileStatement->label);
        }

        const TrackedFlow loopEntry = SaveTrackedFlow();
        BeginTrackedLoop(doWhileStatement->label);
        ++loopDepth;
        CheckBlock(*doWhileStatement->body);
        --loopDepth;
        const TrackedFlow bodyExit = SaveTrackedFlow();
        TrackedLoop loop = EndTrackedLoop();
        std::vector<TrackedFlow> iterationExits = loop.continues;
        iterationExits.push_back(bodyExit);
        MergeTrackedFlows(iterationExits);

        TypeRef condition;
        if (trackedFlowReachable) {
            condition = CheckExpr(*doWhileStatement->condition);
        }
        else {
            const TrackedFlow unreachableExit = SaveTrackedFlow();
            RestoreTrackedFlow(loopEntry);
            condition = CheckExpr(*doWhileStatement->condition);
            RestoreTrackedFlow(unreachableExit);
        }
        if (!condition.IsUnknown() && !condition.IsBool()) {
            EmitError(
                doWhileStatement->condition->location,
                std::format("condition for 'do-while' must have type 'bool', but found '{}'", condition.ToString()));
        }
        const auto *literal = dynamic_cast<const LiteralExpr *>(doWhileStatement->condition.get());
        const bool alwaysTrue =
            literal && literal->token.kind == TokenKind::BoolLiteral && literal->token.text == "true";
        std::vector<TrackedFlow> exits = std::move(loop.breaks);
        if (!alwaysTrue) {
            exits.push_back(SaveTrackedFlow());
        }
        MergeTrackedFlows(exits);

        if (!doWhileStatement->label.empty()) {
            activeLabels.erase(doWhileStatement->label);
        }
    }
    else if (const auto *loopStatement = dynamic_cast<const LoopStmt *>(&statement)) {
        if (!loopStatement->label.empty()) {
            activeLabels.insert(loopStatement->label);
        }
        BeginTrackedLoop(loopStatement->label);
        ++loopDepth;
        CheckBlock(*loopStatement->body);
        --loopDepth;
        TrackedLoop loop = EndTrackedLoop();
        MergeTrackedFlows(loop.breaks);
        if (!loopStatement->label.empty()) {
            activeLabels.erase(loopStatement->label);
        }
    }
    else if (const auto *forStatement = dynamic_cast<const ForStmt *>(&statement)) {
        TypeRef iterableType = CheckExpr(*forStatement->iterable);
        const TrackedFlow loopEntry = SaveTrackedFlow();
        TypeRef elementType = TypeRef::MakeUnknown();
        if (iterableType.IsRange() && !iterableType.IsIterableRange()) {
            EmitError(forStatement->iterable->location,
                      std::format("range type '{}' has no initial value and is not iterable", iterableType.ToString()));
        }
        else if (auto shape = IterationShapeOf(iterableType)) {
            elementType = shape->itemType;
            RecordIteration(*forStatement, *shape);
        }
        else if (!iterableType.IsUnknown()) {
            EmitNotIterable(forStatement->iterable->location, iterableType);
        }

        Symbol *outerVariable = currentScope->Lookup(forStatement->variable);
        const bool reuseOuterVariable = outerVariable != nullptr && outerVariable->kind == Symbol::Kind::Var &&
                                        outerVariable->isMut && !elementType.IsUnknown() &&
                                        outerVariable->type == elementType;
        PushScope();
        if (!reuseOuterVariable) {
            Symbol variable;
            variable.kind = Symbol::Kind::Var;
            variable.name = forStatement->variable;
            variable.location = forStatement->location;
            variable.type = elementType;
            variable.isMut = false;
            DefineTrackedLocal(std::move(variable), true);
        }
        if (!forStatement->label.empty()) {
            activeLabels.insert(forStatement->label);
        }
        BeginTrackedLoop(forStatement->label);
        ++loopDepth;
        CheckBlock(*forStatement->body);
        --loopDepth;
        TrackedLoop loop = EndTrackedLoop();
        if (!forStatement->label.empty()) {
            activeLabels.erase(forStatement->label);
        }
        PopScope();
        const TrackedFlow bodyExit = SaveTrackedFlow();
        std::vector<TrackedFlow> exits = {loopEntry, bodyExit};
        exits.insert(exits.end(), loop.breaks.begin(), loop.breaks.end());
        exits.insert(exits.end(), loop.continues.begin(), loop.continues.end());
        MergeTrackedFlows(exits);
    }
    else if (const auto *matchStatement = dynamic_cast<const MatchStmt *>(&statement)) {
        const TypeRef subjectType = CheckExpr(*matchStatement->subject);
        ConsumeMatchSubject(*matchStatement->subject, subjectType, matchStatement->arms, matchStatement->location);
        const TrackedFlow matchEntry = SaveTrackedFlow();
        std::vector<TrackedFlow> exits;
        std::vector<const Pattern *> patterns;
        patterns.reserve(matchStatement->arms.size());
        bool coveredAll = false;
        for (const auto &arm : matchStatement->arms) {
            patterns.push_back(arm.pattern.get());
            RestoreTrackedFlow(matchEntry);
            PushScope();
            CheckPattern(*arm.pattern, subjectType);
            CheckExpr(*arm.body);
            PopScope();
            if (!coveredAll) {
                exits.push_back(SaveTrackedFlow());
            }
            coveredAll = coveredAll || PatternMatchesEveryValue(*arm.pattern);
        }
        ValidateMatchPatterns(patterns, subjectType);
        if (!MatchPatternsAreExhaustive(patterns, subjectType)) {
            exits.push_back(matchEntry);
        }
        MergeTrackedFlows(exits);
    }
    else if (const auto *returnStatement = dynamic_cast<const ReturnStmt *>(&statement)) {
        if (currentFunctionNoReturn) {
            EmitError(returnStatement->location, "return is not allowed in a '#NoReturn' function");
        }
        bool returnAccepted = false;
        if (returnStatement->value) {
            TypeRef valueType = CheckExpr(**returnStatement->value);
            if (valueType.kind == TypeRef::Kind::Reference) {
                EmitError(returnStatement->location,
                          std::format("reference value '{}' cannot escape through a return", valueType.ToString()),
                          {"references are restricted to parameters, receivers, and local aliases"},
                          "return an owned value instead");
            }
            if (currentReturnType.IsOpaque()) {
                EmitError(returnStatement->location, "'return' cannot have a value in a function with no return type");
            }
            else if (!valueType.IsUnknown() && !currentReturnType.IsUnknown() && !currentReturnType.IsOpaque() &&
                     !CanAssignExprTo(**returnStatement->value, valueType, currentReturnType)) {
                EmitError(returnStatement->location,
                          AssignmentErrorMessage(**returnStatement->value, currentReturnType,
                                                 std::format("'return' value must have type '{}', but found '{}'",
                                                             currentReturnType.ToString(), valueType.ToString())));
            }
            else if (!valueType.IsUnknown() && !currentReturnType.IsUnknown()) {
                returnAccepted = true;
            }
        }
        else if (!currentReturnType.IsOpaque() && !currentReturnType.IsUnknown()) {
            EmitError(returnStatement->location,
                      std::format("'return' requires a value of type '{}'", currentReturnType.ToString()));
        }
        if (returnAccepted) {
            ConsumeRecordedValue(**returnStatement->value, ValueConsumptionKind::Return, returnStatement->location);
        }
        trackedFlowReachable = false;
    }
    else if (const auto *breakStatement = dynamic_cast<const BreakStmt *>(&statement)) {
        if (loopDepth == 0) {
            EmitError(statement.location, "'break' can only be used inside 'while', 'for', or 'loop'");
        }
        else if (!breakStatement->label.empty() && !activeLabels.contains(breakStatement->label)) {
            EmitError(statement.location,
                      std::format("'break' refers to unknown loop label '{}'", breakStatement->label));
        }
        else {
            RecordTrackedLoopExit(breakStatement->label, false);
            trackedFlowReachable = false;
        }
    }
    else if (const auto *continueStatement = dynamic_cast<const ContinueStmt *>(&statement)) {
        if (loopDepth == 0) {
            EmitError(statement.location, "'continue' can only be used inside 'while', 'for', or 'loop'");
        }
        else if (!continueStatement->label.empty() && !activeLabels.contains(continueStatement->label)) {
            EmitError(statement.location,
                      std::format("'continue' refers to unknown loop label '{}'", continueStatement->label));
        }
        else {
            RecordTrackedLoopExit(continueStatement->label, true);
            trackedFlowReachable = false;
        }
    }
    else if (const auto *declarationStatement = dynamic_cast<const DeclStmt *>(&statement)) {
        programIndex.CollectDeclaration(*declarationStatement->decl, *currentScope, currentFile,
                                        [this](const TypeExpr &type) { return ResolveType(type); });
        CheckDecl(*declarationStatement->decl);
    }
}

void SemanticAnalyzerContext::CheckLetPattern(const Pattern &pattern, const TypeRef &type, const bool isMutable) {
    if (!type.IsUnknown()) {
        patternTypes.insert_or_assign(&pattern, type);
    }
    if (const auto *identifierPattern = dynamic_cast<const IdentPattern *>(&pattern)) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = identifierPattern->name;
        symbol.location = identifierPattern->location;
        symbol.type = type;
        symbol.isMut = isMutable;
        DefineTrackedLocal(std::move(symbol), true);
    }
    else if (dynamic_cast<const WildcardPattern *>(&pattern)) {}
    else if (const auto *tuplePattern = dynamic_cast<const TuplePattern *>(&pattern)) {
        if (type.kind != TypeRef::Kind::Tuple) {
            if (!type.IsUnknown()) {
                EmitError(tuplePattern->location,
                          std::format("cannot destructure non-tuple type '{}'", type.ToString()));
            }
            for (const auto &element : tuplePattern->elements) {
                CheckLetPattern(*element, TypeRef::MakeUnknown(), isMutable);
            }
            return;
        }

        if (tuplePattern->elements.size() != type.inner.size()) {
            EmitError(tuplePattern->location,
                      std::format("tuple pattern has {} elements but type '{}' has {}", tuplePattern->elements.size(),
                                  type.ToString(), type.inner.size()));
        }

        const std::size_t count = std::min(tuplePattern->elements.size(), type.inner.size());
        for (std::size_t index = 0; index < count; ++index) {
            CheckLetPattern(*tuplePattern->elements[index], type.inner[index], isMutable);
        }
        for (std::size_t index = count; index < tuplePattern->elements.size(); ++index) {
            CheckLetPattern(*tuplePattern->elements[index], TypeRef::MakeUnknown(), isMutable);
        }
    }
    else {
        EmitError(pattern.location, "unsupported pattern in let binding");
        CheckPattern(pattern, type);
    }
}

void SemanticAnalyzerContext::CheckPattern(const Pattern &pattern, const TypeRef &subjectType) {
    if (!subjectType.IsUnknown()) {
        patternTypes.insert_or_assign(&pattern, subjectType);
    }
    if (const auto *identifierPattern = dynamic_cast<const IdentPattern *>(&pattern)) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = identifierPattern->name;
        symbol.location = identifierPattern->location;
        symbol.type = subjectType;
        symbol.isMut = false;
        DefineTrackedLocal(std::move(symbol), true);
    }
    else if (const auto *literalPattern = dynamic_cast<const LiteralPattern *>(&pattern)) {
        const TypeRef literalType = LiteralType(literalPattern->value);
        const bool compatibleNumeric = literalType.IsNumeric() && subjectType.IsNumeric();
        if (!literalType.IsUnknown() && !subjectType.IsUnknown() && !compatibleNumeric &&
            !literalType.IsAssignableTo(subjectType)) {
            EmitError(literalPattern->location,
                      std::format("pattern has type '{}', but the matched value has type '{}'", literalType.ToString(),
                                  subjectType.ToString()));
        }
    }
    else if (const auto *guardedPattern = dynamic_cast<const GuardedPattern *>(&pattern)) {
        CheckPattern(*guardedPattern->inner, subjectType);
        const TypeRef guardType = CheckExpr(*guardedPattern->guard);
        if (!guardType.IsUnknown() && !guardType.IsBool()) {
            EmitError(guardedPattern->guard->location,
                      std::format("pattern guard must have type 'bool', but found '{}'", guardType.ToString()));
        }
    }
    else if (const auto *rangePattern = dynamic_cast<const RangePattern *>(&pattern)) {
        if (!subjectType.IsUnknown() && !subjectType.IsNumeric()) {
            EmitError(rangePattern->location,
                      std::format("range pattern cannot match value of type '{}'", subjectType.ToString()));
        }
        CheckPattern(*rangePattern->lo, subjectType);
        CheckPattern(*rangePattern->hi, subjectType);
    }
    else if (const auto *tuplePattern = dynamic_cast<const TuplePattern *>(&pattern)) {
        if (!subjectType.IsUnknown() && subjectType.kind != TypeRef::Kind::Tuple) {
            EmitError(tuplePattern->location,
                      std::format("tuple pattern cannot match value of type '{}'", subjectType.ToString()));
        }
        else if (subjectType.kind == TypeRef::Kind::Tuple &&
                 tuplePattern->elements.size() != subjectType.inner.size()) {
            EmitError(tuplePattern->location, std::format("tuple pattern has {} elements, but matched tuple has {}",
                                                          tuplePattern->elements.size(), subjectType.inner.size()));
        }
        for (std::size_t index = 0; index < tuplePattern->elements.size(); ++index) {
            const TypeRef elementType = subjectType.kind == TypeRef::Kind::Tuple && index < subjectType.inner.size()
                                          ? subjectType.inner[index]
                                          : TypeRef::MakeUnknown();
            CheckPattern(*tuplePattern->elements[index], elementType);
        }
    }
    else if (const auto *structPattern = dynamic_cast<const StructPattern *>(&pattern)) {
        const auto declaration = structDecls.find(structPattern->typeName);
        if (!currentScope->Lookup(structPattern->typeName) || declaration == structDecls.end()) {
            EmitError(structPattern->location,
                      std::format("unknown type '{}' in struct pattern", structPattern->typeName));
        }
        else if (!subjectType.IsUnknown() && (subjectType.kind != TypeRef::Kind::Named ||
                                              BaseTypeName(subjectType.name) != structPattern->typeName)) {
            EmitError(structPattern->location, std::format("struct pattern '{}' cannot match value of type '{}'",
                                                           structPattern->typeName, subjectType.ToString()));
        }
        std::unordered_set<std::string> fieldNames;
        for (const auto &field : structPattern->fields) {
            if (!fieldNames.insert(field.name).second) {
                EmitError(field.location, std::format("duplicate field '{}' in struct pattern", field.name));
            }
            const StructDecl::Field *matchedField = nullptr;
            if (declaration != structDecls.end()) {
                const auto found = std::ranges::find(declaration->second->fields, field.name, &StructDecl::Field::name);
                if (found != declaration->second->fields.end()) {
                    matchedField = &*found;
                }
                else {
                    EmitError(field.location,
                              std::format("struct '{}' has no field '{}'", structPattern->typeName, field.name));
                }
            }
            CheckPattern(*field.pattern, matchedField ? ResolveType(*matchedField->type) : TypeRef::MakeUnknown());
        }
    }
    else if (const auto *enumPattern = dynamic_cast<const EnumPattern *>(&pattern)) {
        std::string enumName;
        std::string variantName;
        const EnumDecl *enumDeclaration = nullptr;

        if (enumPattern->path.size() == 1) {
            variantName = enumPattern->path[0];
            if (subjectType.kind != TypeRef::Kind::Named) {
                EmitError(enumPattern->location,
                          std::format("cannot infer enum type for shorthand pattern '.{}' from subject type '{}'",
                                      variantName, subjectType.ToString()));
            }
            else {
                enumName = BaseTypeName(subjectType.name);
                if (const EnumDecl *enumeration = EnumNamed(enumName)) {
                    enumDeclaration = enumeration;
                }
                else {
                    EmitError(enumPattern->location, std::format("type '{}' is not an enum in shorthand pattern '.{}'",
                                                                 subjectType.ToString(), variantName));
                }
            }
        }
        else if (enumPattern->path.size() >= 2) {
            enumName = enumPattern->path[0];
            variantName = enumPattern->path[1];
            if (!currentScope->Lookup(enumName)) {
                EmitError(enumPattern->location, std::format("unknown name '{}' in enum pattern", enumName));
            }
            if (const EnumDecl *enumeration = EnumNamed(enumName)) {
                enumDeclaration = enumeration;
            }
        }

        const EnumDecl::Variant *variant =
            enumName.empty() || variantName.empty() ? nullptr : LookupEnumVariant(enumName, variantName);
        if (enumDeclaration && !variant) {
            EmitError(enumPattern->location, std::format("enum '{}' has no variant '{}'", enumName, variantName));
        }
        if (variant) {
            const std::size_t expectedFields = variant->fields.size() + variant->namedFields.size();
            const std::size_t actualFields = enumPattern->args.size() + enumPattern->namedArgs.size();
            if (actualFields != expectedFields) {
                EmitError(enumPattern->location,
                          std::format("pattern for '{}::{}' expects {} field{}, but found {}", enumName, variantName,
                                      expectedFields, expectedFields == 1 ? "" : "s", actualFields));
            }
        }

        std::unordered_map<std::string, TypeRef> substitutions;
        if (enumDeclaration) {
            const auto typeArguments = ParseTypeArgsFromTypeName(subjectType.name);
            const auto &parameters = enumDeclaration->typeParams;
            const std::size_t count = std::min(parameters.size(), typeArguments.size());
            for (std::size_t index = 0; index < count; ++index) {
                substitutions.emplace(parameters[index].name, typeArguments[index]);
            }
        }
        std::unordered_set<std::string> namedArguments;
        for (const auto &argument : enumPattern->namedArgs) {
            if (!namedArguments.insert(argument.name).second) {
                EmitError(argument.location, std::format("duplicate field '{}' in enum pattern", argument.name));
                continue;
            }

            const EnumDecl::Variant::NamedField *field = nullptr;
            if (variant) {
                for (const auto &candidate : variant->namedFields) {
                    if (candidate.name == argument.name) {
                        field = &candidate;
                        break;
                    }
                }
            }

            if (field) {
                CheckLetPattern(*argument.pattern, ResolveTypeWithSubstitution(*field->type, substitutions), false);
            }
            else {
                if (variant) {
                    EmitError(argument.location, std::format("unknown field '{}' in enum pattern", argument.name));
                }
                CheckPattern(*argument.pattern);
            }
        }
        for (std::size_t index = 0; index < enumPattern->args.size(); ++index) {
            if (variant && index < variant->fields.size()) {
                CheckLetPattern(*enumPattern->args[index],
                                ResolveTypeWithSubstitution(*variant->fields[index], substitutions), false);
            }
            else if (variant && index - variant->fields.size() < variant->namedFields.size()) {
                CheckLetPattern(*enumPattern->args[index],
                                ResolveTypeWithSubstitution(*variant->namedFields[index - variant->fields.size()].type,
                                                            substitutions),
                                false);
            }
            else {
                CheckPattern(*enumPattern->args[index]);
            }
        }
    }
}

void SemanticAnalyzerContext::ValidateMatchPatterns(const std::vector<const Pattern *> &patterns,
                                                    const TypeRef &subjectType) {
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> coveredVariants;
    bool coveredAll = false;
    for (const Pattern *pattern : patterns) {
        if (!pattern) {
            continue;
        }
        if (coveredAll) {
            EmitError(pattern->location, "match arm is unreachable because an earlier pattern matches every value");
            continue;
        }
        const std::string key = PatternKey(*pattern);
        if (!key.empty() && !seen.insert(key).second) {
            EmitError(pattern->location, "duplicate pattern in match");
        }
        if (const auto *enumerator = dynamic_cast<const EnumPattern *>(pattern);
            enumerator && !enumerator->path.empty()) {
            const std::string &variantName = enumerator->path.back();
            if (const auto *variant = LookupEnumVariant(BaseTypeName(subjectType.name), variantName);
                variant && EnumPatternCoversVariant(*enumerator, *variant)) {
                coveredVariants.insert(variantName);
            }
        }
        coveredAll = PatternMatchesEveryValue(*pattern);
    }

    if (coveredAll || subjectType.kind != TypeRef::Kind::Named) {
        return;
    }
    const std::string enumName = BaseTypeName(subjectType.name);
    const EnumDecl *declaration = EnumNamed(enumName);
    if (!declaration) {
        return;
    }
    std::vector<std::string> missing;
    for (const auto &variant : declaration->variants) {
        if (!coveredVariants.contains(variant.name)) {
            missing.push_back(enumName + "::" + variant.name);
        }
    }
    if (!missing.empty()) {
        std::string names;
        for (const auto &name : missing) {
            names += (names.empty() ? "" : ", ") + name;
        }
        EmitError(patterns.empty() ? SourceLocation{} : patterns.back()->location,
                  std::format("match on '{}' is not exhaustive; missing {}", subjectType.ToString(), names));
    }
}

void SemanticAnalyzerContext::ValidateMatchPatterns(const MatchExpr &expression, const TypeRef &subjectType) {
    std::vector<const Pattern *> patterns;
    patterns.reserve(expression.arms.size());
    for (const auto &arm : expression.arms) {
        patterns.push_back(arm.pattern.get());
    }
    ValidateMatchPatterns(patterns, subjectType);
}

bool SemanticAnalyzerContext::MatchPatternsAreExhaustive(const std::vector<const Pattern *> &patterns,
                                                         const TypeRef &subjectType) const {
    if (std::ranges::any_of(patterns,
                            [](const Pattern *pattern) { return pattern && PatternMatchesEveryValue(*pattern); })) {
        return true;
    }
    if (subjectType.IsBool()) {
        bool hasTrue = false;
        bool hasFalse = false;
        for (const Pattern *pattern : patterns) {
            const auto *literal = dynamic_cast<const LiteralPattern *>(pattern);
            hasTrue = hasTrue || (literal && literal->value.text == "true");
            hasFalse = hasFalse || (literal && literal->value.text == "false");
        }
        return hasTrue && hasFalse;
    }
    if (subjectType.kind != TypeRef::Kind::Named) {
        return false;
    }

    const std::string enumName = BaseTypeName(subjectType.name);
    const EnumDecl *declaration = EnumNamed(enumName);
    if (!declaration) {
        return false;
    }
    std::unordered_set<std::string> covered;
    for (const Pattern *pattern : patterns) {
        const auto *enumerator = dynamic_cast<const EnumPattern *>(pattern);
        if (enumerator && !enumerator->path.empty()) {
            const std::string &variantName = enumerator->path.back();
            if (const auto *variant = LookupEnumVariant(enumName, variantName);
                variant && EnumPatternCoversVariant(*enumerator, *variant)) {
                covered.insert(variantName);
            }
        }
    }
    return std::ranges::all_of(declaration->variants,
                               [&](const auto &variant) { return covered.contains(variant.name); });
}

bool SemanticAnalyzerContext::BlockDefinitelyReturns(const Block &block) const {
    std::function<bool(const Block &)> blockReturns;
    std::function<bool(const Expr &)> expressionReturns;
    std::function<bool(const Stmt &)> statementReturns;
    std::function<bool(const Block &, std::string_view, bool)> containsBreak;
    expressionReturns = [&](const Expr &expression) {
        const auto *blockExpression = dynamic_cast<const BlockExpr *>(&expression);
        return blockExpression && blockReturns(*blockExpression->block);
    };
    containsBreak = [&](const Block &candidate, const std::string_view label, const bool allowUnlabeled) {
        for (const auto &inner : candidate.stmts) {
            if (const auto *exit = dynamic_cast<const BreakStmt *>(inner.get())) {
                if ((allowUnlabeled && exit->label.empty()) || (!label.empty() && exit->label == label)) {
                    return true;
                }
            }
            else if (const auto *conditional = dynamic_cast<const IfStmt *>(inner.get())) {
                if (containsBreak(*conditional->thenBlock, label, allowUnlabeled) ||
                    (conditional->elseBlock && containsBreak(*conditional->elseBlock, label, allowUnlabeled)) ||
                    std::ranges::any_of(conditional->elseIfs, [&](const auto &branch) {
                        return branch.block && containsBreak(*branch.block, label, allowUnlabeled);
                    })) {
                    return true;
                }
            }
            else if (const auto *match = dynamic_cast<const MatchStmt *>(inner.get())) {
                if (std::ranges::any_of(match->arms, [&](const auto &arm) {
                        const auto *body = dynamic_cast<const BlockExpr *>(arm.body.get());
                        return body && containsBreak(*body->block, label, allowUnlabeled);
                    })) {
                    return true;
                }
            }
            else if (const auto *nestedWhile = dynamic_cast<const WhileStmt *>(inner.get())) {
                if (containsBreak(*nestedWhile->body, label, false)) {
                    return true;
                }
            }
            else if (const auto *nestedDo = dynamic_cast<const DoWhileStmt *>(inner.get())) {
                if (containsBreak(*nestedDo->body, label, false)) {
                    return true;
                }
            }
            else if (const auto *nestedLoop = dynamic_cast<const LoopStmt *>(inner.get())) {
                if (containsBreak(*nestedLoop->body, label, false)) {
                    return true;
                }
            }
            else if (const auto *nestedFor = dynamic_cast<const ForStmt *>(inner.get())) {
                if (containsBreak(*nestedFor->body, label, false)) {
                    return true;
                }
            }
        }
        return false;
    };
    statementReturns = [&](const Stmt &statement) {
        if (dynamic_cast<const ReturnStmt *>(&statement)) {
            return true;
        }
        if (const auto *expression = dynamic_cast<const ExprStmt *>(&statement)) {
            const auto *call = dynamic_cast<const CallExpr *>(expression->expr.get());
            const auto *callee = call ? dynamic_cast<const IdentExpr *>(call->callee.get()) : nullptr;
            const Symbol *symbol = callee ? currentScope->Lookup(callee->name) : nullptr;
            return symbol && (symbol->intrinsicName == "Panic" ||
                              std::ranges::any_of(symbol->funcOverloads,
                                                  [](const FuncDecl *function) { return function->isNoReturn; }));
        }
        if (const auto *ifStatement = dynamic_cast<const IfStmt *>(&statement)) {
            if (!ifStatement->elseBlock || !blockReturns(*ifStatement->thenBlock) ||
                !blockReturns(*ifStatement->elseBlock)) {
                return false;
            }
            return std::ranges::all_of(ifStatement->elseIfs,
                                       [&](const auto &branch) { return branch.block && blockReturns(*branch.block); });
        }
        if (const auto *match = dynamic_cast<const MatchStmt *>(&statement)) {
            std::vector<const Pattern *> patterns;
            patterns.reserve(match->arms.size());
            for (const auto &arm : match->arms) {
                patterns.push_back(arm.pattern.get());
            }
            const auto type = patterns.empty() ? patternTypes.end() : patternTypes.find(patterns.front());
            return type != patternTypes.end() && MatchPatternsAreExhaustive(patterns, type->second) &&
                   std::ranges::all_of(match->arms, [&](const auto &arm) { return expressionReturns(*arm.body); });
        }
        if (const auto *whileStatement = dynamic_cast<const WhileStmt *>(&statement)) {
            const auto *condition = dynamic_cast<const LiteralExpr *>(whileStatement->condition.get());
            return condition && condition->token.kind == TokenKind::BoolLiteral && condition->token.text == "true" &&
                   !containsBreak(*whileStatement->body, whileStatement->label, true);
        }
        if (const auto *doWhileStatement = dynamic_cast<const DoWhileStmt *>(&statement)) {
            const auto *condition = dynamic_cast<const LiteralExpr *>(doWhileStatement->condition.get());
            return condition && condition->token.kind == TokenKind::BoolLiteral && condition->token.text == "true" &&
                   !containsBreak(*doWhileStatement->body, doWhileStatement->label, true);
        }
        if (const auto *loop = dynamic_cast<const LoopStmt *>(&statement)) {
            return !containsBreak(*loop->body, loop->label, true);
        }
        return false;
    };
    blockReturns = [&](const Block &candidate) {
        return std::ranges::any_of(candidate.stmts,
                                   [&](const auto &statement) { return statement && statementReturns(*statement); });
    };
    return blockReturns(block);
}

const EnumDecl::Variant *SemanticAnalyzerContext::LookupEnumVariant(const std::string &enumName,
                                                                    const std::string &variantName) const {
    const EnumDecl *enumeration = EnumNamed(enumName);
    if (!enumeration) {
        return nullptr;
    }
    for (const auto &variant : enumeration->variants) {
        if (variant.name == variantName) {
            return &variant;
        }
    }
    return nullptr;
}
} // namespace Rux::SemanticDetail
