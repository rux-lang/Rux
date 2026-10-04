// Statement and control-flow checking, including whether a function body
// definitely returns and whether a match covers its subject.

#include "Lexer/Lexer.h"
#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// A stable textual key for a pattern, so two arms written differently but matching the same values compare equal. This
/// is what lets a duplicate or already-covered arm be reported without implementing full pattern subsumption.
std::string PatternKey(const Pattern &pattern) {
    if (const auto *literal = dynamic_cast<const LiteralPattern *>(&pattern)) {
        if (literal->value.kind == TokenKind::CharLiteral) {
            return "character:" + std::to_string(Lexer::DecodeCharLiteralCodePoint(literal->value.text).value_or(0));
        }
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

/// Whether a case pattern accounts for one case, including its payload. Exhaustiveness is decided case by case, so an
/// uncovered declaration member is what the diagnostic names.
bool CasePatternCoversCase(const EnumPattern &pattern, const EnumDecl::Variant &selectedCase,
                           const EnumDecl::Form form) {
    const std::size_t fieldCount = selectedCase.fields.size() + selectedCase.namedFields.size();
    const std::size_t patternFieldCount = pattern.args.size() + pattern.namedArgs.size();
    if ((form == EnumDecl::Form::Enumeration && patternFieldCount != 0) || patternFieldCount != fieldCount) {
        return false;
    }
    return std::ranges::all_of(pattern.args,
                               [](const auto &argument) { return PatternMatchesEveryValue(*argument); }) &&
           std::ranges::all_of(pattern.namedArgs,
                               [](const auto &argument) { return PatternMatchesEveryValue(*argument.pattern); });
}

} // namespace

void AnalysisContext::EmitError(const SourceLocation location, std::string message, std::vector<std::string> notes,
                                std::optional<std::string> help) const {
    // An error found while re-checking an instantiated generic body is about that instantiation, which the body's
    // own location does not show.
    if (activeInstantiation) {
        std::string note = InstantiationNote(*activeInstantiation->decl, activeInstantiation->substitutions);
        if (std::ranges::find(notes, note) == notes.end()) {
            notes.push_back(std::move(note));
        }
    }
    diags.push_back({SemanticDiagnostic::Severity::Error,
                     currentFile,
                     location,
                     std::move(message),
                     std::move(notes),
                     std::move(help),
                     {}});
}

void AnalysisContext::EmitWarning(const SourceLocation location, std::string message) const {
    diags.push_back({SemanticDiagnostic::Severity::Warning, currentFile, location, std::move(message), {}, {}, {}});
}

void AnalysisContext::EmitUndefinedName(const SourceLocation location, const std::string &name) const {
    std::optional<std::string> help;
    if (const Symbol *suggestion = currentScope->Suggest(name)) {
        help = std::format("did you mean '{}'?", suggestion->name);
    }
    EmitError(location, std::format("name '{}' is not defined in this scope", name), {}, std::move(help));
}

void AnalysisContext::PushScope() {
    currentScope = &programIndex.CreateScope(*currentScope);
    moveStates.BeginScope();
}

void AnalysisContext::CheckFallibleDiscard(const TypeRef &type, const SourceLocation location) {
    if (!type.IsFallible()) {
        return;
    }
    // A constructor whose other channel no context completed has no type worth spelling.
    EmitError(location,
              type.IsIncompleteNative() ? std::string("constructed fallible value is discarded")
                                        : std::format("fallible result of type '{}' is discarded", type.ToString()),
              {"a failure that nothing handles is lost"},
              "propagate it with '?', recover with 'catch', or match both '.Success' and '.Failure'");
}

void AnalysisContext::CheckUnreadFallibleLocals() {
    std::erase_if(fallibleLocals, [&](const FallibleLocal &local) {
        if (local.scope != currentScope) {
            return false;
        }
        if (!readSymbols.contains(local.symbol)) {
            // Binding a fallible to `_` discards it as surely as an expression statement does; a named local that is
            // never read is a likely mistake rather than a certain one.
            if (local.symbol->name == "_") {
                EmitError(local.symbol->location,
                          std::format("fallible result of type '{}' is discarded", local.symbol->type.ToString()),
                          {"binding a fallible to '_' does not handle its failure"},
                          "propagate it with '?', recover with 'catch', or match both '.Success' and '.Failure'");
            }
            else {
                EmitWarning(
                    local.symbol->location,
                    std::format("fallible local '{}' is never read; its failure is never handled", local.symbol->name));
            }
        }
        return true;
    });
}

void AnalysisContext::PopScope() {
    assert(currentScope->Parent() != nullptr && "cannot pop global scope");
    CheckUnreadFallibleLocals();
    EndBorrowScope(*currentScope);
    moveStates.EndScope();
    currentScope = currentScope->Parent();
}

void AnalysisContext::CheckBlock(const Block &block) {
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

void AnalysisContext::CheckFunctionBody(const Block &block, const FuncDecl &function, const TypeRef &returnType) {
    CheckBlock(block);
    if (!returnType.IsUnknown() && !returnType.IsOpaque() && !CompletesWithoutValue(returnType) &&
        !function.isNoReturn && !BlockDefinitelyReturns(block)) {
        EmitError(function.location,
                  std::format("function '{}' must return a value of type '{}' on every control-flow path",
                              function.name, returnType.ToString()));
    }
}

void AnalysisContext::CheckBooleanCondition(const TypeRef &type, const SourceLocation location,
                                            const std::string_view construct) const {
    if (!type.IsUnknown() && !type.IsBool()) {
        EmitError(location, std::format("condition for '{}' must have type 'bool', but found '{}'", construct,
                                        type.DisplayString()));
    }
}

void AnalysisContext::CheckStatement(const Stmt &statement) {
    if (const auto *expressionStatement = dynamic_cast<const ExprStmt *>(&statement)) {
        CheckFallibleDiscard(CheckExpr(*expressionStatement->expr), expressionStatement->location);
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
        if (letStatement->init && letStatement->type &&
            dynamic_cast<const ReferenceTypeExpr *>(letStatement->type->get())) {
            RejectSubsetViewUse(*letStatement->init, letStatement->location, "be stored as a reference");
        }
        // The annotation is resolved first so that an initializer whose parts take their type from it, such as the
        // elements of an array literal, is checked against it.
        const TypeRef annotatedType = letStatement->type ? ResolveType(**letStatement->type) : TypeRef::MakeUnknown();
        if (letStatement->init && !annotatedType.IsUnknown()) {
            expectedExpressionTypes.insert_or_assign(letStatement->init.get(), annotatedType);
        }
        TypeRef initializerType = letStatement->init ? CheckExpr(*letStatement->init) : TypeRef::MakeUnknown();
        TypeRef declarationType = letStatement->type ? annotatedType : initializerType;
        // `none`, `.Success(v)`, and `.Failure(e)` leave a part of their type to the context, so a binding cannot take
        // its whole type from one of them.
        const bool incompleteNative = !letStatement->type && initializerType.MentionsIncompleteNative();
        if (incompleteNative) {
            const bool isNone = initializerType.IsNoneValue();
            EmitError(letStatement->location,
                      std::format("cannot infer the type of '{}' from {}", letStatement->name,
                                  isNone ? "'none'" : "a native constructor with an unknown channel"),
                      {},
                      isNone ? "annotate the optional type, as in 'let value: int32? = none;'"
                             : "annotate the fallible type, as in 'let value: int32 ! ParseError = .Success(1i32);'");
            declarationType = TypeRef::MakeUnknown();
        }
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

        if (!letStatement->type && declarationType.IsUnknown() && !letStatement->pattern && !incompleteNative) {
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
                                             std::format("cannot assign '{}' to '{}'", initializerType.DisplayString(),
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
        if (defined && defined->type.IsFallible()) {
            fallibleLocals.push_back({defined, currentScope});
        }
        if (defined && initializerAccepted && declarationType.kind == TypeRef::Kind::Reference) {
            RegisterReferenceBinding(*defined, *letStatement->init, declarationType);
        }
    }
    else if (const auto *ifStatement = dynamic_cast<const IfStmt *>(&statement)) {
        TypeRef condition = ReadBorrowedScalar(*ifStatement->condition, CheckExpr(*ifStatement->condition));
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
            TypeRef elseIfCondition = ReadBorrowedScalar(*elseIf.condition, CheckExpr(*elseIf.condition));
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
            EnterLoopLabel(whileStatement->label, whileStatement->location);
        }

        TypeRef condition = ReadBorrowedScalar(*whileStatement->condition, CheckExpr(*whileStatement->condition));
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
            ExitLoopLabel(whileStatement->label);
        }
    }
    else if (const auto *doWhileStatement = dynamic_cast<const DoWhileStmt *>(&statement)) {
        if (!doWhileStatement->label.empty()) {
            EnterLoopLabel(doWhileStatement->label, doWhileStatement->location);
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
            condition = ReadBorrowedScalar(*doWhileStatement->condition, CheckExpr(*doWhileStatement->condition));
        }
        else {
            const TrackedFlow unreachableExit = SaveTrackedFlow();
            RestoreTrackedFlow(loopEntry);
            condition = ReadBorrowedScalar(*doWhileStatement->condition, CheckExpr(*doWhileStatement->condition));
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
            ExitLoopLabel(doWhileStatement->label);
        }
    }
    else if (const auto *loopStatement = dynamic_cast<const LoopStmt *>(&statement)) {
        if (!loopStatement->label.empty()) {
            EnterLoopLabel(loopStatement->label, loopStatement->location);
        }
        BeginTrackedLoop(loopStatement->label);
        ++loopDepth;
        CheckBlock(*loopStatement->body);
        --loopDepth;
        TrackedLoop loop = EndTrackedLoop();
        MergeTrackedFlows(loop.breaks);
        if (!loopStatement->label.empty()) {
            ExitLoopLabel(loopStatement->label);
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
            EnterLoopLabel(forStatement->label, forStatement->location);
        }
        BeginTrackedLoop(forStatement->label);
        ++loopDepth;
        CheckBlock(*forStatement->body);
        --loopDepth;
        TrackedLoop loop = EndTrackedLoop();
        if (!forStatement->label.empty()) {
            ExitLoopLabel(forStatement->label);
        }
        PopScope();
        const TrackedFlow bodyExit = SaveTrackedFlow();
        std::vector<TrackedFlow> exits = {loopEntry, bodyExit};
        exits.insert(exits.end(), loop.breaks.begin(), loop.breaks.end());
        exits.insert(exits.end(), loop.continues.begin(), loop.continues.end());
        MergeTrackedFlows(exits);
    }
    else if (const auto *matchStatement = dynamic_cast<const MatchStmt *>(&statement)) {
        const TypeRef expressionType = CheckExpr(*matchStatement->subject);
        const TypeRef subjectType = expressionType.kind == TypeRef::Kind::Reference && !expressionType.inner.empty()
                                      ? expressionType.inner.front()
                                      : expressionType;
        ConsumeMatchSubject(*matchStatement->subject, expressionType, matchStatement->arms, matchStatement->location);
        const PatternBorrow armBorrow = IsNativeMatchSubject(subjectType)
                                          ? MatchSubjectBorrow(*matchStatement->subject, expressionType)
                                          : PatternBorrow::Owned;
        const TrackedFlow matchEntry = SaveTrackedFlow();
        std::vector<TrackedFlow> exits;
        std::vector<const Pattern *> patterns;
        patterns.reserve(matchStatement->arms.size());
        bool coveredAll = false;
        for (const auto &arm : matchStatement->arms) {
            patterns.push_back(arm.pattern.get());
            RestoreTrackedFlow(matchEntry);
            PushScope();
            const PatternBorrow savedBorrow = std::exchange(currentPatternBorrow, armBorrow);
            CheckPattern(*arm.pattern, subjectType);
            currentPatternBorrow = savedBorrow;
            // A bare-expression arm of a match statement is an expression statement: its value is discarded.
            const TypeRef armType = CheckExpr(*arm.body);
            if (!dynamic_cast<const BlockExpr *>(arm.body.get()) && !IsDivergingExpression(*arm.body)) {
                CheckFallibleDiscard(armType, arm.body->location);
            }
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
    else if (const auto *failStatement = dynamic_cast<const FailStmt *>(&statement)) {
        CheckFail(failStatement->value.get(), failStatement->location);
    }
    else if (const auto *returnStatement = dynamic_cast<const ReturnStmt *>(&statement)) {
        CheckReturn(returnStatement->value ? returnStatement->value->get() : nullptr, returnStatement->location);
    }
    else if (const auto *breakStatement = dynamic_cast<const BreakStmt *>(&statement)) {
        CheckLoopExit(false, breakStatement->label, statement.location);
    }
    else if (const auto *continueStatement = dynamic_cast<const ContinueStmt *>(&statement)) {
        CheckLoopExit(true, continueStatement->label, statement.location);
    }
    else if (const auto *declarationStatement = dynamic_cast<const DeclStmt *>(&statement)) {
        programIndex.CollectDeclaration(
            *declarationStatement->decl, *currentScope, currentFile,
            [this](const TypeExpr &type) { return ResolveType(type); }, &currentPackage);
        CheckDecl(*declarationStatement->decl);
    }
    else if (const auto *deferStatement = dynamic_cast<const DeferStmt *>(&statement)) {
        if (deferStatement->deferredStmt) {
            CheckStatement(*deferStatement->deferredStmt);
        }
    }
}

bool AnalysisContext::CompletesWithoutValue(const TypeRef &returnType) noexcept {
    return returnType.IsUnit() || (returnType.IsFallible() && returnType.FallibleSuccess().IsUnit());
}

void AnalysisContext::CheckReturn(const Expr *value, const SourceLocation location) {
    if (currentFunctionNoReturn) {
        EmitError(location, "return is not allowed in a '#NoReturn' function");
    }
    bool returnAccepted = false;
    if (value) {
        if (!currentReturnType.IsUnknown() && !currentReturnType.IsOpaque()) {
            expectedExpressionTypes.insert_or_assign(value, currentReturnType);
        }
        TypeRef valueType = CheckExpr(*value);
        if (valueType.kind == TypeRef::Kind::Reference && !valueType.CanReadScalarTo(currentReturnType)) {
            EmitError(location,
                      std::format("reference value '{}' cannot escape through a return", valueType.ToString()),
                      {"references are restricted to parameters, receivers, and local aliases"},
                      "return an owned value instead");
        }
        if (currentReturnType.IsOpaque()) {
            EmitError(location, "'return' cannot have a value in a function with no return type");
        }
        else if (!valueType.IsUnknown() && !currentReturnType.IsUnknown() && !currentReturnType.IsOpaque() &&
                 !CanAssignExprTo(*value, valueType, currentReturnType)) {
            EmitError(location,
                      AssignmentErrorMessage(*value, currentReturnType,
                                             std::format("'return' value must have type '{}', but found '{}'",
                                                         currentReturnType.ToString(), valueType.DisplayString())));
        }
        else if (!valueType.IsUnknown() && !currentReturnType.IsUnknown()) {
            returnAccepted = true;
        }
    }
    // A unit result, or a fallible whose success carries the unit, completes without a written value: `return;` is
    // `return ();`, or its success.
    else if (!currentReturnType.IsOpaque() && !currentReturnType.IsUnknown() &&
             !CompletesWithoutValue(currentReturnType)) {
        EmitError(location, std::format("'return' requires a value of type '{}'", currentReturnType.ToString()));
    }
    if (returnAccepted) {
        ConsumeRecordedValue(*value, ValueConsumptionKind::Return, location);
    }
    trackedFlowReachable = false;
}

void AnalysisContext::CheckFail(const Expr *value, const SourceLocation location) {
    const TypeRef valueType = value ? CheckExpr(*value) : TypeRef::MakeUnknown();
    if (!currentReturnType.IsFallible()) {
        if (!currentReturnType.IsUnknown()) {
            EmitError(location,
                      std::format("'fail' needs an enclosing fallible function, but this function {}",
                                  currentReturnType.IsOpaque()
                                      ? std::string("returns no value")
                                      : std::format("returns '{}'", currentReturnType.ToString())),
                      {}, "declare the function's error channel, as in '-> T ! E' or '-> ! E'");
        }
    }
    else if (value && !valueType.IsUnknown()) {
        // `fail` selects the outer failure channel itself, so only its operand converts, to that channel's payload.
        const TypeRef &error = currentReturnType.FallibleError();
        if (CanAssignExprTo(*value, valueType, error)) {
            ConsumeRecordedValue(*value, ValueConsumptionKind::Return, location);
        }
        else {
            EmitError(location, AssignmentErrorMessage(*value, error,
                                                       std::format("'fail' value must have type '{}', but found '{}'",
                                                                   error.ToString(), valueType.DisplayString())));
        }
    }
    trackedFlowReachable = false;
}

void AnalysisContext::CheckLoopExit(const bool isContinue, const std::string &label, const SourceLocation location) {
    const std::string_view keyword = isContinue ? "continue" : "break";
    if (loopDepth == 0) {
        EmitError(location, std::format("'{}' can only be used inside 'while', 'for', or 'loop'", keyword));
    }
    else if (!label.empty() && !activeLabels.contains(label)) {
        EmitError(location, std::format("'{}' refers to unknown loop label '{}'", keyword, label));
    }
    else {
        RecordTrackedLoopExit(label, isContinue);
        trackedFlowReachable = false;
    }
}

bool AnalysisContext::IsDivergingExpression(const Expr &expression) const {
    if (dynamic_cast<const DivergeExpr *>(&expression)) {
        return true;
    }
    const auto *call = dynamic_cast<const CallExpr *>(&expression);
    const auto *callee = call ? dynamic_cast<const IdentExpr *>(call->callee.get()) : nullptr;
    const Symbol *symbol = callee ? currentScope->Lookup(callee->name) : nullptr;
    return symbol &&
           (symbol->intrinsicName == "Panic" ||
            std::ranges::any_of(symbol->funcOverloads, [](const FuncDecl *function) { return function->isNoReturn; }));
}

void AnalysisContext::EnterLoopLabel(const std::string &label, const SourceLocation location) {
    if (activeLabels.contains(label)) {
        EmitError(location, std::format("loop label '{}' shadows an enclosing loop label", label), {},
                  "give the inner loop a different label");
    }
    activeLabels.insert(label);
}

void AnalysisContext::ExitLoopLabel(const std::string &label) {
    // Labels are counted, so leaving an inner loop that reused a name keeps the enclosing loop's label active.
    if (const auto found = activeLabels.find(label); found != activeLabels.end()) {
        activeLabels.erase(found);
    }
}

void AnalysisContext::CheckLetPattern(const Pattern &pattern, const TypeRef &type, const bool isMutable) {
    if (!type.IsUnknown()) {
        patternTypes.insert_or_assign(&pattern, type);
    }
    if (const auto *identifierPattern = dynamic_cast<const IdentPattern *>(&pattern)) {
        CheckFreeBindingName(identifierPattern->name, identifierPattern->location, type);
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

void AnalysisContext::CheckPattern(const Pattern &pattern, const TypeRef &subjectType) {
    if (!subjectType.IsUnknown()) {
        patternTypes.insert_or_assign(&pattern, subjectType);
        NoteDeferredNativeType(subjectType);
    }
    if (const auto *typedPattern = dynamic_cast<const TypedPattern *>(&pattern)) {
        CheckTypedPattern(*typedPattern, subjectType);
        return;
    }
    if (const auto *presencePattern = dynamic_cast<const PresencePattern *>(&pattern)) {
        // `p?` is exactly `.Some(p)`: it needs an optional at its position and checks `p` against the payload.
        TypeRef payload = TypeRef::MakeUnknown();
        if (subjectType.IsOptional()) {
            payload = subjectType.inner.front();
        }
        else if (!subjectType.IsUnknown()) {
            EmitError(presencePattern->location,
                      std::format("a presence suffix needs an optional subject, but the matched value has type '{}'",
                                  subjectType.ToString()),
                      {},
                      subjectType.IsFallible() ? std::optional<std::string>("match '.Success(...)' or '.Failure(...)'")
                                               : std::nullopt);
        }
        if (presencePattern->inner) {
            CheckPattern(*presencePattern->inner, payload);
        }
        return;
    }
    if (dynamic_cast<const NonePattern *>(&pattern)) {
        if (!subjectType.IsUnknown() && !subjectType.IsOptional()) {
            const std::string nominal =
                subjectType.kind == TypeRef::Kind::Named ? BaseTypeName(subjectType.name) : std::string();
            if (!nominal.empty() && LookupCase(nominal, "None")) {
                EmitError(pattern.location,
                          std::format("'none' matches a native optional; use '.None' for variant '{}'", nominal), {},
                          "write '.None'");
            }
            else {
                EmitError(pattern.location,
                          std::format("'none' needs an optional subject, but the matched value has type '{}'",
                                      subjectType.ToString()));
            }
        }
        return;
    }
    const bool bindsWhole = dynamic_cast<const IdentPattern *>(&pattern) != nullptr ||
                            dynamic_cast<const WildcardPattern *>(&pattern) != nullptr ||
                            dynamic_cast<const GuardedPattern *>(&pattern) != nullptr;
    if (subjectType.IsSum() && !bindsWhole && !dynamic_cast<const RangePattern *>(&pattern)) {
        // A sum member is selected by what the pattern names: a qualified case or struct names its member, a literal
        // or a tuple its member's type. The pattern is then checked against that member, and keeps the sum as its own
        // type so coverage and lowering see where the selection happens.
        std::string issue;
        std::optional<std::string> help;
        if (const std::optional<std::size_t> member = SumMemberOfPattern(pattern, subjectType, &issue, &help)) {
            const TypeRef memberType = subjectType.inner[*member];
            CheckPattern(pattern, memberType);
            patternTypes.insert_or_assign(&pattern, subjectType);
            sumMemberPatterns.insert_or_assign(&pattern, memberType);
        }
        else {
            EmitError(pattern.location, std::move(issue), {}, std::move(help));
            DefinePatternBindings(pattern);
        }
        return;
    }
    if (subjectType.IsOptional() || subjectType.IsFallible()) {
        const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern);
        if (enumeration && enumeration->path.size() == 1) {
            CheckNativeCasePattern(*enumeration, subjectType);
            return;
        }
        if (!bindsWhole) {
            // Native levels are opened only by their own patterns; nothing implicitly enters presence or success.
            const bool optional = subjectType.IsOptional();
            EmitError(pattern.location,
                      std::format("this pattern cannot match native {} '{}'", optional ? "optional" : "fallible",
                                  subjectType.ToString()),
                      {},
                      optional ? "match a present value with '.Some(...)' or 'value?', and absence with 'none'"
                               : "match a channel with '.Success(...)' or '.Failure(...)'");
            DefinePatternBindings(pattern);
            return;
        }
    }
    if (const auto *identifierPattern = dynamic_cast<const IdentPattern *>(&pattern)) {
        CheckFreeBindingName(identifierPattern->name, identifierPattern->location, subjectType);
        Symbol symbol;
        symbol.kind = Symbol::Kind::Var;
        symbol.name = identifierPattern->name;
        symbol.location = identifierPattern->location;
        symbol.type = subjectType;
        symbol.isMut = false;
        RecordPatternBinding(pattern, symbol, false);
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
        // The guard runs with the arm's bindings in place but before the arm is chosen, so it reads them as an
        // ordinary expression and must leave them for whichever arm runs next.
        const PatternBorrow savedBorrow = std::exchange(currentPatternBorrow, PatternBorrow::Owned);
        const TypeRef guardType = CheckExpr(*guardedPattern->guard);
        currentPatternBorrow = savedBorrow;
        if (!guardType.IsUnknown() && !guardType.IsBool()) {
            EmitError(guardedPattern->guard->location,
                      std::format("pattern guard must have type 'bool', but found '{}'", guardType.ToString()));
        }
        CheckGuardKeepsBindings(*guardedPattern);
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
        const auto declaration = structDecls.find(NominalTypeName(structPattern->typeName));
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
                          std::format("cannot infer enum or variant type for shorthand pattern '.{}' from type '{}'",
                                      variantName, subjectType.ToString()));
            }
            else {
                enumName = BaseTypeName(subjectType.name);
                if (const EnumDecl *enumeration = EnumNamed(enumName)) {
                    enumDeclaration = enumeration;
                }
                else {
                    EmitError(enumPattern->location,
                              std::format("type '{}' is not an enum or variant in shorthand pattern '.{}'",
                                          subjectType.ToString(), variantName));
                }
            }
        }
        else if (enumPattern->path.size() >= 2) {
            enumName = enumPattern->path[0];
            variantName = enumPattern->path[1];
            if (!currentScope->Lookup(enumName)) {
                EmitError(enumPattern->location, std::format("unknown name '{}' in case pattern", enumName));
            }
            if (const EnumDecl *enumeration = EnumNamed(enumName)) {
                enumDeclaration = enumeration;
            }
        }

        const std::optional<ResolvedCase> resolved =
            enumName.empty() || variantName.empty() ? std::nullopt : LookupCase(enumName, variantName);
        const EnumDecl::Variant *variant = resolved ? resolved->selectedCase : nullptr;
        if (enumDeclaration && !resolved) {
            EmitError(enumPattern->location,
                      enumDeclaration->IsVariant()
                          ? std::format("variant '{}' has no case '{}'", enumName, variantName)
                          : std::format("enum '{}' has no enumerator '{}'", enumName, variantName));
        }
        if (enumDeclaration && subjectType.kind == TypeRef::Kind::Named &&
            BaseTypeName(subjectType.name) != programIndex.NominalName(*enumDeclaration)) {
            EmitError(enumPattern->location, std::format("{} pattern '{}::{}' cannot match value of type '{}'",
                                                         enumDeclaration->IsVariant() ? "variant" : "enum", enumName,
                                                         variantName, subjectType.ToString()));
        }
        const std::size_t actualFields = enumPattern->args.size() + enumPattern->namedArgs.size();
        if (resolved && resolved->form == EnumDecl::Form::Enumeration && actualFields != 0) {
            EmitError(enumPattern->location,
                      std::format("enum enumerator '{}::{}' cannot bind payload fields", enumName, variantName), {},
                      "remove the payload pattern; enums contain constants, while variants carry data");
            return;
        }
        if (resolved && resolved->form == EnumDecl::Form::Variant) {
            const std::size_t expectedFields = variant->fields.size() + variant->namedFields.size();
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
        if (resolved) {
            casePatterns.insert_or_assign(enumPattern,
                                          ResolvedCasePattern{resolved->declaration, resolved->selectedCase,
                                                              resolved->form, subjectType, substitutions});
        }
        // A payload binds like a `let`, but it sits inside a match arm, so a refutable sub-pattern such as a literal
        // or a range is checked as a match pattern rather than refused as an unsupported binding.
        const auto checkPayload = [&](const Pattern &payload, const TypeRef &type) {
            if (dynamic_cast<const IdentPattern *>(&payload) || dynamic_cast<const WildcardPattern *>(&payload) ||
                dynamic_cast<const TuplePattern *>(&payload)) {
                CheckLetPattern(payload, type, false);
            }
            else {
                CheckPattern(payload, type);
            }
        };
        std::unordered_set<std::string> namedArguments;
        for (const auto &argument : enumPattern->namedArgs) {
            if (!namedArguments.insert(argument.name).second) {
                EmitError(argument.location, std::format("duplicate field '{}' in variant pattern", argument.name));
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
                checkPayload(*argument.pattern, ResolveTypeWithSubstitution(*field->type, substitutions));
            }
            else {
                if (variant) {
                    EmitError(argument.location, std::format("unknown field '{}' in variant pattern", argument.name));
                }
                CheckPattern(*argument.pattern);
            }
        }
        for (std::size_t index = 0; index < enumPattern->args.size(); ++index) {
            if (variant && index < variant->fields.size()) {
                checkPayload(*enumPattern->args[index],
                             ResolveTypeWithSubstitution(*variant->fields[index], substitutions));
            }
            else if (variant && index - variant->fields.size() < variant->namedFields.size()) {
                checkPayload(*enumPattern->args[index],
                             ResolveTypeWithSubstitution(*variant->namedFields[index - variant->fields.size()].type,
                                                         substitutions));
            }
            else {
                CheckPattern(*enumPattern->args[index]);
            }
        }
    }
}

void AnalysisContext::ValidateMatchPatterns(const std::vector<const Pattern *> &patterns, const TypeRef &subjectType) {
    if (IsNativeMatchSubject(subjectType)) {
        ValidateNativeMatch(patterns, subjectType);
        return;
    }
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> coveredVariants;
    bool coveredAll = false;
    for (const Pattern *pattern : patterns) {
        if (!pattern) {
            continue;
        }
        if (coveredAll) {
            // An `else` arm covers whatever remains, possibly nothing, so it is never reported.
            if (!dynamic_cast<const WildcardPattern *>(pattern)) {
                EmitError(pattern->location, "match arm is unreachable because an earlier pattern matches every value");
            }
            continue;
        }
        const std::string key = PatternKey(*pattern);
        if (!key.empty() && !seen.insert(key).second) {
            EmitError(pattern->location, "duplicate pattern in match");
        }
        if (const auto *enumerator = dynamic_cast<const EnumPattern *>(pattern);
            enumerator && !enumerator->path.empty()) {
            const std::string &variantName = enumerator->path.back();
            if (const auto resolved = LookupCase(BaseTypeName(subjectType.name), variantName);
                resolved && CasePatternCoversCase(*enumerator, *resolved->selectedCase, resolved->form)) {
                coveredVariants.insert(variantName);
            }
        }
        coveredAll = MatchesWholeSubject(*pattern, subjectType);
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

void AnalysisContext::ValidateMatchPatterns(const MatchExpr &expression, const TypeRef &subjectType) {
    std::vector<const Pattern *> patterns;
    patterns.reserve(expression.arms.size());
    for (const auto &arm : expression.arms) {
        patterns.push_back(arm.pattern.get());
    }
    ValidateMatchPatterns(patterns, subjectType);
}

bool AnalysisContext::MatchPatternsAreExhaustive(const std::vector<const Pattern *> &patterns,
                                                 const TypeRef &subjectType) const {
    if (IsNativeMatchSubject(subjectType)) {
        const auto found =
            patterns.empty() ? nativeMatchExhaustive.end() : nativeMatchExhaustive.find(patterns.front());
        return found != nativeMatchExhaustive.end() && found->second;
    }
    if (std::ranges::any_of(
            patterns, [&](const Pattern *pattern) { return pattern && MatchesWholeSubject(*pattern, subjectType); })) {
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
            if (const auto resolved = LookupCase(enumName, variantName);
                resolved && CasePatternCoversCase(*enumerator, *resolved->selectedCase, resolved->form)) {
                covered.insert(variantName);
            }
        }
    }
    return std::ranges::all_of(declaration->variants,
                               [&](const auto &variant) { return covered.contains(variant.name); });
}

bool AnalysisContext::MatchesWholeSubject(const Pattern &pattern, const TypeRef &subjectType) const {
    if (PatternMatchesEveryValue(pattern)) {
        return true;
    }
    // A typed pattern whose annotation is exactly the subject's type binds the whole subject, on every form.
    const auto *typed = dynamic_cast<const TypedPattern *>(&pattern);
    const TypeRef *annotation = typed ? TypedPatternAnnotation(*typed) : nullptr;
    return annotation && !subjectType.IsUnknown() && *annotation == subjectType;
}

bool AnalysisContext::BlockDefinitelyReturns(const Block &block) const {
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
        // `fail` leaves the function through its failure channel, so no path continues past it.
        if (dynamic_cast<const ReturnStmt *>(&statement) || dynamic_cast<const FailStmt *>(&statement)) {
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

std::optional<AnalysisContext::ResolvedCase> AnalysisContext::LookupCase(const std::string &typeName,
                                                                         const std::string &caseName) const {
    const EnumDecl *enumeration = EnumNamed(typeName);
    if (!enumeration) {
        return std::nullopt;
    }
    for (const auto &candidate : enumeration->variants) {
        if (candidate.name == caseName) {
            return ResolvedCase{enumeration, &candidate, enumeration->form};
        }
    }
    return std::nullopt;
}
} // namespace Rux::SemanticDetail
